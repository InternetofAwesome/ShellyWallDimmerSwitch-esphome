#!/usr/bin/env python3
"""
Bridge-package safety tests.

The bridge package is the one-time image delivered to a STILL-STOCK dimmer over
Shelly's own OTA. Getting it wrong is the single most dangerous thing this repo
does: the device has no USB, and its only recovery path is the *stock firmware
still sitting in the other app slot*. Any package part that lands on that slot
-- or on the partition table -- destroys the rollback target, and a failed boot
after that is an unrecoverable brick.

This nearly shipped for real. The package used to include a `pt` (partition
table) and an `fs` (filesystem) part, both generated at our build's own sizes.
Those sizes are cut from stock 1.3.3. On a stock 2.0.0 device:

  * our `pt` declares app slots of 0x180000, but 2.0.0's own app image is
    0x186E00 -- 28 KB LARGER -- so the stock image in the fallback slot would no
    longer fit the partition our table declared for it; and
  * our `fs` image is 0x70000, but 2.0.0's fs_0 is only 0x60000 at 0x1a0000, so
    its last 0x10000 bytes land at 0x200000 == app_1 == the fallback slot.

Both were invisible on a 1.3.3-shaped bench unit and only appear on a different
stock version. That is exactly the kind of bug a single-device test can never
find, so it is pinned here instead.

WHAT IS TESTED
--------------
Layer A (always runs; no ESPHome, no QEMU, no proprietary blobs, no compile):
    Drives the REAL packaging code in components/shelly_wall_dimmer/shelly_pkg.py
    with a synthetic app image, then simulates applying the resulting manifest
    against every known stock flash layout, under both plausible readings of how
    stock resolves the target slot. Asserts no write ever escapes its own
    partition, touches the partition table, or touches the fallback app slot.

Layer B (opt-in; needs QEMU + your own stock firmware files):
    Assembles a 4 MB flash laid out like a real 2.0.0 device, applies the
    package the way stock would, boots it under the Espressif QEMU fork against
    the REAL Shelly bootloader, and asserts both that the bootloader still boots
    and that the fallback slot came through byte-for-byte identical.

    Layer B needs Shelly's proprietary bootloader/app, which this repo does not
    and will not redistribute. Point SHELLY_FW_DIR at your own copy (extracted
    from an OTA zip for your device) to enable it; it SKIPS cleanly otherwise.

Usage:
    python3 bridge_package_test.py                  # Layer A only
    SHELLY_FW_DIR=/path/to/fw \\
    SHELLY_APP_BIN=/path/to/firmware.bin \\
        python3 bridge_package_test.py              # A + B (QEMU)
"""

import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import json

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
COMPONENT = os.path.join(REPO, "components", "shelly_wall_dimmer")

FLASH_SIZE = 0x400000  # 4 MiB
PT_ADDR = 0x8000       # partition table lives here on every stock version

# ---------------------------------------------------------------------------
# Known stock flash layouts.
#
# These are plain geometry facts read off the partition tables in Shelly's own
# OTA packages (no Shelly code or binary is reproduced here). They are the whole
# point of the test: the layouts DIFFER between versions, so a package built
# against one can corrupt another. app slot offsets are stable across versions;
# app slot SIZES and the fs partitions are not.
# ---------------------------------------------------------------------------
STOCK_LAYOUTS = {
    "1.3.3": {
        "nvs":     (0x009000, 0x004000),
        "otadata": (0x00d000, 0x002000),
        "app_0":   (0x010000, 0x180000),
        "fs_0":    (0x190000, 0x070000),
        "app_1":   (0x200000, 0x180000),
        "fs_1":    (0x380000, 0x070000),
        "aux":     (0x3f0000, 0x00c000),
        "shelly":  (0x3fc000, 0x004000),
    },
    "2.0.0": {
        "nvs":     (0x009000, 0x004000),
        "otadata": (0x00d000, 0x002000),
        "app_0":   (0x010000, 0x190000),
        "fs_0":    (0x1a0000, 0x060000),
        "app_1":   (0x200000, 0x190000),
        "fs_1":    (0x390000, 0x060000),
        "aux":     (0x3f0000, 0x00c000),
        "shelly":  (0x3fc000, 0x004000),
    },
}

