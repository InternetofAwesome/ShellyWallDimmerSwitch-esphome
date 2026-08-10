import sys
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, uart
from esphome.const import CONF_ID, CONF_UPDATE_INTERVAL
from esphome.core import CORE

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


def _invocation_will_upload():
    """True if this esphome invocation is going to push firmware at a device.

    ESPHome dispatches through one function per CLI command, so read that
    directly off the call stack rather than guessing from sys.argv (which also
    carries `-s KEY VALUE` substitutions and assorted global flags):

        command_compile  -> builds only            (safe for the bridge)
        command_run      -> builds THEN uploads    (not safe, see below)
        command_upload   -> uploads                (not safe)

    Falls back to an argv scan, and finally to "no" -- failing open on purpose,
    because wrongly blocking a plain compile would break the documented
    first-flash flow, while this check is defence-in-depth behind a README that
    already says to use compile-only.
    """
    import inspect

    uploads = {"command_run", "command_upload"}
    builds_only = {"command_compile", "command_config", "command_idedata", "command_clean"}
    try:
        for frame in inspect.stack():
            if frame.function in uploads:
                return True
            if frame.function in builds_only:
                return False
    except Exception:  # noqa: BLE001  -- detection must never break a build
        pass

    argv = sys.argv[1:]
    skip_next = 0
    for i, tok in enumerate(argv):
        if skip_next:
            skip_next -= 1
            continue
        if tok in ("-s", "--substitution"):
            skip_next = 2  # KEY VALUE
            continue
        if tok.startswith("-"):
            continue
        if tok in ("run", "upload"):
            return True
        if tok in ("compile", "config", "clean", "logs", "idedata"):
            return False
    return False


def _validate_bridge_upload(config):
    """Refuse to build the bridge package as part of an upload job.

    `bridge_package` delivers firmware by an entirely separate path: it POSTs
    Shelly.Update at `push_to` during the post-build step. Combining that with
    ESPHome's own OTA in one invocation is genuinely destructive rather than
    merely redundant:

      * On a still-stock dimmer, the bridge flashes the inactive slot and the
        device reboots into THIS firmware -- which speaks ESPHome OTA. The
        upload half of the same command can then land on the OTHER slot, the one
        still holding stock. A single "Install" would consume both slots and
        destroy the rollback target, silently.
      * On an already-converted dimmer, the bridge still POSTs at whatever IP
        `push_to` names. If that value is stale it flashes a DIFFERENT device
        while the OTA updates the intended one.

    So the bridge is compile-only, by construction. In the ESPHome Builder that
    means "Manual download"; on the CLI, `esphome compile`.
    """
    if CONF_BRIDGE_PACKAGE in config and _invocation_will_upload():
        raise cv.Invalid(
            "bridge_package must not run as part of an upload/install job. It "
            "delivers firmware over Shelly's OTA at compile time, so combining it "
            "with an ESPHome OTA in one command can flash two slots (destroying "
            "the stock fallback) or flash the wrong device. Use a compile-only "
            "build -- ESPHome Builder: 'Manual download'; CLI: `esphome compile`. "
            "Once the device is running this firmware, comment out `bridge_package:` "
            "and install wirelessly as normal."
        )
    return config


def _validate_bridge_toolchain(config):
    # bridge_package hooks a PlatformIO post-build script (SCons env.AddPostAction
    # in shelly_pkg.py). As of ESPHome 2026.7.0 the esp32 platform's DEFAULT build
    # backend switched to ESPHome's own native ESP-IDF driver (idf.py/cmake+ninja),
    # which never generates a platformio.ini and never reads anything set via
    # cg.add_platformio_option -- so under that default the hook is silently never
    # invoked; the package is never built and nothing gets pushed, with no error at
    # all. `toolchain: platformio` opts back into the PlatformIO-driven build
    # ("continue to build exactly as they did before", per the 2026.7.0 release
    # notes) and is what actually makes this feature run. Caught here, at
    # config-validate time, instead of failing silently at build time.
    if CONF_BRIDGE_PACKAGE in config and CORE.using_toolchain_esp_idf:
        raise cv.Invalid(
            "bridge_package requires the PlatformIO build backend (it hooks a "
            "PlatformIO post-build script, which ESPHome's native ESP-IDF toolchain "
            "-- the esp32 platform's default since 2026.7.0 -- never runs). Add "
            "`toolchain: platformio` to the `esp32:` block."
        )
    return config


# Enforces the protocol's fixed 115200 8N1 and that both directions are wired
# -- catches a bad `uart:` block at config-validate time instead of a silent
# runtime hang. See PROTOCOL.md. Composed with the bridge/toolchain check above.
FINAL_VALIDATE_SCHEMA = cv.All(
    _validate_bridge_upload,
    _validate_bridge_toolchain,
    uart.final_validate_device_schema(
        "shelly_wall_dimmer",
        baud_rate=115200,
        require_rx=True,
        require_tx=True,
    ),
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
    # unless `bridge_package:` is present, so a plain build is unaffected. Needs
    # `toolchain: platformio` -- see _validate_bridge_toolchain() above, which
    # catches the alternative (silently building nothing) before we get here.
    if CONF_BRIDGE_PACKAGE in config:
        here = Path(__file__).parent
        cg.add_platformio_option("extra_scripts", [f"post:{here / 'shelly_pkg.py'}"])
        cg.add_platformio_option("custom_shelly_bridge", "1")
        push_to = config[CONF_BRIDGE_PACKAGE].get(CONF_PUSH_TO)
        if push_to:
            cg.add_platformio_option("custom_shelly_push_ip", push_to)

        # Make `esphome: project: version:` actually reach esp_app_desc.version.
        #
        # Stock's updater dedupes on the version embedded in the APP IMAGE
        # ("same version: %.*s" in shelly_update.cpp) -- not on manifest.json.
        # ESPHome's `project: version:` only emits a C++ define
        # (ESPHOME_PROJECT_VERSION); it never touches app_desc, which otherwise
        # carries ESPHome's own release number and is therefore IDENTICAL across
        # builds. The practical effect is that a second bridge push of a changed
        # firmware is silently discarded by stock as "same version" -- the exact
        # trap the README warns about, with the documented remedy (bump
        # fw_version) having no effect at all. Wiring it to IDF's
        # CONFIG_APP_PROJECT_VER fixes that.
        #
        # Scoped to bridge_package on purpose: changing any sdkconfig value makes
        # ESPHome clean the build directory, so doing this unconditionally would
        # turn every version bump into a full rebuild for everyone. The bridge is
        # a one-time, per-device operation, so paying it there is fine; normal
        # ESPHome OTA updates (the steady state) are unaffected.
        project = (CORE.config.get("esphome") or {}).get("project") or {}
        version = project.get("version")
        if version:
            # IDF caps CONFIG_APP_PROJECT_VER at 32 bytes including the NUL.
            if len(version) > 31:
                version = version[:31]
            esp32.add_idf_sdkconfig_option("CONFIG_APP_PROJECT_VER_FROM_CONFIG", True)
            esp32.add_idf_sdkconfig_option("CONFIG_APP_PROJECT_VER", version)
