#pragma once

#include <memory>
#include <string>

#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/number/number.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esp_partition.h"  // Option B: read raw otadata (boot-state) for RE

// Framework-agnostic core (command codec, frame parser, kick/ramp/clamp
// state machine). Do not modify these; only instantiate + drive them below.
// NOTE: these live in the *global* `shelly_wall_dimmer` namespace, distinct
// from `esphome::shelly_wall_dimmer` below (same leaf name, different
// enclosing scope) -- always refer to them with a leading `::` to avoid the
// two namespaces shadowing each other.
#include "dimmer_protocol.h"
#include "dimmer_engine.h"
#include "boot_state.h"  // SH0S boot-state read-modify-write (commit/revert)

namespace esphome::shelly_wall_dimmer {

// ---- one entry per live-tunable knob -------------------------------------
// Most target a DimmerParams field; BUTTON_HOLD_OFF_MS targets the wrapper
// instead (the button lives here, not in the framework-agnostic engine).
enum class DimmerNumberType {
  KICK_LEVEL,
  KICK_DWELL_MS,
  MIN_BRIGHTNESS,
  MAX_BRIGHTNESS,
  RAMP_RATE,
  OVERTEMP_LIMIT,
  BUTTON_HOLD_OFF_MS,
  ASSERT_MS,
  ASSERT_INTERVAL_MS,
};

// One typed boolean switch per engine toggle (kick + the three ramp gates).
enum class DimmerSwitchType {
  KICK_ENABLED,
  RAMP_ON_CHANGE,
  RAMP_ON_OFF,
  LIMIT_CORRECT,
  OVERTEMP_PROTECT,
  // Not an engine parameter: permission to let an OTA erase the slot that still
  // holds stock firmware. Persisted, set-once-and-forget. See dfu_wrap.cpp.
  ALLOW_OVERWRITE_STOCK,
};

// ---- front pushbutton: ISR edge LATCH --------------------------------------
// ESPHome's own `binary_sensor: platform: gpio` cannot serve here, and turning
// its debounce filter down does not help. Its ISR stores the pin's CURRENT
// LEVEL and loop() publishes whatever level it finds; a contact that opens and
// closes between two loop iterations round-trips to the old level and is lost
// entirely, interrupts or not. A wall switch that ignores a quick tap is the
// bug this exists to fix, so we latch the EDGE instead of sampling the level:
// once pending_ is set, the press is remembered no matter how brief it was.
//
// No vtables, no allocation -- this is touched from an ISR. Modelled on
// ESPHome's GPIOBinarySensorStore.
class ButtonStore {
 public:
  static void IRAM_ATTR intr(ButtonStore *arg) { arg->pending_ = true; }

  ISRInternalGPIOPin isr_pin_;
  volatile bool pending_{false};
};

class ShellyWallDimmer : public Component, public uart::UARTDevice {
 public:
  float get_setup_priority() const override { return setup_priority::LATE; }

  void setup() override;
  void loop() override;
  void dump_config() override;

  // How often (ms) to send an unsolicited CMD_POLL (0xFF) status request so
  // temperature/state stay fresh even with no touch/HA activity.
  void set_update_interval(uint32_t update_interval_ms) { this->update_interval_ms_ = update_interval_ms; }

  // Bench/sweep mode: the ESP must be ELECTRICALLY MUTE on the MCU link so an
  // external USB-UART adapter can own it without bus contention. The device
  // YAML pairs this with an rx-only `uart:` (tx_pin omitted), which leaves the
  // TX GPIO unconfigured / high-Z -- see __init__.py's FINAL_VALIDATE, which
  // drops require_tx when silent. RX/parsing stays live so the ESP still logs
  // the MCU's frames as a cross-check.
  void set_silent(bool silent) { this->silent_ = silent; }

