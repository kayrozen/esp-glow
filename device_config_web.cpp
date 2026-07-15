#ifdef ESP_PLATFORM

// device_config_web.cpp — device-only glue: see device_config_web.h for
// the design rationale (validate-before-write is what keeps a bad
// reconfigure from bricking the device instead of just failing to boot
// on defaults).

#include "device_config_web.h"
#include "device_config_encoder.h"

#include "esp_http_server.h"  // not pulled in by device_config_web.h -- see its header comment
#include "esp_partition.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <vector>
#include <algorithm>

static const char* TAG = "devcfg_web";

namespace {

DeviceConfigWebCallbacks g_cb = {};
DeviceConfig g_effectiveCfg;

const esp_partition_t* findDevcfgPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, static_cast<esp_partition_subtype_t>(0x40),
                                   DEVCFG_PARTITION_LABEL);
}

esp_err_t devcfg_get_handler(httpd_req_t* req) {
  // Serializes whatever actually took effect THIS boot (devcfg or
  // Kconfig-default fallback -- see device_config_web_set_effective_config's
  // header comment), not a re-read of the raw partition: a "defaults"
  // boot has no valid CFG1 blob in flash to hand back verbatim, and the
  // reconfigure page's "read, edit, write back" flow needs a real starting
  // point either way.
  std::vector<uint8_t> blob = encodeDeviceConfig(g_effectiveCfg);
  httpd_resp_set_type(req, "application/octet-stream");
  httpd_resp_send(req, reinterpret_cast<const char*>(blob.data()), static_cast<ssize_t>(blob.size()));
  return ESP_OK;
}

esp_err_t devcfg_post_handler(httpd_req_t* req) {
  if (g_cb.cuesActive && g_cb.cuesActive()) {
    ESP_LOGW(TAG, "devcfg write refused: cues are active");
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "refused: cues are active; stop the show before reconfiguring\n");
    return ESP_OK;
  }

  if (req->content_len != DEVCFG_BLOB_SIZE) {
    ESP_LOGW(TAG, "devcfg POST: wrong length (%lu, expected %u)",
             (unsigned long)req->content_len, (unsigned)DEVCFG_BLOB_SIZE);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "expected exactly DEVCFG_BLOB_SIZE bytes\n");
    return ESP_OK;
  }

  uint8_t buf[DEVCFG_BLOB_SIZE];
  size_t received = 0;
  while (received < sizeof(buf)) {
    int n = httpd_req_recv(req, reinterpret_cast<char*>(buf) + received, sizeof(buf) - received);
    if (n <= 0) {
      if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
      ESP_LOGE(TAG, "devcfg POST: recv failed (%d)", n);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    received += static_cast<size_t>(n);
  }

  // Validate BEFORE ever touching flash -- the whole point of §6's "gate
  // it": a blob that fails parseDeviceConfig (bad magic/version/CRC) is
  // rejected here, and the currently-running config (and the partition on
  // disk) is left completely untouched.
  DeviceConfig parsed;
  if (!parseDeviceConfig(buf, sizeof(buf), parsed)) {
    ESP_LOGW(TAG, "devcfg POST: rejected (parseDeviceConfig failed -- bad magic/version/CRC)");
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_sendstr(req, "rejected: not a valid CFG1 blob (bad magic/version/CRC)\n");
    return ESP_OK;
  }

  const esp_partition_t* part = findDevcfgPartition();
  if (!part) {
    ESP_LOGE(TAG, "devcfg POST: partition '%s' not found", DEVCFG_PARTITION_LABEL);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  esp_err_t err = esp_partition_erase_range(part, 0, part->size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "devcfg POST: erase failed: %s", esp_err_to_name(err));
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  err = esp_partition_write(part, 0, buf, sizeof(buf));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "devcfg POST: write failed: %s", esp_err_to_name(err));
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "devcfg written (%u bytes) -> partition '%s'; rebooting to apply",
           (unsigned)sizeof(buf), DEVCFG_PARTITION_LABEL);
  httpd_resp_sendstr(req, "OK, rebooting to apply the new config\n");

  vTaskDelay(pdMS_TO_TICKS(500));  // let the HTTP response actually flush
  esp_restart();
  return ESP_OK;
}

}  // namespace

void device_config_web_set_callbacks(const DeviceConfigWebCallbacks* cb) {
  g_cb = cb ? *cb : DeviceConfigWebCallbacks{};
}

void device_config_web_set_effective_config(const DeviceConfig& cfg) {
  g_effectiveCfg = cfg;
}

void device_config_web_register_handlers(void* server) {
  httpd_handle_t handle = static_cast<httpd_handle_t>(server);

  httpd_uri_t getUri = {};
  getUri.uri = "/devcfg";
  getUri.method = HTTP_GET;
  getUri.handler = devcfg_get_handler;
  httpd_register_uri_handler(handle, &getUri);

  httpd_uri_t postUri = {};
  postUri.uri = "/devcfg";
  postUri.method = HTTP_POST;
  postUri.handler = devcfg_post_handler;
  httpd_register_uri_handler(handle, &postUri);

  ESP_LOGI(TAG, "/devcfg (GET+POST) ready");
}

#endif  // ESP_PLATFORM
