#!/usr/bin/env python3
"""
SH0S boot-record semantics, verified against the REAL Shelly bootloader.

Everything this firmware does to stay recoverable rests on one claim: that we
know exactly how Shelly's proprietary boot record (SH0S, in the `otadata`
partition) drives the bootloader's slot choice. Get it wrong on a device with
two app slots and no USB port and there is no way back without opening the wall
plate and soldering. So the claim is not argued here, it is executed: seeded
records are fed to Shelly's actual bootloader running under Espressif's QEMU
fork, and the boot decisions are read out of its own log.

    python3 tests/fetch_stock_fw.py     # one-time, retrieves stock images
    cd tests && make boot-matrix

Skips cleanly (exit 0) without QEMU or the stock firmware.

WHAT IS PINNED
--------------
The load-bearing behavior, as a 2x2 over the committed flag `c` and the
boot-attempts counter `ba`, run against BOTH shipped bootloader builds:

    c=1, ba=3  ->  boots the active slot indefinitely, ba IGNORED, no writes
    c=0, ba=3  ->  counts ba down on each boot, then reverts to the revert slot

The first row is why an image that commits itself is permanent. The second is
why a broken image is self-recovering. The uncommitted app used here crash-loops
under QEMU (no radio peripheral exists to initialize), which is precisely the
"bad image" the countdown is designed to catch -- so the revert path is driven
by a genuine boot failure rather than a simulated one.

This 2x2 also retracts a wrong claim that previously lived in boot_state.h --
that ba>0 defeats c=1. It does not. See the note in BootState::commit().
"""
import os
import re
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
STOCK = os.path.join(HERE, "stock_fw")

MAGIC = 0x53304853  # "SH0S"
COPY_LEN = 0x1000
FLASH_SIZE = 0x400000
OTADATA_OFF = 0xD000
APP0_OFF = 0x10000
APP1_OFF = 0x200000

# Both shipped bootloader builds. Each is paired with ITS OWN partition table and
# application image: stock app sizes differ between versions (2.0.0's app does
# not fit 1.3.3's slots), so mixing them would fail for reasons unrelated to the
# boot record.
VERSIONS = ("1.3.3", "2.0.0")

_pass = _fail = _skip = 0


def check(cond, msg):
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print(f"  FAIL: {msg}")


def skip(msg):
    global _skip
    _skip += 1
    print(f"  SKIP: {msg}")


# ---- SH0S record construction (mirrors boot_state.h::seal_crcs_) -------------
def _crc(crc, buf):
    return zlib.crc32(buf, crc) & 0xFFFFFFFF


def seal(rec):
    h = _crc(0xFFFFFFFF, bytes(rec[0:0x1C]))
    h = _crc(h, b"\x00\x00\x00\x00")
    rec[0x1C:0x20] = struct.pack("<I", h)
    rec[0x1FC:0x200] = struct.pack("<I", _crc(0xFFFFFFFF, bytes(rec[0:0x1FC])))


def make_record(base_record, seq, as_, c, ba, rs, break_crc=False):
    """One 512-byte SH0S record, CRC-sealed.

    break_crc=True edits a control byte AFTER sealing, so the record is
    structurally perfect (magic, seq, mirror all valid) but its CRCs no longer
    match -- exactly the shape of the boot records this project wrote before the
    CRCs were understood.
    """
    rec = bytearray(base_record[0:512])
    struct.pack_into("<I", rec, 0, seq)   # seq
    struct.pack_into("<I", rec, 12, seq)  # mirror
    struct.pack_into("<I", rec, 8, MAGIC)
    # ctrl0: as | mfs(=~c) | c    ctrl1: ba | rs
    rec[0x1D0] = ((as_ & 0xF) << 4) | (c & 1) | (((~c) & 1) << 1)
    rec[0x1D1] = ((ba & 0xF) << 4) | (rs & 0xF)
    seal(rec)
    if break_crc:
        rec[0x1D1] ^= 0x0F  # post-seal edit: stale CRC, everything else intact
    return rec


def otadata_pair(rec0, rec1):
    """8 KB otadata image from two independent copies (either may be None/erased)."""
    img = bytearray(b"\xff" * (2 * COPY_LEN))
    if rec0 is not None:
        img[0:512] = rec0
    if rec1 is not None:
        img[COPY_LEN:COPY_LEN + 512] = rec1
    return img


