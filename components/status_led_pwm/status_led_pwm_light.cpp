#include "status_led_pwm_light.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::status_led_pwm {

static const char *const TAG = "status_led_pwm";

void StatusLedPwmLight::write_state(light::LightState *state) {
  // The light's on/off is the indicator's master enable; its brightness is the
  // blink level. We don't drive the output straight to the brightness here -- in
  // the OK state the indicator stays off; loop() owns the actual output.
  bool on;
  state->current_values_as_binary(&on);
  this->enabled_ = on;
  if (on)
    state->current_values_as_brightness(&this->level_);
  this->refresh_();
}

void StatusLedPwmLight::refresh_() {
  float out = 0.0f;
  if (this->enabled_) {
    uint8_t st = App.get_app_state() & STATUS_LED_MASK;
    // Same cadence as the built-in status_led: fast on error, slow on warning.
    // Healthy (OK) -> stays off; this is an error indicator, not a steady light.
    if ((st & STATUS_LED_ERROR) != 0u)
      out = (millis() % 250u < 150u) ? this->level_ : 0.0f;
    else if ((st & STATUS_LED_WARNING) != 0u)
      out = (millis() % 1500u < 250u) ? this->level_ : 0.0f;
  }
  this->drive_(out);
}

void StatusLedPwmLight::loop() { this->refresh_(); }

void StatusLedPwmLight::drive_(float level) {
  if (level != this->last_out_) {
    this->last_out_ = level;
    if (this->output_ != nullptr)
      this->output_->set_level(level);
  }
}

void StatusLedPwmLight::dump_config() {
  ESP_LOGCONFIG(TAG, "Status LED (PWM): off when healthy, blinks on warning/error at set brightness");
}

}  // namespace esphome::status_led_pwm
