#pragma once

#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/output/float_output.h"

namespace esphome::status_led_pwm {

// A dimmable status/error indicator over a PWM (FloatOutput). It hooks the SAME
// App.get_app_state() mechanism as ESPHome's built-in status_led -- fast blink on
// ERROR, slow blink on WARNING (e.g. Wi-Fi down / connecting / AP mode) -- but
// drives a PWM channel, so the blink level is the light's configurable brightness.
//
// It is an INDICATOR, not a general light: in the healthy (OK) state it stays
// OFF; it only lights to signal a problem. The HA light's on/off is a master
// enable (off = indicator disabled); its brightness sets the blink level.
class StatusLedPwmLight : public light::LightOutput, public Component {
 public:
  void set_output(output::FloatOutput *output) { this->output_ = output; }

  light::LightTraits get_traits() override {
    auto traits = light::LightTraits();
    traits.set_supported_color_modes({light::ColorMode::BRIGHTNESS});
    return traits;
  }

  void write_state(light::LightState *state) override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

 protected:
  // Recompute the output from the current app state + enable/level and push it.
  void refresh_();
  // Set the PWM only on change (avoid re-writing every loop).
  void drive_(float level);

  output::FloatOutput *output_{nullptr};
  bool enabled_{false};   // light on == indicator enabled
  float level_{1.0f};     // blink brightness while enabled
  float last_out_{-1.0f};
};

}  // namespace esphome::status_led_pwm