  // ---- entity plumbing (called from each platform's to_code()) ----
  void set_light_state(light::LightState *state) { this->light_state_ = state; }
  void set_temperature_sensor(sensor::Sensor *s) { this->temperature_sensor_ = s; }
  void set_overtemp_binary_sensor(binary_sensor::BinarySensor *s) { this->overtemp_binary_sensor_ = s; }
  void set_button_binary_sensor(binary_sensor::BinarySensor *s) { this->button_binary_sensor_ = s; }
  void set_last_frame_text_sensor(text_sensor::TextSensor *s) { this->last_frame_text_sensor_ = s; }
  void set_mcu_version_text_sensor(text_sensor::TextSensor *s) { this->mcu_version_text_sensor_ = s; }

  // Front pushbutton ("key" net, GPIO4 on stock). Optional: with no pin
  // configured none of the button code runs at all.
  void set_button_pin(InternalGPIOPin *pin) { this->button_pin_ = pin; }

  // Minimum gap between two accepted presses. Also the window during which
  // release bounce is discarded -- see loop().
  void set_button_hold_off_ms(uint32_t ms) { this->button_hold_off_ms_ = ms; }

  // ---- called by DimmerLight::write_state() ----
  // brightness_pct: 0-100. Routes through the engine's kick/ramp/clamp state
  // machine rather than sending the byte directly.
  //
  // A press arms a short DEADLINE rather than setting a sticky "local" flag,
  // because LightState::perform() defers write_state() by a loop iteration --
  // there is no call we can tag directly. A deadline that expires on its own
  // cannot leak into an unrelated Home Assistant command later; a flag could.
  void request(bool on, uint8_t brightness_pct) {
    const uint32_t now = millis();
    if (this->assert_arm_until_ != 0 && int32_t(now - this->assert_arm_until_) < 0) {
      this->assert_arm_until_ = 0;  // one command per press
      this->engine_.request_local(on, brightness_pct, now);
    } else {
      this->engine_.request(on, brightness_pct, now);
    }
  }

  // ---- called by DimmerTransitionTransformer::start() ----
  // Same as request(), but ramps to the target over exactly transition_ms -- the
  // hook for a Home Assistant "transition: Ns" on a light call. See
  // DimmerEngine::request_transition() for the cadence math.
  void request_transition(bool on, uint8_t brightness_pct, uint32_t transition_ms) {
    this->engine_.request_transition(on, brightness_pct, transition_ms, millis());
  }

  // ---- the live kick/ramp/clamp engine; number.py / switch.py entities
  // write straight into engine_.params() from their control() ----
  ::shelly_dimmer_core::DimmerEngine &get_engine() { return this->engine_; }

  // ---- diagnostics: arbitrary raw byte out, for sweeping the unused
  // command space (PROTOCOL.md: 0x65-0x7F / 0xE5-0xFE) from HA, no reflash ----
  void send_raw(uint8_t b) { this->tx_byte_(b); }

  // Diagnostic for Option B (boot-state RE): dump the raw otadata partition so
  // we can derive the record layout + checksum from real bytes in a known
  // state, then implement commit/revert as a read-modify-write. Logs the head
  // of each of the two BS copies (at otadata 0x0 and 0x1000).
  void dump_boot_state() {
    const esp_partition_t *ota = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, nullptr);
    if (ota == nullptr) { ESP_LOGW("shelly_wall_dimmer", "no otadata partition"); return; }
    // Full SH0S record is ~512 B; control fields (committed/ba at +0x1d0, the
    // marker word at +0x1fc) live past the first 0x140 we used to grab. Read
    // 0x240 so those land in the dump. See CLAUDE.md "SH0S BOOT-STATE RECORD".
    uint8_t buf[0x240];
    for (uint32_t bs : {(uint32_t) 0x000, (uint32_t) 0x1000}) {
      if (esp_partition_read(ota, bs, buf, sizeof(buf)) != ESP_OK) {
        ESP_LOGW("shelly_wall_dimmer", "otadata read failed"); return;
      }
      ESP_LOGI("shelly_wall_dimmer", "=== BS @ otadata+0x%04x ===", (unsigned) bs);
      for (uint32_t off = 0; off < sizeof(buf); off += 16) {
        char line[40];
        for (int i = 0; i < 16; i++) snprintf(line + i * 2, 3, "%02x", buf[off + i]);
        ESP_LOGI("shelly_wall_dimmer", "  +%03x %s", (unsigned) off, line);
      }
    }
  }