# Size of the largest stock app image we know of, used to sanity-check that a
# slot we might shrink could still hold stock's own firmware. (2.0.0's app is
# 1,601,024 B per its manifest; 1.3.3's is 1,495,360.)
STOCK_APP_SIZES = {"1.3.3": 1495360, "2.0.0": 1601024}


# ---- tiny test harness ----------------------------------------------------
_passed = 0
_failed = 0
_skipped = 0


def check(cond, msg):
    global _passed, _failed
    if cond:
        _passed += 1
    else:
        _failed += 1
        print(f"  FAIL: {msg}")


def skip(msg):
    global _skipped
    _skipped += 1
    print(f"  SKIP: {msg}")


# ---------------------------------------------------------------------------
# Layer A -- package safety analysis
# ---------------------------------------------------------------------------
def load_packager():
    """Import the REAL shelly_pkg.py with the PlatformIO globals it expects.

    Importing the shipped module (rather than reimplementing it) is the point:
    it means this test tracks the actual packaging behaviour and cannot drift
    away from it.
    """
    path = os.path.join(COMPONENT, "shelly_pkg.py")
    if not os.path.exists(path):
        return None
    g = {"__name__": "shelly_pkg_under_test"}

    class _FakeEnv:  # stands in for the SCons/PlatformIO env
        def AddPostAction(self, *a, **kw):
            pass

        def GetProjectOption(self, *a, **kw):
            return ""

        def subst(self, s):
            return s

    g["Import"] = lambda *a, **kw: None
    g["env"] = _FakeEnv()
    with open(path) as f:
        exec(compile(f.read(), "shelly_pkg.py", "exec"), g)  # noqa: S102
    return g


def make_fake_app(path, size=910_000, version="9.9.9-test", project="sam.shelly_wall_dimmer"):
    """Synthesize a minimal but structurally valid ESP app image.

    Only needs to satisfy shelly_pkg's header/app-desc reader: magic 0xE9 and an
    esp_app_desc_t at file offset 0x20 carrying version + project name.
    """
    buf = bytearray(b"\x00" * size)
    buf[0] = 0xE9  # ESP image magic
    base = 0x20
    buf[base + 0x10:base + 0x10 + len(version)] = version.encode()
    buf[base + 0x30:base + 0x30 + len(project)] = project.encode()
    with open(path, "wb") as f:
        f.write(bytes(buf))
    return path


def build_real_package(tmp, app_path=None):
    """Run the shipped packager and return (zip_path, manifest dict).

    `app_path` lets Layer B package a REAL built firmware.bin, so QEMU boots the
    actual artifact a user would ship rather than a stand-in.
    """
    pkg = load_packager()
    if pkg is None:
        return None, None
    app = app_path or make_fake_app(os.path.join(tmp, "firmware.bin"))
    out = os.path.join(tmp, "out", "bridge.zip")
    made = pkg["_build_package"](app, out)
    if not made:
        return None, None
    with zipfile.ZipFile(made) as z:
        manifest = json.loads(z.read("manifest.json"))
    return made, manifest


def resolve_writes(manifest, layout, app_slot):
    """Translate manifest parts into concrete (name, start, end) flash writes.

    `app_slot` selects which physical slot the app/fs parts are taken to target,
    so we can test both plausible readings of stock's behaviour: the literal
    "app_0" the manifest names, and the inactive slot stock may resolve for
    itself. A part naming an explicit `addr` is absolute.
    """
    writes = []
    for name, part in manifest.get("parts", {}).items():
        size = part.get("size", 0)
        if "addr" in part:
            start = part["addr"]
        elif "ptn" in part:
            ptn = part["ptn"]
            # Re-point slot-scoped parts at the slot under test.
            if ptn in ("app_0", "app_1"):
                ptn = f"app_{app_slot}"
            elif ptn in ("fs_0", "fs_1"):
                ptn = f"fs_{app_slot}"
            if ptn not in layout:
                # Unknown partition name -- treat as a failure to resolve, which
                # the caller surfaces rather than silently ignoring.
                writes.append((name, None, None))
                continue
            start = layout[ptn][0]
        else:
            continue
        writes.append((name, start, start + size))
    return writes


