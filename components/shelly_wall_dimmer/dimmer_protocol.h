#pragma once
// Framework-agnostic core of the Shelly Plus Wall Dimmer co-processor
// protocol. Pure C++ (no ESPHome deps) so it is unit-testable and the ESPHome
// wrapper just includes it. See PROTOCOL.md for the full contract.

#include <cstdint>
#include <cstddef>

namespace shelly_dimmer_core {

// ---- wire constants -------------------------------------------------------
static constexpr uint8_t CMD_POLL = 0xFF;   // ESP->MCU: request a status frame
static constexpr uint8_t FRAME_SOF = 0x24;  // '$'  MCU->ESP frame start
static constexpr uint8_t FRAME_EOF = 0x23;  // '#'  MCU->ESP frame end
static constexpr uint8_t BRIGHTNESS_MAX = 100;

// ESP->MCU command byte: bit7 = on/off, bits6:0 = brightness (0..100).
// OFF preserves the brightness in the low bits (device remembers it).
inline uint8_t encode_command(bool on, uint8_t brightness) {
  if (brightness > BRIGHTNESS_MAX) brightness = BRIGHTNESS_MAX;
  return on ? uint8_t(0x80 | brightness) : brightness;
}

// Decoded 5-byte status frame: '$' b0 b1 b2 '#'
struct StatusFrame {
  uint8_t brightness;  // b0: live actual brightness 0..100
  bool output_on;      // b1 bit0
  bool flag_bit1;      // b1 bit1: unknown (0 in all captures) — surfaced for study
  uint8_t temp_c;      // b2: die temperature, deg C
};

// Byte-at-a-time parser. Resyncs on '$'. Non-frame bytes (e.g. the boot
// banner "reset!\nshelly_apt_003 mcu ver: ...") are handed to on_stray so the
// wrapper can detect a co-processor reset / read the version.
class FrameParser {
 public:
  // Feed one received byte. Returns true and fills `out` when a complete,
  // well-formed frame is decoded this call.
  bool feed(uint8_t b, StatusFrame &out) {
    switch (state_) {
      case State::IDLE:
        if (b == FRAME_SOF) { state_ = State::PAYLOAD; idx_ = 0; }
        else if (on_stray_) on_stray_(b, on_stray_ctx_);
        return false;

      case State::PAYLOAD:
        payload_[idx_++] = b;
        if (idx_ == 3) state_ = State::EXPECT_EOF;
        return false;

      case State::EXPECT_EOF:
        state_ = State::IDLE;
        if (b == FRAME_EOF) {
          out.brightness = payload_[0];
          out.output_on = (payload_[1] & 0x01) != 0;
          out.flag_bit1 = (payload_[1] & 0x02) != 0;
          out.temp_c = payload_[2];
          return true;
        }
        // Bad EOF: resync. If the offending byte is itself a SOF, restart a
        // frame from it rather than dropping it.
        if (b == FRAME_SOF) { state_ = State::PAYLOAD; idx_ = 0; }
        else if (on_stray_) on_stray_(b, on_stray_ctx_);
        return false;
    }
    return false;
  }

  // Optional sink for out-of-frame bytes (boot banner, noise).
  void set_stray_handler(void (*fn)(uint8_t, void *), void *ctx) {
    on_stray_ = fn; on_stray_ctx_ = ctx;
  }

  void reset() { state_ = State::IDLE; idx_ = 0; }

 private:
  enum class State : uint8_t { IDLE, PAYLOAD, EXPECT_EOF };
  State state_ = State::IDLE;
  uint8_t idx_ = 0;
  uint8_t payload_[3] = {0, 0, 0};
  void (*on_stray_)(uint8_t, void *) = nullptr;
  void *on_stray_ctx_ = nullptr;
};

}  // namespace shelly_dimmer_core
