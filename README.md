# ShellyWallDimmerSwitch-esphome

Custom [ESPHome](https://esphome.io) firmware for the **Shelly Plus Wall Dimmer US** (`SNDM-0013US`). It replaces the stock ESP32 firmware and leaves the dimming co-processor (an APT32S003) alone — the ESP32 just speaks its single-byte UART protocol.

Flashes **over the air — no opening the device, no USB, no soldering.**

> ⚠️ **Pre-release, tracking `master`.** It's built to fail safe (two firmware slots, auto-revert, a partition-layout guard), but you're still reflashing a mains device in your wall. Your risk.
>
> **"Fail safe" is not "reversible to stock."** Auto-revert only swaps back to *the other slot* — whatever's in it. Once both slots run this firmware (any second flash), there's no stock image left on the device, and restoring one is **untested**. See [Getting back to stock](#getting-back-to-stock-untested) before you flash a second time.

---

## Why this exists

Stock is a thin wrapper over capable hardware. The ESP32 talks to a co-processor that takes fine-grained brightness commands and reports temperature and state — and stock surfaces almost none of it to Home Assistant. This replaces the ESP32 side (co-processor untouched) to expose more of it and add behavior stock can't do. The "kick" was just the first thing on the list.

**What you get:**

- **Kick** — cheap LEDs often won't start up at a low level from cold. This feature snaps to the minimum turn-on brightness, then fades to the commanded brightness.
- **Ramps** — Configurable ramp rates for default setpoint change behavior. This dictates the rate at which your lights transition to the next setpoint.
- **HA transitions** — `light.turn_on ... transition: Ns` ramps over exactly that duration, using our own engine (not HA's built-in one, which would fight it). Composes with kick: the strike is instant, the transition covers the ramp after it.
- **Range mapping** — stretch the 0–100 % slider across a `min`/`max` window (e.g. 0–100 % → 20–80 % real).
- **Limit correction** — optionally pull physical touch-dimming back inside that window (best effort - The co-processor gets the first say in this. We can only nudge it back into our desired zone).
- **Dimmable status LEDs** — Power LED as a locator (on when the light's off), WiFi LED as an error indicator. Both PWM, brightness configured in HA.
- **Telemetry** — co-processor die temperature, its firmware version, the raw status frame.
- **Local control** — the physical front button, routed through the kick/ramp engine.
- **Safe flashing** — solder-free first flash, then normal Wi-Fi OTA; dual-slot with auto-revert if a build misbehaves.

---

## Quick start

### Up Front
This is a heavily AI written project. I, a career firmware engineer was in the loop the whole time. I'm rather confident, but don't have a whole lot of reps using this yet. From a safety perspective, this firmware doesn't touch the high-voltage side, or directly control it. It commands the still stock firmware on a the co-processor to control the high-voltage side. This implies relatively high expectation of safety, but does not guarantee it.

**You need:**

- A **Shelly Plus Wall Dimmer US** (`SNDM-0013US`) on your LAN — stock firmware (1.3.x~2.0.0 tested in emulator), local access on, IP known.
- **ESPHome** — the Builder add-on in Home Assistant, or the CLI on the same LAN.
- A filled-in **`secrets.yaml`** in ESPHome (Wi-Fi, API key, OTA password, AP password) — see [`example/secrets.yaml.example`](example/secrets.yaml.example).

**Steps:**

1. Drop [`example/shelly-wall-dimmer.yaml`](example/shelly-wall-dimmer.yaml) and your `secrets.yaml` (if you don't already have one) into `/config/esphome/` (Builder) or a working dir (CLI). The `esp32:` block wants **`board: esp32dev`**.
2. Set a unique `fw_version` each build (a timestamp works). Stock dedupes on it — if it doesn't change, stock downloads the update and silently drops it.
3. Point the bridge at your dimmer:
   ```yaml
   shelly_wall_dimmer:
     id: shelly_esphome_dimmer
     bridge_package:
       push_to: 192.168.10.81   # your dimmer's IP
   ```
4. **Compile** (Builder: *Install → Manual download/compile* — no serial port). On success the component builds a stock-format package and, because `push_to` is set, serves it and triggers `Shelly.Update`. Watch for `>> shelly-bridge:` lines. *(If `push_to` disabled: The zip lands in the build's `shelly-bridge/` folder — deliver it yourself with `Shelly.Update {url: ...}`.)*
5. The dimmer writes it to its **inactive** slot and reboots into it — stock stays in the other slot. Watch `esphome logs` for your `fw_version` and a `layout guard OK` line.
6. After ~30 s healthy it **auto-commits** (see developer options below for commit rollback). A crash-looping build **auto-reverts to stock** on its own. Either way you're covered.
7. From here, update over **Wi-Fi OTA** like any ESPHome device — drop `bridge_package`. OTA stays boot-safe automatically.

**If this first build misbehaves,** it auto-reverts to stock on its own (that's slot B, still untouched). **Boot: Revert** (below) does the same thing manually. Neither of these is "back to stock" once you flash a *second* time — see below.

---

## Getting back to stock (untested)

Be clear-eyed about what "fail safe" covers here: **updates** are safe (bad build → auto-revert to the other slot), but **restoring stock** is a different claim we haven't proven.

The bootloader only ever swaps between the two app slots. Right after your *first* flash, slot B still holds stock — so a crash-loop or a manual revert genuinely lands you back on stock. But flash a *second* time (any update after that) and slot B gets overwritten too. At that point there is no stock image left on the device, and Boot: Revert just swaps between two copies of our firmware.

Getting stock back after that would need one of:
- **Re-deliver a stock package over ESPHome's OTA**, staged through our SH0S wrapper — plausible, but we haven't tried it, and it's unclear whether the wrapper or a stock image cooperates with that path.
- **Serial recovery via GPIO0** — reliable, but means opening the unit and soldering, which defeats the point of the solder-free flash.

If you want a real fallback, keep a copy of your dimmer's original OTA package before you flash the second time, and don't treat "auto-revert" as "uninstall."

---

## Configuration

Everything is a native HA entity. Start from the example and trim what you don't want — the Shelly-specific plumbing (single-core, partition table, OTA safety) is injected by the component, so your YAML stays plain ESPHome.

### Hub — `shelly_wall_dimmer:`

| Option | Default | What |
|---|---|---|
| `id` | — | Referenced by the light/number/etc. platforms. |
| `uart_id` | single UART | The `uart:` to the co-processor. **115200 8N1**, both TX+RX (checked at compile). |
| `update_interval` | `1s` | Co-processor poll rate (keeps temperature/state fresh). |
| `bridge_package` | off | Build the first-flash package on compile (below). |

`bridge_package:` assembles the stock-format OTA zip from build artifacts only — app + the partition table built from this component + an empty filesystem image. **No Shelly binaries are redistributed.** Sub-option `push_to: <ip>` serves it and calls `Shelly.Update` on that device after a good build (the host address is derived from the route to it). Omit `push_to` to just produce the zip.

### Light — `platform: shelly_wall_dimmer`

Brightness-only; needs `shelly_wall_dimmer_id`. The example sets `gamma_correct: 0` (the co-processor has its own dimming curve — ESPHome's gamma would double-correct and crush low levels) and `default_transition_length: 0s` (our engine owns ramping; ESPHome transitions fight it).

### Numbers — `platform: shelly_wall_dimmer`, pick with `type:`

All `config`-category.

| `type` | Range | Default | What |
|---|---|---|---|
| `kick_level` | 0–100 % | 20 | Strike level **and** pivot — every turn-on snaps here first. Set to the lowest % your bulb reliably lights at. |
| `kick_dwell_ms` | 0–2000 ms | 150 | Hold at `kick_level` before ramping down — only when the target is *below* it. |
| `min_brightness` | 0–100 % | 1 | Low end of the mapped window (below). |
| `max_brightness` | 0–100 % | 100 | High end of the mapped window. |
| `ramp_rate` | 1–1000 %/s | 150 | One shared speed for every ramp. |

**Range mapping.** `min`/`max` don't clamp — they **stretch**. HA 0 % → `min`, 100 % → `max`, linear between; device reports map back so HA still reads 0–100 %. So `min=20, max=80` gives 0→20, 50→50, 100→80. Defaults (1/100) are a near-no-op. `kick_level` is in real device terms.

> ⚠️ **Physical touch dimming isn't mapped.** The stretch only applies to *commands* (HA, automations, the front button). The touch surface talks straight to the co-processor and just reports back, on the raw 0–100 scale — so `min`/`max` aren't enforced for a physical swipe (it can go under `min`). `limit_correct` (below) pulls it back, but only *after* the fact.

**Ramps.** `ramp_rate` is percent/second; the firmware picks a step cadence for you, quantized to a 10 ms floor (~1 mains half-cycle — the TRIAC only fires once per 8.33 ms, so finer-grained control is not possible.). No dithering: fixed step + interval, and a partial last step lands exactly on the setpoint. Can't be zero. You can select which operations have a ramp applied to them below; `kick_dwell` and the kick's own descent also use `ramp_rate`.

### Switches — `platform: shelly_wall_dimmer`, pick with `type:`

| `type` | Default | What |
|---|---|---|
| `kick_enabled` | on | Snap to `kick_level` on turn-on. |
| `ramp_on_change` | on | Ramp (not jump) on a brightness change while already on. |
| `ramp_on_off` | off | Fade in on turn-on, fade out on turn-off. |
| `limit_correct` | off | If the touch panel drives outside `[min,max]`, ramp back to the limit (best-effort — reacts after the fact). |

**Kick + ramp together:** with `kick_enabled`, every turn-on from off snaps to `kick_level` (below it the bulb's dark anyway), then reaches the target — dwell + ramp **down** if the target is below `kick_level`, ramp **up** with no dwell if above. With kick off, a turn-on jumps straight to target, or fades in from 0 if `ramp_on_off`.

### Sensors

- `sensor` — I think this is the temp sensor mounted to the triac, but I'm not 100% sure. This is controlled by the co-processor as well, so I just take it at face value.
- `text_sensor` `type: last_frame` — the latest raw status frame as hex (diagnostic).
- `text_sensor` `type: mcu_version` — co-processor firmware version, from its boot banner (e.g. `shelly_apt_003 mcu ver: v1.0.4`).

### Local hardware (front button + LEDs)

Standard ESPHome platforms in the example — no custom options. GPIOs:

- **Button** — GPIO4. A tap runs `light.toggle` (so it gets the kick). Assumes active-low; flip `inverted` if a tap reads backwards.
- **Power LED** — GPIO25, ~1000 Hz PWM. A **locator**: on when the light's off, off when it's on, at its own brightness (it does *not* track the dimmer).
- **WiFi LED** — GPIO33, ~1000 Hz PWM via `status_led_pwm`. An **error indicator**: off when healthy, blinks on AP/connecting/warning/error at its set brightness.
- **UART** — TX GPIO21 / RX GPIO22. If comms fail on the first flash, swap these first.

> LED polarity and the button's active level aren't firmware-pinned — the example flags each with a `BENCH-VERIFY` comment.

### Developer / recovery buttons (shipped off)

The example keeps a set of diagnostic buttons **commented out** — normal operation never needs them (a healthy image self-commits, a bad one auto-reverts). Uncomment the `button:` block (and `raw_tx_byte`, for raw-send) to use them.

- Read-only: **Boot: Log** (decode both boot-state copies), **Dump Boot State** (hex dump), **Send Raw Byte** (push one arbitrary byte at the co-processor).
- Writes flash: **Boot: Commit** (make the running slot permanent now), **Boot: Revert** (hand the next boot back to stock).

Each write touches only one of the two boot-state copies, and the partition-layout guard refuses it on any flash map it doesn't recognize — but press **Boot: Log** and sanity-check first.

**Partition-layout guard.** Boot-slot writes assume fixed offsets (`otadata@0xd000`, `app_0@0x10000`, `app_1@0x200000` — identical across stock 1.3.x and 2.0.0). At boot the firmware checks the live table; on any mismatch it disables all boot-state writes and logs it (`dump_config` shows `Boot-state writes ... DISABLED`). So it fails safe on a layout it doesn't know.

---

## Testing

The kick/ramp/range-mapping logic is a standalone, pure-C++ engine — unit-tested without ESPHome or hardware:

```sh
cd tests && make
```

Needs only a C++17 compiler. ~400 assertions drive the real engine and check the exact bytes it emits: range mapping (plus a round-trip stability sweep), the single-pivot kick, ramp cadence / partial-step / no-overshoot, off-preserves-brightness-bits, fade-out, and the no-op reflection guard. See [`tests/README.md`](tests/README.md).

What that can't reach (ESPHome glue, SH0S boot-state + DFU, the bridge packager, live UART) is covered by `esphome compile` and on-device bring-up.

---

## Repo layout

- `components/shelly_wall_dimmer/` — the main ESPHome component.
- `components/status_led_pwm/` — small light platform: a dimmable PWM status LED that blinks on ESPHome's warning/error state (the WiFi LED).
- `example/` — device config + `secrets.yaml.example`.
- `tests/` — standalone engine unit tests.

## License

GPL-3.0 — see [LICENSE](LICENSE).
