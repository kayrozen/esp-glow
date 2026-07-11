// storage_manager.cpp — LittleFS mount + SHW1 bundle read.
#include "storage_manager.h"

#include "esp_log.h"
#include "esp_littlefs.h"
#include "show_bundle.h"

#include <cstring>
#include <cstdio>

static const char* TAG = "storage";
static bool s_mounted = false;

bool storage_mount(void) {
  if (s_mounted) return true;

  esp_vfs_littlefs_conf_t conf = {};
  conf.partition_label = "littlefs";
  conf.mount_point = "/littlefs";
  conf.format_if_mount_failed = true;  // first boot: format, then it's empty

  esp_err_t e = esp_vfs_littlefs_register(&conf);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(e));
    return false;
  }

  size_t total = 0, used = 0;
  esp_littlefs_info(conf.partition_label, &total, &used);
  ESP_LOGI(TAG, "LittleFS mounted at /littlefs (%u KB total, %u KB used)",
           (unsigned)(total / 1024), (unsigned)(used / 1024));
  s_mounted = true;
  return true;
}

size_t storage_bundle_size(const char* path) {
  if (!s_mounted || !path) return 0;
  FILE* f = fopen(path, "rb");
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fclose(f);
  return sz > 0 ? (size_t)sz : 0;
}

bool storage_read_bundle(const char* path, uint8_t* out_buf, size_t buf_cap, size_t* out_len) {
  if (!s_mounted || !path || !out_buf || !out_len) return false;
  *out_len = 0;

  FILE* f = fopen(path, "rb");
  if (!f) {
    ESP_LOGW(TAG, "bundle not found: %s", path);
    return false;
  }
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz <= 0) {
    ESP_LOGW(TAG, "bundle empty: %s", path);
    fclose(f);
    return false;
  }
  if ((size_t)sz > buf_cap) {
    ESP_LOGE(TAG, "bundle %s is %ld bytes, buffer cap %u", path, sz, (unsigned)buf_cap);
    fclose(f);
    return false;
  }
  size_t got = fread(out_buf, 1, (size_t)sz, f);
  fclose(f);
  if (got != (size_t)sz) {
    ESP_LOGE(TAG, "short read on %s: got %u of %ld", path, (unsigned)got, sz);
    return false;
  }
  *out_len = got;
  ESP_LOGI(TAG, "read %s (%u bytes)", path, (unsigned)got);
  return true;
}

bool storage_load_show(const char* path, LoadedShow* out_show,
                       uint8_t* buf, size_t buf_cap) {
  if (!out_show) return false;
  if (!storage_mount()) return false;

  size_t len = 0;
  if (!storage_read_bundle(path, buf, buf_cap, &len)) return false;

  if (!loadShow(buf, len, *out_show)) {
    ESP_LOGE(TAG, "loadShow failed on %s (%u bytes): corrupt or wrong magic",
             path, (unsigned)len);
    return false;
  }
  ESP_LOGI(TAG, "show loaded: %u universes, %u fixtures, %u matrices",
           out_show->universeCount, (unsigned)out_show->fixtures.size(),
           (unsigned)out_show->matrices.size());
  return true;
}