def make_otadata(base_record, seq, as_, c, ba, rs):
    """Build an 8 KB otadata image with two identical copies."""
    rec = make_record(base_record, seq, as_, c, ba, rs)
    return otadata_pair(rec, bytearray(rec))


def decode(blob, off):
    rec = blob[off:off + 512]
    if rec[:4] == b"\xff\xff\xff\xff":
        return None
    return {
        "seq": struct.unpack("<I", rec[0:4])[0],
        "as": rec[0x1D0] >> 4,
        "c": rec[0x1D0] & 1,
        "ba": rec[0x1D1] >> 4,
        "rs": rec[0x1D1] & 0xF,
    }


def find_qemu():
    env = os.environ.get("QEMU_XTENSA")
    if env and os.path.exists(env):
        return env
    base = os.path.expanduser("~/.espressif/tools/qemu-xtensa")
    found = []
    for root, _dirs, files in os.walk(base):
        if "qemu-system-xtensa" in files:
            found.append(os.path.join(root, "qemu-system-xtensa"))
    return sorted(found)[-1] if found else None


def corrupt_app(fw_dir):
    """Stock app with its payload damaged: the image header still parses, so the
    bootloader makes a normal boot decision and logs it, but the SHA-256 check
    then fails and the CPU resets.

    The payload MUST be non-functional. An earlier version of this test used the
    intact stock app and produced a misleading pass: the 2.0.0 application boots
    far enough under QEMU to run Shelly's OWN recovery logic and revert itself
    ("W shos_ota.cpp:1141 Reverting to slot 1"), which looks identical in the
    final otadata to a bootloader-driven revert but is a completely different
    mechanism. Using an image that cannot execute isolates the bootloader, which
    is the only thing this file claims to characterize.
    """
    with open(os.path.join(fw_dir, "PlusWallDimmer.bin"), "rb") as f:
        app = bytearray(f.read())
    for off in (0x8000, 0x9000):
        if off < len(app):
            app[off] ^= 0xFF
    return bytes(app)


def build_flash(fw_dir, otadata, out_path, app_payload):
    img = bytearray(b"\xff" * FLASH_SIZE)

    def place(name, off):
        with open(os.path.join(fw_dir, name), "rb") as f:
            data = f.read()
        img[off:off + len(data)] = data

    place("bootloader.bin", 0x1000)
    place("partition-table.bin", 0x8000)
    img[OTADATA_OFF:OTADATA_OFF + len(otadata)] = otadata
    # Both slots get the same non-bootable payload, so neither can run code.
    img[APP0_OFF:APP0_OFF + len(app_payload)] = app_payload
    img[APP1_OFF:APP1_OFF + len(app_payload)] = app_payload
    with open(out_path, "wb") as f:
        f.write(img)


ANSI = re.compile(r"\x1b\[[0-9;]*m")


