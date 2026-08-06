#pragma once

#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/output/float_output.h"

namespace esphome::status_led_pwm {

// A brightness-capable status LED. It hooks the SAME app-state mechanism as
// ESPHome's built-in status_led (App.get_app_state(): blink fast on ERROR, slow
// on WARNING) but drives a FloatOutput (PWM) instead of a binary pin -- so its
// steady "on" level is the light's configurable brightness, while it still shows
// AP/connecting/warning/error by blinking. A drop-in dimmable status_led.
class StatusLedPwmLight : public light::LightOutput, public Component {
 public:
  void set_output(output::FloatOutput *output) { this->output_ = output; }

  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return traits;
  }

  void setup_state(light::LightState *state) override {
    this->lightstate_ = state;
    this->write_state(state);
  }
  void write_state(light::LightState *state) override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  // Drive the PWM to the light's current brightness (0 when off). Gamma and
  // on/off are already folded into current_values_as_brightness, matching how
  // the monochromatic light drives its output.
  void apply_light_level_();

  output::FloatOutput *output_{nullptr};
  light::LightState *lightstate_{nullptr};
  uint8_t last_app_state_{0xFF};
};

}  // namespace esphome::status_led_pwm
