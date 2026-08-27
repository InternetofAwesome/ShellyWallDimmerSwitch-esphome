import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    CONF_TYPE,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import CONF_SHELLY_WALL_DIMMER_ID, ShellyWallDimmer

BASE = {
    cv.Required(CONF_SHELLY_WALL_DIMMER_ID): cv.use_id(ShellyWallDimmer),
}

# type -> the setter that hands the entity to the hub. Defaults differ per type,
# so each gets its own schema rather than one schema with a type key bolted on.
#
# `button` reports the front tactile switch. It is a debounced PULSE, not a live
# view of the contact: it goes true when a press is ACCEPTED and false when the
# button re-arms, so a held button reads pressed for the hold-off, not for as
# long as the finger is down. The toggle itself is already handled in firmware
# (see ButtonStore) -- this exists to see presses in HA, not to drive them, and
# an `on_press:` automation here fires IN ADDITION to the built-in toggle.
TYPES = {
    "overtemp": (
        "set_overtemp_binary_sensor",
        binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_PROBLEM,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ).extend(BASE),
    ),
    "button": (
        # No HA binary_sensor device class fits a pushbutton, so none is set.
        "set_button_binary_sensor",
        binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ).extend(BASE),
    ),
}

CONFIG_SCHEMA = cv.typed_schema(
    {name: schema for name, (_setter, schema) in TYPES.items()},
    # Predates the `button` type, when overtemp was the only option and `type:`
    # could be omitted. Kept so existing configs keep validating unchanged.
    default_type="overtemp",
    lower=True,
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    parent = await cg.get_variable(config[CONF_SHELLY_WALL_DIMMER_ID])
    setter, _schema = TYPES[config[CONF_TYPE]]
    cg.add(getattr(parent, setter)(var))
