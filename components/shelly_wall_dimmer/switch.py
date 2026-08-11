import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer, shelly_wall_dimmer_ns

DimmerSwitch = shelly_wall_dimmer_ns.class_(
    "DimmerSwitch", switch.Switch, cg.Component, cg.Parented.template(ShellyWallDimmer)
)
DimmerSwitchType = shelly_wall_dimmer_ns.enum("DimmerSwitchType", is_class=True)

# type -> (enum value, default state, persist). Defaults match DimmerParams:
# kick and ramp-on-change on; the fade and limit-correction gates off (opt-in).
#
# `persist` marks a switch whose value must survive reboots. The tuning knobs are
# re-primed from their YAML default each boot; `allow_overwrite_stock` is instead
# a standing permission the user grants once, so it restores from flash.
TYPES = {
    "kick_enabled": (DimmerSwitchType.KICK_ENABLED, True, False),
    "ramp_on_change": (DimmerSwitchType.RAMP_ON_CHANGE, True, False),
    "ramp_on_off": (DimmerSwitchType.RAMP_ON_OFF, False, False),
    "limit_correct": (DimmerSwitchType.LIMIT_CORRECT, False, False),
    "allow_overwrite_stock": (DimmerSwitchType.ALLOW_OVERWRITE_STOCK, False, True),
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
    # cv.enum() returns the matched key with the mapped tuple on .enum_value --
    # see number.py for the same idiom.
    type_, default, persist = config[CONF_TYPE].enum_value
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await switch.register_switch(var, config)
    cg.add(var.set_type(type_))
    if persist:
        # Force a restoring mode: ESPHome's switch schema defaults to ALWAYS_OFF,
        # which would silently reset this permission on every reboot and defeat
        # the point of a set-once-and-forget safety switch. RESTORE_DEFAULT_OFF
        # keeps a fresh device protected while remembering a deliberate flip.
        cg.add(var.set_restore_mode(switch.RESTORE_MODES["RESTORE_DEFAULT_OFF"]))
    else:
        # Publish the YAML-implied default before setup() runs so
        # DimmerSwitch::setup() primes the engine from a real value.
        cg.add(var.publish_state(default))
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(var.set_parent(parent))
