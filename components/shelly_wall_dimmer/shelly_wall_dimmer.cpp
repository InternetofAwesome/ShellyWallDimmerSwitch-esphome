#include "shelly_wall_dimmer.h"

#include <nvs.h>
#include <nvs_flash.h>

namespace esphome::shelly_wall_dimmer {

static const char *const TAG = "shelly_wall_dimmer";

void ShellyWallDimmer::setup() {
  this->engine_.set_send_handler(&ShellyWallDimmer::send_byte_trampoline_, this);
  this->parser_.set_stray_handler(&ShellyWallDimmer::stray_byte_trampoline_, this);

  // Proactive partition-layout guard: check the live flash geometry ONCE, up
  // front, and cache it. If it doesn't match the offsets our SH0S boot-state
  // logic was reverse-engineered against, every boot-state write (commit /
  // revert / DFU-stage / auto-commit) refuses for the rest of this boot. Running
  // it here means the ERROR banner is in the log from the start, not only when
  // the first write is attempted. On the expected stock table this logs one
  // "layout guard OK" line.
  {
    ::shelly_dimmer_core::BootState bs;
    this->boot_state_layout_ok_ = bs.layout_ok();
  }

  if (this->button_pin_ != nullptr) {
    this->button_pin_->setup();
    this->button_.isr_pin_ = this->button_pin_->to_isr();
    // RISING is the LOGICAL edge: ESP32's gpio.cpp flips the IDF edge type for an
    // inverted pin, and to_isr() carries `inverted` through to digital_read(), so
    // both this and the re-arm check below read "pressed" regardless of whether
    // the board wires the button active-low or active-high. `inverted:` in the
    // YAML stays the single polarity knob.
    this->button_pin_->attach_interrupt(&ButtonStore::intr, &this->button_,
                                        gpio::INTERRUPT_RISING_EDGE);
    // Don't act on whatever the line happened to be doing during boot.
    this->button_.pending_ = false;
    this->button_armed_ = !this->button_.isr_pin_.digital_read();
    // Otherwise the entity reads "unknown" in HA until someone presses it.
    if (this->button_binary_sensor_ != nullptr)
      this->button_binary_sensor_->publish_initial_state(false);
  }

  // Kick off with a poll so we learn the co-processor's current state fast,
  // rather than waiting for it to spontaneously stream something. tx_byte_()
  // drops this in silent/bench mode (the ESP stays mute; an adapter polls).
  this->tx_byte_(::shelly_dimmer_core::CMD_POLL);
  this->last_poll_ms_ = millis();
}

void ShellyWallDimmer::loop() {
  while (this->available()) {
    uint8_t c;
    if (!this->read_byte(&c)) break;

    ::shelly_dimmer_core::StatusFrame frame;
    if (this->parser_.feed(c, frame)) {
      this->handle_status_frame_(frame);
    }
  }

  const uint32_t now = millis();

  this->service_button_();
  this->engine_.tick(now);

  if (now - this->last_poll_ms_ >= this->update_interval_ms_) {
    this->last_poll_ms_ = now;
    this->tx_byte_(::shelly_dimmer_core::CMD_POLL);
  }

  this->maybe_autocommit_();
}

void ShellyWallDimmer::service_button_() {
  if (this->button_pin_ == nullptr) return;
  const uint32_t now = millis();

  if (this->button_.pending_) {
    this->button_.pending_ = false;
    if (this->button_armed_) {
      this->button_armed_ = false;
      this->last_press_ms_ = now;
      this->button_press_();
    }
  }

  // Re-arm only once the hold-off has passed AND the line reads inactive. The
  // hold-off alone is not enough: a press held longer than it would let the
  // RELEASE bounce latch a second edge and toggle straight back, netting to
  // nothing -- which is exactly the failure an any-edge scheme has.
  if (!this->button_armed_ && now - this->last_press_ms_ >= this->button_hold_off_ms_ &&
      !this->button_.isr_pin_.digital_read()) {
    this->button_.pending_ = false;  // discard bounce latched while disarmed
    this->button_armed_ = true;
    if (this->button_binary_sensor_ != nullptr)
      this->button_binary_sensor_->publish_state(false);
  }
}

void ShellyWallDimmer::button_press_() {
  // Tag the command that this toggle is about to produce as button-originated,
  // so it gets the setpoint assert. See ShellyWallDimmer::request().
  this->assert_arm_until_ = millis() + LOCAL_COMMAND_WINDOW_MS;

  if (this->light_state_ != nullptr) {
    // Route through the light layer rather than commanding the engine directly,
    // so Home Assistant sees the state change and the entity's own remembered
    // brightness is what gets restored on a turn-on.
    auto call = this->light_state_->toggle();
    call.set_transition_length(0);
    call.perform();
  }
  if (this->button_binary_sensor_ != nullptr)
    this->button_binary_sensor_->publish_state(true);
}

void ShellyWallDimmer::maybe_autocommit_() {
  // See AUTOCOMMIT_HEALTHY_MS in the header. Runs at most once per boot; only
  // writes flash when the currently-booted slot is still uncommitted (a fresh
  // DFU/OTA). Normal committed boots hit the early return after one cheap read.
  if (this->autocommit_done_)
    return;
  // Layout guard failed at setup(): boot-state writes are disabled this boot, so
  // don't even walk the commit path (it would only refuse in mutate_ anyway).
  if (!this->boot_state_layout_ok_)
    return;
  if (millis() < AUTOCOMMIT_HEALTHY_MS)
    return;
  this->autocommit_done_ = true;  // attempt at most once per boot

  ::shelly_dimmer_core::BootState bs;
  ::shelly_dimmer_core::BootStatePair p = bs.read();
  if (!p.ok || p.winner < 0) {
    ESP_LOGW(TAG, "auto-commit: cannot read boot state; skipping");
    return;
  }
  if (p.copy[p.winner].committed) {
    ESP_LOGD(TAG, "auto-commit: running slot already committed; nothing to do");
    return;
  }
  ESP_LOGW(TAG, "auto-commit: healthy for %us, running slot uncommitted -> committing",
           (unsigned) (AUTOCOMMIT_HEALTHY_MS / 1000));
  bool ok = bs.commit();
  ESP_LOGW(TAG, "auto-commit: %s", ok ? "OK (slot now permanent)" : "FAILED (winner intact; retries next boot)");
}

void ShellyWallDimmer::handle_status_frame_(const ::shelly_dimmer_core::StatusFrame &frame) {
  // Over-temperature FIRST, before reconciling state: above the configured
  // limit the engine commands the output off and refuses to let anything turn
  // it back on, and notify_status() below depends on that flag already being
  // set so a touch-panel turn-on during a thermal event gets undone.
  this->engine_.notify_temperature(frame.temp_c);
  const bool overtemp = this->engine_.overtemp();
  if (!this->have_published_overtemp_ || overtemp != this->last_overtemp_) {
    if (overtemp) {
      ESP_LOGE(TAG,
               "OVER-TEMPERATURE: co-processor reports %u C, limit %u C -- output "
               "switched off and held off until it cools.",
               frame.temp_c, this->engine_.effective_overtemp_limit());
    } else if (this->have_published_overtemp_) {
      ESP_LOGW(TAG, "Over-temperature cleared (%u C); the light can be switched on again.",
               frame.temp_c);
    }
    this->have_published_overtemp_ = true;
    this->last_overtemp_ = overtemp;
    if (this->overtemp_binary_sensor_ != nullptr)
      this->overtemp_binary_sensor_->publish_state(overtemp);
  }

  // Let the engine reconcile: it only adopts this as new truth when it isn't
  // mid-command (kick/ramp in flight), per the README's manual-override
  // rule -- this is what keeps us from fighting our own output.
  this->engine_.notify_status(frame.brightness, frame.output_on, millis());

  // Publish telemetry only on change -- the poll repeats the same reply every
  // ~1s and publish_state() would emit an identical state update each time
  // (the once-per-second "'Dimmer Temperature' >> 25 °C" console spam).
  if (this->temperature_sensor_ != nullptr &&
      (!this->have_published_temp_ || frame.temp_c != this->last_published_temp_)) {
    this->have_published_temp_ = true;
    this->last_published_temp_ = frame.temp_c;
    this->temperature_sensor_->publish_state(frame.temp_c);
  }

  if (this->last_frame_text_sensor_ != nullptr) {
    // Reconstruct the raw wire bytes for diagnostics. b1's only documented
    // bits are bit0 (on/off) and bit1 (unknown flag, always 0 so far); any
    // other bits aren't preserved by StatusFrame, so this is the closest
    // faithful reconstruction available from the decoded frame.
    uint8_t b1 = (frame.output_on ? 0x01 : 0x00) | (frame.flag_bit1 ? 0x02 : 0x00);
    uint8_t raw[5] = {::shelly_dimmer_core::FRAME_SOF, frame.brightness, b1, frame.temp_c,
                       ::shelly_dimmer_core::FRAME_EOF};
    char hex[16];
    format_hex_to(hex, raw, sizeof(raw));
    if (this->last_published_frame_ != hex) {
      this->last_published_frame_ = hex;
      this->last_frame_text_sensor_->publish_state(hex);
    }
  }

  // Push the engine's resulting state to HA, but only on an actual change --
  // the co-processor only streams frames when something changed anyway, but
  // our own CMD_POLL replies can repeat the same values.
  // Publish on the HA 0-100 scale: publish_brightness_ha() inverse-maps the
  // device brightness back through the min/max window so HA shows 0-100 even
  // though the wire value is stretched into [min,max]. It is deliberately NOT
  // current_brightness_ha(): that saturates to 0 at or below min_brightness,
  // and a 0 brightness published with state=on is rewritten by ESPHome into a
  // turn-off, which would switch off a lamp the co-processor reports as lit.
  this->maybe_publish_light_state_(this->engine_.publish_brightness_ha(), this->engine_.is_on());
}

void ShellyWallDimmer::maybe_publish_light_state_(uint8_t brightness_pct, bool on) {
  if (this->light_state_ == nullptr) return;

  // Don't reflect intermediate kick/ramp steps: make_call().perform() below
  // re-enters DimmerLight::write_state() (deferred to a later loop) and would
  // retarget the in-flight ramp. Only reflect settled / device-driven state
  // (engine idle). Once idle, request() treats the reflected value as a no-op.
  if (this->engine_.busy()) return;

  if (this->have_reported_ && brightness_pct == this->last_reported_brightness_ && on == this->last_reported_on_) {
    return;
  }
  this->have_reported_ = true;
  this->last_reported_brightness_ = brightness_pct;
  this->last_reported_on_ = on;

  auto call = this->light_state_->make_call();
  call.set_state(on);
  if (on) call.set_brightness(brightness_pct / 100.0f);
  // This is a state *sync* from the hardware, not a user-initiated dim --
  // apply it immediately, don't run it through the light's default fade.
  call.set_transition_length(0);
  call.perform();
}

void ShellyWallDimmer::handle_stray_byte_(uint8_t b) {
  // Boot banner is unframed ASCII: "reset!\nshelly_apt_003 mcu ver: v1.0.4".
  // Accumulate a line at a time; treat "reset!" as a co-processor-reset
  // signal, and publish the line that follows it as the MCU version.
  if (b == '\r') return;

  if (b == '\n') {
    if (this->boot_line_ == "reset!") {
      ESP_LOGW(TAG, "Co-processor reset detected");
      this->awaiting_version_line_ = true;
    } else if (this->awaiting_version_line_ && !this->boot_line_.empty()) {
      ESP_LOGI(TAG, "Co-processor boot line: %s", this->boot_line_.c_str());
      if (this->mcu_version_text_sensor_ != nullptr) {
        this->mcu_version_text_sensor_->publish_state(this->boot_line_);
      }
      this->awaiting_version_line_ = false;
    }
    this->boot_line_.clear();
    return;
  }

  // Guard against unbounded growth from line noise that never sees a '\n'.
  if (this->boot_line_.size() < 63) {
    this->boot_line_.push_back(static_cast<char>(b));
  }
}

void ShellyWallDimmer::dump_config() {
  ESP_LOGCONFIG(TAG, "Shelly Wall Dimmer:");
  const auto &params = this->engine_.params();
  ESP_LOGCONFIG(TAG, "  Kick: %s, level: %u%%, dwell: %ums", ONOFF(params.kick_enabled),
                params.kick_level, (unsigned) params.kick_dwell_ms);
  ESP_LOGCONFIG(TAG, "  Range map: %u-%u%%", params.min_brightness, params.max_brightness);
  ESP_LOGCONFIG(TAG, "  Ramp: %u%%/s (on-change:%s on/off:%s limit-correct:%s)",
                (unsigned) params.ramp_rate, ONOFF(params.ramp_on_change),
                ONOFF(params.ramp_on_off), ONOFF(params.limit_correct));
  ESP_LOGCONFIG(TAG, "  Setpoint assert: %ums every %ums (button commands only)",
                (unsigned) params.assert_ms, (unsigned) params.assert_interval_ms);
  if (this->button_pin_ != nullptr) {
    LOG_PIN("  Button pin: ", this->button_pin_);
    ESP_LOGCONFIG(TAG, "  Button hold-off: %ums", (unsigned) this->button_hold_off_ms_);
  } else {
    ESP_LOGCONFIG(TAG, "  Button: not configured");
  }
  if (this->silent_) {
    ESP_LOGCONFIG(TAG, "  UART TX: SILENT (bench mode) -- ESP will NOT drive the MCU link");
  }
  ESP_LOGCONFIG(TAG, "  Over-temp cutout: always on, limit: %u C (firmware ceiling %u C)",
                this->engine_.effective_overtemp_limit(),
                (unsigned) ::shelly_dimmer_core::OVERTEMP_LIMIT_MAX_C);
  ESP_LOGCONFIG(TAG, "  Status poll interval: %ums", (unsigned) this->update_interval_ms_);
  ESP_LOGCONFIG(TAG, "  Boot-state writes (commit/revert/DFU): %s",
                this->boot_state_layout_ok_ ? "ENABLED (layout guard OK)"
                                            : "DISABLED (partition layout guard FAILED)");
  // NVS headroom. Unlike a normal ESPHome device, this firmware arrives by OTA
  // into Shelly's partition table and inherits Shelly's `nvs` -- only 16 KB, and
  // still holding whatever stock wrote there (it is never erased on our path).
  // Every persisted setting lives in that same partition, and if nvs_open() ever
  // fails ESPHome erases the WHOLE partition to recover -- taking stock's own
  // config with it. Log the numbers so a nearly-full NVS is visible before that
  // happens rather than after.
  nvs_stats_t stats{};
  if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
    ESP_LOGCONFIG(TAG, "  NVS: %zu/%zu entries used, %zu free (%zu namespaces)", stats.used_entries,
                  stats.total_entries, stats.free_entries, stats.namespace_count);
  } else {
    ESP_LOGW(TAG, "  NVS: stats unavailable -- persisted settings may not be saving");
  }
  this->check_uart_settings(115200);
}

}  // namespace esphome::shelly_wall_dimmer
