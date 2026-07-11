// wifi_manager.cpp — STA bring-up + auto-reconnect with bounded backoff.
#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "lwip/ip4_addr.h"
#include <string.h>

static const char* TAG = "wifi";

#define BIT_GOT_IP    (1 << 0)
#define BIT_LOST_IP   (1 << 1)

static EventGroupHandle_t s_evt = nullptr;
static esp_netif_t* s_sta_netif = nullptr;
static volatile bool s_connected = false;
static char s_ssid[33] = {0};
static char s_pass[65] = {0};

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
  } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
    s_connected = false;
    xEventGroupSetBits(s_evt, BIT_LOST_IP);
  }
}

// Reconnect manager: waits for LOST_IP and retries connect with backoff,
// capped at 15s. This avoids the esp_wifi auto-reconnect storm when the AP is
// far away.
static void reconnect_task(void*) {
  int backoff = 1;  // seconds
  while (true) {
    EventBits_t b = xEventGroupWaitBits(s_evt, BIT_LOST_IP, pdTRUE, pdTRUE,
                                        pdMS_TO_TICKS(1000));
    if (b & BIT_LOST_IP) {
      vTaskDelay(pdMS_TO_TICKS(backoff * 1000));
      ESP_LOGI(TAG, "reconnect attempt (backoff=%ds)", backoff);
      esp_wifi_connect();
      backoff = backoff * 2;
      if (backoff > 15) backoff = 15;
    }
    // Reset backoff once connected.
    if (s_connected) backoff = 1;
  }
}

bool wifi_start_sta(const WifiStaConfig* cfg) {
  if (!cfg || !cfg->ssid) return false;

  strncpy(s_ssid, cfg->ssid, sizeof(s_ssid) - 1);
  strncpy(s_pass, cfg->password ? cfg->password : "", sizeof(s_pass) - 1);

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

  xTaskCreate(reconnect_task, "wifi_rc", 2048, nullptr,
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

void wifi_stop(void) {
  esp_wifi_stop();
  esp_wifi_deinit();
  s_connected = false;
}
