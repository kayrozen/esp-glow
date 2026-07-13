#ifdef ESP_PLATFORM

// ota_manager.cpp — device-only glue: see ota_manager.h for the design
// rationale (self-validation is what makes rollback actually mean
// something instead of being defeated by an immediate mark-valid).

#include "ota_manager.h"
#include "web_protocol.h"  // buildOtaStatusJson

#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <algorithm>

static const char* TAG = "ota";

namespace {

OtaCallbacks g_cb = {};

bool     g_isPendingVerify = false;
bool     g_validated = false;
bool     g_frameOk = false;
bool     g_wifiOk = false;
bool     g_dmxOk = false;
uint32_t g_frameCount = 0;
int64_t  g_deadlineUs = 0;

// A real run of frames, not just one -- proves the render loop is actually
// alive and pumping (not that app_main merely returned). At the render
// task's 44 Hz nominal rate this is ~2.3s; the validation deadline below is
// far more generous, so a healthy image clears this almost immediately.
constexpr uint32_t kFramesNeeded = 100;

// Generous relative to kFramesNeeded: WiFi association can legitimately
// take several seconds, especially the first time against a new AP.
constexpr int64_t kValidateTimeoutUs = 20 * 1000 * 1000;  // 20s

void report(const char* phase, const char* message, int percent) {
  if (!g_cb.broadcast) return;
  char buf[256];
  size_t n = buildOtaStatusJson(phase, message, percent, buf, sizeof(buf));
  g_cb.broadcast(buf, n);
}

}  // namespace

void ota_manager_set_callbacks(const OtaCallbacks* cb) {
  g_cb = cb ? *cb : OtaCallbacks{};
}

void ota_manager_init(void) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (running != nullptr &&
      esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    g_isPendingVerify = true;
    g_deadlineUs = esp_timer_get_time() + kValidateTimeoutUs;
    ESP_LOGW(TAG, "booted a pending-verify OTA image; must self-validate "
                  "(wifi up + %u rendered frames + DMX ready) within %lld s "
                  "or the bootloader rolls back on next reset",
             (unsigned)kFramesNeeded, (long long)(kValidateTimeoutUs / 1000000));
  }
}

void ota_manager_note_frame_rendered(void) {
  if (!g_isPendingVerify || g_validated) return;
  if (g_frameCount < kFramesNeeded) ++g_frameCount;
  if (g_frameCount >= kFramesNeeded) g_frameOk = true;
}

void ota_manager_note_wifi_connected(void) {
  if (!g_isPendingVerify || g_validated) return;
  g_wifiOk = true;
}

void ota_manager_note_dmx_ready(void) {
  if (!g_isPendingVerify || g_validated) return;
  g_dmxOk = true;
}

void ota_manager_tick(void) {
  if (!g_isPendingVerify || g_validated) return;

  if (g_frameOk && g_wifiOk && g_dmxOk) {
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    g_validated = true;  // stop ticking either way -- retrying won't help
    if (e == ESP_OK) {
      ESP_LOGI(TAG, "OTA self-validated (frames+wifi+dmx OK); rollback cancelled.");
      report("done", "self-validated; rollback cancelled", -1);
    } else {
      ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %s", esp_err_to_name(e));
    }
    return;
  }

  if (esp_timer_get_time() < g_deadlineUs) return;

  char reason[160];
  std::snprintf(reason, sizeof(reason),
                "OTA self-validation timed out (frames=%d wifi=%d dmx=%d); rolling back",
                g_frameOk, g_wifiOk, g_dmxOk);
  ESP_LOGE(TAG, "%s", reason);
  report("error", reason, -1);
  if (g_cb.onRollbackImminent) g_cb.onRollbackImminent(reason);

  // Give the serial log / WS broadcast a moment to actually go out before
  // the reboot cuts them off.
  vTaskDelay(pdMS_TO_TICKS(300));

  g_validated = true;  // don't re-enter this branch if the reboot is slow
  esp_ota_mark_app_invalid_rollback_and_reboot();
  // Does not return on success. If it somehow returns (e.g. this wasn't
  // really a pending-verify boot after all), fall back to a hard restart
  // rather than silently keep running an image that just failed its own
  // validation -- never leave that ambiguous.
  esp_restart();
}

// --- /ota POST handler -------------------------------------------------

namespace {

esp_err_t ota_post_handler(httpd_req_t* req) {
  if (g_cb.cuesActive && g_cb.cuesActive()) {
    ESP_LOGW(TAG, "OTA refused: cues are active");
    report("refused", "cues are active; stop the show before OTA", -1);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "refused: cues are active; stop the show before OTA\n");
    return ESP_OK;
  }

  const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
  if (target == nullptr) {
    ESP_LOGE(TAG, "no OTA update partition available");
    report("error", "no OTA update partition available", -1);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  esp_ota_handle_t handle = 0;
  esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    report("error", "esp_ota_begin failed", -1);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA upload starting -> partition '%s' (%lu bytes declared)",
           target->label, (unsigned long)req->content_len);
  report("receiving", "upload starting", 0);

  static char buf[4096];
  int remaining = (int)req->content_len;
  size_t received = 0;
  size_t lastReportedPct = 0;

  while (remaining > 0) {
    int toRead = std::min(remaining, (int)sizeof(buf));
    int recvLen = httpd_req_recv(req, buf, (size_t)toRead);
    if (recvLen <= 0) {
      if (recvLen == HTTPD_SOCK_ERR_TIMEOUT) continue;  // retry the read
      ESP_LOGE(TAG, "OTA upload recv failed (%d)", recvLen);
      esp_ota_abort(handle);
      report("error", "upload connection failed", -1);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }

    err = esp_ota_write(handle, buf, (size_t)recvLen);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
      esp_ota_abort(handle);
      report("error", "flash write failed", -1);
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }

    received += (size_t)recvLen;
    remaining -= recvLen;

    if (req->content_len > 0) {
      size_t pct = (received * 100u) / req->content_len;
      if (pct != lastReportedPct) {
        lastReportedPct = pct;
        report("receiving", nullptr, (int)pct);
      }
    }
  }

  // esp_ota_end() is the image-validity boundary: it checks the image
  // header/checksum (and signature, if secure boot is enabled -- out of
  // scope here, see the README note) BEFORE the image is ever considered
  // bootable. A corrupted upload is rejected right here, never reaching
  // esp_ota_set_boot_partition, so the currently-running image is
  // untouched -- this is the first of two independent safety nets (the
  // second is the post-boot self-validate/rollback in ota_manager_tick,
  // for images that pass this check but still don't actually work).
  err = esp_ota_end(handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed (image invalid): %s", esp_err_to_name(err));
    report("error", "uploaded image failed validation", -1);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  err = esp_ota_set_boot_partition(target);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
    report("error", "could not set boot partition", -1);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "OTA complete (%u bytes) -> '%s'; rebooting into new slot",
           (unsigned)received, target->label);
  report("done", "flashed; rebooting into new slot", 100);
  httpd_resp_sendstr(req, "OK, rebooting into new slot\n");

  vTaskDelay(pdMS_TO_TICKS(500));  // let the HTTP response actually flush
  esp_restart();
  return ESP_OK;
}

}  // namespace

void ota_register_handlers(httpd_handle_t server) {
  httpd_uri_t otaUri = {};
  otaUri.uri = "/ota";
  otaUri.method = HTTP_POST;
  otaUri.handler = ota_post_handler;
  httpd_register_uri_handler(server, &otaUri);
  ESP_LOGI(TAG, "/ota (POST) ready");
}

#endif  // ESP_PLATFORM
