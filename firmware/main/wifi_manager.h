// wifi_manager.h — minimal WiFi STA bring-up with auto-reconnect.
//
// F2 scope: connect to a configured SSID as a station, get an IP, and keep
// reconnecting with bounded backoff if the link drops. AP-fallback is a stub
// the user can enable for field provisioning (see TODO).
//
// Coexistence: WiFi/lwIP tasks run on core 0 (sdkconfig
// CONFIG_ESP_WIFI_TASK_CORE_AFFINITY_0). The DMX render task on core 1 is
// unaffected.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WifiStaConfig {
  const char* ssid;
  const char* password;
  // If true and STA fails to connect after the first attempt, start a softAP
  // so the device is still reachable for field provisioning. (F2 stub.)
  bool ap_fallback;
};

// Initialise the WiFi subsystem in STA mode and begin connecting. Returns
// true if the STA started (not necessarily connected yet — watch
// wifi_is_connected()).
bool wifi_start_sta(const struct WifiStaConfig* cfg);

// True if we currently have an IP. The Art-Net sink is only useful while this
// is true; the render task keeps flushing DMX regardless.
bool wifi_is_connected(void);

// Fill out_ip with our current IPv4 if connected. Returns false if not up.
bool wifi_get_ip(uint32_t* out_ip);

// Stop WiFi cleanly (used by OTA to swap modes if needed).
void wifi_stop(void);

#ifdef __cplusplus
}
#endif
