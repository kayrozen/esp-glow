#ifdef ESP_PLATFORM

//
// Web console input — device scaffold.
//
// The testable JSON core lives in web_protocol.h/.cpp (host-tested, no ESP-IDF
// dependency). This file is the device-only glue: it owns the WebSocket
// server, calls parseWebCommand on each incoming frame, feeds the resulting
// ControlEvent into LiveControl, and pushes `config` (and optionally
// `state`) messages back to the UI.
//
// Hardware wiring (httpd ws endpoint, LittleFS/SPIFFS file serving, the
// actual `now` clock) is left as `// TODO` for the same reasons as
// midi_input.cpp and osc_input.cpp: it cannot be verified without hardware.
//
// Architecture:
//   - The device's setup code calls web_input_init(live, cues, nCues,
//     scenes, nScenes, hasMaster) once. The cues/scenes arrays are borrowed
//     for the lifetime of the server; they back every `config` push.
//   - web_input_handle_text_frame(json, len, now) is called per inbound
//     WebSocket text frame. It runs the testable parser, then dispatches:
//       cue/scene/master -> LiveControl::handle(ev, now)
//       hello            -> send config back to this client
//   - web_server_task() is the FreeRTOS task that owns the httpd ws server.
//     It calls the helpers above; its body is TODO.
//

#include "web_protocol.h"
#include "live_control.h"

#include <cstdint>
#include <cstring>

// --- borrowed state set up at init time ----------------------------------

static LiveControl*       g_live       = nullptr;
static const WebCueInfo*  g_cues       = nullptr;
static size_t             g_nCues      = 0;
static const WebSceneInfo* g_scenes    = nullptr;
static size_t             g_nScenes    = 0;
static bool               g_hasMaster  = false;

// TODO: add an httpd_handle_t / socket set here when the real server lands.
// static httpd_handle_t g_server = nullptr;

extern "C" {

// Initialize the web input layer with the LiveControl instance it dispatches
// into, plus the cue/scene metadata used to build `config` messages. The
// cues/scenes arrays are borrowed (not copied); they must outlive the server.
void web_input_init(LiveControl& live,
                    const WebCueInfo* cues, size_t nCues,
                    const WebSceneInfo* scenes, size_t nScenes,
                    bool hasMaster) {
  g_live       = &live;
  g_cues       = cues;
  g_nCues      = nCues;
  g_scenes     = scenes;
  g_nScenes    = nScenes;
  g_hasMaster  = hasMaster;
}

// Build a `config` message into `buf`. Returns bytes written (excluding the
// NUL) or the would-be length if `bufLen` is too small. Caller may pass
// buf=nullptr,bufLen=0 to measure.
size_t web_input_build_config(char* buf, size_t bufLen) {
  return buildConfigJson(g_cues, g_nCues, g_scenes, g_nScenes, g_hasMaster,
                         buf, bufLen);
}

// Build a `state` message into `buf` listing currently-active cue ids.
// `activeIds` may be nullptr if nActive == 0.
size_t web_input_build_state(const uint16_t* activeIds, size_t nActive,
                             char* buf, size_t bufLen) {
  return buildStateJson(activeIds, nActive, buf, bufLen);
}

// Handle one inbound WebSocket text frame. `now` is the current show time
// in seconds (the same clock ShowController::evaluate uses).
//
// Returns:
//   1  - message was a cue/scene/master command; dispatched to LiveControl
//   0  - message was `hello`; caller should respond by sending `config`
//   -1 - message was malformed or unknown; caller should ignore
int web_input_handle_text_frame(const char* json, size_t len, float now) {
  if (g_live == nullptr) return -1;

  // Fast path: `hello` is handled here, not by parseWebCommand (the parser
  // is strict and returns false for hello, since hello isn't a ControlEvent).
  // We detect it cheaply first; if it's not hello, fall through to the
  // testable core.
  if (len >= 14 && std::memcmp(json, "{\"type\":\"hello\"", 15) == 0) {
    return 0;
  }

  ControlEvent ev;
  if (!parseWebCommand(json, len, ev)) {
    return -1;
  }

  g_live->handle(ev, now);
  return 1;
}

// --- WebSocket server task (hardware-specific, scaffold only) -------------

void web_server_task() {
  // TODO: Start an ESP-IDF httpd instance with a WebSocket handler on a
  // known URI (e.g. "/ws"). For each inbound text frame:
  //
  //   float now = currentShowTimeSeconds();
  //   int rc = web_input_handle_text_frame(frame, len, now);
  //   if (rc == 0) {
  //     // hello: push config back to this client only
  //     char buf[512];
  //     size_t n = web_input_build_config(buf, sizeof(buf));
  //     httpd_ws_send_frame(req, buf, n, TEXT);
  //   }
  //
  // On a new client connection (before any message), also push config.
  //
  // Optionally (Phase 4) push `state` whenever the show controller's
  // active-cue set changes; the controller already knows this set.
  //
  // The static files for the Phase-2 console bundle (index.html, app.js,
  // styles.css, vendor/) live in LittleFS/SPIFFS and are served from a
  // separate httpd URI handler that maps "/" -> "/spiffs/index.html" etc.
}

}  // extern "C"

#endif  // ESP_PLATFORM
