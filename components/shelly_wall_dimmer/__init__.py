from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, uart
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL

CODEOWNERS = ["@InternetofAwesome"]
DEPENDENCIES = ["uart"]
# The hub header (shelly_wall_dimmer.h) unconditionally includes the light/
# number/sensor/switch/text_sensor base classes, so those component libraries
# must be compiled whenever the hub is configured -- even before any platform
# entity is added. AUTO_LOAD pulls them in so a hub-only config still builds.
AUTO_LOAD = ["light", "number", "sensor", "switch", "text_sensor"]
MULTI_CONF = False  # one dimmer / one UART link per device

shelly_wall_dimmer_ns = cg.esphome_ns.namespace("shelly_wall_dimmer")
ShellyWallDimmer = shelly_wall_dimmer_ns.class_(
    "ShellyWallDimmer", cg.Component, uart.UARTDevice
)

# Shared across every platform file (light.py, number.py, switch.py,
# sensor.py, text_sensor.py) to point an entity at its owning dimmer.
CONF_SHELLY_WALL_DIMMER_ID = "shelly_wall_dimmer_id"

# Optional "bridge package" build: on a successful compile, assemble the
# stock-format Shelly OTA zip (the one-time image that gets this firmware onto a
# still-stock dimmer via Shelly's own OTA) as a post-build step -- and, if
# `push_to` is given, push it straight to that device. See shelly_pkg.py.
CONF_BRIDGE_PACKAGE = "bridge_package"
CONF_PUSH_TO = "push_to"

BRIDGE_PACKAGE_SCHEMA = cv.Schema(
    {
        # Device IP (or hostname) to push the finished package to via
        # Shelly.Update after the build. Omit to only produce the zip.
        cv.Optional(CONF_PUSH_TO): cv.string_strict,
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ShellyWallDimmer),
            cv.Optional(CONF_UPDATE_INTERVAL, default="1s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_BRIDGE_PACKAGE): BRIDGE_PACKAGE_SCHEMA,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)

# Enforces the protocol's fixed 115200 8N1 and that both directions are wired
# -- catches a bad `uart:` block at config-validate time instead of a silent
# runtime hang. See PROTOCOL.md.
FINAL_VALIDATE_SCHEMA = uart.final_validate_device_schema(
    "shelly_wall_dimmer",
    baud_rate=115200,
    require_rx=True,
    require_tx=True,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL]))

    # ---- Shelly-bootloader requirements, injected here so the device YAML stays
    # a plain ESPHome config (no partitions:/sdkconfig_options:/advanced: needed).
    # These are mandatory on this hardware; hiding them keeps the user config
    # standard-looking while still producing a correct, SH0S-safe image.

    # 1. The exact Shelly flash layout. MUST match the table the Shelly
    #    bootloader has at 0x8000 -- app_0 @0x10000 / app_1 @0x200000 are what the
    #    DFU slot mapping (dfu_wrap.cpp) and the bootloader expect. Supplying it
    #    here means the user's YAML needs no `partitions:` key. add_extra_build_file
    #    wins over ESPHome's default table (esp32/__init__.py writes the default
    #    only if "partitions.csv" isn't already registered).
    esp32.add_extra_build_file("partitions.csv", Path(__file__).parent / "partitions.csv")

    # 2. IDF app-rollback OFF. It writes the standard ota_select otadata (at OTA
    #    begin and via safe_mode), which would corrupt the Shelly SH0S otadata.
    #    We use SH0S's own ba-countdown rollback instead. See dfu_wrap.cpp.
    esp32.add_idf_sdkconfig_option("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE", False)

    # 3. Single-core, matching Shelly's stock build (the module is a single-core
    #    Xtensa SKU; a dual-core image would try to start a nonexistent APP_CPU).
    esp32.add_idf_sdkconfig_option("CONFIG_FREERTOS_UNICORE", True)

    # 4. DFU safety: redirect the TWO IDF functions that write otadata to our
    #    SH0S-aware shims (dfu_wrap.cpp) so ESPHome's native OTA (and safe_mode)
    #    write the Shelly SH0S boot record -- or nothing -- instead of ota_select.
    #      set_boot_partition (OTA end)                  -> stage an SH0S DFU record
    #      mark_app_valid_cancel_rollback (begin + boot) -> hard no-op
    cg.add_build_flag("-Wl,--wrap=esp_ota_set_boot_partition")
    cg.add_build_flag("-Wl,--wrap=esp_ota_mark_app_valid_cancel_rollback")

    # ---- optional bridge-package build (post-build hook) --------------------
    # When the user opts in, wire shelly_pkg.py in as a PlatformIO post-build
    # script and hand it the paths/flags it needs via custom_ project options
    # (readable from the SCons env with GetProjectOption). Nothing is injected
    # unless `bridge_package:` is present, so a plain build is unaffected.
    if CONF_BRIDGE_PACKAGE in config:
        here = Path(__file__).parent
        cg.add_platformio_option("extra_scripts", [f"post:{here / 'shelly_pkg.py'}"])
        cg.add_platformio_option("custom_shelly_bridge", "1")
        cg.add_platformio_option("custom_shelly_fs_img", str(here / "bridge_fs_empty.img"))
        push_to = config[CONF_BRIDGE_PACKAGE].get(CONF_PUSH_TO)
        if push_to:
            cg.add_platformio_option("custom_shelly_push_ip", push_to)
