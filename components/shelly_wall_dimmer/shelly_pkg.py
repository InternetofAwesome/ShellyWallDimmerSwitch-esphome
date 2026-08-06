# PlatformIO post-build hook: assemble a stock-format Shelly OTA package from
# the ESPHome build artifacts, and (optionally) push it straight to a device.
#
# This is the "bridge" image: the one-time package that gets our ESPHome
# firmware onto a still-stock dimmer via Shelly's own OTA. After that first
# flash, updates go over ESPHome-native OTA (+ our SH0S DFU staging) and this
# package is never needed again.
#
# It is injected by the component's __init__.py ONLY when the user's YAML has a
# `bridge_package:` block, via:
#   extra_scripts = post:<this file>
#   custom_shelly_bridge  = 1
#   custom_shelly_fs_img  = <abs path to bridge_fs_empty.img>
#   custom_shelly_push_ip = <device ip>        (present only if push requested)
#
# Everything here is Python stdlib only -- it runs inside PlatformIO's own
# interpreter, which has no guaranteed third-party packages.
#
# Package composition (all self-contained; no Shelly binaries redistributed):
#   pt  = the partition-table.bin generated from our own partitions.csv
#   app = this build's firmware.bin (ESPHome app image)
#   fs  = a generic empty littlefs image bundled with the component
# Manifest: compatible "DimmerUS", app name "PlusWallDimmer", cs_sha1+cs_sha256
# per part, no signature (relies on the target's signature-required eFuse being
# unburned -- the solder-free delivery gate; see CLAUDE.md OTA PATH ANALYSIS).

Import("env")  # noqa: F821  (provided by PlatformIO/SCons)

import hashlib
import json
import os
import struct
import zipfile


APP_NAME = "PlusWallDimmer"   # stock's expected app name (manifest "Wrong app name" gate)
COMPATIBLE = "DimmerUS"       # stock's `%s is compatible with` gate
PLATFORM = "esp32"


def _sha(path, algo):
    h = algo()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def _read_app_desc(app_path):
    """Pull version + project name out of the esp_app_desc_t at file offset 0x20."""
    with open(app_path, "rb") as f:
        head = f.read(0x80)
    if len(head) < 0x80 or head[0] != 0xE9:
        return None, None
    base = 0x20
    version = head[base + 0x10:base + 0x30].split(b"\0", 1)[0].decode("latin1", "replace")
    project = head[base + 0x30:base + 0x50].split(b"\0", 1)[0].decode("latin1", "replace")
    return version, project


def _find_partition_table(build_dir):
    for cand in (
        os.path.join(build_dir, "partition_table", "partition-table.bin"),  # esp-idf framework
        os.path.join(build_dir, "partitions.bin"),                          # arduino framework
    ):
        if os.path.exists(cand):
            return cand
    return None


def _build_package(app, pt, fs, out_zip):
    version, project = _read_app_desc(app)
    if not version:
        print("!! shelly-bridge: firmware.bin is not a valid ESP app image; skipping package")
        return None
    # The manifest version is cosmetic -- stock's "same version, skipped" dedupe
    # actually compares the esp_app_desc.version embedded in the *app image*
    # (which is `version` here) against the running firmware's. So we surface the
    # value and warn if it looks like the un-bumped default, which WOULD dedupe on
    # a re-flash. Append the app hash so the manifest string itself is unique.
    app_tag = _sha(app, hashlib.sha256)[:8]
    if version in ("", "0.0.0-dev", "1.0.0"):
        print(f"!! shelly-bridge: app version is '{version}' -- stock OTA dedupes on this. "
              f"Set the `fw_version` substitution to something unique per build.")

    parts_spec = [
        ("pt", pt, {"type": "pt", "addr": 32768}),
        ("app", app, {"type": "app", "ptn": "app_0"}),
        ("fs", fs, {"type": "fs", "ptn": "fs_0", "fs_size": os.path.getsize(fs)}),
    ]
    manifest = {
        "name": APP_NAME,
        "platform": PLATFORM,
        "version": f"{version}+{app_tag}",
        "build_id": f"esphome-{app_tag}",
        "compatible": COMPATIBLE,
        "parts": {},
    }
    for name, path, extra in parts_spec:
        entry = {
            "type": extra["type"],
            "src": os.path.basename(path),
            "size": os.path.getsize(path),
            "cs_sha1": _sha(path, hashlib.sha1),
            "cs_sha256": _sha(path, hashlib.sha256),
        }
        entry.update({k: v for k, v in extra.items() if k != "type"})
        manifest["parts"][name] = entry

    os.makedirs(os.path.dirname(out_zip), exist_ok=True)
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_STORED) as z:
        z.writestr("manifest.json", json.dumps(manifest, indent=1).encode())
        for _, path, _ in parts_spec:
            z.write(path, os.path.basename(path))
    print(f">> shelly-bridge: wrote {out_zip} ({os.path.getsize(out_zip)} B), "
          f"app version '{version}'")
    return out_zip


