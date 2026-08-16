#!/usr/bin/env python3
"""
Fetch the stock Shelly firmware packages the QEMU tests need.

This repo does not and will not redistribute Shelly's bootloader or application
images. It does not have to: the packages are served from Shelly's own CDN at a
content-addressed URL, so this script retrieves them on demand and drops them
where the tests look. Nothing downloaded here is ever committed.

    python3 tests/fetch_stock_fw.py

Produces:

    tests/stock_fw/1.3.3/{bootloader,partition-table,boot_state,PlusWallDimmer,fs}.*
    tests/stock_fw/2.0.0/{...}

which is the layout tests/boot_matrix_test.py uses directly, and which
bridge_package_test.py accepts as SHELLY_FW_DIR (it wants bootloader.bin,
boot_state.bin and PlusWallDimmer.bin, all present in each version directory).

INTEGRITY
---------
The last path segment of each CDN URL is the SHA-256 of the zip it serves --
verified against a known-good local copy. So the URL is self-authenticating:
every download is checked against the hash embedded in its own address, and each
extracted part is then checked against the cs_sha256 recorded in the package's
own manifest.json. Substituted content cannot pass either check.

That matters because TLS alone will not help you here. fwcdn.shelly.cloud
presents a certificate issued by Allterco's PRIVATE CA, not a public one, so a
stock trust store rejects it and the CA is not something this repo ships. We
therefore attempt a normally-verified connection first and fall back to an
unverified transport, relying on the content hashes above for integrity. The
hash pin is the actual security boundary, not the TLS session.
"""
import hashlib
import json
import os
import ssl
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEST = os.path.join(HERE, "stock_fw")
CDN = "https://fwcdn.shelly.cloud/gen2-ntest/PlusWallDimmer/"

# version -> sha256 of the package zip, which is also its CDN path segment.
PACKAGES = {
    "1.3.3": "ca4e5321a4a89d7a462d7641c028804205c018809d2e62cd2de7fb20c3ad6fd7",
    "2.0.0": "0dcfd9159907e177cca1ae2613e2389c0129c5e4e3044f4a75f10f854de0f931",
}

# The tests expect the otadata part under one name; 1.3.3 calls it otadata.bin
# and 2.0.0 calls it boot_state.bin. Normalize to the 2.0.0 spelling.
OTADATA_ALIASES = ("boot_state.bin", "otadata.bin")


def _download(url):
    """GET url, preferring a verified TLS session, falling back to unverified.
    The caller hash-checks the result either way (see module docstring)."""
    try:
        with urllib.request.urlopen(url, timeout=120) as r:
            return r.read(), "verified TLS"
    except ssl.SSLError:
        pass
    except urllib.error.URLError as e:
        if not isinstance(getattr(e, "reason", None), ssl.SSLError):
            raise
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with urllib.request.urlopen(url, timeout=120, context=ctx) as r:
        return r.read(), "unverified TLS (content pinned by SHA-256)"


def fetch(version, want_sha):
    out_dir = os.path.join(DEST, version)
    marker = os.path.join(out_dir, ".sha256")
    if os.path.exists(marker) and open(marker).read().strip() == want_sha:
        print(f"  {version}: already present, skipping")
        return out_dir

    url = CDN + want_sha
    print(f"  {version}: downloading {url}")
    blob, how = _download(url)

    got = hashlib.sha256(blob).hexdigest()
    if got != want_sha:
        print(f"!! {version}: SHA-256 MISMATCH\n     expected {want_sha}\n     got      {got}")
        return None
    print(f"  {version}: {len(blob)} B, sha256 OK ({how})")

    os.makedirs(out_dir, exist_ok=True)
    zpath = os.path.join(out_dir, "package.zip")
    with open(zpath, "wb") as f:
        f.write(blob)

    with zipfile.ZipFile(zpath) as z:
        z.extractall(out_dir)
        manifest = json.loads(z.read("manifest.json"))

    # Cross-check every extracted part against the package's own manifest.
    ok = True
    for name, part in manifest.get("parts", {}).items():
        src = part.get("src")
        want = part.get("cs_sha256")
        if not src or not want:
            continue  # e.g. the `nvs` erase directive, which has no src
        path = os.path.join(out_dir, src)
        if not os.path.exists(path):
            print(f"!! {version}: manifest names {src} but it is not in the zip")
            ok = False
            continue
        with open(path, "rb") as f:
            got = hashlib.sha256(f.read()).hexdigest()
        if got != want:
            print(f"!! {version}: part '{name}' ({src}) failed cs_sha256")
            ok = False
    if not ok:
        return None
    print(f"  {version}: all manifest parts verified")

    # Normalize the otadata filename so both versions look the same to tests.
    have = [a for a in OTADATA_ALIASES if os.path.exists(os.path.join(out_dir, a))]
    if have and not os.path.exists(os.path.join(out_dir, OTADATA_ALIASES[0])):
        os.rename(os.path.join(out_dir, have[0]),
                  os.path.join(out_dir, OTADATA_ALIASES[0]))

    os.remove(zpath)
    with open(marker, "w") as f:
        f.write(want_sha)
    return out_dir


def main():
    print("fetching stock Shelly firmware for the QEMU tests")
    print("(not redistributed by this repo; retrieved from Shelly's CDN)\n")
    os.makedirs(DEST, exist_ok=True)

    results = {}
    for version, sha in PACKAGES.items():
        try:
            results[version] = fetch(version, sha)
        except Exception as e:  # noqa: BLE001 -- report, don't traceback
            print(f"!! {version}: download failed: {e}")
            results[version] = None

    print()
    if not any(results.values()):
        print("nothing fetched. The QEMU layers will skip; the rest of the suite still runs.")
        return 1
    for version, path in results.items():
        print(f"  {version}: {'ok -> ' + path if path else 'FAILED'}")
    print("\nnow runnable:")
    print("    cd tests && make boot-matrix")
    two = results.get("2.0.0")
    if two:
        print(f"    SHELLY_FW_DIR={two} SHELLY_APP_BIN=/path/to/firmware.bin make bridge")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