def overlaps(a_start, a_end, b_start, b_end):
    return a_start < b_end and b_start < a_end


def test_package_never_touches_fallback():
    print("[A] bridge package: fallback-slot safety")
    with tempfile.TemporaryDirectory() as tmp:
        zip_path, manifest = build_real_package(tmp)
        if manifest is None:
            check(False, "could not build a package from the shipped shelly_pkg.py")
            return

        parts = set(manifest.get("parts", {}))
        # The core regression guard. `pt` and `fs` are the two parts that caused
        # the near-miss; if either returns, every assertion below is at risk, so
        # fail loudly and specifically here first.
        check("app" in parts, "package must contain an app part")
        check("pt" not in parts,
              "package ships a 'pt' part -- our partition table is version-specific "
              "and would mis-size the fallback slot on other stock versions")
        check("fs" not in parts,
              "package ships an 'fs' part -- our fs image is version-specific and "
              "overflows fs_0 into app_1 (the fallback slot) on stock 2.0.0")

        for version, layout in STOCK_LAYOUTS.items():
            for active in (0, 1):
                fallback = 1 - active
                fb_start, fb_size = layout[f"app_{fallback}"]
                fb_end = fb_start + fb_size
                writes = resolve_writes(manifest, layout, active)

                for name, start, end in writes:
                    ctx = f"[stock {version}, app in slot {active}] part '{name}'"
                    if start is None:
                        check(False, f"{ctx}: names a partition absent from this layout")
                        continue

                    # 1. Never write into the fallback app slot.
                    check(not overlaps(start, end, fb_start, fb_end),
                          f"{ctx} writes 0x{start:06x}-0x{end:06x}, which overlaps the "
                          f"FALLBACK slot app_{fallback} (0x{fb_start:06x}-0x{fb_end:06x}) "
                          f"-- this destroys the rollback target")

                    # 2. Never rewrite the partition table.
                    check(not overlaps(start, end, PT_ADDR, PT_ADDR + 0x1000),
                          f"{ctx} writes over the partition table at 0x{PT_ADDR:06x}")

                    # 3. Stay inside the partition it claims, and inside flash.
                    check(end <= FLASH_SIZE, f"{ctx} runs past the end of flash")
                    part = manifest["parts"][name]
                    ptn = part.get("ptn")
                    if ptn:
                        ptn = ptn.replace("_0", f"_{active}").replace("_1", f"_{active}")
                        if ptn in layout:
                            p_start, p_size = layout[ptn]
                            check(end <= p_start + p_size,
                                  f"{ctx} is {end - (p_start + p_size)} bytes larger than "
                                  f"{ptn} on stock {version} -- it would spill into whatever "
                                  f"follows")

        # 4. The app must fit the SMALLEST app slot across known stock versions.
        app_size = manifest["parts"]["app"]["size"]
        smallest = min(l["app_0"][1] for l in STOCK_LAYOUTS.values())
        check(app_size <= smallest,
              f"app is {app_size} B but the smallest known stock app slot is {smallest} B")


