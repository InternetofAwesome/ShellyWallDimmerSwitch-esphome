import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer, shelly_wall_dimmer_ns

DimmerSwitch = shelly_wall_dimmer_ns.class_(
    "DimmerSwitch", switch.Switch, cg.Component, cg.Parented.template(ShellyWallDimmer)
)
DimmerSwitchType = shelly_wall_dimmer_ns.enum("DimmerSwitchType", is_class=True)

# type -> (enum value, default state). Defaults match DimmerParams: kick and
# ramp-on-change on; the fade and limit-correction gates off (opt-in).
TYPES = {
    "kick_enabled": (DimmerSwitchType.KICK_ENABLED, True),
    "ramp_on_change": (DimmerSwitchType.RAMP_ON_CHANGE, True),
    "ramp_on_off": (DimmerSwitchType.RAMP_ON_OFF, False),
    "limit_correct": (DimmerSwitchType.LIMIT_CORRECT, False),
}

CONFIG_SCHEMA = switch.switch_schema(
    DimmerSwitch,
    entity_category=ENTITY_CATEGORY_CONFIG,
).extend(
    {
        cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
        cv.Required(CONF_TYPE): cv.enum(TYPES, lower=True),
    }
)


async def to_code(config):
    # cv.enum() returns the matched key with the mapped (enum, default) on
    # .enum_value -- see number.py for the same idiom.
    type_, default = config[CONF_TYPE].enum_value
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)
    cg.add(var.set_type(type_))
    # Publish the YAML-implied default before setup() runs so DimmerSwitch::setup()
    # primes the engine from a real value.
    cg.add(var.publish_state(default))
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(var.set_parent(parent))
