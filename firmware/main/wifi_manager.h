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
  // F5: if true, after kApFallbackThreshold (wifi_manager.cpp) consecutive
  // failed STA reconnect attempts, bring up a SoftAP (mode APSTA) alongside
  // the still-retrying STA link, so the console stays reachable at the
  // venue even when the venue's own WiFi is gone or was never right in the
  // first place. STA reconnect attempts keep happening in the background;
  // the AP is never torn down once started (simplest correct behavior —
  // the alternative, tearing it down on STA reconnect, risks yanking the
  // console out from under someone mid-session).
  //
  // HIL FLAG: SoftAP + DMX timing coexistence has not been verified on
  // hardware. AP mode adds beacon/probe-response airtime on the same radio
  // driver that also carries Art-Net; DMX itself is a dedicated UART and
  // should be unaffected, but this needs a real soak test before being
  // trusted at a gig with SoftAP enabled.
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

// True once the SoftAP fallback (see WifiStaConfig::ap_fallback) has been
// started this boot.
bool wifi_ap_fallback_active(void);

// Stop WiFi cleanly (used by OTA to swap modes if needed).
void wifi_stop(void);

#ifdef __cplusplus
}
#endif
