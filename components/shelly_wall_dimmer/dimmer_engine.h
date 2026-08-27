#pragma once
// Framework-agnostic kick / ramp / clamp state machine for the Shelly Plus
// Wall Dimmer. Pure C++ (no ESPHome deps). Driven by tick(now_ms) and a
// send-byte callback; the ESPHome wrapper wires the callback to the UART,
// feeds it status frames, and exposes the parameters as HA number entities.
// The behavioural contract (kick, ramp cadence, range mapping, thermal
// cutout) is documented in the README; the wire protocol is in PROTOCOL.md.

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

  // ---- setpoint assert (front-button commands only) -----------------------
  // After a LOCAL command -- one that originated at the front pushbutton, via
  // request_local() -- re-send the byte we just emitted every assert_interval_ms
  // for assert_ms.
  //
  // Why: the same finger that presses the button lands on the capacitive plate,
  // which is owned by the co-processor and applied to the TRIAC before we ever
  // hear about it (PROTOCOL.md). A kicked turn-on emits exactly ONE strike byte
  // and then sits silent for the whole kick dwell, so a touch position captured
  // in that window simply overwrites it and the bulb never strikes. A short
  // burst wins that race; a single byte loses it. It applies to turn-OFF too --
  // the plate can re-assert a level just as easily in that direction.
  //
  // Only the button path arms this. Home Assistant commands go through plain
  // request() and are unchanged, because nothing is touching the plate then.
  // 0 disables.
  uint32_t assert_ms = 50;
  uint32_t assert_interval_ms = 5;

  // ---- over-temperature cutout (belt and suspenders) ----------------------
  // Over the limit, the output is commanded off. That is the whole rule.
  //
  // Deliberately not a thermal model and deliberately not an attempt to infer
  // what the co-processor is doing: the limit sits far above anything normal
  // and far below anything that damages parts, so it catches a runaway without
  // needing to be accurate.
  //
  // The default is 65 C, not the 75 C first proposed, because the number is
  // still PROVISIONAL and an unverified thermal limit should err low. Tripping
  // early costs light; tripping late costs hardware.
  //
  // Measured so far: 25-27 C idle, ~29 C after hours at 24% load. That is a
  // weak data point, because 24% is nowhere near worst case -- see below.
  // Typical weakest-link ratings in this class are 85 C (industrial MCUs,
  // electrolytics), with TRIAC junctions higher.
  //
  // CHARACTERIZING THIS PROPERLY: worst-case dissipation in a phase-cut dimmer
  // is at MID-RANGE brightness, around 50%, NOT at full brightness. The device
  // switches at the instantaneous mains voltage, which is at its peak around a
  // 90-degree firing angle; at full conduction it switches at the zero crossing
  // where switching loss approaches zero. So testing at 100% measures close to
  // the BEST case and yields falsely reassuring headroom. Sweep the range with
  // the heaviest fixture and dwell at mid-range.
  //
  // This is an ADDITIONAL layer, not the only one: a thermal cutoff is bonded
  // to the TRIAC in hardware. Neither the co-processor nor stock's ESP32
  // firmware acts on temperature -- stock only reports it, confirmed by
  // disassembly -- so nothing here replaces a protection that already existed.
  //
  bool overtemp_protect = true;
  uint8_t overtemp_limit_c = 65;  // above this, command the output off
};

