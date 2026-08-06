import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import CONF_ID, CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer, shelly_wall_dimmer_ns

DimmerNumber = shelly_wall_dimmer_ns.class_(
    "DimmerNumber", number.Number, cg.Component, cg.Parented.template(ShellyWallDimmer)
)
DimmerNumberType = shelly_wall_dimmer_ns.enum("DimmerNumberType", is_class=True)

# type -> (enum value, min, max, step, default)   -- ranges/defaults per BEHAVIOR.md
# ramp_rate is a single shared rate in percent/second (min 1 so it can never be
# zero; 150 == the old 3%/20ms cadence). The engine quantizes it to a step/interval.
TYPES = {
    "kick_level": (DimmerNumberType.KICK_LEVEL, 0, 100, 1, 20),
    "kick_dwell_ms": (DimmerNumberType.KICK_DWELL_MS, 0, 2000, 10, 150),
    "min_brightness": (DimmerNumberType.MIN_BRIGHTNESS, 0, 100, 1, 1),
    "max_brightness": (DimmerNumberType.MAX_BRIGHTNESS, 0, 100, 1, 100),
    "ramp_rate": (DimmerNumberType.RAMP_RATE, 1, 1000, 1, 150),
}

CONFIG_SCHEMA = number.number_schema(
    DimmerNumber,
    entity_category=ENTITY_CATEGORY_CONFIG,
).extend(
    {
        cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
        cv.Required(CONF_TYPE): cv.enum(TYPES, lower=True),
    }
)


async def to_code(config):
    # cv.enum() returns a str-subclass (the matched key) with the mapped
    # value stashed on `.enum_value` -- NOT the mapped tuple itself, so it
    # must be unpacked from there, not from config[CONF_TYPE] directly.
    type_, min_v, max_v, step, default = config[CONF_TYPE].enum_value
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await number.register_number(var, config, min_value=min_v, max_value=max_v, step=step)
    cg.add(var.set_type(type_))
    # Publish the YAML-implied default before setup() runs so the engine
    # param and the HA-visible number agree from boot (see DimmerNumber::setup()).
    cg.add(var.publish_state(default))
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(var.set_parent(parent))
