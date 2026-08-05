import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, ENTITY_CATEGORY_CONFIG

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer, shelly_wall_dimmer_ns

DimmerKickSwitch = shelly_wall_dimmer_ns.class_(
    "DimmerKickSwitch", switch.Switch, cg.Component, cg.Parented.template(ShellyWallDimmer)
)

CONFIG_SCHEMA = switch.switch_schema(
    DimmerKickSwitch,
    entity_category=ENTITY_CATEGORY_CONFIG,
).extend(
    {
        cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)
    # BEHAVIOR.md: kick_enabled defaults on. Publish it before setup() runs
    # so DimmerKickSwitch::setup() primes the engine from a real value.
    cg.add(var.publish_state(True))
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(var.set_parent(parent))
