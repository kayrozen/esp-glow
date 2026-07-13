// ota_manager.h — F5: OTA over the existing web console's HTTP server, with
// A/B rollback and mandatory self-validation.
//
// The core safety property: a bad image must never brick the device. ESP-
// IDF's bootloader already handles the easy case for free (CONFIG_
// BOOTLOADER_APP_ROLLBACK_ENABLE, see sdkconfig.defaults) — an image that
// crashes/resets before being marked valid gets rolled back automatically
// on the next boot, no app code required. The hard case, and the one this
// file exists for, is the image that *doesn't* crash but also doesn't
// actually work — it boots, app_main returns, and then nothing renders
// (driver stall, a WiFi init regression, a DMX bring-up bug). A naive
// "mark valid immediately after boot" defeats rollback entirely for
// exactly that failure. So "valid" is defined concretely and checked
// before ever cancelling the pending rollback: WiFi has come up, the
// render loop has produced a real run of frames, and DMX bring-up
// succeeded. See ota_manager_tick's implementation for the exact
// definition.
//
// Device-only (real esp_ota_ops / esp_http_server calls) — same status as
// web_input.cpp/midi_input.cpp/osc_input.cpp: the header always parses so
// main.cpp/web_input.cpp can be written against a stable interface, but the
// implementation is `#ifdef ESP_PLATFORM`-guarded and untestable without
// hardware.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef ESP_PLATFORM

// Deliberately NOT `#include "esp_http_server.h"` here: this header is
// included from main.cpp, which only calls the callback/lifecycle
// functions below and never touches httpd_handle_t directly -- only
// web_input.cpp (registering the /ota handler on its already-running
// server) needs the real type, and it gets esp_http_server.h on its own
// (glow_core PRIV_REQUIRES esp_http_server; main does not, and doesn't
// need to just for this). ota_register_handlers takes an opaque `void*`
// here and casts back to httpd_handle_t inside ota_manager.cpp, which
// does include the real header.

#ifdef __cplusplus
extern "C" {
#endif

// Callbacks main.cpp supplies so this module never has to know about
// Show/ShowController/the web console's client-tracking directly — same
// decoupling idiom as web_input.h's FxErrorReplyFn / ScriptListCallback.
struct OtaCallbacks {
  // Called exactly once, with a human-readable reason, immediately before
  // this boot triggers esp_ota_mark_app_invalid_rollback_and_reboot() (see
  // ota_manager_tick) -- main.cpp wires this to glow_safe_blackout so the
  // operator sees why the board is about to reboot into the old slot,
  // instead of it just vanishing and coming back on a different image with
  // no explanation.
  void (*onRollbackImminent)(const char* reason);
  // Polled before accepting an upload. Return true to refuse the request
  // (T4: "refuse OTA while cues are running" -- a reboot mid-set helps
  // nobody). Wired to ShowController::anyActive() in main.cpp.
  bool (*cuesActive)(void);
  // Broadcast a JSON status message (built with buildOtaStatusJson, see
  // web_protocol.h) to every connected console client. Wired to
  // web_ws_broadcast in main.cpp.
  void (*broadcast)(const char* json, size_t len);
};

// Set the callbacks above. Call once, early in app_main -- before the
// render task starts (ota_manager_tick may fire as soon as the first
// frame renders) and well before ota_register_handlers (the /ota POST
// handler needs cuesActive/broadcast from its very first request).
void ota_manager_set_callbacks(const OtaCallbacks* cb);

// Registers the /ota POST handler on the already-running httpd server (see
// web_input.cpp's web_server_task -- OTA reuses that server; it never
// starts a second one). `server` is actually an httpd_handle_t -- see this
// file's header comment for why it's typed as void* here. Call after
// httpd_start.
void ota_register_handlers(void* server);

// Call once at boot (main.cpp, before the render task starts): checks
// whether this boot is running a pending-verify OTA slot (i.e. this is the
// first boot after an OTA write) and, if so, arms the self-validation
// deadline. A normal boot of an already-valid image is a no-op.
void ota_manager_init(void);

// Self-validation criteria, fed from wherever each is actually known true
// so this module never has to guess at state owned elsewhere:
//   - note_frame_rendered: call once per rendered frame (render_tick_hooks,
//     post phase). Requires a real run of frames, not just one, so a
//     render loop that renders once and then wedges doesn't pass.
//   - note_wifi_connected: call whenever wifi_is_connected() is observed
//     true (main.cpp polls this once per frame too). Sticky: once WiFi has
//     come up at all this boot, this criterion stays satisfied even if it
//     later drops -- a venue AP flapping after boot is a WiFi-manager
//     concern (T2), not evidence this OTA image is bad.
//   - note_dmx_ready: call once, right after DmxSink::begin() succeeds.
void ota_manager_note_frame_rendered(void);
void ota_manager_note_wifi_connected(void);
void ota_manager_note_dmx_ready(void);

// Call once per frame (render_tick_hooks, post phase), after the note_*
// calls above for that frame. No-op unless this boot is a pending-verify
// OTA slot that hasn't validated yet. Once every criterion is satisfied,
// marks this image valid (cancelling the pending rollback) and never does
// anything again for the rest of this boot. If the validation deadline
// passes first, invokes cb.onRollbackImminent with a diagnosis, then
// triggers esp_ota_mark_app_invalid_rollback_and_reboot() -- which reboots
// the device into the previous (known-good) slot and does not return.
void ota_manager_tick(void);

#ifdef __cplusplus
}
#endif

#endif  // ESP_PLATFORM
