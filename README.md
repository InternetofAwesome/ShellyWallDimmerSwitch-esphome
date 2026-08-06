# ShellyWallDimmerSwitch-esphome

Custom [ESPHome](https://esphome.io) firmware for the **Shelly Plus Wall Dimmer US**
(`SNDM-0013US`). It replaces the stock ESP32 firmware while leaving the separate
dimming co-processor (an APT32S003) completely untouched — the ESP32 just speaks
the co-processor's simple single-byte UART protocol.

---

## Overview

Stock firmware can't briefly overshoot brightness on turn-on, which is what many
cheap LED loads need in order to strike before settling to a low level. This
firmware adds that **"kick"** (plus configurable ramp timing and min/max clamps),
and exposes the co-processor's temperature and status to Home Assistant.

You flash it **without opening the device, without USB, and without soldering** —
entirely over the air, using Shelly's own update mechanism to deliver the first
image. In outline:

1. **Build** the firmware in ESPHome. The component also produces a one-time
   *"bridge" package* in Shelly's OTA format as a side-effect of compiling.
2. **Deliver** that package to the still-stock dimmer over its local OTA. The
   device writes it to its **inactive** firmware slot and reboots into it — your
   working stock firmware stays in the other slot the whole time.
3. **It proves itself or rolls back.** If the new firmware runs healthy for 30 s
   it makes itself permanent automatically. If it crash-loops, the bootloader
   counts down and **auto-reverts to stock** — no action needed. This makes the
   first flash reversible.
4. **From then on**, updates go over normal ESPHome Wi-Fi OTA.

> ⚠️ **Status:** tracking `master`, pre-release. Flashing custom firmware to your
> dimmer is at your own risk. It is designed to fail safe (dual-slot + auto-revert
> + a partition-layout guard), but you are still replacing firmware on a mains
> device installed in your wall.

---

## Prerequisites

Before you start, make sure you have:

- **The right hardware.** A **Shelly Plus Wall Dimmer US**, marked `SNDM-0013US`
  (FCC ID `2ALAYSNDM-0013US`). This firmware is specific to that SKU. It is a
  classic dual-core ESP32 talking to an APT32S003 co-processor.
- **The device on your LAN with a known IP**, still running stock firmware, with
  **local access enabled** (default). You'll point the updater at that IP.
- **A factory-fresh-ish unit (for the solder-free path).** Shelly's stock updater
  can require *signed* packages, gated by an eFuse. On factory units that latch is
  typically **unburned**, so it accepts the unsigned package this tool builds. If
  your unit's stock OTA rejects the package with a **signature error**, the
  solder-free path is closed for that unit (recovery would require opening it and
  wiring to GPIO0 — out of scope here).
- **ESPHome**, either:
  - the **ESPHome Builder** add-on in Home Assistant (recommended — easiest), or
  - the **ESPHome CLI** on a machine on the same LAN.
- **A host on the same network the device can reach** — the updater briefly serves
  the package from this machine and tells the device to fetch it.
- **Your Wi-Fi credentials and a `secrets.yaml`** (see
  [`example/secrets.yaml.example`](example/secrets.yaml.example)): Wi-Fi SSID/
  password, an API encryption key, an OTA password, and a fallback-AP password.

No cables, no USB-serial adapter, and no disassembly are required for a
factory unit.

---

## Use it

Reference the component from your ESPHome config:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/InternetofAwesome/ShellyWallDimmerSwitch-esphome
      ref: master
    refresh: 0s
    components: [shelly_wall_dimmer]
```

Start from [`example/shelly-wall-dimmer.yaml`](example/shelly-wall-dimmer.yaml) —
it's a complete device config (light + kick/ramp/clamp controls, temperature and
diagnostic sensors, the front button and status LEDs). Copy
[`example/secrets.yaml.example`](example/secrets.yaml.example) to `secrets.yaml`
and fill it in first.

### Home Assistant / ESPHome Builder

The ESPHome dashboard only lists configs it finds in **`/config/esphome/`**. Drop
your device YAML (based on the example) into `/config/esphome/`, and put your
filled-in `secrets.yaml` in that **same** directory. The `esp32:` block needs an
explicit **`board: esp32dev`** (generic classic-ESP32, 4 MB) — the Builder
requires a board, and this is the correct one for this device.

---

## Flashing a factory switch, step by step

1. **Put the config in place.** Copy the example YAML into `/config/esphome/`
   (Builder) or a working dir (CLI), alongside your `secrets.yaml`.

2. **Give the build a unique version.** Set the `fw_version` substitution to
   something that changes every build (a date/time works well):

   ```yaml
   substitutions:
     fw_version: "2026.08.05-2130"
   ```

   Stock's updater dedupes on the version baked into the app image; if it doesn't
   change, stock downloads the package and silently discards it as "same version."

3. **Point the bridge package at your device.** In the `shelly_wall_dimmer:`
   block, enable the package build and give it the dimmer's IP:

   ```yaml
   shelly_wall_dimmer:
     id: dimmer_coprocessor
     bridge_package:
       push_to: 192.168.10.81   # your dimmer's IP
   ```

4. **Compile.** In the Builder, use **Install → Manual download / compile** (do
   *not* pick a serial port — there's no cable). On the CLI, `esphome compile
   your.yaml`. On success, the component:
   - assembles the Shelly-format package from the build artifacts, then
   - (because `push_to` is set) serves it and calls `Shelly.Update` on your
     device, waits for the device to fetch it, and prints progress lines prefixed
     `>> shelly-bridge:`.

   The device verifies the package, writes it to its **inactive** slot, and
   reboots into it. *(No `push_to`? The package is written under the build's
   `shelly-bridge/` folder; deliver it yourself with
   `Shelly.Update {url: http://<host>/<pkg>.zip}`.)*

5. **Confirm it came up.** Watch **`esphome logs`** (or the Builder log stream).
   You should see the ESPHome boot banner with your `fw_version`, the entities
   appear in Home Assistant, and a `layout guard OK` line from the safety check.

6. **Let it settle.** After ~30 s of healthy running the firmware **auto-commits**
   its slot (you'll see an `auto-commit ... committing` log line) — now it's
   permanent. If instead the image had crash-looped, the bootloader would have
   **auto-reverted to stock** after a few attempts, and you'd be back where you
   started.

7. **Future updates use Wi-Fi OTA.** You no longer need `bridge_package`. Update
   with the Builder's wireless **Install** (or `esphome upload`) like any ESPHome
   device. These OTA writes are made SH0S-safe automatically by the component.

**Recovery.** Because the two firmware slots and auto-revert are always in play, a
bad image reverts itself. To hand control back to stock deliberately, enable the
developer **Boot State: Revert To Stock** button (see below) and press it.

---

## Configuration reference

### `shelly_wall_dimmer:` (the hub)

| Option | Default | Description |
|---|---|---|
| `id` | — | Component id, referenced by the light/number/etc. platforms. |
| `uart_id` | (single UART) | The `uart:` bus wired to the co-processor. Must be **115200 8N1** with both TX and RX (validated at compile time). |
| `update_interval` | `1s` | How often to poll the co-processor (`0xFF`) so temperature/state stay fresh. |
| `bridge_package` | (off) | Build the first-flash Shelly package on compile. See below. |

**`bridge_package:`** — when present, a post-build step assembles the stock-format
Shelly OTA zip from build artifacts only (app image + the partition table built
from this component + a bundled empty filesystem image — **no Shelly binaries are
redistributed**).

| Sub-option | Default | Description |
|---|---|---|
| `push_to` | (none) | Device IP/hostname to push to via `Shelly.Update` right after a good build. Only the device IP is needed — the reachable host address is derived from the route to it. Omit to just produce the zip. |

### `light:` — `platform: shelly_wall_dimmer`

A brightness-only light. Required: `shelly_wall_dimmer_id`. Recommended settings
(see the example):

- `gamma_correct: 0` — the co-processor already applies its own dimming curve;
  ESPHome's default gamma would double-correct and crush low levels to 0.
- `default_transition_length: 0s` — this firmware's own engine owns ramping;
  ESPHome transitions would fight it.

### `number:` — `platform: shelly_wall_dimmer`

One entity per tunable, selected by `type:`. All are `config`-category. Ranges and
defaults:

| `type` | Range (step) | Default | Meaning |
|---|---|---|---|
| `kick_level` | 0–100 % (1) | 20 | The strike level **and** the pivot — every turn-on snaps here first (see kick below). Set to the lowest % your bulb reliably lights at. |
| `kick_dwell_ms` | 0–2000 ms (10) | 150 | How long to hold `kick_level` before ramping **down** — applies only when the target is below `kick_level`. |
| `min_brightness` | 0–100 % (1) | 1 | Low end of the usable brightness window (see range mapping below). |
| `max_brightness` | 0–100 % (1) | 100 | High end of the usable brightness window. |
| `ramp_rate` | 1–1000 %/s (1) | 150 | One shared ramp speed, in **percent per second**, used by every ramp (see ramps below). |

**Range mapping (min/max).** `min_brightness`/`max_brightness` don't clamp — they
**stretch** the 0–100 % command scale across a narrower physical window. Setting
the light to 0 % drives `min_brightness`, 100 % drives `max_brightness`, and
everything in between is linear. With `min=20, max=80`: Home Assistant 0 % → 20 %
real, 50 % → 50 %, 100 % → 80 %. Device reports are inverse-mapped, so HA still
shows a clean 0–100 %. Defaults (`1`/`100`) are effectively a no-op — narrow the
window to use it. `kick_level` is in **real** device terms, so the kick keys off
the actual level the LED sees.

> ⚠️ **Caveat — physical touch dimming is not range-mapped.** The mapping is
> applied only on the *command* path (Home Assistant, automations, and the front
> push-button). Brightness changed **directly at the switch's touch surface** is
> handled by the co-processor and merely *reported* to this firmware — it lands on
> the raw 0–100 scale, so the `min`/`max` window is **not enforced** for physical
> dimming (the touch plate can still drive the load below `min` or above `max`).
> This firmware can't intercept it; it can only re-scale it for display, so Home
> Assistant stays consistent. If you rely on `min` to keep a fussy LED from
> dropping out, know that a physical swipe can still take it under that floor.
> The `limit_correct` switch (below) is the opt-in enforcement — but note it can
> only pull the level back *after* the touch change, not pre-empt it.

**Ramps.** A single `ramp_rate` (percent/second) drives every ramp; the firmware
converts it to a step cadence for you, quantized to a 10 ms floor (~one mains
half-cycle — the TRIAC only acts once per 8.33 ms, so finer buys nothing). It
never dithers: a ramp holds one fixed step and interval, and a leftover partial
step just lands exactly on the setpoint. `ramp_rate` can't be set to zero. Which
transitions actually ramp is controlled by the three switches below; `kick_dwell`
and the kick's own descent to target also use `ramp_rate`.

### `switch:` — `platform: shelly_wall_dimmer`

All `config`-category, selected by `type:`:

| `type` | Default | Description |
|---|---|---|
| `kick_enabled` | **on** | Enable the kick (the strike pulse on a dim turn-on). |
| `ramp_on_change` | **on** | Ramp (rather than jump) on a brightness change while already on. |
| `ramp_on_off` | off | Fade in on turn-on and fade out on turn-off, instead of jumping. |
| `limit_correct` | off | If the **physical touch panel** drives brightness outside `[min,max]`, ramp back to the nearest limit. Best-effort — see the touch caveat above; it reacts after the fact and can't pre-empt a physical swipe. |

With `kick_enabled` on, **every** turn-on from off snaps to `kick_level` first
(the strike — below it the bulb is dark anyway), then reaches the target: it holds
`kick_dwell` and ramps **down** if the target is below `kick_level`, or ramps
**up** with no dwell if above. With `kick_enabled` off, a turn-on either jumps
straight to the target or, with `ramp_on_off`, fades in from 0.

### `sensor:` — `platform: shelly_wall_dimmer`

Co-processor **die temperature** in °C (reported in every status frame).

### `text_sensor:` — `platform: shelly_wall_dimmer`

`diagnostic`-category, selected by `type:`:

| `type` | Description |
|---|---|
| `last_frame` | The most recent raw 5-byte status frame, as hex — protocol diagnostics. |
| `mcu_version` | The co-processor's firmware version, parsed from its boot banner (e.g. `shelly_apt_003 mcu ver: v1.0.4`). |

### Local control (front button + status LEDs)

The example also wires the physical hardware through standard ESPHome platforms —
no custom options, but note the GPIOs and the two things to bench-verify:

- `binary_sensor` (**GPIO4**) — front tactile button; a tap runs `light.toggle`
  so turn-on gets the kick. Assumes active-low (`inverted: true`); flip if a tap
  reads backwards.
- `output` (**GPIO25**) — power LED, driven to follow on/off.
- `status_led` (**GPIO33**) — Wi-Fi/status LED.
- `uart` — **TX GPIO21 / RX GPIO22** to the co-processor. If comms fail on the
  first flash, swapping these is the first thing to try.

LED polarity and the button's active level are cosmetic and not firmware-pinned;
the example flags each with a `BENCH-VERIFY` comment.

### Developer / recovery tools (shipped **off** by default)

The example ships a set of diagnostic buttons **commented out**. Normal operation
never needs them — a healthy image self-commits and a bad one auto-reverts, with
no button press — but they're left in place, documented, for development and
recovery. Uncomment the `button:` block (and the `raw_tx_byte` number, if you want
raw-send) to enable them.

Safe, read-only:

| Button | Action |
|---|---|
| **Boot State: Log** | Decode both boot-state copies (active/revert slot, boot-attempts, committed) and log which the bootloader would boot. |
| **Dimmer Dump Boot State** | Hex-dump the raw boot-state region to the log. |
| **Dimmer Send Raw Byte** | Send one arbitrary byte (from the `raw_tx_byte` number) at the co-processor — for sweeping the unused command space. Needs `raw_tx_byte` uncommented too. |

Writes flash — **use with care**:

| Button | Action |
|---|---|
| **Boot State: Commit** | Make the currently-running slot permanent now (instead of waiting for auto-commit). |
| **Boot State: Revert To Stock** | Point the next boot at the other slot (hand control back to stock). |

Each write touches exactly one of the two boot-state copies, leaving the other
intact as a fallback, and the **partition-layout guard** (below) refuses the write
entirely on any flash map it doesn't recognize. Still: press **Boot State: Log**
and confirm the decode looks sane before using the two write buttons on a unit.

---

## Safety: the partition-layout guard

The boot-slot handling (commit / revert / OTA staging) writes Shelly's `SH0S` boot
record and addresses the two firmware slots at fixed flash offsets. Those offsets
— `otadata@0xd000`, `app_0@0x10000`, `app_1@0x200000` — are identical across the
stock 1.3.x and 2.0.0 partition tables (only the app-slot *sizes* and the
filesystem partitions move between versions, and the bootloader adjusts those
itself). At boot, the firmware verifies the **live** partition table still places
those three where expected. On any mismatch it **disables every boot-state write**
and logs an error; `dump_config` then reports `Boot-state writes ... DISABLED`. So
flashing onto some future stock layout we haven't validated fails safe rather than
corrupting the boot record.

---

## Layout

- `components/shelly_wall_dimmer/` — the ESPHome external component.
- `example/` — example device config + `secrets.yaml.example`.

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
