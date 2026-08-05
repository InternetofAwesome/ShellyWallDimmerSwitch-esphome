#pragma once
// Framework-agnostic kick / ramp / clamp state machine for the Shelly Plus
// Wall Dimmer. Pure C++ (no ESPHome deps). Driven by tick(now_ms) and a
// send-byte callback; the ESPHome wrapper wires the callback to the UART,
// feeds it status frames, and exposes the parameters as HA number entities.
// See BEHAVIOR.md for the behavioural spec.

#include <cstdint>
#include "dimmer_protocol.h"

namespace shelly_dimmer_core {

struct DimmerParams {
  bool kick_enabled = true;
  uint8_t kick_threshold = 20;   // kick only when on-target <= this
  uint8_t kick_level = 100;      // brightness to prime at
  uint32_t kick_dwell_ms = 150;  // hold kick_level this long
  uint8_t min_brightness = 1;    // floor clamp for non-zero targets
  uint8_t max_brightness = 100;  // ceiling clamp
  uint32_t ramp_step_ms = 20;    // interval between ramp steps (0 = instant)
  uint8_t ramp_step_size = 3;    // brightness units per ramp step (>=1)
};

class DimmerEngine {
 public:
  using SendFn = void (*)(uint8_t byte, void *ctx);
  void set_send_handler(SendFn fn, void *ctx) { send_ = fn; send_ctx_ = ctx; }

  DimmerParams &params() { return p_; }
  const DimmerParams &params() const { return p_; }

  uint8_t current_brightness() const { return current_; }
  bool is_on() const { return on_; }
  bool busy() const { return mode_ != Mode::IDLE; }

  // External request from HA / the light layer / the pushbutton.
  void request(bool on, uint8_t brightness, uint32_t now_ms) {
    // Idempotent no-op: when settled (IDLE) and already in exactly the
    // requested state, do nothing. This makes a device-driven state
    // reflection — a status frame echoed back through the light layer, or the
    // cap-touch slider's value — a no-op instead of re-commanding, so we never
    // fight the co-processor over a level it already holds (and never re-clamp
    // a manual slider value below min_brightness). Raw compare (not clamped),
    // because current_ tracks the device value verbatim via notify_status().
    if (mode_ == Mode::IDLE) {
      bool want_on = on && brightness != 0;
      uint8_t b = brightness > BRIGHTNESS_MAX ? BRIGHTNESS_MAX : brightness;
      if (!want_on && !on_) return;
      if (want_on && on_ && b == current_) return;
    }
    if (!on || brightness == 0) {  // OFF is immediate; remember brightness bits
      emit(encode_command(false, current_));
      on_ = false;
      mode_ = Mode::IDLE;
      return;
    }
    uint8_t t = clamp_target(brightness);
    bool was_off = !on_;
    target_ = t;

    if (p_.kick_enabled && was_off && t <= p_.kick_threshold) {
      // Prime the driver, then ramp down from kick_level to the target.
      emit(encode_command(true, p_.kick_level));
      current_ = p_.kick_level;
      on_ = true;
      mode_ = Mode::KICK_PRIMING;
      t_event_ = now_ms + p_.kick_dwell_ms;
    } else if (was_off) {
      // Plain turn-on: jump straight to target (no flash of a stale level).
      emit(encode_command(true, t));
      current_ = t;
      on_ = true;
      mode_ = Mode::IDLE;
    } else {
      // Brightness change while on: ramp from current to target.
      on_ = true;
      start_ramp(now_ms);
    }
  }

  // Call frequently from the wrapper's loop().
  void tick(uint32_t now_ms) {
    switch (mode_) {
      case Mode::KICK_PRIMING:
        if (int32_t(now_ms - t_event_) >= 0) start_ramp(now_ms);
        break;
      case Mode::RAMPING:
        if (int32_t(now_ms - t_next_step_) >= 0) {
          step_toward_target();
          emit(encode_command(true, current_));
          if (current_ == target_) mode_ = Mode::IDLE;
          else t_next_step_ = now_ms + p_.ramp_step_ms;
        }
        break;
      case Mode::IDLE:
      default:
        break;
    }
  }

  // Reconcile with an unsolicited status frame (e.g. cap-touch slider moved
  // the level, or the pushbutton toggled at the MCU's echo). Only adopt it
  // when we are not mid-command, so we never fight our own output.
  void notify_status(uint8_t b0_brightness, bool output_on) {
    if (mode_ == Mode::IDLE) {
      current_ = b0_brightness;
      on_ = output_on;
    }
  }

 private:
  enum class Mode : uint8_t { IDLE, KICK_PRIMING, RAMPING };

  void emit(uint8_t byte) { if (send_) send_(byte, send_ctx_); }

  uint8_t clamp_target(uint8_t b) const {
    uint8_t lo = p_.min_brightness, hi = p_.max_brightness;
    if (lo > hi) lo = hi;                 // guard against inverted config
    if (b < lo) b = lo;
    if (b > hi) b = hi;
    if (b > BRIGHTNESS_MAX) b = BRIGHTNESS_MAX;
    return b;
  }

  void start_ramp(uint32_t now_ms) {
    int32_t delta = int32_t(target_) - int32_t(current_);
    uint32_t adist = delta < 0 ? uint32_t(-delta) : uint32_t(delta);
    uint8_t step = p_.ramp_step_size < 1 ? 1 : p_.ramp_step_size;
    if (p_.ramp_step_ms == 0 || adist <= step) {
      current_ = target_;
      emit(encode_command(true, current_));
      mode_ = Mode::IDLE;
    } else {
      mode_ = Mode::RAMPING;
      t_next_step_ = now_ms;  // first step on the next tick
    }
  }

  void step_toward_target() {
    uint8_t step = p_.ramp_step_size < 1 ? 1 : p_.ramp_step_size;
    if (current_ < target_)
      current_ = uint8_t(current_ + step > target_ ? target_ : current_ + step);
    else
      current_ = uint8_t(current_ < step || current_ - step < target_ ? target_
                                                                       : current_ - step);
  }

  DimmerParams p_{};
  SendFn send_ = nullptr;
  void *send_ctx_ = nullptr;

  Mode mode_ = Mode::IDLE;
  uint8_t current_ = 0;  // our model of the last brightness byte sent (0..100)
  bool on_ = false;
  uint8_t target_ = 0;
  uint32_t t_event_ = 0;      // kick dwell deadline
  uint32_t t_next_step_ = 0;  // next ramp step time
};

}  // namespace shelly_dimmer_core
