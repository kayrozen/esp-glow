// storage_manager.cpp — raw-partition SHW1 bundle read.
#include "storage_manager.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "show_bundle.h"

#include <cstring>

static const char* TAG = "storage";

bool storage_load_show(LoadedShow* out_show, uint8_t* buf, size_t buf_cap) {
  if (!out_show || !buf || buf_cap == 0) return false;

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
      STORAGE_SHOW_PARTITION_LABEL);
  if (!part) {
    ESP_LOGE(TAG, "partition '%s' not found", STORAGE_SHOW_PARTITION_LABEL);
    return false;
  }

  // loadShow() bounds-checks every read and doesn't require an exact
  // trailing length, so it's safe to hand it the whole (buffer-capped)
  // partition, erased-flash padding and all.
  size_t len = buf_cap < part->size ? buf_cap : part->size;
  esp_err_t e = esp_partition_read(part, 0, buf, len);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "partition '%s' read failed: %s", STORAGE_SHOW_PARTITION_LABEL,
              esp_err_to_name(e));
    return false;
  }

  if (!loadShow(buf, len, *out_show)) {
    ESP_LOGE(TAG, "loadShow failed on partition '%s' (%u bytes): corrupt, blank, or wrong magic",
              STORAGE_SHOW_PARTITION_LABEL, (unsigned)len);
    return false;
  }
  ESP_LOGI(TAG, "show loaded from partition '%s': %u universes, %u fixtures, %u matrices",
           STORAGE_SHOW_PARTITION_LABEL, out_show->universeCount,
           (unsigned)out_show->fixtures.size(), (unsigned)out_show->matrices.size());
  return true;
}