  // ---- SH0S boot-state helpers (decoded view + the two mutations) ----------
  // These build on the code-confirmed model in CLAUDE.md ("SH0S BOOT-STATE
  // RECORD") via ::shelly_dimmer_core::BootState. log_ is read-only and safe to
  // press anytime; commit_/revert_ each perform ONE guarded flash write (target
  // = the non-winning otadata copy only) and MUST NOT be pressed until the live
  // dump has confirmed the model on this unit. No boot-time/auto write exists --
  // a human triggers these via the diagnostic buttons only.

  // Read + decode both otadata copies and log the decoded view (winner marked).
  void log_boot_state() {
    ::shelly_dimmer_core::BootState bs;
    ::shelly_dimmer_core::BootStatePair p = bs.read();
    bs.log_state(p);
  }

  // Mark the currently-booted (winning) slot committed. One-shot flash write to
  // the non-winning copy; winner stays intact as fallback until read-back OK.
  void commit_boot_state() {
    ::shelly_dimmer_core::BootState bs;
    bool ok = bs.commit();
    ESP_LOGW("shelly_wall_dimmer", "commit_boot_state() -> %s",
             ok ? "OK (non-winning copy now wins, committed)" : "FAILED (no change; winner intact)");
  }

  // Hand control back to the stock slot (swap active<->revert + commit). One-shot
  // flash write to the non-winning copy; winner stays intact until read-back OK.
  void revert_boot_state_to_stock() {
    ::shelly_dimmer_core::BootState bs;
    bool ok = bs.revert_to_stock();
    ESP_LOGW("shelly_wall_dimmer", "revert_boot_state_to_stock() -> %s",
             ok ? "OK (next boot runs the reverted slot)" : "FAILED (no change; winner intact)");
  }

 protected:
  // Trampolines: DimmerEngine/FrameParser take plain C function pointers +
  // a void* context (they're framework-agnostic, no std::function), so we
  // bounce back into member functions here.
  static void send_byte_trampoline_(uint8_t b, void *ctx) {
    reinterpret_cast<ShellyWallDimmer *>(ctx)->tx_byte_(b);
  }

  // Single outbound choke point. In silent/bench mode every ESP-originated byte
  // (poll, engine command, raw diagnostic) is dropped here so the ESP never
  // drives the link; an external adapter owns it. RX is unaffected.
  void tx_byte_(uint8_t b) {
    if (this->silent_) return;
    this->write_byte(b);
  }
  static void stray_byte_trampoline_(uint8_t b, void *ctx) {
    reinterpret_cast<ShellyWallDimmer *>(ctx)->handle_stray_byte_(b);
  }

  void handle_status_frame_(const ::shelly_dimmer_core::StatusFrame &frame);
  void handle_stray_byte_(uint8_t b);
  void maybe_publish_light_state_(uint8_t brightness_pct, bool on);

  // Drain the ISR latch and, if a press is due, act on it. Called from loop().
  void service_button_();
  void button_press_();

  // How long a press stays "local" for request()'s benefit. Generous because it
  // only has to outlive LightState::perform()'s one-loop deferral, and it is
  // consumed by the first request() that sees it either way.
  static constexpr uint32_t LOCAL_COMMAND_WINDOW_MS = 200;

  // Auto-commit-on-healthy: once we've run this long without a crash/reboot,
  // treat the boot as healthy and (if still uncommitted, e.g. just DFU'd) make
  // the slot permanent so the bootloader stops the ba countdown. A crash-loop
  // never reaches this uptime, so a bad image auto-reverts instead. One-shot.
  static constexpr uint32_t AUTOCOMMIT_HEALTHY_MS = 30000;
  void maybe_autocommit_();

