# Engine tests

Standalone unit tests for the dimmer control engine — the kick / ramp /
range-mapping logic that this firmware exists to provide.

They build and run with nothing but a **C++17 compiler**: no ESPHome, no ESP-IDF
toolchain, no hardware. This is possible because the engine
([`components/shelly_wall_dimmer/dimmer_engine.h`](../components/shelly_wall_dimmer/dimmer_engine.h)
and [`dimmer_protocol.h`](../components/shelly_wall_dimmer/dimmer_protocol.h)) is
deliberately framework-agnostic pure C++ — the ESPHome wrapper only drives it.
So a developer or a skeptic can check the behavioral claims against the *actual
shipped code* in a couple of seconds.

## Run

```sh
cd tests
make            # builds with g++ and runs; exit code is non-zero on any failure
```

Override the compiler if you like: `make CXX=clang++`.

The test emits a `<n> passed, <n> failed` summary and returns non-zero if
anything fails, so it drops into a script or a pre-commit hook trivially.

## What's covered

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

## What is NOT covered here

These need the full build or a real device, and are checked by `esphome compile`
(integration) and on-device bring-up rather than by this suite:

- the ESPHome wrapper glue (entities, UART, publish-on-change);
- SH0S boot-state and the DFU staging — they call `esp_partition` / ROM CRC and
  write real flash, so they only run on the ESP32;
- the bridge-package builder — needs a compiled `firmware.bin` to wrap;
- UART framing against the live co-processor and real-world timing.
