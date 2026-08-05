import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_TYPE, ENTITY_CATEGORY_DIAGNOSTIC

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer

CONF_LAST_FRAME = "last_frame"
CONF_MCU_VERSION = "mcu_version"

# type -> setter on ShellyWallDimmer that takes the text_sensor::TextSensor*
TYPES = {
    CONF_LAST_FRAME: "set_last_frame_text_sensor",
    CONF_MCU_VERSION: "set_mcu_version_text_sensor",
}

# This platform doesn't declare its own C++ class -- like DEV_REFERENCE.md
# §5, a plain text_sensor::TextSensor* is created and handed *into* the
# parent, which publishes to it from its own loop()/handle_status_frame_().
CONFIG_SCHEMA = text_sensor.text_sensor_schema(
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend(
    {
        cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
        cv.Required(CONF_TYPE): cv.enum(TYPES, lower=True),
    }
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    setter_name = config[CONF_TYPE].enum_value
    cg.add(getattr(parent, setter_name)(var))
