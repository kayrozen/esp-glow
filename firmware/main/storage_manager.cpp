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

bool storage_load_devcfg(DeviceConfig* out_cfg) {
  if (!out_cfg) return false;

  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
      DEVCFG_PARTITION_LABEL);
  if (!part) {
    ESP_LOGW(TAG, "partition '%s' not found", DEVCFG_PARTITION_LABEL);
    return false;
  }

  // Fixed-size blob (DEVCFG_BLOB_SIZE), well under the 4 KB partition --
  // no PSRAM scratch buffer needed, unlike the (much larger) show bundle.
  uint8_t buf[DEVCFG_BLOB_SIZE];
  size_t len = part->size < sizeof(buf) ? part->size : sizeof(buf);
  if (len < DEVCFG_BLOB_SIZE) {
    // partitions.csv defines this partition at >= DEVCFG_BLOB_SIZE; a
    // smaller one is a build misconfiguration, not a runtime condition to
    // silently tolerate -- but still just falls back to defaults rather
    // than crashing, same as every other rejection path here.
    ESP_LOGE(TAG, "partition '%s' is %u bytes, smaller than a CFG1 blob (%u)",
              DEVCFG_PARTITION_LABEL, (unsigned)part->size, (unsigned)DEVCFG_BLOB_SIZE);
    return false;
  }

  esp_err_t e = esp_partition_read(part, 0, buf, sizeof(buf));
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "partition '%s' read failed: %s", DEVCFG_PARTITION_LABEL,
              esp_err_to_name(e));
    return false;
  }

  if (!parseDeviceConfig(buf, sizeof(buf), *out_cfg)) {
    ESP_LOGW(TAG, "parseDeviceConfig rejected partition '%s': missing, blank, or corrupt CFG1 "
                  "(falling back to compiled-in defaults)",
              DEVCFG_PARTITION_LABEL);
    return false;
  }

  // Never log the password (see device_config.h) -- main.cpp's own
  // GLOW-TEST: cfg / human-readable log line is the canonical "what took
  // effect" report; this is just proof the partition itself parsed.
  ESP_LOGI(TAG, "devcfg loaded from partition '%s': ssid=\"%s\" dmx_tx=%u usb_midi=%d skip_wifi=%d",
           DEVCFG_PARTITION_LABEL, out_cfg->wifiSsid, (unsigned)out_cfg->dmxTxGpio,
           (int)out_cfg->usbMidiHost, (int)out_cfg->skipWifi);
  return true;
}
