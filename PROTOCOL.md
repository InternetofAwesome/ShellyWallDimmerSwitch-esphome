# Shelly Plus Wall Dimmer — ESP32 ↔ co-processor protocol

Reverse-engineered from stock firmware (app v2.0.0, verified against the
1.2.2 device on the bench, both directions). This is the contract the ESPHome
component reproduces. The co-processor (APTCHIP APT32S003F8PT, C-SKY core) is
left **stock**; we only replace the ESP32 side.

## Physical link
| | |
|---|---|
| Peripheral | ESP32 **UART1** |
| Pins | **TX = GPIO21, RX = GPIO22** |
| Baud | **115200**, 8N1 |
| Direction encodings | ESP→MCU: bare single bytes. MCU→ESP: `$…#` framed + unframed boot banner. |

> **TX/RX confirmed by disassembly** (traced `init` → wrapper → pin-dispatch
> helper → `uart_set_pin`): ESP32 **TX = GPIO21** (routed via `gpio_matrix_out`),
> **RX = GPIO22** (via `gpio_matrix_in`). This is the *reverse* of the naive
> struct-storage order — a pin-dispatch helper loads the two pin bytes swapped
> before the `uart_set_pin` call. Physical sanity check: TX (GPIO21) carries the
> `0xFF` poll and `0x80|B` commands; RX (GPIO22) receives the `$ b0 b1 b2 #`
> frames.

## ESP → co-processor (commands): one raw byte, no framing
```
0x80 | B   = ON at brightness B      (B = 0..100, integer percent)
B          = OFF (bit7 clear); low 7 bits = brightness to remember
0xFF       = status poll (request a status frame)
```
- **bit 7** = output on/off; **bits 6:0** = brightness 0–100.
- Applied by the co-processor **instantly — there is no fade/duration field.**
  Any ramp/transition/kick is produced by the ESP **streaming** successive
  bytes. This is what makes kick/ramp fully ours to control.
- Values `0x65`–`0x7F` and `0xE5`–`0xFE` are **unused** by stock (brightness
  clamps at 100). **All 53 were swept on the bench** with the ESP32 held off
  the bus: every one is inert. The co-processor clamps brightness >100 in its
  own firmware and honours only bit 7, so the unused space toggles on/off while
  keeping the previous level. There is no hidden command and no ASCII console.

## Co-processor → ESP (status): fixed 5-byte frame
```
'$'(0x24)  b0  b1  b2  '#'(0x23)
```
No checksum, no length, no command id. Fixed 3-byte payload:
| Byte | Meaning |
|---|---|
| `b0` | **live actual brightness** 0–100 (reflects slider/command changes) |
| `b1` | bit0 = output on/off state; **bit1 = unknown flag** (stayed 0 in all captures — not touch, not on/off) |
| `b2` | **die temperature, °C** (integer; 25 cold-boot, 26 warm) |

The co-processor **streams** these frames as state changes (e.g. during a
cap-touch slide, b0 follows finger position). Parser: resync on `$`, require
`#` at index 4, drop malformed frames.

## Boot banner (unframed ASCII, MCU→ESP)
On co-processor reset it emits, outside the `$…#` protocol:
```
reset!\nshelly_apt_003 mcu ver: v1.0.4
```
Useful as a **co-processor-reset signal**. Stock parses the version out of the
line after the first `\n`. Watch for `reset!` to re-sync state.

## Local controls (two, with opposite authority)
1. **Pushbutton (on/off tap)** — wired to an **ESP32 GPIO** (interrupt-driven).
   The ESP owns the toggle: on press it sends `0x80|B` / `B` (B = configured
   toggle brightness). **PREEMPTIVE** — ESPHome decides before the MCU acts,
   so the kick applies cleanly here. **GPIO4**, input, no internal pull,
   any-edge interrupt (confirmed from the stock app's board init).

   *This firmware does it differently, on purpose.* It attaches a **single-edge
   (press) interrupt** that only LATCHES the edge, then fires on the leading
   edge and disarms until a hold-off has passed and the line reads inactive.
   Any-edge would fire on press *and* release, so a press held longer than the
   hold-off toggles twice and nets to nothing; and sampling the pin's level in
   the main loop (what ESPHome's `gpio` binary sensor does) drops a tap shorter
   than one loop iteration entirely.
2. **Capacitive touch slider (brightness)** — handled by the **co-processor
   autonomously**. Finger position → absolute brightness applied immediately,
   then streamed to the ESP as `$…#` frames. **REACTIVE ONLY** — ESPHome
   learns of the change after it is applied; it cannot intercept before the
   TRIAC. Best lever is the change stream (level + timing).

**These two fight each other, because one finger drives both.** A press on the
plate registers as a touch position *and* a button press, and the co-processor
applies its touch level before the ESP32 hears anything. A kicked turn-on emits
exactly one strike byte and then goes silent for the kick dwell, so the touch
level simply overwrites it — press the lower plate to turn on and the bulb never
strikes. This firmware answers by re-sending a button-originated command in a
short burst (the "setpoint assert", ~50 ms at ~5 ms intervals, both tunable at
runtime) so it wins the race a single byte loses. Home Assistant commands are
not asserted; nothing is touching the plate then.

## Status LEDs
Flex nets `wifi_led` / `pw_led` drive front status LEDs from ESP32 GPIOs:
**sys/Wi-Fi LED = GPIO33, power LED = GPIO25** (confirmed from the stock app's
LED-manager construction, which matches the `WD_UI` config schema
`{sys_led_enable:%B, power_led:%Q}`). Both are **active-low**, bench-confirmed.
Stock config: `power_led` ∈ {match_output, inverted_output, on, off}.

## The complete ESP32 pin map — there is nothing else
`GPIO1`/`GPIO3` UART0 console, `GPIO21` UART1 TX, `GPIO22` UART1 RX, `GPIO4`
button, `GPIO25` power LED, `GPIO33` sys LED. Six pins. The flex nets `rst`,
`swm`, `scl`, `sda`, `wake` and `int` are **never driven by the ESP32** in
stock firmware, so there is no reset line to the co-processor and no way for it
to interrupt us — which is why it pushes frames up the UART unsolicited, and
why a wedged co-processor cannot be recovered from firmware.

## What the component implements
The behavioural layer built on this protocol — kick, ramp cadence, range
mapping, the thermal cutout and the entity set — is documented in the
[README](README.md), which is kept in step with the code. It is deliberately
not restated here: two specifications of the same behaviour drift apart, and
this one already did once.

## Notes / non-goals
- **No energy monitoring**: cross-referenced against the Gen1 `shelly_dimmer`
  (STM32) protocol — this co-processor reports only temperature, not
  power/voltage/current. Do not expect wattage.
- Brightness resolution is fixed at **101 levels** (0–100). Coarsest at the
  low end, which is where the kick operates.
