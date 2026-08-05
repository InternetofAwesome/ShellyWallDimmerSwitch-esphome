import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer, shelly_wall_dimmer_ns

DimmerLight = shelly_wall_dimmer_ns.class_(
    "DimmerLight", light.LightOutput, cg.Parented.template(ShellyWallDimmer)
)

CONFIG_SCHEMA = light.light_schema(DimmerLight, light.LightType.BRIGHTNESS_ONLY).extend(
    {
        cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
    }
)


async def to_code(config):
    var = await light.new_light(config)
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(var.set_parent(parent))