def run_qemu(qemu, flash, until=None, min_boots=0, seconds=45, serial_log=None):
    """Boot the image and collect UART output.

    SERIAL GOES TO A FILE, NOT STDIO. `-nographic` wires the guest UART to
    QEMU's own stdin/stdout and changes behavior depending on whether stdin is
    a terminal -- so it works when run from a shell and produces ZERO bytes
    when run from a harness whose stdin is not a TTY (or vice versa). That is
    exactly the "passes here, fails there" failure this test hit. `-display
    none -serial file:` writes the UART straight to disk and is identical in
    every environment; stdin is also explicitly detached so nothing can depend
    on it. QEMU's own stdout/stderr is still captured, separately, for
    reporting process-level errors.

    The payload deliberately cannot boot, so the guest crash-loops forever and
    QEMU never exits on its own. Waiting out `seconds` on every case would cost
    ~45 s each for evidence that lands in the first few. So stream the log and
    stop as soon as it is conclusive: `until` is a substring to look for, and
    `min_boots` ends the run after that many bootloader banners. `seconds`
    remains a hard backstop.

    IMPORTANT: matching `until` does NOT stop the run immediately. The
    bootloader logs its decision BEFORE persisting the resulting record, so
    killing QEMU on the log line captures the decision but loses the write --
    which reads back as "the revert never happened". Instead, run on until the
    NEXT boot banner: by then the record has been written AND re-read by the
    bootloader, which is the state this test actually asserts on.
    """
    proc = subprocess.Popen(
        [qemu, "-machine", "esp32",
         "-drive", f"file={flash},if=mtd,format=raw",
         "-display", "none", "-serial", f"file:{serial_log}"],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, errors="replace",
    )

    # Tail the serial file while the guest runs, so we keep both the live
    # progress output and the ability to stop as soon as the result is in.
    deadline = time.time() + seconds
    pos, pending, boots, seen_until, stop = 0, "", 0, False, False
    try:
        while not stop:
            grew = False
            if os.path.exists(serial_log):
                with open(serial_log, "r", errors="replace") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
                if chunk:
                    grew = True
                    pending += chunk
                    while "\n" in pending and not stop:
                        line, pending = pending.split("\n", 1)
                        is_banner = "Shelly OS loader" in line
                        if is_banner:
                            boots += 1
                            print(".", end="", flush=True)
                        if "failed to boot, reverting" in line:
                            print("R", end="", flush=True)
                        elif "Uncommitted boot" in line:
                            print("u", end="", flush=True)
                        if until is not None:
                            if seen_until and is_banner:
                                stop = True  # post-decision write is persisted
                            if until in line:
                                seen_until = True
                        elif min_boots and boots >= min_boots:
                            stop = True
            if stop:
                break
            if time.time() > deadline:
                break
            if proc.poll() is not None and not grew:
                break  # QEMU exited and the file stopped growing
            if not grew:
                time.sleep(0.05)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
        proc_out = ""
        if proc.stdout:
            try:
                proc_out = proc.stdout.read() or ""
            except Exception:  # noqa: BLE001
                pass
            proc.stdout.close()

    serial = ""
    if os.path.exists(serial_log):
        with open(serial_log, "r", errors="replace") as f:
            serial = f.read()
    # Surface QEMU's own diagnostics (bad args, missing accel, ...) alongside
    # the guest log, so a process-level failure is never silently empty.
    if proc_out.strip():
        serial += "\n[qemu process output] " + proc_out.strip()
    return ANSI.sub("", serial)


def run_case(qemu, fw_dir, tmp, version, label, c, ba, until=None, min_boots=0):
    """Seed a record, boot it, and return (log, final otadata state)."""
    with open(os.path.join(fw_dir, "boot_state.bin"), "rb") as f:
        base = f.read()
    ota = make_otadata(base, seq=2607100050, as_=0, c=c, ba=ba, rs=1)
    flash = os.path.join(tmp, f"flash_{version}_{label}.bin")
    build_flash(fw_dir, ota, flash, corrupt_app(fw_dir))
    serial_log = os.path.join(tmp, f"serial_{version}_{label}.log")
    log = run_qemu(qemu, flash, until=until, min_boots=min_boots, serial_log=serial_log)
    with open(flash, "rb") as f:
        final = f.read()

    # QEMU's stderr is merged into `log`, so when a run produces no bootloader
    # output at all the reason is almost always sitting right there -- and
    # silently discarding it turns an explainable failure into a mystery. Dump
    # it, plus enough environment to tell a bad invocation from a bad image.
    if "Shelly OS loader" not in log:
        print()
        print(f"      !! no bootloader output for {version}/{label}. QEMU said:")
        body = log.strip()
        if not body:
            print("         (nothing at all -- QEMU produced no output)")
        for line in body.splitlines()[:15]:
            print(f"         | {line}")
        print(f"         qemu:  {qemu}")
        print(f"         flash: {flash} ({os.path.getsize(flash)} B)")
        print(f"         fw:    {fw_dir}")
        for n in ("bootloader.bin", "partition-table.bin", "boot_state.bin", "PlusWallDimmer.bin"):
            p = os.path.join(fw_dir, n)
            print(f"           {n}: {os.path.getsize(p) if os.path.exists(p) else 'MISSING'}")

    return log, decode(final, OTADATA_OFF)


