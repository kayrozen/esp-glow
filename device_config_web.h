// device_config_web.h — §6: reconfigure without reflashing. Exposes the
// "devcfg" partition (CFG1, see device_config.h) over the same httpd
// server the web console already runs (GET/POST /devcfg), so changing
// WiFi/DMX GPIOs/Art-Net fallback/USB-MIDI does not require a USB cable.
//
// Safety (§6's "gate it" requirement): a bad devcfg written over the
// network could make the device unreachable (wrong SSID/password, wrong
// DMX pins). POST validates with the exact same parseDeviceConfig the
// boot-time loader uses BEFORE ever writing flash -- a blob that fails to
// parse is rejected with 400 and the partition is left untouched. The
// CRC-fallback path (device_config.h) is the safety net for the case this
// can't catch (a valid-looking config for the wrong network): a corrupt
// or unreachable-in-practice devcfg still boots on Kconfig defaults next
// time, rather than bricking. Changing WiFi always requires the new
// config to actually be written+rebooted-into to take effect, same as any
// other reconfigure.
//
// Device-only (real esp_partition_write / esp_http_server calls) -- same
// status as ota_manager.h: the header always parses so main.cpp/
// web_input.cpp can be written against a stable interface, but the
// implementation is `#ifdef ESP_PLATFORM`-guarded and untestable without
// hardware. The format itself (parseDeviceConfig/encodeDeviceConfig) IS
// host-tested -- see device_config.h/device_config_encoder.h and
// test_device_config.cpp.
#pragma once

#include "device_config.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM

// Deliberately NOT `#include "esp_http_server.h"` here -- same reasoning
// as ota_manager.h: only device_config_web.cpp (registering the /devcfg
// handlers on the already-running server) needs the real httpd_handle_t
// type. device_config_web_register_handlers takes an opaque `void*` and
// casts back inside the .cpp, which does include the real header.

#ifdef __cplusplus
extern "C" {
#endif

// Callbacks main.cpp supplies -- same decoupling idiom as OtaCallbacks
// (ota_manager.h).
struct DeviceConfigWebCallbacks {
  // Polled before accepting a POST. Return true to refuse the request
  // (same rationale as OTA's cuesActive: rebooting mid-show helps
  // nobody). Wired to ShowController::anyActive() in main.cpp.
  bool (*cuesActive)(void);
};

// Set the callbacks above. Call once, early in app_main, before
// device_config_web_register_handlers.
void device_config_web_set_callbacks(const DeviceConfigWebCallbacks* cb);

// Records the config that actually took effect THIS boot (whether read
// from devcfg or the compiled-in Kconfig defaults -- see main.cpp's
// app_main) so GET /devcfg has something to serialize. Call once, right
// after main.cpp resolves `cfg`, before the httpd server (and therefore
// GET /devcfg) can possibly be reached.
void device_config_web_set_effective_config(const DeviceConfig& cfg);

// Registers the GET/POST /devcfg handlers on the already-running httpd
// server (see web_input.cpp's web_server_task -- reuses that server, same
// as ota_register_handlers). `server` is actually an httpd_handle_t -- see
// this file's header comment for why it's typed as void* here. Call after
// httpd_start.
void device_config_web_register_handlers(void* server);

#ifdef __cplusplus
}
#endif

#endif  // ESP_PLATFORM
