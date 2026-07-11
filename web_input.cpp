<<<<<<< ours
#ifdef ESP_PLATFORM

//
// Web console input — device scaffold.
//
// The testable JSON core lives in web_protocol.h/.cpp (host-tested, no ESP-IDF
// dependency). This file is the device-only glue: it owns the WebSocket
// server, calls parseWebCommand on each incoming frame, pushes the resulting
// ControlEvent to the control-event queue, and sends `config` (and optionally
// `state`) messages back to the UI.
//
// The render task drains the queue via pumpControlEvents() at the top of
// each frame and dispatches to LiveControl — the web transport no longer
// touches LiveControl/ShowController directly, eliminating the cross-core
// data race. See control_queue.h for the rationale.
//
// Hardware wiring (httpd ws endpoint, LittleFS/SPIFFS file serving) is left
// as `// TODO` for the same reasons as midi_input.cpp and osc_input.cpp:
// it cannot be verified without hardware.
//
// Architecture:
//   - The device's setup code calls web_input_init(queue, cues, nCues,
//     scenes, nScenes, hasMaster) once. The queue and cues/scenes arrays
//     are borrowed for the lifetime of the server.
//   - web_input_handle_text_frame(json, len) is called per inbound
//     WebSocket text frame. It runs the testable parser, then:
//       cue/scene/master -> queue.push(ev)
//       hello            -> caller sends config back to this client
//   - web_server_task() is the FreeRTOS task that owns the httpd ws server.
//     It calls the helpers above; its body is TODO.
//

#include "web_protocol.h"
#include "control_queue.h"   // IControlEventQueue, ControlEvent (transitively)
#include "live_control.h"    // ControlType (for parseWebCommand's out param)

#include <cstdint>
#include <cstring>

// --- borrowed state set up at init time ----------------------------------

static IControlEventQueue* g_queue      = nullptr;
static const WebCueInfo*   g_cues       = nullptr;
static size_t              g_nCues      = 0;
static const WebSceneInfo* g_scenes     = nullptr;
static size_t              g_nScenes    = 0;
static bool                g_hasMaster  = false;

// TODO: add an httpd_handle_t / socket set here when the real server lands.
// static httpd_handle_t g_server = nullptr;

