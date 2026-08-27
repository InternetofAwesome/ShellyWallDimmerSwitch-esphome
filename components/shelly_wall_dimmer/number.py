import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number
from esphome.const import (
    CONF_ID,
    CONF_INITIAL_VALUE,
    CONF_TYPE,
    ENTITY_CATEGORY_CONFIG,
)

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer, shelly_wall_dimmer_ns

DimmerNumber = shelly_wall_dimmer_ns.class_(
    "DimmerNumber", number.Number, cg.Component, cg.Parented.template(ShellyWallDimmer)
)
DimmerNumberType = shelly_wall_dimmer_ns.enum("DimmerNumberType", is_class=True)

# type -> (enum value, min, max, step, default). Ranges and defaults are
# documented in the README's Option reference.
# The default is a FIRST-BOOT fallback only: every value set from HA is stored in
# NVS and restored on the next boot, so tuning survives reboots and OTA updates.
# Override it per entity with `initial_value:` (see _default_initial_value below).
# ramp_rate is a single shared rate in percent/second (min 1 so it can never be
# zero; 150 == the old 3%/20ms cadence). The engine quantizes it to a step/interval.
TYPES = {
    "kick_level": (DimmerNumberType.KICK_LEVEL, 0, 100, 1, 20),
    "kick_dwell_ms": (DimmerNumberType.KICK_DWELL_MS, 0, 2000, 10, 150),
    "min_brightness": (DimmerNumberType.MIN_BRIGHTNESS, 0, 100, 1, 1),
    "max_brightness": (DimmerNumberType.MAX_BRIGHTNESS, 0, 100, 1, 100),
    "ramp_rate": (DimmerNumberType.RAMP_RATE, 1, 1000, 1, 150),
    # Over-temperature cutout. Above this, the output is switched off and
    # held off. Default errs LOW because it is provisional; the firmware
    # clamps anything above the ceiling. See dimmer_engine.h.
    "overtemp_limit": (DimmerNumberType.OVERTEMP_LIMIT, 40, 85, 1, 65),
    # ---- front button + setpoint assert -------------------------------------
    # All three are live knobs on purpose. How long a finger lingers on the
    # touch plate after a press, and how bouncy a given unit's switch is, are
    # properties of the hardware in the wall -- tuning them should be a slider
    # in HA, not a reflash of a device with no USB port.
    #
    # Minimum gap between accepted presses, and the window in which release
    # bounce is discarded. 0 accepts every latched edge.
    "button_hold_off_ms": (DimmerNumberType.BUTTON_HOLD_OFF_MS, 0, 1000, 10, 100),
    # How long a button-originated command is re-asserted for, and how often.
    # assert_ms 0 disables the assert entirely. See dimmer_engine.h.
    "assert_ms": (DimmerNumberType.ASSERT_MS, 0, 1000, 10, 50),
    "assert_interval_ms": (DimmerNumberType.ASSERT_INTERVAL_MS, 1, 100, 1, 5),
}


def _default_initial_value(config):
    # The permitted range is per-type, so `initial_value:` can't be range-checked
    # by the schema -- do it here, once `type:` is known, and fill in the TYPES
    # default when the YAML omits it. Out-of-range is rejected at build time
    # rather than silently ignored at boot: DimmerNumber::setup() range-checks
    # what it loads from NVS and would fall back, which is hard to debug.
    _, min_v, max_v, _, default = config[CONF_TYPE].enum_value
    if CONF_INITIAL_VALUE not in config:
        config[CONF_INITIAL_VALUE] = default
    elif not min_v <= config[CONF_INITIAL_VALUE] <= max_v:
        raise cv.Invalid(
            f"initial_value must be between {min_v} and {max_v} for "
            f"type: {config[CONF_TYPE]}",
            path=[CONF_INITIAL_VALUE],
        )
    return config


CONFIG_SCHEMA = cv.All(
    number.number_schema(
        DimmerNumber,
        entity_category=ENTITY_CATEGORY_CONFIG,
    ).extend(
        {
            cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
            cv.Required(CONF_TYPE): cv.enum(TYPES, lower=True),
            # First-boot value only; ignored once NVS holds one. No default here
            # -- it is per-type, filled in by _default_initial_value().
            cv.Optional(CONF_INITIAL_VALUE): cv.float_,
        }
    ),
    _default_initial_value,
)


async def to_code(config):
    # cv.enum() returns a str-subclass (the matched key) with the mapped
    # value stashed on `.enum_value` -- NOT the mapped tuple itself, so it
    # must be unpacked from there, not from config[CONF_TYPE] directly.
    type_, min_v, max_v, step, _default = config[CONF_TYPE].enum_value
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await number.register_number(var, config, min_value=min_v, max_value=max_v, step=step)
    cg.add(var.set_type(type_))
    # The live value is restored from NVS in DimmerNumber::setup(); this is only
    # the fallback for a device that has never had one stored.
    cg.add(var.set_initial_value(config[CONF_INITIAL_VALUE]))
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    cg.add(var.set_parent(parent))