def test_our_layout_would_not_shrink_stock_slots():
    """Guard the specific hazard behind dropping the `pt` part.

    If a future change ever reintroduces a partition table, this states the rule
    it must satisfy: our declared app-slot size can never be smaller than any
    stock app image that might be sitting in the fallback slot.
    """
    print("[A] partition-table sizing vs stock app images")
    csv = os.path.join(COMPONENT, "partitions.csv")
    if not os.path.exists(csv):
        skip("partitions.csv not found")
        return
    our_app_slot = None
    with open(csv) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#") or not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 5 and cols[0] == "app_0":
                our_app_slot = int(cols[4], 0)
    if our_app_slot is None:
        skip("could not parse app_0 from partitions.csv")
        return

    for version, size in STOCK_APP_SIZES.items():
        fits = size <= our_app_slot
        # Not a hard failure: our table is never shipped to the device (that is
        # exactly why the `pt` part was dropped). It is reported so the coupling
        # stays visible if anyone reconsiders shipping it.
        status = "ok" if fits else "SMALLER THAN STOCK"
        print(f"      our app_0=0x{our_app_slot:06x} vs stock {version} app "
              f"{size} B (0x{size:X}) -> {status}"
              + ("" if fits else "  <-- must NOT ship a `pt` part"))
    check(True, "informational")


# ---------------------------------------------------------------------------
# Layer B -- QEMU boot against the real Shelly bootloader
# ---------------------------------------------------------------------------
def _supports_esp32(binary):
    """Only Espressif's QEMU fork has the `esp32` machine; the stock distro
    qemu-system-xtensa does not. Verify rather than assume -- picking the wrong
    one produces a confusing 'unsupported machine type' instead of a clean skip.
    """
    try:
        out = subprocess.run([binary, "-machine", "help"],
                             capture_output=True, text=True, timeout=15)
        return "esp32" in (out.stdout + out.stderr)
    except (OSError, subprocess.SubprocessError):
        return False


def find_qemu():
    candidates = []
    env = os.environ.get("QEMU_XTENSA")
    if env:
        candidates.append(env)
    # Espressif's install location first -- it's the fork that has -machine esp32.
    base = os.path.expanduser("~/.espressif/tools/qemu-xtensa")
    if os.path.isdir(base):
        for root, _dirs, files in os.walk(base):
            if "qemu-system-xtensa" in files:
                candidates.append(os.path.join(root, "qemu-system-xtensa"))
    on_path = shutil.which("qemu-system-xtensa")
    if on_path:
        candidates.append(on_path)

    for c in candidates:
        if os.path.exists(c) and _supports_esp32(c):
            return c
    return None


def place(image, blob, offset):
    image[offset:offset + len(blob)] = blob


def apply_manifest_to_flash(image, zip_path, manifest, layout, app_slot):
    """Apply the package the way stock would: each part written at its partition."""
    with zipfile.ZipFile(zip_path) as z:
        for name, part in manifest.get("parts", {}).items():
            src = part.get("src")
            if not src:
                continue
            data = z.read(src)
            for wname, start, _end in resolve_writes(manifest, layout, app_slot):
                if wname == name and start is not None:
                    place(image, data, start)


