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
  // kick_level is the single pivot AND the strike level: the lowest brightness at
  // which the bulb reliably lights. On a turn-on from off (kick enabled) we snap
  // here first, then reach the target -- dwell + ramp DOWN if the target is below
  // kick_level (the kick), or ramp UP with no dwell if above (already lit; fading
  // up from 0 would just sit dark until here anyway). Tune to your bulb, keeping
  // a little margin so the dwell reliably strikes.
  uint8_t kick_level = 20;       // device-side %
  uint32_t kick_dwell_ms = 150;  // hold kick_level this long before ramping down
  // min/max define the usable DEVICE brightness window that the 0-100 command
  // scale is stretched across (NOT a clamp): a command of 0 maps to
  // min_brightness and 100 maps to max_brightness, linearly. e.g. min=20,max=80:
  // HA 0% -> 20% real, HA 100% -> 80% real. The inverse is applied to device
  // reports so HA still shows 0-100. Defaults 1/100 == near-identity (feature
  // effectively off until you narrow the window). See map_to_device/map_to_ha.
  uint8_t min_brightness = 1;
  uint8_t max_brightness = 100;
  // A single ramp RATE in percent/second, shared by every ramp below. The engine
  // converts it to a fixed (step, interval) cadence at ramp start, quantized to a
  // RAMP_MIN_PERIOD_MS floor (~one mains half-cycle: the TRIAC acts once per
  // 8.33 ms, so finer buys nothing) with NO dithering -- one step size and one
  // interval for the whole ramp; a leftover partial step just lands on the
  // setpoint. Clamped to [RAMP_RATE_MIN, RAMP_RATE_MAX] so it is never zero. The
  // old 3%/20ms default == 150 %/s.
  uint16_t ramp_rate = 150;      // percent per second
  // Which transitions actually ramp (all use ramp_rate):
  bool ramp_on_change = true;    // ramp on a brightness change while already on
  bool ramp_on_off = false;      // fade in on turn-on / fade out on turn-off
  bool limit_correct = false;    // if a touch report lands outside [min,max], ramp back to it
};

// Ramp-cadence quantization (see DimmerParams::ramp_rate). The 10 ms floor is far
// above the 1 ms RTOS tick (CONFIG_FREERTOS_HZ=1000) so it is always resolvable,
// and just above the 60 Hz mains half-cycle so it is the finest step that changes
// anything on the load.
static constexpr uint32_t RAMP_MIN_PERIOD_MS = 10;
static constexpr uint16_t RAMP_RATE_MIN = 1;      // %/s -- never zero
static constexpr uint16_t RAMP_RATE_MAX = 1000;   // %/s -- 0->100 in 100 ms

class DimmerEngine {
 public:
  using SendFn = void (*)(uint8_t byte, void *ctx);
  void set_send_handler(SendFn fn, void *ctx) { send_ = fn; send_ctx_ = ctx; }

  DimmerParams &params() { return p_; }
  const DimmerParams &params() const { return p_; }

