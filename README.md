# ShellyWallDimmerSwitch-esphome

Custom [ESPHome](https://esphome.io) firmware for the **Shelly Plus Wall Dimmer US**
(`SNDM-0013US`). Replaces the stock ESP32 firmware while leaving the dimming
co-processor (APT32S003) untouched — the ESP32 just speaks the co-processor's
single-byte UART protocol.

The headline feature stock can't do: a **"kick"** priming pulse on turn-on
(briefly jump to a higher brightness so cheap LED drivers strike, then drop to
the requested low level), plus configurable ramp timing and min/max clamps —
all implemented ESP-side.

> ⚠️ Status: tracking `master`, pre-release. Flashing custom firmware to your
> dimmer is at your own risk.

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

See [`example/shelly-wall-dimmer.yaml`](example/shelly-wall-dimmer.yaml)
for a full example (light + kick/ramp/clamp number entities, temperature sensor,
local button/LED control). Copy `example/secrets.yaml.example` to
`secrets.yaml` and fill in your Wi-Fi/API/OTA values first.

## Layout

- `components/shelly_wall_dimmer/` — the ESPHome external component.
- `example/` — example device config + `secrets.yaml.example`.

## License

GNU General Public License v3.0 — see [LICENSE](LICENSE).