def test_qemu_boot_preserves_fallback():
    print("[B] QEMU: apply package to a 2.0.0-layout flash, boot, verify fallback")
    fw_dir = os.environ.get("SHELLY_FW_DIR")
    app_bin = os.environ.get("SHELLY_APP_BIN")
    qemu = find_qemu()

    if not qemu:
        skip("qemu-system-xtensa not found (set QEMU_XTENSA=/path/to/qemu-system-xtensa)")
        return
    if not fw_dir or not os.path.isdir(fw_dir):
        skip("SHELLY_FW_DIR not set -- needs your own stock bootloader/otadata "
             "(not redistributable, so not in this repo)")
        return

    boot = os.path.join(fw_dir, "bootloader.bin")
    otad = os.path.join(fw_dir, "boot_state.bin")
    stock_app = os.path.join(fw_dir, "PlusWallDimmer.bin")
    missing = [p for p in (boot, otad, stock_app) if not os.path.exists(p)]
    if missing:
        skip(f"missing in SHELLY_FW_DIR: {', '.join(os.path.basename(m) for m in missing)}")
        return
    if not app_bin or not os.path.exists(app_bin):
        skip("SHELLY_APP_BIN not set to a built ESPHome firmware.bin")
        return

    layout = STOCK_LAYOUTS["2.0.0"]

    with tempfile.TemporaryDirectory() as tmp:
        # Package the REAL firmware, so what QEMU boots is the actual artifact.
        zip_path, manifest = build_real_package(tmp, app_path=app_bin)
        if manifest is None:
            check(False, "could not build package for QEMU test")
            return

        # Emulate a 2.0.0 device mid-update: stock is COMMITTED in app_0, and the
        # package targets the inactive slot (app_1). app_0 is therefore the
        # fallback we must protect.
        image = bytearray(b"\xff" * FLASH_SIZE)
        place(image, open(boot, "rb").read(), 0x1000)
        place(image, build_pt_binary(layout), PT_ADDR)
        place(image, open(otad, "rb").read(), 0xd000)
        place(image, open(stock_app, "rb").read(), layout["app_0"][0])

        fb_start, fb_size = layout["app_0"]
        before = bytes(image[fb_start:fb_start + fb_size])

        # Apply the package exactly as stock would, into the inactive slot.
        apply_manifest_to_flash(image, zip_path, manifest, layout, app_slot=1)

        after = bytes(image[fb_start:fb_start + fb_size])
        check(before == after,
              "applying the bridge package modified the FALLBACK slot app_0 -- "
              "the stock rollback target was corrupted")

        flash = os.path.join(tmp, "flash.bin")
        with open(flash, "wb") as f:
            f.write(image)

        try:
            proc = subprocess.run(
                [qemu, "-nographic", "-machine", "esp32",
                 "-drive", f"file={flash},if=mtd,format=raw", "-no-reboot"],
                capture_output=True, text=True, timeout=40,
            )
            log = proc.stdout + proc.stderr
        except subprocess.TimeoutExpired as e:
            log = (e.stdout or b"").decode("latin1") + (e.stderr or b"").decode("latin1")

        # The bootloader must still run and reach a boot decision. (The app then
        # panics in QEMU at radio init -- there is no WiFi peripheral to
        # emulate -- which is expected and happens well after the boot choice.)
        check("Shelly OS loader" in log or "shos" in log.lower(),
              "Shelly bootloader banner not seen -- flash image may be malformed")
        check("no valid boot state" not in log,
              "bootloader reported no valid boot state (otadata seed is bad)")
        if "Booting app" in log:
            check(True, "bootloader reached a boot decision")
        else:
            check(False, f"bootloader never announced a slot; tail:\n{log[-600:]}")

        # The fallback must not merely survive as bytes -- it must still RUN.
        check(f"offset 0x{fb_start:x}" in log,
              f"bootloader did not load the fallback app from 0x{fb_start:x}")
        check("PlusWallDimmer" in log,
              "stock firmware did not start from the fallback slot after the package "
              "was applied -- rollback would not work")

        # --- second scenario: boot the slot the package actually targeted -----
        # Point the boot record at app_1 (uncommitted, with attempts left, exactly
        # as a staged DFU would) and confirm OUR firmware loads under the REAL
        # stock bootloader. This is the half that proves the delivered image is
        # bootable on this stock version, not just that stock survived.
        image2 = bytearray(image)
        place(image2, make_shos_otadata(open(otad, "rb").read(),
                                        active_slot=1, committed=0,
                                        boot_attempts=3, revert_slot=0), 0xd000)
        flash2 = os.path.join(tmp, "flash_target.bin")
        with open(flash2, "wb") as f:
            f.write(image2)
        try:
            proc2 = subprocess.run(
                [qemu, "-nographic", "-machine", "esp32",
                 "-drive", f"file={flash2},if=mtd,format=raw", "-no-reboot"],
                capture_output=True, text=True, timeout=40,
            )
            log2 = proc2.stdout + proc2.stderr
        except subprocess.TimeoutExpired as e:
            log2 = (e.stdout or b"").decode("latin1") + (e.stderr or b"").decode("latin1")

        tgt_start = layout["app_1"][0]
        check(f"offset 0x{tgt_start:x}" in log2,
              f"bootloader did not load our app from the targeted slot 0x{tgt_start:x}; "
              f"tail:\n{log2[-500:]}")
        # The staged-DFU record must be understood as such: an uncommitted boot of
        # slot 1 with attempts remaining, i.e. the rollback countdown is armed.
        check("boot of app 1" in log2,
              "bootloader did not report an uncommitted boot of slot 1 -- the SH0S "
              "DFU record was not interpreted as staged")
        # Note: the app then panics in QEMU at WiFi radio init (there is no radio
        # peripheral to emulate), which is expected and happens long after the boot
        # decision, so it is deliberately not asserted on. Likewise, a
        # "PT1 is not valid" line is an artifact of this synthetic flash not
        # carrying the backup partition-table copies a factory device has; the
        # bootloader falls through and boots normally regardless.