def test_version(qemu, version, tmp):
    fw_dir = os.path.join(STOCK, version)
    needed = ("bootloader.bin", "partition-table.bin", "boot_state.bin", "PlusWallDimmer.bin")
    missing = [n for n in needed if not os.path.exists(os.path.join(fw_dir, n))]
    if missing:
        skip(f"{version}: missing {', '.join(missing)} -- run tests/fetch_stock_fw.py")
        return

    print(f"\n[{version}] stock bootloader from the {version} package", flush=True)
    print("      legend: . = emulated boot, u = uncommitted-boot countdown, R = revert")

    # --- committed: ba must be IGNORED --------------------------------------
    # A committed record pins its slot. Note what this means with a payload that
    # cannot boot: the device loops forever and never falls back. That is the
    # real cost of committing, and it is why this firmware only auto-commits
    # after an image has demonstrably run for 30 s (see AUTOCOMMIT_HEALTHY_MS).
    print("      c=1 ba=3 (committed; ba must be ignored)  ", end="", flush=True)
    log, final = run_case(qemu, fw_dir, tmp, version, "c1", c=1, ba=3, min_boots=6)
    print()
    banner = next((l.strip() for l in log.splitlines() if "Shelly OS loader" in l), "")
    check(bool(banner), f"{version}: bootloader banner seen")
    if banner:
        print(f"      {banner}")
    boots = len(re.findall(r"Booting app 0", log))
    check(boots >= 4, f"{version}: committed record boots the active slot repeatedly ({boots}x)")
    check("Uncommitted boot" not in log,
          f"{version}: committed record is never treated as a pending boot")
    check("failed to boot, reverting" not in log,
          f"{version}: committed record never reverts, even on a dead image")
    check(final is not None and final["ba"] == 3 and final["c"] == 1 and final["as"] == 0,
          f"{version}: committed record left untouched (as=0 c=1 ba=3)")
    print(f"      c=1 ba=3 -> {boots} boots of app 0, ba never decremented, final {final}")

    # --- uncommitted: matched control, must count down and revert -----------
    print("      c=0 ba=3 (uncommitted; must count down)   ", end="", flush=True)
    log, final = run_case(qemu, fw_dir, tmp, version, "c0", c=0, ba=3,
                          until="failed to boot, reverting")
    print()
    attempts = [int(m) for m in re.findall(r"Uncommitted boot of app 0 \(attempts (\d+)\)", log)]
    check(attempts[:3] == [3, 2, 1],
          f"{version}: ba counts down once per boot (saw {attempts[:4]})")
    check("failed to boot, reverting" in log,
          f"{version}: reverts once ba is exhausted")
    check(final is not None and final["as"] == 1,
          f"{version}: after revert the active slot is the fallback (as=1)")
    check(final is not None and final["rs"] == 0,
          f"{version}: the slot just abandoned becomes the new fallback (rs=0)")
    check(final is not None and final["c"] == 1,
          f"{version}: the reverted slot is committed by the bootloader")
    # Isolation: prove the revert came from the BOOTLOADER, not from Shelly's
    # application. An earlier version of this test used the intact stock app,
    # which boots far enough under QEMU to revert itself via its own recovery
    # path ("shos_ota.cpp:NNNN Reverting to slot 1") -- same end state, entirely
    # different mechanism. App-level ESP-IDF logs carry a file:line tag; the
    # bootloader's do not.
    check(not re.search(r"shos_ota\.cpp:\d+", log),
          f"{version}: no application-level revert ran -- the countdown is the "
          f"bootloader's alone")
    print(f"      c=0 ba=3 -> attempts {attempts[:3]} then revert, final {final}")


def run_pair(qemu, fw_dir, tmp, tag, rec0, rec1, until=None, min_boots=0, seconds=45):
    """Boot a flash seeded with two independently-specified otadata copies."""
    flash = os.path.join(tmp, f"flash_{tag}.bin")
    build_flash(fw_dir, otadata_pair(rec0, rec1), flash, corrupt_app(fw_dir))
    serial_log = os.path.join(tmp, f"serial_{tag}.log")
    log = run_qemu(qemu, flash, until=until, min_boots=min_boots,
                   seconds=seconds, serial_log=serial_log)
    with open(flash, "rb") as f:
        blob = f.read()
    return log, decode(blob, OTADATA_OFF), decode(blob, OTADATA_OFF + COPY_LEN)