  ::shelly_dimmer_core::DimmerEngine engine_;
  ::shelly_dimmer_core::FrameParser parser_;

  light::LightState *light_state_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
  binary_sensor::BinarySensor *overtemp_binary_sensor_{nullptr};
  binary_sensor::BinarySensor *button_binary_sensor_{nullptr};
  text_sensor::TextSensor *last_frame_text_sensor_{nullptr};
  text_sensor::TextSensor *mcu_version_text_sensor_{nullptr};

  uint32_t update_interval_ms_{1000};
  uint32_t last_poll_ms_{0};

  // ---- front pushbutton ----------------------------------------------------
  InternalGPIOPin *button_pin_{nullptr};
  ButtonStore button_{};
  uint32_t button_hold_off_ms_{100};
  uint32_t last_press_ms_{0};
  // Leading-edge arming: a press fires the instant it is latched, then the
  // button is disarmed until the hold-off has passed AND the line has gone
  // inactive again. One toggle per press however long it is held.
  bool button_armed_{true};
  // Deadline marking "the next request() came from the button" -- see request().
  uint32_t assert_arm_until_{0};

  // Bench/sweep mode -- see set_silent(). When true, tx_byte_() drops every
  // outbound byte so the ESP is electrically mute on the MCU link.
  bool silent_{false};

  // Edge-detect cache so we only push a light update to HA on genuine
  // device-side change (manual override / our own settled command), not on
  // every single status frame.
  bool have_reported_{false};
  uint8_t last_reported_brightness_{0};
  bool last_reported_on_{false};

  // One-shot latch for maybe_autocommit_() so it runs at most once per boot.
  bool autocommit_done_{false};

  // Cached result of the setup()-time partition-layout guard. False disables
  // every boot-state write (BootState::mutate_ enforces the same check, but this
  // lets us skip the auto-commit path entirely and report status in dump_config).
  bool boot_state_layout_ok_{false};

  // Telemetry dedup: the co-processor reply is polled every ~1s and usually
  // repeats byte-for-byte (same temp, same frame). publish_state() emits a
  // state update to HA / the log stream on EVERY call, so republishing an
  // unchanged value spams once per second. Only publish on an actual change.
  // Edge-detect for the over-temperature cutout, so the trip is logged and
  // published once rather than on every poll while it stays hot.
  bool have_published_overtemp_{false};
  bool last_overtemp_{false};

  bool have_published_temp_{false};
  float last_published_temp_{0.0f};
  std::string last_published_frame_;

  // Unframed boot-banner line buffer: "reset!\nshelly_apt_003 mcu ver: v1.0.4"
  std::string boot_line_;
  bool awaiting_version_line_{false};
};

// ---- HA "transition: Ns" support --------------------------------------------
// ESPHome's light core owns EXPLICIT transitions: create_default_transition()
// is called once per light call that carries one, and the returned transformer's
// apply() is called every loop tick, interpolating brightness itself and (if it
// returns a value) driving write_state() with each intermediate step. That would
// fight our own kick/ramp engine, which owns a single, quantized, non-dithered
// cadence of its own. (default_transition_length: 0s in the example only sets
// the IMPLICIT default when no transition is given -- an explicit one still
// reaches here regardless of that setting.)
//
// So instead of letting the base transformer interpolate, this hands the
// requested duration straight to DimmerEngine::request_transition(), which
// ramps using the SAME cadence machinery as ramp_rate, just at a rate derived
// from this call's length. apply() returns no color values -- per LightState's
// own contract ("if the transition has written directly to the output,
// current_values is outdated, so update it"), that's the documented way for a
// transformer to drive the hardware itself; write_state() is then never called
// during the transition, so nothing double-drives the UART.
//
// One narrow consequence of driving the hardware directly here: start() reads
// the RAW (pre-gamma) target brightness, whereas a plain write_state() call
// gamma-corrects via current_values_as_brightness(). Identical in practice --
// this component already requires gamma_correct: 0 (the co-processor has its
// own curve) -- but would diverge if that were ever changed.
class DimmerTransitionTransformer : public light::LightTransformer {
 public:
  explicit DimmerTransitionTransformer(ShellyWallDimmer *parent) : parent_(parent) {}

