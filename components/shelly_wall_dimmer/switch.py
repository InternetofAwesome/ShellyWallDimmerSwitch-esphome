import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch
from esphome.const import CONF_ID, CONF_RESTORE_MODE, CONF_TYPE, ENTITY_CATEGORY_CONFIG

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer, shelly_wall_dimmer_ns

DimmerSwitch = shelly_wall_dimmer_ns.class_(
    "DimmerSwitch", switch.Switch, cg.Component, cg.Parented.template(ShellyWallDimmer)
)
DimmerSwitchType = shelly_wall_dimmer_ns.enum("DimmerSwitchType", is_class=True)

# type -> (enum value, default state). Defaults match DimmerParams: kick and
# ramp-on-change on; the fade and limit-correction gates off (opt-in).
#
# Every switch here persists: the engine gates are tuning knobs that must survive
# a reboot or an update, and `allow_overwrite_stock` is a standing permission the
# user grants once. The default below is only the first-boot fallback, applied via
# RESTORE_DEFAULT_ON / RESTORE_DEFAULT_OFF when NVS holds nothing yet.
TYPES = {
    "kick_enabled": (DimmerSwitchType.KICK_ENABLED, True),
    "ramp_on_change": (DimmerSwitchType.RAMP_ON_CHANGE, True),
    "ramp_on_off": (DimmerSwitchType.RAMP_ON_OFF, False),
    "limit_correct": (DimmerSwitchType.LIMIT_CORRECT, False),
    "allow_overwrite_stock": (DimmerSwitchType.ALLOW_OVERWRITE_STOCK, False),
}


_RESTORE_MODE = cv.enum(switch.RESTORE_MODES, upper=True, space="_")


def _default_restore_mode(config):
    # ESPHome's switch schema hard-defaults restore_mode to ALWAYS_OFF, which
    # would reset every knob on each boot. The right default is per-type, so the
    # key is redeclared without a default above and filled in here -- an explicit
    # `restore_mode:` in YAML still wins.
    if CONF_RESTORE_MODE not in config:
        _, default = config[CONF_TYPE].enum_value
        # Run it through the validator rather than indexing RESTORE_MODES
        # directly: that yields the same cv.enum wrapper the schema would have
        # produced, which `esphome config` can dump and codegen unwraps.
        config[CONF_RESTORE_MODE] = _RESTORE_MODE(
            "RESTORE_DEFAULT_ON" if default else "RESTORE_DEFAULT_OFF"
        )
    return config


CONFIG_SCHEMA = cv.All(
    switch.switch_schema(
        DimmerSwitch,
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(
        {
            cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
            cv.Required(CONF_TYPE): cv.enum(TYPES, lower=True),
            # Redeclared without a default so _default_restore_mode() can tell
            # "user asked for this" from "nobody said" (see there).
            cv.Optional(CONF_RESTORE_MODE): _RESTORE_MODE,
        }
    ),
    _default_restore_mode,
)


async def to_code(config):
    # cv.enum() returns the matched key with the mapped tuple on .enum_value --
    # see number.py for the same idiom.
    type_, _default = config[CONF_TYPE].enum_value
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # register_switch() emits set_restore_mode() from CONF_RESTORE_MODE, which
    # _default_restore_mode() has already populated; DimmerSwitch::setup() then
    # primes the engine from the restored (or defaulted) state.
    await switch.register_switch(var, config)
    cg.add(var.set_type(type_))
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(var.set_parent(parent))
