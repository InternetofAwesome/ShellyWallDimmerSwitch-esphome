import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_TYPE,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer

# Only one type today. Kept as a `type:` key so additional diagnostics can be
# added later without changing existing configs.
TYPES = ["overtemp"]

CONFIG_SCHEMA = binary_sensor.binary_sensor_schema(
    device_class=DEVICE_CLASS_PROBLEM,
    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
).extend(
    {
        cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
        cv.Optional(CONF_TYPE, default="overtemp"): cv.one_of(*TYPES, lower=True),
    }
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(parent.set_overtemp_binary_sensor(var))
