#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/number/number.h"
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

// ---- one C++ member field per live-tunable DimmerParams field -------------
enum class DimmerNumberType {
  KICK_LEVEL,
  KICK_DWELL_MS,
  MIN_BRIGHTNESS,
  MAX_BRIGHTNESS,
  RAMP_RATE,
};

// One typed boolean switch per engine toggle (kick + the three ramp gates).
enum class DimmerSwitchType {
  KICK_ENABLED,
  RAMP_ON_CHANGE,
  RAMP_ON_OFF,
  LIMIT_CORRECT,
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

  // ---- entity plumbing (called from each platform's to_code()) ----
  void set_light_state(light::LightState *state) { this->light_state_ = state; }
  void set_temperature_sensor(sensor::Sensor *s) { this->temperature_sensor_ = s; }
  void set_last_frame_text_sensor(text_sensor::TextSensor *s) { this->last_frame_text_sensor_ = s; }
  void set_mcu_version_text_sensor(text_sensor::TextSensor *s) { this->mcu_version_text_sensor_ = s; }

  // ---- called by DimmerLight::write_state() ----
  // brightness_pct: 0-100. Routes through the engine's kick/ramp/clamp state
  // machine rather than sending the byte directly.
  void request(bool on, uint8_t brightness_pct) { this->engine_.request(on, brightness_pct, millis()); }

  // ---- the live kick/ramp/clamp engine; number.py / switch.py entities
  // write straight into engine_.params() from their control() ----
  ::shelly_dimmer_core::DimmerEngine &get_engine() { return this->engine_; }

  // ---- diagnostics: arbitrary raw byte out, for sweeping the unused
  // command space (PROTOCOL.md: 0x65-0x7F / 0xE5-0xFE) from HA, no reflash ----
  void send_raw(uint8_t b) { this->write_byte(b); }

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
    reinterpret_cast<ShellyWallDimmer *>(ctx)->write_byte(b);
  }
  static void stray_byte_trampoline_(uint8_t b, void *ctx) {
    reinterpret_cast<ShellyWallDimmer *>(ctx)->handle_stray_byte_(b);
  }

  void handle_status_frame_(const ::shelly_dimmer_core::StatusFrame &frame);
  void handle_stray_byte_(uint8_t b);
  void maybe_publish_light_state_(uint8_t brightness_pct, bool on);

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
  text_sensor::TextSensor *last_frame_text_sensor_{nullptr};
  text_sensor::TextSensor *mcu_version_text_sensor_{nullptr};

  uint32_t update_interval_ms_{1000};
  uint32_t last_poll_ms_{0};

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
  bool have_published_temp_{false};
  float last_published_temp_{0.0f};
  std::string last_published_frame_;

  // Unframed boot-banner line buffer: "reset!\nshelly_apt_003 mcu ver: v1.0.4"
  std::string boot_line_;
  bool awaiting_version_line_{false};
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

  void setup() override {
    // Prime the engine's param from the entity's boot-time value (the YAML
    // default, published explicitly by number.py's to_code()) so the
    // kick/ramp state machine and the HA-visible number agree from t=0.
    this->control(this->state);
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
    }
    this->publish_state(value);
  }

  DimmerNumberType type_{DimmerNumberType::KICK_LEVEL};
};

// ---- engine toggle switches (kick + the three ramp gates) ------------------
class DimmerSwitch : public switch_::Switch, public Component, public Parented<ShellyWallDimmer> {
 public:
  void set_type(DimmerSwitchType type) { this->type_ = type; }

  void setup() override {
    // Prime the engine from the switch's boot-time (restored or YAML
    // default) state, mirroring DimmerNumber::setup().
    this->write_state(this->state);
  }

 protected:
  void write_state(bool state) override {
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
    }
    this->publish_state(state);
  }

  DimmerSwitchType type_{DimmerSwitchType::KICK_ENABLED};
};

}  // namespace esphome::shelly_wall_dimmer