  void start() override {
    bool on = this->target_values_.is_on();
    auto brightness_pct = static_cast<uint8_t>(this->target_values_.get_brightness() * 100.0f + 0.5f);
    this->parent_->request_transition(on, brightness_pct, this->length_);
  }

  optional<light::LightColorValues> apply() override { return {}; }

  // Finished when our OWN ramp settles, not purely by elapsed wall time -- the
  // quantized cadence can land a little before/after the nominal length.
  bool is_finished() override { return !this->parent_->get_engine().busy(); }

 protected:
  ShellyWallDimmer *parent_;
};

// ---- dimmable light, split from the UART owner per DEV_REFERENCE.md §3 ---
class DimmerLight : public light::LightOutput, public Parented<ShellyWallDimmer> {
 public:
  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return traits;
  }

  // Captures the LightState so the parent can push device-side state
  // changes (cap-touch slider moves, kick/ramp completion) back to HA.
  void setup_state(light::LightState *state) override { this->parent_->set_light_state(state); }

  std::unique_ptr<light::LightTransformer> create_default_transition() override {
    return std::make_unique<DimmerTransitionTransformer>(this->parent_);
  }

  void write_state(light::LightState *state) override {
    bool on;
    float brightness;  // 0.0 - 1.0
    state->current_values_as_binary(&on);
    state->current_values_as_brightness(&brightness);
    auto brightness_pct = static_cast<uint8_t>(brightness * 100.0f + 0.5f);
    // Diagnostic: log every value ESPHome hands us. A trailing `on=0` here
    // means the transition/light layer is issuing the off; its absence means
    // the off originates in the engine/reconciliation path.
    ESP_LOGD("shelly_wall_dimmer", "write_state: on=%d brightness_pct=%u", on, brightness_pct);
    this->parent_->request(on, brightness_pct);
  }
};

// ---- live-tunable number entities, one per DimmerParams field ------------
class DimmerNumber : public number::Number, public Component, public Parented<ShellyWallDimmer> {
 public:
  void set_type(DimmerNumberType type) { this->type_ = type; }
  // First-boot fallback: the per-type default from number.py's TYPES table.
  void set_initial_value(float value) { this->initial_value_ = value; }

  void setup() override {
    // These knobs are tuned once and expected to stay tuned, so the last value
    // set from HA lives in NVS and survives reboots AND firmware updates (an
    // OTA rewrites only the app partition). The YAML default applies only when
    // nothing is stored yet -- i.e. a first flash or an erased NVS.
    this->pref_ = this->make_entity_preference<float>();
    float value = this->initial_value_;
    float restored;
    if (this->pref_.load(&restored) && restored >= this->traits.get_min_value() &&
        restored <= this->traits.get_max_value()) {
      // Range-checked: a stored value from an older build whose TYPES range has
      // since shrunk would otherwise push an out-of-range param into the engine.
      value = restored;
    }
    // Prime the engine's param from that value and publish it, so the kick/ramp
    // state machine and the HA-visible number agree from t=0.
    this->control(value);
  }

