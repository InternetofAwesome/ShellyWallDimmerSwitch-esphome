import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_TEMPERATURE,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
)

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer

CONFIG_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_CELSIUS,
    accuracy_decimals=0,
    device_class=DEVICE_CLASS_TEMPERATURE,
    state_class=STATE_CLASS_MEASUREMENT,
).extend(
    {
        cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
    }
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(parent.set_temperature_sensor(var))
