# ShellyWallDimmerSwitch-esphome

Custom [ESPHome](https://esphome.io) firmware for the **Shelly Plus Wall Dimmer US** (`SNDM-0013US`), so Home Assistant can control it properly.

Flashes **over the air — no opening the device, no USB, no soldering.**

Only the ESP32 (the Wi-Fi part) is replaced. **The co-processor that actually switches mains keeps running its untouched factory firmware.**

> ⚠️ You are reflashing a mains-powered device in your wall, using a path the manufacturer never intended. It is built to fail safe, but **you accept the risk** — please read [Use at your own risk](#use-at-your-own-risk) and [What was done to de-risk this](#what-was-done-to-de-risk-this).
>
> **Install a [tagged release](#releases-and-stability).** Untagged code, `master` included, is unstable and may never have been run on hardware.
>
> One thing to know up front: **"fail safe" is not "reversible to stock."** See [Getting back to stock](#getting-back-to-stock-untested) before your *second* flash.

**What you get over stock:**

- **Kick** — cheap LEDs often won't strike at a low level from cold. Turn-on snaps to a brightness that reliably lights, then fades to what you asked for.
- **Ramps** — configurable ramp rate for every brightness change, plus real support for `transition:` from Home Assistant.
- **Range mapping** — stretch the 0–100 % slider across a usable window (e.g. 20–80 % real).
- **Dimmable status LEDs** — power LED as a locator, Wi-Fi LED as an error indicator, both with brightness sliders.
- **Telemetry** — co-processor die temperature, its firmware version, raw status frames.
- **Local control** — the physical button, routed through the same kick/ramp engine.

---

## Contents

1. [Releases and stability](#releases-and-stability)
2. [What you need](#what-you-need)
3. [How flashing works](#how-flashing-works)
4. [First flash: getting this onto a stock dimmer](#first-flash-getting-this-onto-a-stock-dimmer)
5. [Updating after the first flash](#updating-after-the-first-flash)
6. [Minimal configuration](#minimal-configuration)
7. [Full configuration](#full-configuration)
8. [Option reference](#option-reference)
   - [Hub options](#hub-options)
   - [Light options](#light-options)
   - [Number options](#number-options)
   - [Switch options](#switch-options)
   - [Sensor options](#sensor-options)
   - [Local hardware: button and LEDs](#local-hardware-button-and-leds)
9. [How the kick and ramps behave](#how-the-kick-and-ramps-behave)
10. [Converting more switches](#converting-more-switches)
11. [What persists](#what-persists)
12. [Getting back to stock (untested)](#getting-back-to-stock-untested)
13. [Developer and recovery buttons](#developer-and-recovery-buttons)
14. [Troubleshooting](#troubleshooting)
15. [Use at your own risk](#use-at-your-own-risk)
16. [What was done to de-risk this](#what-was-done-to-de-risk-this)
17. [Testing](#testing)
18. [Repo layout](#repo-layout)
19. [License](#license)

---

## Releases and stability

**A tagged release has run on real hardware.** Every tag means that exact build was
flashed to an actual dimmer and used in a real installation — not that its tests went
green. The test suite is necessary and it is not sufficient; nothing gets tagged on
passing CI alone. Tags are immutable, so what you pin is what you get.

**Everything else is unstable.** That explicitly includes the tip of `master`: it may be
mid-refactor, it may be partially tested, and **it may never have been run on real
hardware.** Commits land here continuously and there is no commitment that any given
push has been flashed to a physical dimmer.

So pin `external_components` to a tag:

```yaml
    source:
      type: git
      url: https://github.com/InternetofAwesome/ShellyWallDimmerSwitch-esphome
      ref: v0.0.0          # a release tag, not `master`
    refresh: 1d
```

A pre-release suffix (`-alpha`, `-beta`) does **not** mean untested — it still ran on
hardware, or it would not have been tagged. It means there is not much real-world time
behind it yet: few units, few days, and corners that have not come up in daily use.

Field time on the author's own switches accrues to the specific builds that were
installed on them. It does not transfer forward to a release automatically, and this
README will not claim otherwise.

---

## What you need

- A **Shelly Plus Wall Dimmer US** (`SNDM-0013US`), installed and working on stock firmware, on your LAN.
- **Its IP address.** You point the installer at it directly.
- Local access enabled on the device (it is, by default).
- **ESPHome** — either the Builder add-on in Home Assistant (assumed below) or the CLI.

That's all. No cable, no USB-serial adapter, no disassembly, no soldering.

---

## How flashing works

Worth thirty seconds, because the first flash and every later one use **completely different paths** — and mixing them up is the one way to lose your safety net.

The dimmer has **two firmware slots**. Only one runs at a time; the other is the fallback. The bootloader will automatically switch back to the fallback if the running firmware fails to boot a few times in a row. That is what makes this reversible.

**Stage 1 — the first flash ("bridge").** Your dimmer is running stock and has never heard of ESPHome, so it will not accept an ESPHome update. It *will* accept a Shelly-format update over Shelly's own protocol. So the component builds a Shelly-format package and hands it to the device using stock's own updater. It lands in the **empty slot**; stock stays in the other one.

**Stage 2 — every update after that.** Now the device speaks ESPHome, so you use normal ESPHome wireless updates like any other device.

```
       stock firmware                  this firmware
  ┌──────────┬──────────┐        ┌──────────┬──────────┐
  │  slot A  │  slot B  │        │  slot A  │  slot B  │
  │  STOCK   │  empty   │  ───►  │  STOCK   │   OURS   │
  │ (running)│          │ bridge │(fallback)│ (running)│
  └──────────┴──────────┘        └──────────┴──────────┘
                                        │
                                        │  next ESPHome update targets slot A —
                                        │  the one still holding stock. The
                                        ▼  firmware REFUSES until you allow it.
```

**The important consequence:** after the first flash, your stock image is sitting in the other slot as your way back. The next wireless update would overwrite exactly that slot. The firmware **refuses** that update until you explicitly permit it once — see [Getting back to stock](#getting-back-to-stock-untested).

---

## First flash: getting this onto a stock dimmer

1. **ESPHome Builder → `+ NEW DEVICE`.** Give it a name (e.g. `office-dimmer`); pick **ESP32** if asked. The wizard generates an API encryption key and OTA password and puts your Wi-Fi credentials in `secrets.yaml`. Let it finish, then **skip** the install it offers.

2. **Edit the new device** and replace its contents with the [minimal config](#minimal-configuration) below — but **keep the `api:` and `ota:` blocks the wizard generated**, since those hold this device's keys. Add `ap_password` to your `secrets.yaml`.

3. **Point it at your dimmer** by uncommenting the bridge block and setting your device's IP:

   ```yaml
   shelly_wall_dimmer:
     id: dimmer
     bridge_package:
       push_to: 192.168.1.50      # YOUR dimmer's IP
   ```

   Also make sure `toolchain: platformio` is present in the `esp32:` block (it's in the example). The bridge needs it; without it the component stops you at config time rather than silently doing nothing.

4. **Give the build a unique version.** Stock ignores an update whose version matches what it's already running, so bump `fw_version` for each bridge push:

   ```yaml
   substitutions:
     fw_version: "2026.08.16-1"
   ```

5. **Install → `Manual download`.** ***Not*** **Wireless.** Manual download just *compiles*, which is all the bridge needs — it then assembles the package, serves it, and calls the device itself. Watch the log for:

   ```
   >> shelly-bridge: wrote .../PlusWallDimmer-bridge.zip
   >> shelly-bridge: triggering Shelly.Update on 192.168.1.50 ...
   >> shelly-bridge: device fetched the package
   ```

   Ignore the `.bin` your browser offers to download.

   > **Why not Wireless?** With `bridge_package` configured, "Wireless" would deliver firmware *twice by two different routes* in one command — which can write **both** slots and destroy your stock fallback. The component refuses to build at all for an install/upload job. Use Manual download.

6. **The dimmer flashes its empty slot and reboots into this firmware.** It appears **online** in the Builder. Open its **Logs** and confirm your `fw_version` in the boot banner and a `layout guard OK` line.

7. **Home Assistant discovers it.** Settings → Devices & Services → ESPHome should offer the new device. Add it (paste the API key if prompted) and the entities appear.

8. **Comment out `bridge_package:`** — it is a first-flash-only tool. Leave `toolchain: platformio` alone. **Don't reinstall just to apply that edit**; your next install is your *second* flash, which is the one that overwrites stock.

**If this first build misbehaves,** it auto-reverts to stock on its own after a few failed boots. That safety net exists only until your second flash.

---

## Updating after the first flash

Normal ESPHome: **Install → Wireless**.

The first time you do this, it will **fail on purpose**, with this in the log:

```
OTA REFUSED: target slot at 0x200000 still holds the stock Shelly firmware
(project "PlusWallDimmer", version "2.0.0") -- your only rollback on a device
with no USB. Turn on the "Allow Overwrite Stock" switch in Home Assistant to
proceed. This is irreversible...
```

That is the guard doing its job: the update would erase your only copy of stock. To proceed, flip **Allow Overwrite Stock** on the device page in Home Assistant (under *Configuration*), then install again. Set once, then forget — after that slot is overwritten there is nothing left to protect.

The check happens *before* anything is erased, so a refused update leaves stock completely untouched.

---

## Minimal configuration

The smallest config that works. Everything Shelly-specific — single-core build, the exact partition table, boot-record safety — is injected by the component, so this stays plain ESPHome.

```yaml
substitutions:
  name: office-dimmer
  fw_version: "2026.08.16-1"    # bump for each bridge push

esphome:
  name: ${name}
  project:
    name: "shelly.wall_dimmer"
    version: "${fw_version}"

esp32:
  board: esp32dev
  toolchain: platformio          # required by bridge_package
  framework:
    type: esp-idf

external_components:
  - source:
      type: git
      url: https://github.com/InternetofAwesome/ShellyWallDimmerSwitch-esphome
      ref: v1.0.0-alpha   # not published yet -- see Releases and stability
    components: [shelly_wall_dimmer, status_led_pwm]

# UART to the dimming co-processor. These pins are not negotiable.
uart:
  tx_pin: GPIO21
  rx_pin: GPIO22
  baud_rate: 115200

shelly_wall_dimmer:
  id: dimmer
  # Uncomment for the FIRST flash only, then comment out again:
  # bridge_package:
  #   push_to: 192.168.1.50

light:
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    name: "Light"
    gamma_correct: 0             # required — see below
    default_transition_length: 0s # required — see below

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "Dimmer Fallback"
    password: !secret ap_password

captive_portal:

api:
  encryption:
    key: !secret encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

logger:
```

**The two `light:` settings are required, not stylistic:**

- `gamma_correct: 0` — the co-processor applies its own dimming curve. ESPHome's default gamma (2.8) double-corrects and crushes low levels to nothing (5 % → ~0).
- `default_transition_length: 0s` — this component owns ramping. ESPHome's own transitions stream competing brightness writes and fight it. (An explicit `transition:` in a service call still works — it's handed to our engine.)

With just this you get the light, the kick, ramps, and defaults for everything else. Add pieces from the full config as you want them.

---

## Full configuration

Every option this component offers. The [`example/`](example/shelly-wall-dimmer.yaml) file is the same thing with much longer explanatory comments.

```yaml
substitutions:
  name: office-dimmer
  friendly_name: "Office Dimmer"
  fw_version: "2026.08.16-1"

esphome:
  name: ${name}
  friendly_name: ${friendly_name}
  project:
    name: "shelly.wall_dimmer"
    version: "${fw_version}"

esp32:
  board: esp32dev
  toolchain: platformio
  framework:
    type: esp-idf

external_components:
  - source:
      type: git
      url: https://github.com/InternetofAwesome/ShellyWallDimmerSwitch-esphome
      ref: v1.0.0-alpha   # not published yet -- see Releases and stability
    refresh: 1d
    components: [shelly_wall_dimmer, status_led_pwm]

uart:
  id: dimmer_uart
  tx_pin: GPIO21
  rx_pin: GPIO22
  baud_rate: 115200

shelly_wall_dimmer:
  id: dimmer
  uart_id: dimmer_uart
  update_interval: 1s
  # silent: false            # bench mode: ESP goes electrically mute on the bus
  # bridge_package:
  #   push_to: 192.168.1.50

light:
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    id: dimmer_light
    name: "Light"
    gamma_correct: 0
    default_transition_length: 0s
    on_turn_on:
      - light.turn_off: power_led
    on_turn_off:
      - light.turn_on: power_led

  - platform: monochromatic          # power LED — locator
    id: power_led
    name: "Power LED"
    output: power_led_pwm
    entity_category: config
    gamma_correct: 0
    default_transition_length: 0s
    restore_mode: RESTORE_DEFAULT_ON

  - platform: status_led_pwm         # Wi-Fi LED — error indicator
    id: wifi_led
    name: "WiFi LED"
    output: wifi_led_pwm
    entity_category: config
    gamma_correct: 0
    default_transition_length: 0s
    restore_mode: RESTORE_DEFAULT_ON

number:
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: kick_level
    name: "Kick Level"
    initial_value: 20
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: kick_dwell_ms
    name: "Kick Dwell"
    initial_value: 150
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: min_brightness
    name: "Min Brightness"
    initial_value: 1
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: max_brightness
    name: "Max Brightness"
    initial_value: 100
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: ramp_rate
    name: "Ramp Rate"
    initial_value: 150

switch:
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: kick_enabled
    name: "Kick Enabled"
    restore_mode: RESTORE_DEFAULT_ON
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: ramp_on_change
    name: "Ramp On Change"
    restore_mode: RESTORE_DEFAULT_ON
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: ramp_on_off
    name: "Fade In-Out"
    restore_mode: RESTORE_DEFAULT_OFF
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: limit_correct
    name: "Limit Correct"
    restore_mode: RESTORE_DEFAULT_OFF
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: allow_overwrite_stock
    name: "Allow Overwrite Stock"
    restore_mode: RESTORE_DEFAULT_OFF

sensor:
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    name: "Temperature"

text_sensor:
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: last_frame
    name: "Last Frame"
  - platform: shelly_wall_dimmer
    shelly_wall_dimmer_id: dimmer
    type: mcu_version
    name: "MCU Version"

binary_sensor:
  - platform: gpio                   # front tactile button
    name: "Button"
    pin:
      number: GPIO4
      mode:
        input: true
      inverted: true
    filters:
      - delayed_on_off: 30ms
    on_press:
      - light.toggle: dimmer_light

output:
  - platform: ledc
    id: power_led_pwm
    pin:
      number: GPIO25
      inverted: true                 # LEDs are active-low
    frequency: 1000Hz
  - platform: ledc
    id: wifi_led_pwm
    pin:
      number: GPIO33
      inverted: true
    frequency: 1000Hz

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:
    ssid: "${friendly_name} Fallback"
    password: !secret ap_password

captive_portal:

api:
  encryption:
    key: !secret encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

logger:
  level: DEBUG
```

---

## Option reference

### Hub options

`shelly_wall_dimmer:`

| Option | Default | What it does |
|---|---|---|
| `id` | — | Name referenced by every other platform below. |
| `uart_id` | the single UART | Which `uart:` talks to the co-processor. Must be **115200 8N1** with both TX and RX — checked at compile time. |
| `update_interval` | `1s` | How often to poll the co-processor, keeping temperature and state fresh. |
| `bridge_package` | off | Build the first-flash package during compile. Sub-option `push_to: <ip>` also serves it and triggers the update on that device. Omit `push_to` to just produce the zip in the build's `shelly-bridge/` folder. Requires `toolchain: platformio`. |
| `silent` | `false` | Bench mode: the ESP32 sends nothing at all on the co-processor bus, so an external USB-serial adapter can drive it without contention. Pair with an RX-only `uart:`. You do not want this in normal use. |

The bridge package contains **only the application image** — no partition table, no filesystem, and **no Shelly binaries are redistributed**. That is a safety requirement, not a simplification: stock partition layouts differ between firmware versions, so shipping either part would corrupt the fallback slot on a device whose stock version doesn't match.

### Light options

`platform: shelly_wall_dimmer` — brightness-only. Requires `shelly_wall_dimmer_id`.

| Option | Required value | Why |
|---|---|---|
| `gamma_correct` | `0` | The co-processor has its own dimming curve; ESPHome's default gamma double-corrects and crushes low brightness to zero. |
| `default_transition_length` | `0s` | This component owns ramping. ESPHome's transitions would stream competing writes. An explicit `transition:` in a service call is still honoured and handed to our engine. |

### Number options

All are `platform: shelly_wall_dimmer` with a `type:`, and all are `config`-category entities. **Every value persists** — see [What persists](#what-persists). The "Default" column is the *first-boot* value; override per entity with `initial_value:`, which is range-checked at build time.

| `type` | Range | Default | What it does |
|---|---|---|---|
| `kick_level` | 0–100 % | 20 | Strike level **and** pivot: every turn-on snaps here first. Set it to the lowest percentage your bulb reliably lights at. |
| `kick_dwell_ms` | 0–2000 ms | 150 | How long to hold at `kick_level` before ramping down — only when the target is *below* it. |
| `min_brightness` | 0–100 % | 1 | Low end of the mapped window. |
| `max_brightness` | 0–100 % | 100 | High end of the mapped window. |
| `ramp_rate` | 1–1000 %/s | 150 | One shared speed for every ramp. Cannot be zero. |

**Range mapping.** `min`/`max` don't clamp, they **stretch**. Home Assistant 0 % maps to `min`, 100 % maps to `max`, linear in between, and device reports map back so HA still reads 0–100 %. So `min=20, max=80` gives 0→20, 50→50, 100→80. The defaults (1/100) are effectively a no-op. `kick_level` is always in real device terms, not mapped.

> ⚠️ **Physical touch dimming is not mapped.** The stretch applies to *commands* — Home Assistant, automations, the front button. The touch strip talks straight to the co-processor and only reports back afterwards, on the raw 0–100 scale, so it can go below `min`. `limit_correct` pulls it back, but only after the fact.

### Switch options

All are `platform: shelly_wall_dimmer` with a `type:`. They persist the same way numbers do, but take their first-boot value from the standard `restore_mode:` key. Use `ALWAYS_ON`/`ALWAYS_OFF` to pin one at every boot and opt out of persistence.

| `type` | Default | What it does |
|---|---|---|
| `kick_enabled` | on | Snap to `kick_level` on every turn-on. |
| `ramp_on_change` | on | Ramp rather than jump when brightness changes while already on. |
| `ramp_on_off` | off | Fade in on turn-on and fade out on turn-off. |
| `limit_correct` | off | If the touch strip drives outside `[min,max]`, ramp back to the nearest limit. Best effort — it reacts after the fact. |
| `allow_overwrite_stock` | off | Permit an update to erase the slot still holding stock firmware. Until this is on, wireless updates that would destroy your rollback are refused. See [Getting back to stock](#getting-back-to-stock-untested). |

### Sensor options

| Platform | `type` | What it reports |
|---|---|---|
| `sensor` | — | Co-processor die temperature in °C. Believed to be the sensor on the TRIAC, though that isn't confirmed — the co-processor reports it and it's taken at face value. |
| `text_sensor` | `last_frame` | The latest raw status frame as hex. Diagnostic. |
| `text_sensor` | `mcu_version` | Co-processor firmware version from its boot banner, e.g. `shelly_apt_003 mcu ver: v1.0.4`. |

### Local hardware: button and LEDs

Standard ESPHome platforms — no custom options. The GPIOs are fixed by the hardware:

| Function | GPIO | Notes |
|---|---|---|
| Front button | `GPIO4` | A tap runs `light.toggle`, so it gets the kick. Assumed active-low; flip `inverted` if a tap reads backwards. |
| Power LED | `GPIO25` | ~1000 Hz PWM. A **locator**: on when the light is off. Does not track dimmer brightness. |
| Wi-Fi LED | `GPIO33` | ~1000 Hz PWM via `status_led_pwm`. An **error indicator**: off when healthy, blinks on AP/connecting/warning/error. |
| Co-processor UART | `GPIO21` TX / `GPIO22` RX | If communication fails on the first flash, swapping these is the first thing to try. |

Both LEDs are **active-low** (bench-confirmed), which is why the `output:` blocks set `inverted: true` at the pin — that keeps the brightness sliders meaning what they say.

---

## How the kick and ramps behave

**Kick.** With `kick_enabled`, every turn-on from off snaps straight to `kick_level` — below that the bulb is dark anyway, so there is nothing to fade through. Then it reaches your target:

- Target **below** `kick_level`: hold for `kick_dwell_ms`, then ramp **down**. This is the kick.
- Target **above** `kick_level`: ramp **up** immediately, no dwell — it's already lit.
- Target **equal**: stop there.

With kick off, a turn-on jumps straight to target, or fades up from 0 if `ramp_on_off` is on.

**Ramps.** `ramp_rate` is in percent per second. The firmware converts it to a fixed step cadence, quantized to a 10 ms floor — about one mains half-cycle, since the TRIAC can only act once per half-cycle (8.33 ms at 60 Hz) and finer steps buy nothing. There is no dithering: one step size, one interval, and a partial final step lands exactly on the setpoint.

**Transitions.** `light.turn_on ... transition: Ns` ramps over exactly that duration using our engine rather than ESPHome's, so the two don't fight. It composes with the kick: the strike stays instant, and the requested duration covers the ramp after it.

---

## Converting more switches

**Don't copy a working device's YAML.** It's the obvious move and it quietly couples the two devices. Repeat step 1 instead — **Builder → `+ NEW DEVICE`** — so the wizard mints this device its own API key and OTA password, paste the config over it as before, then carry across **only your tuned values** (kick level and dwell, ramp rate, min/max, LED brightness).

Never carry across:

| Don't copy | Why |
|---|---|
| `ota: password:` | Distinct passwords are an **interlock**: aim an install at the wrong device and it *fails*. Share them and a mis-aimed wireless install silently succeeds, flashing one switch with another's firmware — it then comes up claiming the wrong name and Home Assistant gets very confused. |
| `api: encryption: key:` | Two devices answering to one key invites identity mix-ups during discovery. |
| `name:` / `friendly_name:` | Duplicate names collide on mDNS, and the Builder can end up talking to whichever answers first. |
| `bridge_package: push_to:` | Must be the **new** switch's IP. Left pointing at an already-converted one it just fails (that device no longer speaks Shelly's update protocol), so nothing gets flashed and it isn't obvious why. |

Each switch keeps its **own** stock image in its own spare slot, with its own **Allow Overwrite Stock** switch, default off — converting one has no effect on another's rollback.

---

## What persists

Everything persisted lives in the **`nvs` partition at `0x9000`, 16 KB** — Shelly's, not ours. This firmware ships app-only, so neither the first flash nor any later update touches it. Writes are batched and flushed every 60 s and on a clean reboot or update (`preferences: flash_write_interval:` to change that).

Persisted by this component — the key is a hash of the entity's **object ID**, so renaming an entity orphans its stored value and it reverts to the first-boot default:

| Entity | Stored | First-boot value from |
|---|---|---|
| `number` × 5 (`kick_level`, `kick_dwell_ms`, `min_brightness`, `max_brightness`, `ramp_rate`) | last value set | `initial_value:` |
| `switch` × 5 (the four engine gates plus `allow_overwrite_stock`) | last state | `restore_mode:` |

Also in that same 16 KB, from stock ESPHome components in the example config:

| Source | Stored | When |
|---|---|---|
| `light` Power LED, `light` WiFi LED | on/off + brightness | `restore_mode: RESTORE_DEFAULT_ON` |
| `safe_mode` | boot-attempt counter | every boot, cleared 60 s later |
| `wifi` | SSID/PSK | only if set via captive portal or Improv — YAML credentials are compiled in, not stored |
| `api` | noise encryption key | only if changed at runtime rather than in YAML |

**Not** persisted: the light entity itself (the co-processor reports its own state at boot and the entity syncs to that), the temperature sensor, both text sensors, and the front button.

Three things wipe stored values: a **full-erase serial flash**, **reverting to stock via a full factory package** (Shelly's manifest erases `nvs`), and **a failed `nvs_open`** — if the partition is exhausted, ESPHome erases the whole partition to recover. That last one is why the boot log prints `NVS: <used>/<total> entries used`: on this device that space is shared with stock's leftovers. If free entries are in the single digits, clear it deliberately rather than waiting for the automatic wipe. Nothing that boots the device lives there.

---

## Getting back to stock (untested)

Be clear-eyed about what "fail safe" covers: **updates** are safe (a bad build auto-reverts to the other slot), but **restoring stock** is a different claim, and it has not been proven.

The bootloader only ever swaps between the two app slots. Right after your *first* flash, the other slot still holds stock, so a crash-loop or a manual revert genuinely lands you back on stock. But flash a *second* time and that slot gets overwritten too. At that point there is no stock image left on the device, and a revert just swaps between two copies of this firmware.

Getting stock back after that would need one of:

- **Re-delivering a stock package over ESPHome's OTA**, staged through our boot-record wrapper — plausible, but untried, and it's unclear whether the wrapper or a stock image cooperates with that path.
- **Serial recovery via GPIO0** — reliable, but means opening the unit and soldering, which defeats the point.

If you want a real fallback, **keep a copy of your dimmer's original firmware package before you flash the second time**, and don't treat "auto-revert" as "uninstall."

This is why the firmware refuses that second update until you flip **Allow Overwrite Stock**. It costs one toggle, once, in exchange for not discovering later that your rollback quietly disappeared.

---

## Developer and recovery buttons

The example keeps a set of diagnostic buttons **commented out** — normal operation never needs them, since a healthy image commits itself and a bad one auto-reverts. Worth enabling for a first conversion of a hard-to-reach unit.

Read-only, safe to press any time:

- **Boot: Log** — decode both boot-record copies.
- **Dump Boot State** — hex-dump the raw boot record.
- **Send Raw Byte** — push one arbitrary byte at the co-processor (needs a matching `raw_tx_byte` number entity).

Writes flash, one boot-record copy at a time:

- **Boot: Commit** — make the running slot permanent now.
- **Boot: Revert** — point the next boot at the *other* slot, which is stock only until your second flash overwrites it.

A partition-layout guard refuses all of these on any flash map it doesn't recognize, and `dump_config` reports whether writes are enabled. Still, press **Boot: Log** and sanity-check the decode first.

---

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Bridge push runs but the device never updates | The version didn't change. Stock skips an update matching its running version — bump `fw_version`. |
| `bridge_package` seems to do nothing, no errors | Missing `toolchain: platformio`. The component now catches this at config time. |
| Build refuses with "must not run as part of an upload job" | You used Install → Wireless with `bridge_package` configured. Use **Manual download**. |
| Wireless update refused with `OTA REFUSED` | Working as designed — see [Updating after the first flash](#updating-after-the-first-flash). |
| Device boots but no state, no temperature | UART wiring — try swapping `tx_pin`/`rx_pin`. |
| Light responds backwards to the button | Flip `inverted:` on the `GPIO4` binary sensor. |
| LEDs full-on when they should be off | LED polarity — the `inverted: true` on the `ledc` outputs. |
| Low brightness does nothing / jumps oddly | `gamma_correct: 0` missing, or `kick_level` set below what your bulb can strike. |
| Ramps stutter or fight themselves | `default_transition_length: 0s` missing. |
| HA logs `Invalid encryption key` right after conversion | Expected for a reboot or two while the device comes up on new firmware. Clears itself; if it persists, the key really is mismatched. |

---

## Use at your own risk

**The firmware that actually switches mains is not modified by this project.** Zero-cross detection and TRIAC gate drive live on a separate co-processor running its factory image; this replaces only the ESP32 that handles Wi-Fi and Home Assistant, and commands that co-processor exactly the way stock did. No high-voltage timing, switching, or protection logic is altered.

That materially limits the blast radius — it does not eliminate it. This is a community project with no affiliation to Shelly or Allterco, replacing firmware on a **mains-powered device installed in your wall**, through an update path the manufacturer never intended or documented.

**On how this was built:** it was written with heavy AI assistance, reviewed throughout by the author — a firmware engineer with ~20 years of professional industry experience. That is not a claim of perfection, and it is not a substitute for testing. It means someone who has shipped firmware for a living had their eyes on every part of it. The [de-risking section](#what-was-done-to-de-risk-this) below is written to be read as an engineer would want it: it states what was verified and how, names the gaps deliberately left open, and gives you the commands to reproduce the parts that are reproducible. Where something is untested, it says so.

It is built to fail safe: two app slots, automatic rollback, CRC-verified boot records, a partition-layout guard, and a refusal to overwrite your stock image without explicit consent. None of that is a guarantee. You may end up with a device that needs opening and a soldered serial connection to recover, or one that is simply dead. **You accept that risk entirely.** No warranty, express or implied; the author accepts no liability for damage to your hardware, wiring, or property. If you are not comfortable with that, do not flash it.

---

## What was done to de-risk this

**Architecture.** Three processors, two of which are untouched:

```
ESP32  ──UART 115200 8N1──►  co-processor 1  ────────►  co-processor 2
(Wi-Fi, Home Assistant)      APT32S003:                 capacitive slider
      REPLACED               zero-cross + TRIAC         + 7-LED bar
                             STOCK                      STOCK
```

This project replaces **only the ESP32 firmware**. Co-processor 1 (`shelly_apt_003`, self-reported `v1.0.4`) owns mains zero-cross detection and TRIAC gate drive, and runs the factory image. Co-processor 2 drives the capacitive slider and its LED bar; it is not on the ESP32's bus at all — the ESP32 reaches it only indirectly, via co-processor 1. Touch events therefore arrive as *already-applied* state changes relayed by co-processor 1, and the LED bar is **not** controllable from this firmware. Co-processor 2 has not been identified or enumerated; nothing here modifies it.

The ESP32 sends co-processor 1 the same single-byte on-off/brightness commands stock sent. **No mains-switching logic was rewritten, patched, or re-timed.**

### Protocol

- ESP32↔co-processor-1 link recovered by Xtensa disassembly of the stock 2.0.0 application image, then checked against logic-analyzer captures of the live bus at 115200 8N1. Framing, poll byte, on/off bit and the 0–100 brightness range all matched the disassembly. <sup>[b](#footnote-b)</sup>
- Full command-space sweep: all 53 byte values stock never emits, driven at co-processor 1 from a USB-UART adapter with the ESP32 held off the bus. No undocumented commands, no lockups, no resets; co-processor 1 clamps brightness >100 in its own firmware. <sup>[b](#footnote-b)</sup>
- Bus behavior identical on stock 1.2.2 and 2.0.0, so the protocol is stable across the versions available to test. <sup>[b](#footnote-b)</sup>
- The codec, frame parser and kick/ramp/range-mapping engine are dependency-free C++ and are exercised directly → `cd tests && make engine`

### Boot and rollback

Two app slots, **no USB port** — the bootloader's slot-switching is the only recovery path. Shelly replaces ESP-IDF's standard `ota_select` boot record with a proprietary format ("SH0S") in the `otadata` partition, the flash region that tells the bootloader which slot to boot. It carries an active slot (`as`), a revert slot (`rs`), a committed flag (`c`), a boot-attempts counter (`ba`), and two CRC-32s. A malformed record means a device that will not boot and cannot be recovered without opening the wall plate and soldering.

- Record format, including both mandatory CRC-32s, derived from bootloader disassembly and validated by booting Shelly's **real bootloader** under Espressif's QEMU fork against a synthesized 4 MB flash image.
- Boot-decision matrix pinned as a test: a committed record boots its slot indefinitely and **ignores `ba`**; an uncommitted record counts `ba` down once per boot and then reverts to `rs`, committing it. The payload used is a deliberately corrupted image, so the countdown is driven by a genuine boot failure and no application code can influence the result. → `make boot-matrix` <sup>[a](#footnote-a)</sup>
- **Verified against both shipped bootloader builds** — `1.0.0 (IDF 4.4.4-a9)` from the 1.3.3 package and `1.0.3 (IDF 5.5.2-s6)` from 2.0.0 — against identical seeded records. Identical decisions; they differ only in log verbosity. The 1.0.0 line is what ships on the hardware. → same target <sup>[a](#footnote-a)</sup>
- Every boot-record write is a single-copy read-modify-write: CRC resealed, written to the **non-winning** of the two copies, then read back and re-validated against the bootloader's own acceptance test. The winning copy is never touched in the same operation, so a failed, rejected or power-interrupted write always leaves one valid record behind.
- A partition-layout guard re-checks the `otadata`, `app_0` and `app_1` offsets against the live partition table at every boot and disables all boot-record writes on any mismatch.

### Flash path

- Reversibility was proven *before* any real firmware existed: a throwaway app was delivered through Shelly's own OTA to confirm the stock bootloader accepts a foreign image and that an uncommitted slot auto-reverts unattended. It did, with stock untouched in the other slot. <sup>[b](#footnote-b)</sup>
- The first-flash package contains the **application partition only** — no partition table, no filesystem. Stock partition layouts differ between firmware versions, so shipping either part would corrupt the fallback slot on a device whose stock version doesn't match. Tests assert the package can never overlap the fallback app slot or the partition table, against every known stock layout. → `make bridge`
- The same assertions are re-checked end-to-end by assembling a 2.0.0-shaped flash, applying the package, and booting it against the real bootloader. → `make stock-fw && make bridge` <sup>[a](#footnote-a)</sup>
- Subsequent updates use ESPHome-native OTA, validated end-to-end on hardware: staged to the inactive slot, booted, auto-committed on a healthy boot. <sup>[b](#footnote-b)</sup>
- An update targeting a slot that still holds stock firmware is refused at `esp_ota_begin` — the ESP-IDF call that *erases* the destination region, i.e. before anything is destroyed — unless the protection is explicitly overridden.

### Known gaps

Stated plainly, because a de-risking section that lists only successes isn't one:

- **Command rate above ~20 Hz is unvalidated on hardware.** 20 Hz is stock's own cadence and is proven good. `ramp_rate: 1000` emits roughly 100 Hz against a ~120 Hz physical ceiling. The top of that slider's range has no evidence behind it.
- **Power loss during a boot-record write** is mitigated by design (single-copy writes, the other copy always valid) but has not been empirically tested.
- **Restoring stock after the fallback slot is overwritten is untested** — see [Getting back to stock](#getting-back-to-stock-untested).
- **One hardware variant.** US SKU, one board revision, four units — and that field time belongs to the builds that were actually installed, not automatically to the current release. See [Releases and stability](#releases-and-stability).
- **Co-processor fault flags are not surfaced.** Temperature is read; the second status bit remains undecoded and unused. Stock's overtemp / no-load / non-dimmable handling is not reproduced.
- **The ESPHome integration layer has no automated coverage.** What is tested is the engine, the frame parser, the boot records and the packaging. The glue binding them to Home Assistant entities is covered by bring-up and daily field use, not by tests.
- **A committed slot with a broken image does not self-recover.** Committing is what makes an image permanent, and the bootloader will loop on it forever rather than fall back. This firmware only auto-commits after an image has run healthily for 30 s, which is what keeps that safe.

<a name="footnote-a"></a>
<sup>**a**</sup> Needs QEMU plus stock firmware images, which this repo does not redistribute. `make stock-fw` fetches them from Shelly's CDN and verifies them against content hashes. These targets skip cleanly without them.
<a name="footnote-b"></a>
<sup>**b**</sup> Bench-verified with hardware and a logic analyzer; not reproducible from this repo alone. See [PROTOCOL.md](PROTOCOL.md) and [BEHAVIOR.md](BEHAVIOR.md).

---

## Testing

Three suites. The first runs anywhere with a C++17 compiler; the other two skip cleanly without QEMU and stock images:

```sh
cd tests && make            # everything
cd tests && make stock-fw   # one-time: fetch stock images for the QEMU layers
```

**Engine + parser** (`make engine`) — the kick/ramp/range-mapping logic and the status-frame parser are standalone pure C++, so 1,629 assertions drive the *real* code with nothing but a compiler and check the exact bytes it puts on the wire: range mapping and a round-trip stability sweep, the single-pivot kick, ramp cadence / partial-step / no-overshoot, off-preserves-brightness-bits, fade-out, the no-op reflection guard, the publish floor that stops a lit lamp being reported as off, parser framing / resync / boot-banner handling, and a wire-format sweep asserting no command byte ever leaves the 0-100 range and no turn-on sequence ever emits a spurious OFF.

**Bridge package** (`make bridge`) — asserts the first-flash package can never write onto the fallback app slot or the partition table, checked against every known stock layout. Its opt-in QEMU layer assembles a 2.0.0-shaped flash, applies the package, and boots it against the **real Shelly bootloader**.

**Boot matrix** (`make boot-matrix`) — feeds seeded boot records to the **real Shelly bootloader** under QEMU and reads the decisions out of its own log, across both shipped bootloader builds.

See [`tests/README.md`](tests/README.md). What none of them reach — the ESPHome integration layer, boot-record writes on real flash, live UART timing — is covered by `esphome compile` and on-device bring-up; see [Known gaps](#known-gaps).

---

## Repo layout

- `components/shelly_wall_dimmer/` — the main ESPHome component.
- `components/status_led_pwm/` — small light platform: a dimmable PWM status LED that blinks on ESPHome's warning/error state.
- `example/` — a fully commented device config plus `secrets.yaml.example`.
- `tests/` — engine and parser unit tests, bridge-package safety tests, and the boot matrix.

---

## License

GPL-3.0 — see [LICENSE](LICENSE).