def _push_to_device(zip_path, device_ip):
    """Serve the zip over an ephemeral HTTP port and trigger Shelly.Update on the
    device. Only needs the device IP; the reachable host IP is derived from the
    route to the device. Blocks until the device has fetched the file (or times
    out), then shuts the server down."""
    import http.server
    import socket
    import threading
    import time
    import urllib.error
    import urllib.request

    # Local IP on the route to the device (no traffic actually sent by connect()).
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((device_ip, 80))
        host_ip = s.getsockname()[0]
    finally:
        s.close()

    serve_dir = os.path.dirname(os.path.abspath(zip_path))
    fname = os.path.basename(zip_path)
    fetched = {"done": False}

    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **kw):
            super().__init__(*a, directory=serve_dir, **kw)

        def log_message(self, fmt, *args):  # quiet; we print our own lines
            pass

        def do_GET(self):
            super().do_GET()
            if fname in self.path:
                fetched["done"] = True

    httpd = http.server.ThreadingHTTPServer((host_ip, 0), Handler)
    port = httpd.server_address[1]
    threading.Thread(target=httpd.serve_forever, daemon=True).start()

    url = f"http://{host_ip}:{port}/{fname}"
    print(f">> shelly-bridge: serving {url}")
    print(f">> shelly-bridge: triggering Shelly.Update on {device_ip} ...")
    try:
        req = urllib.request.Request(
            f"http://{device_ip}/rpc/Shelly.Update",
            data=json.dumps({"url": url}).encode(),
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=15) as r:
            print(f">> shelly-bridge: Shelly.Update accepted ({r.status})")
    except urllib.error.HTTPError as e:
        body = e.read().decode("latin1", "replace")[:300]
        print(f"!! shelly-bridge: Shelly.Update HTTP {e.code}: {body}")
        print("!! If this says local update is disabled, allow it first "
              "(e.g. Sys.SetConfig) and re-run.")
        httpd.shutdown()
        return
    except Exception as e:  # noqa: BLE001
        print(f"!! shelly-bridge: could not reach {device_ip}: {e}")
        httpd.shutdown()
        return

    # Wait for the device to actually pull the file, then linger briefly.
    deadline = time.time() + 120
    while not fetched["done"] and time.time() < deadline:
        time.sleep(0.5)
    if fetched["done"]:
        time.sleep(3)  # let the last bytes drain
        print(">> shelly-bridge: device fetched the package; it will verify, flash the "
              "inactive slot, and reboot. Watch `esphome logs` for the new version.")
    else:
        print("!! shelly-bridge: device never fetched the package within 120s "
              "(check network / that the URL is reachable from the device).")
    httpd.shutdown()


def _after_build(source, target, env):  # noqa: ARG001
    if env.GetProjectOption("custom_shelly_bridge", "0") != "1":
        return
    build_dir = env.subst("$BUILD_DIR")
    app = os.path.join(build_dir, "firmware.bin")
    if not os.path.exists(app):
        print("!! shelly-bridge: firmware.bin not found; skipping package")
        return
    pt = _find_partition_table(build_dir)
    if pt is None:
        print("!! shelly-bridge: partition-table.bin not found; skipping package")
        return
    fs = env.GetProjectOption("custom_shelly_fs_img", "")
    if not fs or not os.path.exists(fs):
        print(f"!! shelly-bridge: fs image missing ({fs}); skipping package")
        return

    out_zip = os.path.join(build_dir, "shelly-bridge", f"{APP_NAME}-bridge.zip")
    made = _build_package(app, pt, fs, out_zip)
    if not made:
        return

    push_ip = env.GetProjectOption("custom_shelly_push_ip", "")
    if push_ip:
        _push_to_device(made, push_ip)
    else:
        print(f">> shelly-bridge: no push target configured; deliver {made} via "
              f"Shelly.Update {{url:...}} yourself.")


# Fire after the app image is produced.
env.AddPostAction("$BUILD_DIR/firmware.bin", _after_build)  # noqa: F821