 protected:
  void control(float value) override {
    auto &params = this->parent_->get_engine().params();
    switch (this->type_) {
      case DimmerNumberType::KICK_LEVEL:
        params.kick_level = static_cast<uint8_t>(value);
        break;
      case DimmerNumberType::KICK_DWELL_MS:
        params.kick_dwell_ms = static_cast<uint32_t>(value);
        break;
      case DimmerNumberType::MIN_BRIGHTNESS:
        params.min_brightness = static_cast<uint8_t>(value);
        break;
      case DimmerNumberType::MAX_BRIGHTNESS:
        params.max_brightness = static_cast<uint8_t>(value);
        break;
      case DimmerNumberType::RAMP_RATE:
        params.ramp_rate = static_cast<uint16_t>(value);
        break;
      case DimmerNumberType::OVERTEMP_LIMIT:
        params.overtemp_limit_c = static_cast<uint8_t>(value);
        break;
      case DimmerNumberType::ASSERT_MS:
        params.assert_ms = static_cast<uint32_t>(value);
        break;
      case DimmerNumberType::ASSERT_INTERVAL_MS:
        params.assert_interval_ms = static_cast<uint32_t>(value);
        break;
      case DimmerNumberType::BUTTON_HOLD_OFF_MS:
        // The only knob here that is NOT a DimmerParams field: the button lives
        // in the wrapper, since the engine has no GPIOs.
        this->parent_->set_button_hold_off_ms(static_cast<uint32_t>(value));
        break;
    }
    this->publish_state(value);
    // Unchanged values are dropped by the NVS backend, so re-saving the boot
    // value from setup() costs no flash wear.
    this->pref_.save(&value);
  }

  DimmerNumberType type_{DimmerNumberType::KICK_LEVEL};
  float initial_value_{0.0f};
  ESPPreferenceObject pref_;
};

// ---- engine toggle switches (kick + the three ramp gates) ------------------
class DimmerSwitch : public switch_::Switch, public Component, public Parented<ShellyWallDimmer> {
 public:
  void set_type(DimmerSwitchType type) { this->type_ = type; }

  void setup() override {
    // Every switch here restores from NVS: the engine gates are tuning knobs
    // that must survive an update, and allow_overwrite_stock is a standing
    // permission. switch.py picks each one's RESTORE_DEFAULT_{ON,OFF} so an
    // unwritten preference falls back to the documented default. Writing the
    // state also primes the engine (see write_state below).
    this->write_state(this->get_initial_state_with_restore_mode().value_or(false));
  }

 protected:
  void write_state(bool state) override {
    if (this->type_ == DimmerSwitchType::ALLOW_OVERWRITE_STOCK) {
      ::shelly_dimmer_core::g_allow_overwrite_stock = state;
      if (state) {
        ESP_LOGW("shelly_wall_dimmer",
                 "Allow Overwrite Stock ENABLED: the next OTA may erase the slot holding "
                 "stock firmware. This permanently removes the ability to revert to stock.");
      } else {
        ESP_LOGI("shelly_wall_dimmer", "Allow Overwrite Stock disabled: stock slot protected");
      }
      this->publish_state(state);
      return;
    }
    auto &params = this->parent_->get_engine().params();
    switch (this->type_) {
      case DimmerSwitchType::KICK_ENABLED:
        params.kick_enabled = state;
        break;
      case DimmerSwitchType::RAMP_ON_CHANGE:
        params.ramp_on_change = state;
        break;
      case DimmerSwitchType::RAMP_ON_OFF:
        params.ramp_on_off = state;
        break;
      case DimmerSwitchType::LIMIT_CORRECT:
        params.limit_correct = state;
        break;
      case DimmerSwitchType::OVERTEMP_PROTECT:
        params.overtemp_protect = state;
        if (!state)
          ESP_LOGW("shelly_wall_dimmer",
                   "Over-temperature cutout DISABLED. The output will no longer be switched "
                   "off if the co-processor reports an excessive temperature.");
        break;
      case DimmerSwitchType::ALLOW_OVERWRITE_STOCK:
        break;  // handled by the early return above; not an engine parameter
    }
    this->publish_state(state);
  }

  DimmerSwitchType type_{DimmerSwitchType::KICK_ENABLED};
};

}  // namespace esphome::shelly_wall_dimmer
