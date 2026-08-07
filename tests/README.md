# Tests

Two suites: the **dimmer engine** (kick / ramp / range mapping) and
**bridge-package safety** (the first-flash image that could brick a device).

```sh
cd tests
make            # run both
make engine     # engine only
make bridge     # bridge-package only
```

---

## Bridge-package safety — `bridge_package_test.py`

The bridge package is the one-time image delivered to a **still-stock** dimmer
over Shelly's own OTA. It is the most dangerous artifact in this repo: the device
has no USB, and its only recovery path is the stock firmware still sitting in the
*other* app slot. Anything the package writes onto that slot — or onto the
partition table — destroys the rollback target.

This suite exists because that nearly shipped. The package used to include a
partition table and a filesystem image sized from stock **1.3.3**. On stock
**2.0.0** those same parts would have corrupted the fallback slot two different
ways (a slot declared 28 KB smaller than stock's own app; a filesystem image
overflowing `fs_0` into `app_1`). A bench unit running 1.3.3 could never reveal
it — so the invariant is pinned here instead.

**Layer A** (always runs — no ESPHome, no QEMU, no compile, no proprietary blobs)
drives the *real* packaging code with a synthetic app image, then simulates
applying the resulting manifest against every known stock layout, under both
plausible readings of how stock picks the target slot. It asserts no write ever
lands on the fallback app slot, rewrites the partition table, escapes its own
partition, or runs past end of flash.

**Layer B** (opt-in) assembles a 4 MB flash shaped like a real 2.0.0 device,
applies the package the way stock would, and boots it under the Espressif QEMU
fork against the **real Shelly bootloader** — verifying both that the fallback
slot is byte-for-byte unchanged *and still boots*, and that our firmware loads
from the slot the package targeted with the rollback countdown armed.

Layer B needs Shelly's proprietary bootloader/app, which this repo does not
redistribute. Point it at your own copy (extracted from an OTA zip for your
device); it skips cleanly otherwise:

```sh
SHELLY_FW_DIR=/path/to/stock \
SHELLY_APP_BIN=/path/to/build/firmware.bin \
  make bridge
```

`SHELLY_FW_DIR` needs `bootloader.bin`, `boot_state.bin`, `PlusWallDimmer.bin`.
QEMU is auto-discovered from `~/.espressif/tools/qemu-xtensa` or `$PATH` (it
verifies the binary actually supports `-machine esp32` — the stock distro
`qemu-system-xtensa` does not); override with `QEMU_XTENSA=/path/to/binary`.

---

## Engine tests — `engine_test.cpp`

Standalone unit tests for the dimmer control engine — the kick / ramp /
range-mapping logic that this firmware exists to provide.

They build and run with nothing but a **C++17 compiler**: no ESPHome, no ESP-IDF
toolchain, no hardware. This is possible because the engine
([`components/shelly_wall_dimmer/dimmer_engine.h`](../components/shelly_wall_dimmer/dimmer_engine.h)
and [`dimmer_protocol.h`](../components/shelly_wall_dimmer/dimmer_protocol.h)) is
deliberately framework-agnostic pure C++ — the ESPHome wrapper only drives it.
So a developer or a skeptic can check the behavioral claims against the *actual
shipped code* in a couple of seconds.

### Run

```sh
cd tests
make engine     # builds with g++ and runs; exit code is non-zero on any failure
```

Override the compiler if you like: `make CXX=clang++`.

The test emits a `<n> passed, <n> failed` summary and returns non-zero if
anything fails, so it drops into a script or a pre-commit hook trivially.

### What's covered

Every assertion drives the real engine through its public API (`request()`,
`tick()`, `notify_status()`) and inspects the exact command bytes it puts on the
wire. Groups:

- **range-map** — the min/max brightness *stretch* and its inverse: endpoints
  (HA 0 → min, 100 → max), a midpoint, out-of-window saturation, and a
  device→HA→device **round-trip stability** sweep across several windows
  (including the shipped `min=1` default).
- **kick** — the single-pivot kick: strike to `kick_level` first, then dwell +
  ramp **down** below it, ramp **up** with no dwell above it, land exactly on it,
  and jump straight to target when disabled.
- **ramp** — a change while on ramps in multiple steps and lands exactly on the
  setpoint; a partial final step goes to the setpoint with no overshoot/dither
  (step > 1 rate); and `ramp_on_change: off` jumps instantly.
- **off** — an immediate off preserves the brightness bits (the falsifiable wire
  prediction), and a fade-out ramps to 0 before sending the off command.
- **no-op** — a device report reflected back through the light layer does not
  re-command the co-processor.

### What is NOT covered here

These need the full build or a real device, and are checked by `esphome compile`
(integration) and on-device bring-up rather than by this suite:

- the ESPHome wrapper glue (entities, UART, publish-on-change);
- SH0S boot-state writes and DFU staging *on real flash* — they call
  `esp_partition` / ROM CRC, so they only run on the ESP32 (the bridge suite's
  QEMU layer does exercise the bootloader's side of that record);
- UART framing against the live co-processor and real-world timing.

The bridge package itself is covered by the bridge-package suite above.