def make_shos_otadata(base_record, active_slot, committed, boot_attempts, revert_slot,
                      seq=2607100777):
    """Build an 8 KB SH0S otadata image selecting a given boot slot.

    Mirrors components/shelly_wall_dimmer/boot_state.h: a 512-byte record whose
    control bytes live at +0x1d0/+0x1d1, sealed with the two ROM-crc32 fields at
    +0x1c (header) and +0x1fc (body). Seeded from a real stock record so every
    unmodelled byte stays authentic.
    """
    import zlib

    def rom_crc32_le(crc, buf):  # == esp_rom_crc32_le, verified against stock
        return zlib.crc32(buf, crc) & 0xFFFFFFFF

    rec = bytearray(base_record[:512])
    struct.pack_into("<I", rec, 0, seq)               # seq
    struct.pack_into("<I", rec, 8, 0x53304853)        # "SH0S"
    struct.pack_into("<I", rec, 12, seq)              # mirror
    c = 1 if committed else 0
    rec[0x1d0] = ((active_slot & 0xF) << 4) | c | (((~c) & 1) << 1)
    rec[0x1d1] = ((boot_attempts & 0xF) << 4) | (revert_slot & 0xF)
    h = rom_crc32_le(0xFFFFFFFF, bytes(rec[0:0x1C]))
    h = rom_crc32_le(h, b"\x00\x00\x00\x00")
    struct.pack_into("<I", rec, 0x1C, h)
    struct.pack_into("<I", rec, 0x1FC, rom_crc32_le(0xFFFFFFFF, bytes(rec[0:0x1FC])))

    img = bytearray(b"\xff" * 0x2000)
    img[0:512] = rec           # copy 0 @ 0x0
    img[0x1000:0x1000 + 512] = rec  # copy 1 @ 0x1000
    return bytes(img)


def build_pt_binary(layout):
    """Build an ESP-IDF partition-table image for a layout (so Layer B needs no
    Shelly partition-table blob). Entry: magic, type, subtype, offset, size,
    name[16], flags -- then the 0xEBEB.. MD5 marker IDF appends."""
    import hashlib
    types = {"nvs": (1, 2), "otadata": (1, 0), "app_0": (0, 0x10), "app_1": (0, 0x11),
             "fs_0": (1, 0x82), "fs_1": (1, 0x82), "aux": (0x55, 0), "shelly": (1, 0x88)}
    out = b""
    for name in ("nvs", "otadata", "app_0", "fs_0", "app_1", "fs_1", "aux", "shelly"):
        off, size = layout[name]
        t, st = types[name]
        out += struct.pack("<2sBBII16sI", b"\xaa\x50", t, st, off, size,
                           name.encode().ljust(16, b"\0"), 0)
    md5 = hashlib.md5(out).digest()  # noqa: S324  (format-mandated, not security)
    out += b"\xeb\xeb" + b"\xff" * 14 + md5
    return out.ljust(0x1000, b"\xff")


def main():
    print("bridge-package safety tests\n")
    test_package_never_touches_fallback()
    test_our_layout_would_not_shrink_stock_slots()
    test_qemu_boot_preserves_fallback()
    print(f"\n{_passed} passed, {_failed} failed, {_skipped} skipped")
    return 1 if _failed else 0


if __name__ == "__main__":
    sys.exit(main())
