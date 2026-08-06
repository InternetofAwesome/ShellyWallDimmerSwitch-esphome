import esphome.codegen as cg
from esphome.components import light, output
import esphome.config_validation as cv
from esphome.const import CONF_OUTPUT, CONF_OUTPUT_ID

from . import status_led_pwm_ns

AUTO_LOAD = ["output"]

StatusLedPwmLight = status_led_pwm_ns.class_(
    "StatusLedPwmLight", light.LightOutput, cg.Component
)

# Same shape as the monochromatic light (a brightness-only light over a
# FloatOutput), but the C++ side also blinks the output on app warning/error --
# see status_led_pwm_light.cpp. So it's a drop-in dimmable replacement for the
# stock `status_led` on a PWM (ledc) channel.
CONFIG_SCHEMA = light.BRIGHTNESS_ONLY_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(StatusLedPwmLight),
        cv.Required(CONF_OUTPUT): cv.use_id(output.FloatOutput),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await cg.register_component(var, config)
    await light.register_light(var, config)
    out = await cg.get_variable(config[CONF_OUTPUT])
    cg.add(var.set_output(out))
