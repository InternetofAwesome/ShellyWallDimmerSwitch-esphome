#DO NOT USE THIS COMMIT



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

- A **Shelly Plus Wall Dimmer US** (`SNDM-0013US`) on your LAN — stock firmware (1.3.x–2.0.0 tested in emulator), local access on, and **its IP address** (you'll point the bridge at it).
- **ESPHome** — the Builder add-on in Home Assistant (these steps), or the CLI.

Nothing else: no cable, no USB-serial adapter, no disassembly.

**Steps:**

1. **ESPHome Builder → `+ NEW DEVICE`.** Give it a name (e.g. `office-lights`); pick **ESP32** if asked. The wizard writes a starter config with a generated API encryption key and OTA password, and puts your Wi-Fi credentials in `secrets.yaml`. Let it finish, then **skip** the install it offers.

2. **Edit the new device and replace its contents** with [`example/shelly-wall-dimmer.yaml`](example/shelly-wall-dimmer.yaml) — but **keep the `api:` and `ota:` blocks the wizard generated**, since those hold this device's keys. Set `name:` / `friendly_name:` to match, and add an `ap_password:` to `secrets.yaml` for the fallback hotspot. Leave the `esp32:` block's **`board: esp32dev`** and **`toolchain: platformio`** as they are — the bridge hooks a PlatformIO post-build step, and since ESPHome 2026.7.0 the esp32 default is the native ESP-IDF backend, which never runs it. (Configure `bridge_package` without it and the component stops you at config time rather than silently doing nothing.)

3. **Point the bridge at your dimmer and set a unique version:**
   ```yaml
   substitutions:
     fw_version: "2026.08.07-1"   # bump this for every bridge push

   shelly_wall_dimmer:
     id: dimmer_coprocessor       # the entities below refer to this name
     bridge_package:
       push_to: 192.168.1.50      # YOUR dimmer's IP
   ```
   Stock dedupes on the version baked into the app image, so an unchanged `fw_version` means stock downloads the package and silently discards it. Changing it rewrites `sdkconfig`, which forces a full rebuild — expect the first builds to take a while.

4. **Install → `Manual download`.** *Not* Wireless. Manual download simply **compiles**, which is all the bridge needs — the post-build step assembles the package, serves it, and calls `Shelly.Update` on the device itself. This isn't just a preference: with `bridge_package` configured the component **refuses to build at all** for an install/upload job, because the two delivery paths in one command can flash both slots and destroy your stock fallback (see below). Watch the log for:
   ```
   >> shelly-bridge: wrote .../PlusWallDimmer-bridge.zip
   >> shelly-bridge: triggering Shelly.Update on 192.168.1.50 ...
   >> shelly-bridge: device fetched the package
   ```
   You can ignore/discard the `.bin` the browser offers to download. *(No `push_to`? The zip lands in the build's `shelly-bridge/` folder — deliver it yourself with `Shelly.Update {url: ...}`.)*

5. **The dimmer flashes its inactive slot and reboots into our firmware** — stock stays untouched in the other slot. The device goes **online** in the Builder; open its **Logs** and confirm your `fw_version` in the boot banner and a `layout guard OK` line.

6. **Home Assistant discovers it.** Settings → Devices & Services → the ESPHome integration should offer the new device; add it (paste the API key if prompted) and the entities appear.

7. **Comment out `bridge_package:`** — it's a first-flash-only tool. Leave `toolchain: platformio` alone. **Don't reinstall just to apply that edit:** your next install is your *second* flash, and that is what overwrites the slot still holding stock. From then on, `Install → Wireless` works like any ESPHome device, and stays boot-safe automatically.

**If this first build misbehaves,** it auto-reverts to stock on its own after a few failed boots (that's slot B, still untouched); **Boot: Revert** (below) does the same on demand. Neither is "back to stock" once you've flashed a *second* time — see the next section.

---

## Converting more switches

**Don't copy a working device's YAML.** It's the obvious move and it quietly couples the two devices. Repeat step 1 instead — **Builder → `+ NEW DEVICE`** — so the wizard mints this device its own API key and OTA password, paste the example over it as before, and then carry across **only your tuned values** (kick level/dwell, ramp rate, min/max, LED brightness).

Never carry across:

| Don't copy | Why |
|---|---|
| `ota: password:` | Distinct passwords are an **interlock**: aim an install at the wrong device and it *fails*. Share them and a mis-aimed wireless install silently succeeds, flashing one switch with another's firmware — it then comes up claiming the wrong name and Home Assistant gets very confused. |
| `api: encryption: key:` | Two devices answering to one key invites identity mix-ups during discovery. |
| `name:` / `friendly_name:` | Duplicate names collide on mDNS, and the Builder can end up talking to whichever answers first. |
| `bridge_package: push_to:` | Must be the **new** switch's IP. Left pointing at an already-converted one it just fails (that device no longer speaks `Shelly.Update`), so nothing gets flashed and it isn't obvious why. |

Each switch keeps its **own** stock image in its own spare slot, with its own **Allow Overwrite Stock** switch, default off — converting one has no effect on another's rollback. And comment out `bridge_package:` once a switch is converted, as in step 7.

> Right after a conversion, Home Assistant may log `Invalid encryption key` for the new device once or twice. That's expected while it finishes rebooting into the new firmware — HA is still holding the key from before the flash. It clears on its own; if it persists, the key really is mismatched.

---

## Getting back to stock (untested)

Be clear-eyed about what "fail safe" covers here: **updates** are safe (bad build → auto-revert to the other slot), but **restoring stock** is a different claim we haven't proven.

The bootloader only ever swaps between the two app slots. Right after your *first* flash, slot B still holds stock — so a crash-loop or a manual revert genuinely lands you back on stock. But flash a *second* time (any update after that) and slot B gets overwritten too. At that point there is no stock image left on the device, and Boot: Revert just swaps between two copies of our firmware.

Getting stock back after that would need one of:
- **Re-deliver a stock package over ESPHome's OTA**, staged through our SH0S wrapper — plausible, but we haven't tried it, and it's unclear whether the wrapper or a stock image cooperates with that path.
- **Serial recovery via GPIO0** — reliable, but means opening the unit and soldering, which defeats the point of the solder-free flash.

If you want a real fallback, keep a copy of your dimmer's original OTA package before you flash the second time, and don't treat "auto-revert" as "uninstall."

**The firmware makes this a deliberate choice rather than an accident.** An ESPHome OTA writes to the *other* app slot — the one still holding stock — so the first routine wireless update after conversion would silently consume your only way back. Instead, the firmware **refuses that OTA** and tells you why:

```
OTA REFUSED: target slot at 0x200000 still holds the stock Shelly firmware
(project "PlusWallDimmer", version "2.0.0") -- your only rollback on a device
with no USB. Turn on the "Allow Overwrite Stock" switch in Home Assistant to
proceed. This is irreversible...
```

To go ahead, flip **Allow Overwrite Stock** on the device page in HA (under *Configuration*), then update as normal. It persists across reboots — set once and forget — and becomes irrelevant afterwards, since once that slot is overwritten there is no stock image left to protect. The check is enforced at `esp_ota_begin`, i.e. *before* the erase, so a refused update leaves the stock image completely untouched. A blank or unreadable slot is never protected.

This is deliberately the one thing standing between you and a routine update: it costs one toggle, once, in exchange for not discovering later that your rollback quietly disappeared.

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

`bridge_package:` assembles the stock-format OTA zip containing **only the app image** — no partition table, no filesystem. **No Shelly binaries are redistributed.** Sub-option `push_to: <ip>` serves it and calls `Shelly.Update` on that device after a good build (the host address is derived from the route to it). Omit `push_to` to just produce the zip. Requires `toolchain: platformio` (see Quick start).

**The bridge is compile-only, and that is enforced.** It delivers firmware over Shelly's OTA during the post-build step, so pairing it with ESPHome's own OTA in a single command is destructive rather than redundant: on a still-stock dimmer the bridge flashes the inactive slot and the device reboots into *this* firmware — which speaks ESPHome OTA — so the upload half of the same command can land on the **other** slot, the one still holding stock. One "Install" would consume both slots and silently destroy the rollback target. (On an already-converted dimmer, a stale `push_to` would instead flash a *different* device while the OTA updates the intended one.) Configure `bridge_package` and run an install/upload and the component fails validation before anything is built or sent. The check **fails closed**: if it can't determine what kind of invocation it's in (e.g. a future ESPHome renames its CLI commands) it refuses rather than assuming the safe case, since a false block is a visible, harmless error while a false allow can cost your only rollback path. Set `SHELLY_BRIDGE_ALLOW_UNVERIFIED_INVOCATION=1` to proceed anyway once you've confirmed the build is compile-only.

> ⚠️ **The guard cannot cover every path.** The Builder's `Install → Wireless` runs **two separate commands** — a `compile` (which the bridge treats as safe, so it *will* push) followed by an `upload` that reuses ESPHome's validated-config cache and skips validation entirely. Neither half looks dangerous on its own, so the guard does not fire. Use **Manual download** for the bridge, as above; the guard reliably catches only the single-command forms (`esphome run`/`upload`).

App-only is a safety requirement, not a simplification. Stock partition layouts **differ between firmware versions** (1.3.3 uses 0x180000 app slots with a 0x70000 `fs_0`; 2.0.0 uses 0x190000 and 0x60000). A package is built once and may land on either, so shipping our own table or a fixed-size filesystem image corrupts the *other* slot — the stock firmware that is your only rollback path — on any device whose version doesn't match what those parts were cut from. Neither part is needed: the three offsets this firmware depends on (`otadata@0xd000`, `app_0@0x10000`, `app_1@0x200000`) are identical across versions, the Shelly bootloader re-syncs the live table from its own copy anyway, and this firmware never mounts a filesystem. [`tests/bridge_package_test.py`](tests/bridge_package_test.py) pins this, including a QEMU boot against the real Shelly bootloader.

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
| `allow_overwrite_stock` | off | Permit an OTA to erase the slot still holding stock firmware. **Persists across reboots.** Until you turn this on, wireless updates that would destroy your rollback are refused — see [Getting back to stock](#getting-back-to-stock-untested). |

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
- Writes flash: **Boot: Commit** (make the running slot permanent now), **Boot: Revert** (point the next boot at the *other slot* — which is stock only until your second flash overwrites it).

Each write touches only one of the two boot-state copies, and the partition-layout guard refuses it on any flash map it doesn't recognize — but press **Boot: Log** and sanity-check first.

**Partition-layout guard.** Boot-slot writes assume fixed offsets (`otadata@0xd000`, `app_0@0x10000`, `app_1@0x200000` — identical across stock 1.3.x and 2.0.0). At boot the firmware checks the live table; on any mismatch it disables all boot-state writes and logs it (`dump_config` shows `Boot-state writes ... DISABLED`). So it fails safe on a layout it doesn't know.

---

## Testing

Two suites, both runnable without hardware:

```sh
cd tests && make
```

**Engine** — the kick/ramp/range-mapping logic is standalone pure C++, so ~400 assertions drive the *real* engine with nothing but a C++17 compiler and check the exact bytes it puts on the wire: range mapping (plus a round-trip stability sweep), the single-pivot kick, ramp cadence / partial-step / no-overshoot, off-preserves-brightness-bits, fade-out, and the no-op reflection guard.

**Bridge package** — asserts the first-flash package can never write onto the fallback app slot or the partition table, checked against every known stock layout. Its opt-in QEMU layer assembles a 2.0.0-shaped flash, applies the package, and boots it against the **real Shelly bootloader** to confirm stock still comes up and our firmware loads from the targeted slot with the rollback countdown armed. That layer needs your own (non-redistributable) stock firmware files and skips cleanly without them.

See [`tests/README.md`](tests/README.md). What neither reaches — ESPHome glue, SH0S boot-state writes on real flash, live UART timing — is covered by `esphome compile` and on-device bring-up.

---

## Repo layout

- `components/shelly_wall_dimmer/` — the main ESPHome component.
- `components/status_led_pwm/` — small light platform: a dimmable PWM status LED that blinks on ESPHome's warning/error state (the WiFi LED).
- `example/` — device config + `secrets.yaml.example`.
- `tests/` — engine unit tests + bridge-package safety tests (incl. a QEMU boot).

## License

GPL-3.0 — see [LICENSE](LICENSE).
