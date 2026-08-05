// Linker --wrap shim that makes ESPHome's native OTA safe on the Shelly SH0S
// bootloader.
// ---------------------------------------------------------------------------
// ESPHome's esp-idf OTA backend writes the new image to the inactive app slot
// (correctly chosen by esp_ota_get_next_update_partition(), which is otadata-
// independent) and then calls esp_ota_set_boot_partition() to point the
// bootloader at it. On a normal ESP32 that writes the standard ota_select
// otadata -- but this device boots via Shelly's proprietary SH0S otadata, which
// ota_select would CORRUPT (-> 0 valid copies -> brick).
//
// We intercept that ONE call with `-Wl,--wrap=esp_ota_set_boot_partition` (added
// in __init__.py) and, instead of the real function, write an SH0S "dfu_stage"
// record via the CRC-sealed BootState primitive. We deliberately do NOT call
// __real_esp_ota_set_boot_partition -- keeping ota_select out of the otadata
// entirely. The IDF app-rollback machinery (the other otadata writer, at
// esp_ota_begin) is compiled out via CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n in
// the YAML, so this wrap is the ONLY otadata write left in the OTA path.
//
// Record written (as=target, rs=running, c=0, ba=3): the bootloader boots the
// freshly-flashed target slot; if that image crash-loops it counts ba down and
// auto-reverts to the still-good running slot. A healthy new image commits
// itself (auto-commit-on-healthy in the component). This exact record + flow was
// validated against the real bootloader under QEMU (see CLAUDE.md "QEMU RIG").

#include "esphome/core/defines.h"

#if defined(USE_ESP32) && defined(USE_ESP_IDF)

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_err.h>

#include "esphome/core/log.h"
#include "boot_state.h"

namespace {
constexpr const char *DFU_TAG = "shelly_wall_dimmer.dfu";

// Fixed device flash layout -> SH0S slot index. (app_0 @ 0x10000, app_1 @
// 0x200000; see the partition table in CLAUDE.md.) Returns -1 if unrecognized.
int slot_for_addr(uint32_t addr) {
  if (addr == 0x10000u) return 0;
  if (addr == 0x200000u) return 1;
  return -1;
}
}  // namespace

// C linkage: --wrap redirects the C symbol esp_ota_set_boot_partition here.
extern "C" esp_err_t __wrap_esp_ota_set_boot_partition(const esp_partition_t *partition) {
  if (partition == nullptr) {
    ESP_LOGE(DFU_TAG, "set_boot_partition(nullptr) -- refusing");
    return ESP_ERR_INVALID_ARG;
  }
  int target = slot_for_addr(partition->address);
  const esp_partition_t *running = esp_ota_get_running_partition();
  int run_slot = (running != nullptr) ? slot_for_addr(running->address) : -1;

  ESP_LOGW(DFU_TAG, "DFU: intercept set_boot_partition -> 0x%06x (slot %d); running slot %d",
           (unsigned) partition->address, target, run_slot);

  if (target < 0 || run_slot < 0) {
    ESP_LOGE(DFU_TAG, "DFU: unrecognized app-slot address -- refusing SH0S write (otadata untouched)");
    return ESP_ERR_NOT_SUPPORTED;
  }
  if (target == run_slot) {
    // esp_ota_get_next_update_partition should never hand us the running slot,
    // but guard it: staging onto the running slot would be nonsensical.
    ESP_LOGE(DFU_TAG, "DFU: target slot == running slot (%d) -- refusing", target);
    return ESP_ERR_INVALID_STATE;
  }

  ::shelly_dimmer_core::BootState bs;
  if (!bs.stage_ota(static_cast<uint8_t>(target), static_cast<uint8_t>(run_slot))) {
    ESP_LOGE(DFU_TAG, "DFU: SH0S stage_ota write failed -- running slot still boots");
    return ESP_FAIL;
  }
  ESP_LOGW(DFU_TAG, "DFU: staged SH0S boot record (as=%d rs=%d c=0 ba=%d); reboot boots the new image",
           target, run_slot, (int) ::shelly_dimmer_core::SHOS_DFU_BOOT_ATTEMPTS);
  return ESP_OK;
}

// The OTHER otadata writer in the IDF OTA path. ESPHome calls this at OTA begin
// (USE_OTA_ROLLBACK) AND safe_mode calls it every boot. It is NOT guarded by
// CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE: esp_ota_current_ota_is_workable() reads
// otadata as ota_select and, if it finds a valid active entry, rewrites it ->
// would corrupt SH0S. In practice SH0S records fail the ota_select CRC check so
// it returns without writing, but we do NOT rely on that coincidence. Make it a
// hard no-op: SH0S permanence is handled by our own commit() (ba->0), and IDF
// ota_select rollback is disabled anyway. Inert for both callers.
extern "C" esp_err_t __wrap_esp_ota_mark_app_valid_cancel_rollback(void) {
  ESP_LOGD(DFU_TAG, "mark_app_valid_cancel_rollback: no-op (SH0S uses commit, not ota_select)");
  return ESP_OK;
}

#endif  // USE_ESP32 && USE_ESP_IDF