extern "C" {

// Initialize the web input layer with the control-event queue it pushes
// to, plus the cue/scene metadata used to build `config` messages. The
// queue and cues/scenes arrays are borrowed (not copied); they must
// outlive the server.
void web_input_init(IControlEventQueue& queue,
                    const WebCueInfo* cues, size_t nCues,
                    const WebSceneInfo* scenes, size_t nScenes,
                    bool hasMaster) {
  g_queue      = &queue;
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

// Handle one inbound WebSocket text frame.
//
// Returns:
//   1  - message was a cue/scene/master command; pushed to the queue
//   0  - message was `hello`; caller should respond by sending `config`
//   -1 - message was malformed or unknown; caller should ignore
int web_input_handle_text_frame(const char* json, size_t len) {
  if (g_queue == nullptr) return -1;

  // Fast path: `hello` is handled here, not by parseWebCommand (the parser
  // is strict and returns false for hello, since hello isn't a ControlEvent).
  // We detect it cheaply first; if it's not hello, fall through to the
  // testable core.
  if (isHelloCommand(json, len)) {
    return 0;
  }

  ControlEvent ev;
  if (!parseWebCommand(json, len, ev)) {
    return -1;
  }

  g_queue->push(ev);
  return 1;
}

// --- WebSocket server task (hardware-specific, scaffold only) -------------

void web_server_task() {
  // TODO: Start an ESP-IDF httpd instance with a WebSocket handler on a
  // known URI (e.g. "/ws"). For each inbound text frame:
  //
  //   int rc = web_input_handle_text_frame(frame, len);
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
  //
  // The render task (separate from this httpd task) calls
  //   pumpControlEvents(queue, live, t);
  //   show.renderFrame(t);
  // at the top of each frame. The web transport just pushes events here;
  // it never touches LiveControl or ShowController.
}

}  // extern "C"
=======
// web_input.cpp — device-only HTTP + WebSocket server for the web console.
#ifdef ESP_PLATFORM

#include "web_input.h"
#include "web_input_handler.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include <cstdio>
#include <cstring>
#include <cstdio>

static const char* TAG = "web_input";

static LiveControl*      s_live = nullptr;
static httpd_handle_t    s_server = nullptr;
static int               s_ws_fd = -1;   // latest connected client fd

static float now_sec() { return (float)(esp_timer_get_time() / 1000000.0); }

// --- WebSocket handler at /ws ---
static esp_err_t ws_handler(httpd_req_t* req) {
  // On the initial GET (the WS upgrade), esp_http_server sends the 101 and
  // then invokes this handler once. We use that first call to push the config
  // snapshot and record the fd for later broadcasts.
  s_ws_fd = httpd_req_to_sockfd(req);

  if (s_live) {
    char buf[512];
    size_t n = web_input_build_config(*s_live, buf, sizeof(buf));
    if (n > 0) {
      httpd_ws_frame_t f = {};
      f.type = HTTPD_WS_TYPE_TEXT;
      f.payload = (uint8_t*)buf;
      f.len = n;
      f.final = true;
      httpd_ws_send_frame(req, &f);
    }
  }

  // Read one inbound frame (if any). The first call after upgrade may have no
  // pending frame; recv with len 0 peeks the size.
  httpd_ws_frame_t f = {};
  f.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t e = httpd_ws_recv_frame(req, &f, 0);
  if (e != ESP_OK || f.len == 0) {
    return ESP_OK;  // no payload yet (upgrade-only invocation)
  }
  if (f.len > 256) {
    // Oversized frame: drain and ignore.
    uint8_t tmp[256];
    f.payload = tmp;
    httpd_ws_recv_frame(req, &f, sizeof(tmp));
    return ESP_OK;
  }
  char text[257] = {0};
  f.payload = (uint8_t*)text;
  e = httpd_ws_recv_frame(req, &f, sizeof(text) - 1);
  if (e != ESP_OK) return ESP_OK;
  text[f.len] = 0;

  if (s_live) {
    web_input_handle_text_frame(text, *s_live, now_sec());
  }
  return ESP_OK;
}

// --- Static file handler: /littlefs/console/<uri> ---
static esp_err_t file_handler(httpd_req_t* req) {
  const char* uri = req->uri;
  if (strcmp(uri, "/") == 0) uri = "/index.html";

  char path[80];
  snprintf(path, sizeof(path), "/littlefs/console%s", uri);

  FILE* fp = fopen(path, "rb");
  if (!fp) {
    httpd_resp_send_404(req);
    return ESP_OK;
  }

  if (strstr(uri, ".html"))      httpd_resp_set_type(req, "text/html");
  else if (strstr(uri, ".js"))   httpd_resp_set_type(req, "application/javascript");
  else if (strstr(uri, ".css"))  httpd_resp_set_type(req, "text/css");
  else if (strstr(uri, ".json")) httpd_resp_set_type(req, "application/json");
  else                            httpd_resp_set_type(req, "application/octet-stream");

  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
    if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) break;
  }
  fclose(fp);
  httpd_resp_send_chunk(req, nullptr, 0);
  return ESP_OK;
}

bool web_input_start(const WebInputConfig* cfg) {
  if (!cfg || !cfg->live) return false;
  s_live = cfg->live;

  httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
  hc.server_port = cfg->port ? cfg->port : 80;
  hc.max_uri_handlers = 8;
  hc.lru_purge_enable = true;
  hc.stack_size = 6144;

  esp_err_t e = httpd_start(&s_server, &hc);
  if (e != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(e));
    return false;
  }

  httpd_uri_t ws = {};
  ws.uri = "/ws";
  ws.method = HTTP_GET;
  ws.handler = ws_handler;
  ws.is_websocket = true;
  ws.support_ws_subprotocol = nullptr;
  httpd_register_uri_handler(s_server, &ws);

  httpd_uri_t files = {};
  files.uri = "/*";
  files.method = HTTP_GET;
  files.handler = file_handler;
  httpd_register_uri_handler(s_server, &files);

  ESP_LOGI(TAG, "web console on http://<ip>:%u/  (ws at /ws)", hc.server_port);
  return true;
}

void web_input_stop(void) {
  if (s_server) {
    httpd_stop(s_server);
    s_server = nullptr;
    s_ws_fd = -1;
  }
}

void web_input_broadcast_state(const char* json) {
  if (!json || s_ws_fd < 0 || !s_server) return;
  httpd_ws_frame_t f = {};
  f.type = HTTPD_WS_TYPE_TEXT;
  f.payload = (uint8_t*)json;
  f.len = strlen(json);
  f.final = true;
  httpd_ws_send_frame_async(s_server, s_ws_fd, &f);
}
>>>>>>> theirs

#endif  // ESP_PLATFORM