// Hard ceiling on the cutout, enforced in firmware and not settable from
// anywhere else. The limit is exposed to Home Assistant so it can be lowered
// without a reflash, but a safety envelope that a network peer can widen is not
// an envelope: a mistyped entry or a buggy automation could otherwise raise the
// trip past the parts' rating, or disable protection outright. So the firmware
// clamps whatever it is told. Home Assistant can make the cutout stricter,
// never weaker, and cannot switch it off at all.
static constexpr uint8_t OVERTEMP_LIMIT_MAX_C = 85;

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

  // The HA-scale brightness a wrapper should PUBLISH for the current state.
  // Use this, not current_brightness_ha(), for anything that reaches a
  // light entity.
  //
  // Never returns 0 while the output is on. ESPHome's LightCall::validate_()
  // rewrites any zero brightness into an explicit state=false, which comes back
  // through write_state() as a turn-off and puts an OFF byte on the wire -- so
  // publishing 0 for a lamp that is lit switches it off. map_to_ha() saturates
  // to 0 for any device level at or below min_brightness, which is reachable
  // three ways: a touch-plate move under the window, a target that lands exactly
  // on min, or a degenerate min>=max window (where EVERY level maps to 0). The
  // floor lives here rather than in map_to_ha() so the mapping stays a clean
  // inverse and only the publish path is constrained.
  uint8_t publish_brightness_ha() const {
    uint8_t ha = map_to_ha(current_);
    return (on_ && ha == 0) ? 1 : ha;
  }
  // Busy == "the engine still owns the wire". True during a kick/ramp AND
  // during a setpoint assert, so state reflection back to Home Assistant and
  // transition completion both wait out the (short) assert window rather than
  // publishing a value the co-processor may still be arguing with.
  bool busy() const { return mode_ != Mode::IDLE || asserting_; }

  // Same as request(), but ramp to the target over EXACTLY transition_ms instead
  // of the configured ramp_rate -- the engine-side hook for a Home Assistant
  // "transition: Ns" on a light call (see DimmerTransitionTransformer in the
  // ESPHome wrapper). Overrides ramp_on_change/ramp_on_off for this one call:
  // an EXPLICIT transition always ramps, even if those toggles are off. If a
  // kick is in play, the strike snap to kick_level is still instantaneous
  // (that's what makes low brightnesses reachable at all) -- the requested
  // duration covers the ramp after it, the same segment ramp_rate would cover.
  // transition_ms == 0 behaves exactly like request().
  void request_transition(bool on, uint8_t brightness, uint32_t transition_ms, uint32_t now_ms) {
    if (transition_ms == 0) {
      request(on, brightness, now_ms);
      return;
    }
    transition_ms_override_ = transition_ms;
    force_ramp_ = true;
    request(on, brightness, now_ms);
    force_ramp_ = false;
    // A kicked below-pivot turn-on defers its ramp to tick() (after the dwell) --
    // leave the override set so THAT start_ramp_to() call still picks it up.
    // Otherwise it's already been consumed (a ramp fired synchronously above) or
    // never applicable (a no-op / an immediate command with no ramp at all) --
    // clear it so it can't leak into a later, unrelated ramp.
    if (mode_ != Mode::KICK_PRIMING)
      transition_ms_override_ = 0;
  }

  // A request that originated at the FRONT PUSHBUTTON. Identical to request(),
  // plus it arms the setpoint assert (see DimmerParams::assert_ms) so the
  // cap-touch plate under the same finger cannot overwrite the command.
  //
  // The window is armed only if the request actually put a byte on the wire --
  // an idempotent no-op has nothing to re-assert, and replaying a byte from
  // some earlier, unrelated command would be worse than doing nothing.
  void request_local(bool on, uint8_t brightness, uint32_t now_ms) {
    uint32_t before = emit_count_;
    request(on, brightness, now_ms);
    if (p_.assert_ms == 0 || emit_count_ == before) return;
    asserting_ = true;
    assert_until_ = now_ms + p_.assert_ms;
    t_next_assert_ = now_ms + assert_period_();
  }

  // External request from HA / the light layer / the pushbutton.
  void request(bool on, uint8_t brightness, uint32_t now_ms) {
    // Over-temperature cutout: nothing may turn the output on while it is too
    // hot -- not Home Assistant, not an automation, not the wall button. Off
    // is always allowed through.
    if (overtemp_ && on && brightness != 0)
      return;
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
      // Reflection of an OUT-OF-WINDOW device level. The round-trip above is an
      // identity only for levels inside [min,max]; below min (a touch move under
      // the window) map_to_ha() saturates and publish_brightness_ha() floors at
      // 1, so the value we published does NOT map back to current_. Without this
      // second test, echoing our own published state would re-command the lamp
      // up into the window -- that is limit_correct's job, and it is off by
      // default. Compare against exactly what we publish, closing the loop.
      if (want_on && on_ && bha == publish_brightness_ha()) return;
    }
    // OFF: fade to black then off (ramp_on_off), or immediate off. The immediate
    // path preserves the brightness bits so a later on can restore the level.
    if (!on || brightness == 0) {
      if ((p_.ramp_on_off || force_ramp_) && on_ && current_ > 0) {
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
      if (p_.ramp_on_off || force_ramp_) {
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
      if (p_.ramp_on_change || force_ramp_) {
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

    // Setpoint assert. Deliberately serviced OUTSIDE the mode switch: the window
    // it exists to cover is KICK_PRIMING, the silent kick dwell, and it must also
    // survive an immediate command settling straight back to IDLE. It replays
    // last_byte_, which emit() keeps current, so a command arriving mid-window is
    // asserted rather than the one it replaced.
    if (asserting_) {
      if (mode_ == Mode::RAMPING || int32_t(now_ms - assert_until_) >= 0) {
        // A ramp already puts a byte on the wire every ~10 ms, which IS an
        // assert; replaying a stale one alongside it would only fight the ramp.
        asserting_ = false;
      } else if (int32_t(now_ms - t_next_assert_) >= 0) {
        emit(last_byte_);
        t_next_assert_ = now_ms + assert_period_();
      }
    }
  }

  // Over-temperature cutout. Fed every status frame with the co-processor's
  // reported die temperature. Above the limit the output is commanded off
  // immediately, aborting any ramp in flight, and stays refused until the
  // temperature comes back down. Nothing auto-restores: clearing the condition
  // only permits a turn-on again, it does not perform one -- coming home to
  // lights that switched themselves back on after a thermal event would hide
  // the very fault this exists to expose.
  void notify_temperature(uint8_t temp_c) {
    if (!p_.overtemp_protect) { overtemp_ = false; return; }
    if (temp_c > effective_overtemp_limit()) {
      if (!overtemp_) {
        overtemp_ = true;
        // Command off directly rather than going through request(): a ramp may
        // be mid-flight, and the cutout must not be subject to the ramp_on_off
        // fade that a normal turn-off would honour.
        mode_ = Mode::IDLE;
        pending_off_ = false;
        emit(encode_command(false, current_));
        on_ = false;
      }
    } else {
      overtemp_ = false;
    }
  }

  // True while the output is held off by the over-temperature cutout.
  bool overtemp() const { return overtemp_; }

  // The limit actually enforced: whatever was configured, clamped down to the
  // firmware ceiling. Configuring a higher value silently has no effect, which
  // is the intended failure direction.
  uint8_t effective_overtemp_limit() const {
    return p_.overtemp_limit_c > OVERTEMP_LIMIT_MAX_C ? OVERTEMP_LIMIT_MAX_C
                                                      : p_.overtemp_limit_c;
  }

  // Reconcile with an unsolicited status frame (cap-touch slider moved the level,
  // or the pushbutton toggled at the MCU's echo). Only adopt when settled, so we
  // never fight our own output. With limit_correct on, a touch value outside
  // [min,max] is pulled back to the nearest limit via a ramp -- best effort: we
  // can only react AFTER the co-processor reports it (README caveat), and while
  // correcting we ignore further reports (mode != IDLE).
  void notify_status(uint8_t b0_brightness, bool output_on, uint32_t now_ms) {
    // busy(), not mode_: a report arriving mid-assert is the touch plate winning
    // a race we are still running, so adopting it as truth would hand the plate
    // the argument the assert exists to win.
    if (busy()) return;
    current_ = b0_brightness;
    on_ = output_on;
    // The touch panel commands the co-processor directly, so during a thermal
    // event someone can physically switch the output back on without us being
    // asked. Put it back off; a cutout that a finger can override is not one.
    if (overtemp_ && output_on) {
      emit(encode_command(false, current_));
      on_ = false;
      return;
    }
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

  void emit(uint8_t byte) {
    last_byte_ = byte;
    emit_count_++;
    if (send_) send_(byte, send_ctx_);
  }

  // Never zero: a zero interval would re-emit on every tick and flood the link.
  uint32_t assert_period_() const {
    return p_.assert_interval_ms ? p_.assert_interval_ms : 1;
  }

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

  // Convert a rate (%/s) into a fixed (step, interval) cadence, quantized to
  // RAMP_MIN_PERIOD_MS. No dithering: one step size and one interval per ramp;
  // the only imprecision is integer-ms rounding of the interval (<= half a
  // period). For R <= 100 %/s a 1% step is slower than the floor, so we stretch
  // the interval; above that we widen the step and keep interval >= the floor.
  // Takes the rate explicitly (rather than always reading p_.ramp_rate) so the
  // same math serves both the configured ramp_rate and a one-shot rate derived
  // from an explicit transition duration (see start_ramp_to()).
  static void ramp_cadence_(uint16_t rate_pps, uint8_t &step, uint32_t &interval_ms) {
    uint32_t R = rate_pps;
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

  // Begin (or instantly complete) a ramp from current_ to tgt. Uses ramp_rate,
  // UNLESS transition_ms_override_ is set (an explicit Home Assistant
  // "transition: Ns" via request_transition()) -- then it derives the %/s rate
  // that spans exactly this ramp segment in that duration, clamped to the same
  // bounds as ramp_rate, and consumes (clears) the override: it applies to
  // exactly the one ramp segment it was requested for.
  // to_off: finish by sending the OFF command (fade-out) instead of an on level.
  void start_ramp_to(uint8_t tgt, uint32_t now_ms, bool to_off) {
    target_ = tgt;
    pending_off_ = to_off;
    uint32_t adist = current_ >= target_ ? uint32_t(current_ - target_)
                                         : uint32_t(target_ - current_);
    uint16_t rate = p_.ramp_rate;
    if (transition_ms_override_ != 0) {
      uint32_t r = (adist * 1000u + transition_ms_override_ / 2) / transition_ms_override_;
      if (r < RAMP_RATE_MIN) r = RAMP_RATE_MIN;
      if (r > RAMP_RATE_MAX) r = RAMP_RATE_MAX;
      rate = uint16_t(r);
      transition_ms_override_ = 0;
    }
    ramp_cadence_(rate, ramp_step_, ramp_interval_ms_);
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
  bool overtemp_ = false;         // output held off by the over-temperature cutout

  // Setpoint assert (see DimmerParams::assert_ms and tick()).
  bool asserting_ = false;
  uint32_t assert_until_ = 0;    // window deadline
  uint32_t t_next_assert_ = 0;   // next replay time
  uint8_t last_byte_ = 0;        // the byte emit() last put on the wire
  uint32_t emit_count_ = 0;      // total emits; request_local() watches it for a no-op

  // request_transition() support (see there + start_ramp_to()).
  bool force_ramp_ = false;             // ramp even if the ramp_on_*/toggle is off
  uint32_t transition_ms_override_ = 0;  // one-shot explicit duration; 0 = use ramp_rate
};

}  // namespace shelly_dimmer_core