def test_record_acceptance(qemu, version, tmp):
    """Pin the bootloader's ACCEPTANCE rules, not just its slot arithmetic.

    Each case here corresponds to a failure this project actually suffered or a
    safety invariant the firmware relies on. They are the reason boot-record
    writes are safe to perform at all.
    """
    fw_dir = os.path.join(STOCK, version)
    with open(os.path.join(fw_dir, "boot_state.bin"), "rb") as f:
        base = f.read()
    N = 2607100050

    print(f"\n[{version}] boot-record acceptance rules")

    # 1. CRCs ARE ENFORCED.
    # This project once believed the record had "no checksum, purely structural"
    # and wrote boot records without resealing the two CRC-32s. Every commit and
    # revert then became a SILENT NO-OP -- the bootloader recomputed the CRCs,
    # rejected the edited copy, and kept booting the untouched one. That cost
    # three bench cycles and produced a wrong model of `ba` that survived in the
    # comments for weeks. Here the higher-seq copy points at slot 1 but has a
    # stale CRC; if CRCs were ignored the device would boot slot 1.
    print("      stale CRC on the higher-seq copy         ", end="", flush=True)
    good = make_record(base, seq=N, as_=0, c=1, ba=0, rs=1)
    stale = make_record(base, seq=N + 10, as_=1, c=1, ba=0, rs=0, break_crc=True)
    log, _, _ = run_pair(qemu, fw_dir, tmp, f"{version}_crc", good, stale, min_boots=3)
    print()
    check("Booting app 0" in log,
          f"{version}: a stale-CRC record is REJECTED (fell back to the valid copy)")
    check("Booting app 1" not in log,
          f"{version}: the stale-CRC copy never wins despite its higher seq")

    # 2. HIGHEST-SEQ *VALID* COPY WINS.
    # The whole one-copy-at-a-time write strategy depends on a newly written
    # copy outranking the old one purely by seq.
    print("      higher-seq valid copy wins               ", end="", flush=True)
    older = make_record(base, seq=N, as_=0, c=1, ba=0, rs=1)
    newer = make_record(base, seq=N + 1, as_=1, c=1, ba=0, rs=0)
    log, _, _ = run_pair(qemu, fw_dir, tmp, f"{version}_seq", older, newer, min_boots=3)
    print()
    check("Booting app 1" in log and "Booting app 0" not in log,
          f"{version}: the higher-seq valid copy decides the boot slot")

    # 3. ONE VALID COPY IS ENOUGH.
    # This is the invariant that makes every write safe: mutate_() only ever
    # touches the NON-winning copy, so a failed, rejected or power-interrupted
    # write leaves the other copy intact and bootable.
    print("      one erased copy, one valid               ", end="", flush=True)
    valid = make_record(base, seq=N, as_=0, c=1, ba=0, rs=1)
    log, _, _ = run_pair(qemu, fw_dir, tmp, f"{version}_single", None, valid, min_boots=3)
    print()
    check("Booting app 0" in log,
          f"{version}: boots normally with the other copy erased "
          f"(single-copy writes are survivable)")

    # 4. ZERO VALID COPIES IS THE BRICK GATE.
    # The counterpart to (3), and the reason the cardinal rule is "never
    # invalidate both copies in one operation". With no valid record the
    # bootloader does NOT fall back to app_0 and does NOT synthesize a default;
    # it reports the condition and stops. On a device with no USB that is
    # unrecoverable without soldering.
    print("      both copies erased (brick gate)          ", end="", flush=True)
    log, _, _ = run_pair(qemu, fw_dir, tmp, f"{version}_none", None, None,
                         until="no valid boot state", seconds=20)
    print()
    check("no valid boot state" in log.lower(),
          f"{version}: with zero valid copies the bootloader reports it")
    check("Booting app" not in log,
          f"{version}: with zero valid copies NO app is booted -- confirms the "
          f"brick gate, and why writes touch one copy at a time")


def main():
    print("SH0S boot-record semantics vs the real Shelly bootloader (QEMU)")
    qemu = find_qemu()
    if not qemu:
        skip("qemu-system-xtensa not found (set QEMU_XTENSA=/path/to/qemu-system-xtensa)")
        print(f"\n[boot-matrix] {_pass} passed, {_fail} failed, {_skip} skipped")
        return 0
    if not os.path.isdir(STOCK):
        skip("no tests/stock_fw -- run `python3 tests/fetch_stock_fw.py` first")
        print(f"\n[boot-matrix] {_pass} passed, {_fail} failed, {_skip} skipped")
        return 0

    import tempfile
    with tempfile.TemporaryDirectory() as tmp:
        for version in VERSIONS:
            test_version(qemu, version, tmp)
            test_record_acceptance(qemu, version, tmp)

    # The cross-version claim is only meaningful if both actually ran.
    print()
    print(f"[boot-matrix] {_pass} passed, {_fail} failed, {_skip} skipped")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
