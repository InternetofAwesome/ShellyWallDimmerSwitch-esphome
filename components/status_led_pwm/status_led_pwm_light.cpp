#include "status_led_pwm_light.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::status_led_pwm {

static const char *const TAG = "status_led_pwm";

void StatusLedPwmLight::apply_light_level_() {
  if (this->output_ == nullptr || this->lightstate_ == nullptr)
    return;
  float bright;
  this->lightstate_->current_values_as_brightness(&bright);
  this->output_->set_level(bright);
}

void StatusLedPwmLight::write_state(light::LightState *state) {
  // In the OK state, honor the light (its brightness, or 0 when off). During a
  // warning/error the loop() owns the LED (blinking); don't fight it here --
  // loop() restores the configured level once the condition clears. This mirrors
  // the built-in status_led's write_state guard.
  if ((App.get_app_state() & (STATUS_LED_ERROR | STATUS_LED_WARNING)) == 0u)
    this->apply_light_level_();
}

void StatusLedPwmLight::loop() {
  if (this->output_ == nullptr)
    return;
  uint8_t new_state = App.get_app_state() & STATUS_LED_MASK;
  // Same cadence as the built-in status_led: fast blink on error, slow on
  // warning. Blink at full brightness for visibility, then hand control back to
  // the configured brightness (apply_light_level_) once the condition clears.
  if ((new_state & STATUS_LED_ERROR) != 0u) {
    this->output_->set_level(millis() % 250u < 150u ? 1.0f : 0.0f);
    this->last_app_state_ = new_state;
  } else if ((new_state & STATUS_LED_WARNING) != 0u) {
    this->output_->set_level(millis() % 1500u < 250u ? 1.0f : 0.0f);
    this->last_app_state_ = new_state;
  } else if (new_state != this->last_app_state_) {
    this->apply_light_level_();
    this->last_app_state_ = new_state;
  }
}

void StatusLedPwmLight::dump_config() {
  ESP_LOGCONFIG(TAG, "Status LED (PWM, dimmable): blinks on warning/error, else shows brightness");
}

}  // namespace esphome::status_led_pwm