  // current_ is the last DEVICE-side brightness byte (0..100). current_brightness()
  // returns it raw (for wire reconstruction); current_brightness_ha() maps it back
  // onto the 0..100 command scale for display in HA -- so a touch-plate change the
  // co-processor reports still shows sensibly, just un-stretched (see caveat: we
  // cannot constrain physical dimming to the min/max window, only re-scale it).
  uint8_t current_brightness() const { return current_; }
  uint8_t current_brightness_ha() const { return map_to_ha(current_); }
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
      // Compare in DEVICE scale: a device report is reflected back to HA as
      // map_to_ha(current_), which the light layer feeds back here; mapping it
      // to the device scale returns current_, so the reflection is a no-op and
      // we don't re-command a level the co-processor already holds.
      uint8_t bha = brightness > BRIGHTNESS_MAX ? BRIGHTNESS_MAX : brightness;
      uint8_t bdev = map_to_device(bha);
      if (!want_on && !on_) return;
      if (want_on && on_ && bdev == current_) return;
    }
    // OFF: fade to black then off (ramp_on_off), or immediate off. The immediate
    // path preserves the brightness bits so a later on can restore the level.
    if (!on || brightness == 0) {
      if (p_.ramp_on_off && on_ && current_ > 0) {
        start_ramp_to(0, now_ms, /*to_off=*/true);
      } else {
        emit(encode_command(false, current_));
        on_ = false;
        mode_ = Mode::IDLE;
      }
      return;
    }

    uint8_t t = map_to_device(brightness);
    bool was_off = !on_;
    pending_off_ = false;

    if (was_off && p_.kick_enabled) {
      // Kicked turn-on: always start at kick_level. Below it the bulb is dark, so
      // there's no point fading up from 0 -- snap to the strike level, then reach
      // the target. Below kick_level: dwell, then ramp DOWN (the kick). Above:
      // ramp UP immediately, no dwell (already lit). Equal: land here.
      emit(encode_command(true, p_.kick_level));
      current_ = p_.kick_level;
      on_ = true;
      target_ = t;
      if (t < p_.kick_level) {
        mode_ = Mode::KICK_PRIMING;
        t_event_ = now_ms + p_.kick_dwell_ms;
      } else if (t > p_.kick_level) {
        start_ramp_to(t, now_ms, false);
      } else {
        mode_ = Mode::IDLE;
      }
    } else if (was_off) {
      // Non-kick turn-on: fade in from 0 (ramp_on_off) or jump straight there.
      on_ = true;
      if (p_.ramp_on_off) {
        current_ = 0;
        start_ramp_to(t, now_ms, false);
      } else {
        emit(encode_command(true, t));
        current_ = t;
        mode_ = Mode::IDLE;
      }
    } else {
      // Brightness change while on: ramp (ramp_on_change) or jump.
      on_ = true;
      if (p_.ramp_on_change) {
        start_ramp_to(t, now_ms, false);
      } else {
        emit(encode_command(true, t));
        current_ = t;
        mode_ = Mode::IDLE;
      }
    }
  }

  // Call frequently from the wrapper's loop().
  void tick(uint32_t now_ms) {
    switch (mode_) {
      case Mode::KICK_PRIMING:
        // Dwell elapsed: ramp from kick_level to the target (either direction).
        if (int32_t(now_ms - t_event_) >= 0) start_ramp_to(target_, now_ms, false);
        break;
      case Mode::RAMPING:
        if (int32_t(now_ms - t_next_step_) >= 0) {
          step_toward_target();
          if (current_ == target_) {
            finish_ramp_();  // emits the final level, or the OFF command
          } else {
            emit(encode_command(true, current_));
            t_next_step_ = now_ms + ramp_interval_ms_;
          }
        }
        break;
      case Mode::IDLE:
      default:
        break;
    }
  }

  // Reconcile with an unsolicited status frame (cap-touch slider moved the level,
  // or the pushbutton toggled at the MCU's echo). Only adopt when settled, so we
  // never fight our own output. With limit_correct on, a touch value outside
  // [min,max] is pulled back to the nearest limit via a ramp -- best effort: we
  // can only react AFTER the co-processor reports it (README caveat), and while
  // correcting we ignore further reports (mode != IDLE).
  void notify_status(uint8_t b0_brightness, bool output_on, uint32_t now_ms) {
    if (mode_ != Mode::IDLE) return;
    current_ = b0_brightness;
    on_ = output_on;
    if (p_.limit_correct && on_) {
      uint8_t lo = p_.min_brightness, hi = p_.max_brightness;
      if (lo > hi) lo = hi;
      if (hi > BRIGHTNESS_MAX) hi = BRIGHTNESS_MAX;
      if (b0_brightness < lo) start_ramp_to(lo, now_ms, false);
      else if (b0_brightness > hi) start_ramp_to(hi, now_ms, false);
    }
  }

 private:
  enum class Mode : uint8_t { IDLE, KICK_PRIMING, RAMPING };

  void emit(uint8_t byte) { if (send_) send_(byte, send_ctx_); }

  // Stretch a 0..100 command onto the [min,max] device window: 0 -> min,
  // 100 -> max, linear with rounding. (Replaces the old floor/ceiling clamp.)
  uint8_t map_to_device(uint8_t ha) const {
    uint8_t lo = p_.min_brightness, hi = p_.max_brightness;
    if (lo > hi) lo = hi;                    // guard against inverted config
    if (hi > BRIGHTNESS_MAX) hi = BRIGHTNESS_MAX;
    if (ha > BRIGHTNESS_MAX) ha = BRIGHTNESS_MAX;
    uint32_t span = uint32_t(hi - lo);
    uint8_t r = uint8_t(lo + (span * ha + 50) / 100);  // round to nearest
    return r > BRIGHTNESS_MAX ? BRIGHTNESS_MAX : r;
  }

  // Inverse of map_to_device: fold a device brightness back onto the 0..100
  // command scale for display. Values outside [min,max] (only reachable by a
  // direct touch-plate change) saturate at 0/100. Chosen so
  // map_to_device(map_to_ha(r)) == r for r in [min,max], keeping the HA
  // reflection a stable no-op.
  uint8_t map_to_ha(uint8_t real) const {
    uint8_t lo = p_.min_brightness, hi = p_.max_brightness;
    if (lo > hi) lo = hi;
    if (hi <= lo) return 0;                  // degenerate window
    if (real <= lo) return 0;
    if (real >= hi) return BRIGHTNESS_MAX;
    uint32_t span = uint32_t(hi - lo);
    return uint8_t((uint32_t(real - lo) * 100 + span / 2) / span);
  }

  // Convert ramp_rate (%/s) into a fixed (step, interval) cadence, quantized to
  // RAMP_MIN_PERIOD_MS. No dithering: one step size and one interval per ramp;
  // the only imprecision is integer-ms rounding of the interval (<= half a
  // period). For R <= 100 %/s a 1% step is slower than the floor, so we stretch
  // the interval; above that we widen the step and keep interval >= the floor.
  void ramp_cadence_(uint8_t &step, uint32_t &interval_ms) const {
    uint32_t R = p_.ramp_rate;
    if (R < RAMP_RATE_MIN) R = RAMP_RATE_MIN;
    if (R > RAMP_RATE_MAX) R = RAMP_RATE_MAX;
    if (1000u / R >= RAMP_MIN_PERIOD_MS) {
      step = 1;
      interval_ms = (1000u + R / 2) / R;  // round(1000 / R)
    } else {
      uint32_t s = (R * RAMP_MIN_PERIOD_MS + 999) / 1000;  // ceil(R * Tq / 1000)
      step = uint8_t(s < 1 ? 1 : s);
      interval_ms = (s * 1000u + R / 2) / R;  // round(step * 1000 / R)
    }
    if (interval_ms < 1) interval_ms = 1;
  }

  // Begin (or instantly complete) a ramp from current_ to tgt at ramp_rate.
  // to_off: finish by sending the OFF command (fade-out) instead of an on level.
  void start_ramp_to(uint8_t tgt, uint32_t now_ms, bool to_off) {
    target_ = tgt;
    pending_off_ = to_off;
    ramp_cadence_(ramp_step_, ramp_interval_ms_);
    uint32_t adist = current_ >= target_ ? uint32_t(current_ - target_)
                                         : uint32_t(target_ - current_);
    if (adist <= ramp_step_) {  // one move (or none) lands exactly on the setpoint
      current_ = target_;
      finish_ramp_();
      return;
    }
    on_ = true;
    mode_ = Mode::RAMPING;
    t_next_step_ = now_ms;  // first step on the next tick
  }

  // Reached the target: emit the final on level, or the OFF command for a fade.
  void finish_ramp_() {
    if (pending_off_) {
      emit(encode_command(false, current_));
      on_ = false;
      pending_off_ = false;
    } else {
      emit(encode_command(true, current_));
    }
    mode_ = Mode::IDLE;
  }

  void step_toward_target() {
    uint8_t step = ramp_step_ < 1 ? 1 : ramp_step_;
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
  uint8_t ramp_step_ = 1;         // current ramp's step size (pct), set at start
  uint32_t ramp_interval_ms_ = 20;  // current ramp's step interval, set at start
  bool pending_off_ = false;      // fade-out in progress: send OFF on completion
};

}  // namespace shelly_dimmer_core
