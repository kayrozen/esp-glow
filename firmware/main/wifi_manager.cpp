// wifi_manager.cpp — STA bring-up + auto-reconnect with bounded backoff,
// plus an optional SoftAP fallback (F5).
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "wifi";

#define BIT_GOT_IP    (1 << 0)
#define BIT_LOST_IP   (1 << 1)

// F5: backoff caps at 30s (was 15s) -- see T2: "never a tight retry loop".
// A 30s ceiling still notices a returning AP within a reasonable window
// without flooding the log or spending meaningful core-0 time that could
// crowd WiFi/lwIP housekeeping (DMX itself is on core 1 and structurally
// can't be affected either way -- see render_task.cpp).
static constexpr int kBackoffCapSec = 30;

// F5: after this many consecutive failed reconnect attempts (with the
// backoff above, that's on the order of several minutes), bring up the
// SoftAP fallback if WifiStaConfig::ap_fallback was requested. See its
// header comment for the HIL flag on AP+DMX coexistence.
static constexpr uint32_t kApFallbackThreshold = 10;

static const char* kApSsidPrefix = "esp-glow-setup";

static EventGroupHandle_t s_evt = nullptr;
static esp_netif_t* s_sta_netif = nullptr;
static esp_netif_t* s_ap_netif = nullptr;
static volatile bool s_connected = false;
static volatile bool s_ap_active = false;
static char s_ssid[33] = {0};
static char s_pass[65] = {0};
static bool s_ap_fallback_requested = false;

// F5 telemetry: consecutive failed attempts since the last successful
// connect (reset to 0 on IP_EVENT_STA_GOT_IP). Read by the reconnect task
// and by the event handler's telemetry log line.
static volatile uint32_t s_attempts = 0;

static void start_ap_fallback(void) {
  if (s_ap_active) return;
  s_ap_active = true;

  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  char apSsid[40];
  std::snprintf(apSsid, sizeof(apSsid), "%s-%02x%02x", kApSsidPrefix, mac[4], mac[5]);

  if (!s_ap_netif) {
    s_ap_netif = esp_netif_create_default_wifi_ap();
  }

  wifi_config_t apCfg = {};
  std::strncpy((char*)apCfg.ap.ssid, apSsid, sizeof(apCfg.ap.ssid) - 1);
  apCfg.ap.ssid_len = (uint8_t)std::strlen(apSsid);
  apCfg.ap.channel = 1;
  apCfg.ap.max_connection = 4;
  // Reuse the STA password if it's WPA2-legal (>=8 chars); otherwise fall
  // back to an open network rather than reject a shorter provisioning
  // password outright -- reachability for recovery matters more here than
  // keeping this fallback network private (see the HIL flag: this is a
  // "get back into the console" path, not the production network).
  if (std::strlen(s_pass) >= 8) {
    std::strncpy((char*)apCfg.ap.password, s_pass, sizeof(apCfg.ap.password) - 1);
    apCfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  } else {
    apCfg.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &apCfg));
  ESP_LOGW(TAG, "GLOW-TEST: wifi softAP fallback started ssid=%s (STA still retrying "
                "in the background)", apSsid);
}

static void on_event(void* arg, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        ESP_LOGI(TAG, "STA started, connecting...");
        break;
      case WIFI_EVENT_STA_DISCONNECTED: {
        s_connected = false;
        xEventGroupSetBits(s_evt, BIT_LOST_IP);
        // Reconnect with bounded backoff handled in the manager task below.
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)data;
        ESP_LOGW(TAG, "disconnected (reason=%d); will retry", d->reason);
        break;
      }
      default: break;
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* ev = (ip_event_got_ip_t*)data;
    s_connected = true;
    xEventGroupSetBits(s_evt, BIT_GOT_IP);
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&ev->ip_info.ip));
    ESP_LOGI(TAG, "GLOW-TEST: wifi state=connected attempts=%u", (unsigned)s_attempts);
    s_attempts = 0;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
    s_connected = false;
    xEventGroupSetBits(s_evt, BIT_LOST_IP);
  }
}

// Reconnect manager: waits for LOST_IP and retries connect with backoff,
// capped at kBackoffCapSec. This avoids the esp_wifi auto-reconnect storm
// when the AP is far away or gone entirely.
//
// F5: the rig must keep rendering DMX regardless of what happens here --
// this task never touches Show/ShowController/the render loop, never
// blocks longer than one backoff sleep at a time, and runs at low priority
// on whatever core FreeRTOS schedules it on (WiFi/lwIP's own core 0 per
// sdkconfig, see the header comment) -- it structurally cannot starve the
// core-1 render task. A dropped AP must never blackout the rig (see
// safe_blackout.h): nothing here calls into that path, on purpose. DMX is
// local and Art-Net is best-effort UDP; only the web console and Art-Net
// output actually depend on the link being up.
static void reconnect_task(void*) {
  int backoff = 1;  // seconds
  while (true) {
    EventBits_t b = xEventGroupWaitBits(s_evt, BIT_LOST_IP, pdTRUE, pdTRUE,
                                        pdMS_TO_TICKS(1000));
    if (b & BIT_LOST_IP) {
      vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
      // Plain read + plain store, not `s_attempts++` -- compound increment
      // on a volatile-qualified operand is deprecated as of C++20 (P1152R4)
      // and this toolchain's default C++ standard rejects it under -Werror.
      s_attempts = s_attempts + 1;
      ESP_LOGI(TAG, "reconnect attempt (backoff=%ds)", backoff);
      ESP_LOGI(TAG, "GLOW-TEST: wifi state=retrying attempts=%u", (unsigned)s_attempts);
      esp_wifi_connect();
      backoff = backoff * 2;
      if (backoff > kBackoffCapSec) backoff = kBackoffCapSec;

      if (s_ap_fallback_requested && !s_ap_active && s_attempts >= kApFallbackThreshold) {
        start_ap_fallback();
      }
    }
    // Reset backoff once connected.
    if (s_connected) backoff = 1;
  }
}

bool wifi_start_sta(const WifiStaConfig* cfg) {
  if (!cfg || !cfg->ssid) return false;

  strncpy(s_ssid, cfg->ssid, sizeof(s_ssid) - 1);
  strncpy(s_pass, cfg->password ? cfg->password : "", sizeof(s_pass) - 1);
  s_ap_fallback_requested = cfg->ap_fallback;

  s_evt = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  s_sta_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&ic));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_LOST_IP, &on_event, nullptr, nullptr));

  wifi_config_t wc = {};
  strcpy((char*)wc.sta.ssid, s_ssid);
  strcpy((char*)wc.sta.password, s_pass);
  wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());

  xTaskCreate(reconnect_task, "wifi_rc", 2560, nullptr,
              tskIDLE_PRIORITY + 1, nullptr);
  return true;
}

bool wifi_is_connected(void) { return s_connected; }

bool wifi_get_ip(uint32_t* out_ip) {
  if (!s_connected || !s_sta_netif) return false;
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK) return false;
  if (out_ip) *out_ip = ip.ip.addr;  // host-byte-order field; ntohl at call site
  return true;
}

bool wifi_ap_fallback_active(void) { return s_ap_active; }

void wifi_stop(void) {
  esp_wifi_stop();
  esp_wifi_deinit();
  s_connected = false;
  s_ap_active = false;
}
