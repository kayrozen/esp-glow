#ifdef ESP_PLATFORM

//
// Web console input — device scaffold.
//
// The testable JSON core lives in web_protocol.h/.cpp (host-tested, no ESP-IDF
// dependency). This file is the device-only glue: it owns the WebSocket
// server, calls the web_protocol parsers on each incoming frame, pushes
// control events / eval submissions onto their respective queues, hits
// scripts_storage.h directly for script CRUD (LittleFS is safe to touch
// from any task, unlike the single-owner Lua VM -- see eval_queue.h), and
// sends `config`/`scripts`/`script`/`eval_result`/`fx_error` messages back
// to the UI.
//
// The render task drains the control-event queue via pumpControlEvents()
// and the eval queue via glow::pumpEvalSubmissions() at the top of each
// frame (see control_queue.h, glow_fennel.h) — the web transport never
// touches LiveControl/ShowController/the Lua VM directly, eliminating the
// cross-core data races those single-owner disciplines exist to prevent.
// fx_error notifications are polled the same way, once per frame, via
// GlowLuaApi::pollNewlyDisabledEffects (glow_lua_api.h) — see
// web_input_poll_fx_error below.
//
// The httpd WS endpoint and the console's static-file serving are real
// (F4 T3) -- see web_server_task below. Untestable without hardware, same
// status as midi_input.cpp / osc_input.cpp's transports.
//

#include "web_input.h"

#include "web_protocol.h"
#include "control_queue.h"   // IControlEventQueue, ControlEvent (transitively)
#include "eval_queue.h"      // IEvalSubmissionQueue, EvalSubmission
#include "glow_lua_api.h"    // GlowLuaApi::pollNewlyDisabledEffects (web_input_poll_fx_error)
#include "live_control.h"    // ControlType (for parseWebCommand's out param)
#include "scripts_storage.h"
#include "ota_manager.h"     // F5: /ota registers onto this same httpd server

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

static const char* TAG = "web_input";

// --- borrowed state set up at init time ----------------------------------

static IControlEventQueue*   g_controlQueue = nullptr;
static IEvalSubmissionQueue* g_evalQueue    = nullptr;
static const WebCueInfo*     g_cues         = nullptr;
static size_t                g_nCues        = 0;
static const WebSceneInfo*   g_scenes       = nullptr;
static size_t                g_nScenes      = 0;
static bool                  g_hasMaster    = false;

static httpd_handle_t g_server = nullptr;

// --- WS client tracking ----------------------------------------------------
//
// A tiny fixed-size fd list, written by the httpd (WS) task on connect and
// read by the render task via web_ws_broadcast (post-render, alongside
// gcStepSlack/web_input_poll_fx_error -- see main.cpp). Protected by a
// FreeRTOS mutex: it's the only state shared across that boundary here, and
// it's small enough that a plain mutex is simpler than anything lock-free.
// Dead fds are dropped lazily, on the next failed broadcast send -- there's
// no eager on-close callback (see web_ws_broadcast).

constexpr int kMaxWsClients = 8;
static int g_wsClientFds[kMaxWsClients];
static bool g_wsClientUsed[kMaxWsClients];
static SemaphoreHandle_t g_wsClientsMutex = nullptr;

static void wsClientAdd(int fd) {
  xSemaphoreTake(g_wsClientsMutex, portMAX_DELAY);
  for (int i = 0; i < kMaxWsClients; ++i) {
    if (!g_wsClientUsed[i]) {
      g_wsClientUsed[i] = true;
      g_wsClientFds[i] = fd;
      break;
    }
  }
  xSemaphoreGive(g_wsClientsMutex);
}

static void wsClientRemove(int fd) {
  xSemaphoreTake(g_wsClientsMutex, portMAX_DELAY);
  for (int i = 0; i < kMaxWsClients; ++i) {
    if (g_wsClientUsed[i] && g_wsClientFds[i] == fd) {
      g_wsClientUsed[i] = false;
      break;
    }
  }
  xSemaphoreGive(g_wsClientsMutex);
}

// Lists every script and builds a `scripts` reply directly into buf --
// shared by script_list and by script_save/script_delete, both of which
// reply with the refreshed list so the sidebar updates in one round trip
// instead of the client re-requesting it. Caps at kMaxScripts entries
// (a pathological number of files on the partition truncates the reply
// rather than growing it unboundedly).
static size_t buildRefreshedScriptsReply(char* buf, size_t bufCap) {
  constexpr size_t kMaxScripts = 64;
  static char names[kMaxScripts][256];
  static const char* namePtrs[kMaxScripts];
  struct Collector {
    size_t count = 0;
    static bool cb(const char* name, void* ctx) {
      auto* self = static_cast<Collector*>(ctx);
      if (self->count >= kMaxScripts) return false;
      std::snprintf(names[self->count], sizeof(names[self->count]), "%s", name);
      namePtrs[self->count] = names[self->count];
      ++self->count;
      return true;
    }
  } collector;
  scripts_storage_list(&Collector::cb, &collector);
  return buildScriptsJson(namePtrs, collector.count, buf, bufCap);
}

// Initialize the web input layer with the queues it pushes to, plus the
// cue/scene metadata used to build `config` messages. All pointers are
// borrowed (not copied); they must outlive the server.
void web_input_init(IControlEventQueue& controlQueue, IEvalSubmissionQueue& evalQueue,
                    const WebCueInfo* cues, size_t nCues,
                    const WebSceneInfo* scenes, size_t nScenes,
                    bool hasMaster) {
  g_controlQueue = &controlQueue;
  g_evalQueue    = &evalQueue;
  g_cues         = cues;
  g_nCues        = nCues;
  g_scenes       = scenes;
  g_nScenes      = nScenes;
  g_hasMaster    = hasMaster;
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

// WebInputAction is declared in web_input.h.

// Handle one inbound WebSocket text frame. Script CRUD (list/load/save/
// delete) is handled synchronously right here on the WS task -- LittleFS,
// unlike the Lua VM, has no single-owner requirement (see eval_queue.h's
// header for why *eval* still has to go through a queue while this
// doesn't). `outBuf`/`outBufCap` are scratch space for WEB_INPUT_REPLY;
// `*outLen` is set to the reply's length when that's returned.
WebInputAction web_input_handle_text_frame(const char* json, size_t len,
                                           char* outBuf, size_t outBufCap, size_t* outLen) {
  if (g_controlQueue == nullptr) return WEB_INPUT_IGNORED;

  // `hello` is handled here, not by parseWebCommand (the parser is strict
  // and returns false for hello, since hello isn't a ControlEvent).
  if (isHelloCommand(json, len)) {
    return WEB_INPUT_SEND_HELLO;
  }

  ControlEvent ev;
  if (parseWebCommand(json, len, ev)) {
    g_controlQueue->push(ev);
    return WEB_INPUT_CONTROL_EVENT;
  }

  // Eval: enqueue for the render task. Never evaluate Fennel here -- the
  // VM is owned by the render task; see eval_queue.h.
  {
    uint32_t seq = 0;
    char srcBuf[EVAL_SRC_MAX];
    size_t srcLen = 0;
    if (parseEvalCommand(json, len, seq, srcBuf, sizeof(srcBuf), srcLen)) {
      if (g_evalQueue != nullptr) {
        EvalSubmission sub;
        if (makeEvalSubmission(srcBuf, srcLen, seq, sub)) {
          g_evalQueue->push(sub);
        }
      }
      return WEB_INPUT_EVAL_QUEUED;
    }
  }

  if (isScriptListCommand(json, len)) {
    *outLen = buildRefreshedScriptsReply(outBuf, outBufCap);
    return WEB_INPUT_REPLY;
  }

  {
    const char* opType = nullptr;
    char name[256];
    if (parseScriptNameCommand(json, len, &opType, name, sizeof(name))) {
      if (std::strcmp(opType, "load") == 0) {
        char src[EVAL_SRC_MAX];
        size_t srcLen = 0;
        bool ok = scripts_storage_load(name, src, sizeof(src), &srcLen);
        *outLen = buildScriptJson(name, ok ? src : "", ok ? srcLen : 0, outBuf, outBufCap);
        return WEB_INPUT_REPLY;
      }
      if (std::strcmp(opType, "delete") == 0) {
        scripts_storage_delete(name);
        *outLen = buildRefreshedScriptsReply(outBuf, outBufCap);
        return WEB_INPUT_REPLY;
      }
    }
  }

  {
    char name[256];
    char src[EVAL_SRC_MAX];
    size_t srcLen = 0;
    if (parseScriptSaveCommand(json, len, name, sizeof(name), src, sizeof(src), srcLen)) {
      scripts_storage_save(name, src, srcLen);
      *outLen = buildRefreshedScriptsReply(outBuf, outBufCap);
      return WEB_INPUT_REPLY;
    }
  }

  return WEB_INPUT_IGNORED;
}

// Called once per frame (render task frame slack, alongside
// pumpControlEvents/pumpEvalSubmissions/gcStepSlack) to check for effects
// that just broke. GlowLuaApi::pollNewlyDisabledEffects marks EVERY
// currently-disabled-and-unreported effect as reported in one call (see
// its own header comment) -- so onFxError is invoked once per newly-
// disabled effect found this poll, not just the first; a version of this
// function that returned only one message per call and dropped the rest
// would silently lose any effect that broke in the same frame as another
// one (see test_fx_error_pipeline.cpp's two-effects-in-one-frame test,
// which exists specifically to catch that regression). Same bounded-work-
// via-callback idiom as glow::pumpEvalSubmissions/pumpControlEvents.
//
// `json`/`len` passed to onFxError is already a complete `fx_error`
// message (web_protocol.h's buildFxErrorJson); the caller's contract is to
// broadcast it to every connected client (not just one), since any of
// them may be running the live REPL that needs to see it. Returns the
// number of notifications delivered this call. FxErrorReplyFn is declared
// in web_input.h.
int web_input_poll_fx_error(GlowLuaApi& api, FxErrorReplyFn onFxError, void* ctx) {
  std::vector<std::pair<std::string, std::string>> notifications;
  api.pollNewlyDisabledEffects(notifications);
  for (const auto& n : notifications) {
    char buf[512];
    size_t len = buildFxErrorJson(n.first.c_str(), n.second.c_str(), buf, sizeof(buf));
    onFxError(ctx, buf, len);
  }
  return (int)notifications.size();
}

// Sends `json` to every tracked client via httpd_ws_send_frame_async (safe
// to call from the render task -- unlike httpd_ws_send_frame, the _async
// variant queues the send onto the httpd worker instead of running it
// inline, so this never blocks on network I/O). A fd whose send fails is
// dropped from the list -- the client already disconnected or is wedged;
// there is no on-close callback registered, so this is the only place fds
// actually get cleaned up (see the client-tracking comment above).
void web_ws_broadcast(const char* json, size_t len) {
  if (g_server == nullptr) return;

  int fds[kMaxWsClients];
  int n = 0;
  xSemaphoreTake(g_wsClientsMutex, portMAX_DELAY);
  for (int i = 0; i < kMaxWsClients; ++i) {
    if (g_wsClientUsed[i]) fds[n++] = g_wsClientFds[i];
  }
  xSemaphoreGive(g_wsClientsMutex);

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(json));
  frame.len = len;

  for (int i = 0; i < n; ++i) {
    esp_err_t err = httpd_ws_send_frame_async(g_server, fds[i], &frame);
    if (err != ESP_OK) {
      wsClientRemove(fds[i]);
    }
  }
}

// --- /ws WebSocket endpoint -------------------------------------------------

// Largest inbound frame we accept: the biggest message on the wire is a
// script save (name + full source), JSON-escaped. Ordinary Fennel/Lua
// source needs at most a couple of extra characters per escaped quote/
// backslash/control char, not the pathological 6x (\u00XX) blowup, so 2x
// EVAL_SRC_MAX plus room for the name field and JSON envelope is a
// generous practical margin, not a hard worst-case bound. Static: the
// httpd default config runs exactly one worker task, so handlers never
// execute concurrently and reusing one buffer across calls is safe (see
// httpd_config_t::task_priority / max_open_sockets in web_server_task --
// this relies on the default single-worker behavior, not a custom config).
constexpr size_t kWsBufCap = EVAL_SRC_MAX * 2 + 512;

static esp_err_t ws_handler(httpd_req_t* req) {
  if (req->method == HTTP_GET) {
    // The initial GET is the WS handshake, delivered once per new
    // connection (is_websocket=true on the uri registration below) --
    // esp-idf's documented way to detect "a client just connected".
    int fd = httpd_req_to_sockfd(req);
    wsClientAdd(fd);

    static char cfgBuf[kWsBufCap];
    size_t n = web_input_build_config(cfgBuf, sizeof(cfgBuf));
    httpd_ws_frame_t out = {};
    out.type = HTTPD_WS_TYPE_TEXT;
    out.payload = reinterpret_cast<uint8_t*>(cfgBuf);
    out.len = n;
    httpd_ws_send_frame(req, &out);
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {};
  frame.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);  // length-only probe
  if (ret != ESP_OK) return ret;
  if (frame.len == 0 || frame.len >= kWsBufCap) {
    return ESP_OK;  // empty control frame, or bigger than anything we ever
                     // send -- drop rather than risk a truncated parse
  }

  static char rxBuf[kWsBufCap];
  frame.payload = reinterpret_cast<uint8_t*>(rxBuf);
  ret = httpd_ws_recv_frame(req, &frame, frame.len);
  if (ret != ESP_OK) return ret;
  if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;  // ping/pong/close: nothing to parse

  static char outBuf[kWsBufCap];
  size_t outLen = 0;
  WebInputAction action = web_input_handle_text_frame(rxBuf, frame.len, outBuf, sizeof(outBuf), &outLen);

  httpd_ws_frame_t out = {};
  out.type = HTTPD_WS_TYPE_TEXT;
  switch (action) {
    case WEB_INPUT_SEND_HELLO: {
      // Reuses outBuf (not otherwise live at this point -- the switch's
      // other case fills it from web_input_handle_text_frame instead).
      out.len = web_input_build_config(outBuf, sizeof(outBuf));
      out.payload = reinterpret_cast<uint8_t*>(outBuf);
      httpd_ws_send_frame(req, &out);
      break;
    }
    case WEB_INPUT_REPLY:
      out.payload = reinterpret_cast<uint8_t*>(outBuf);
      out.len = outLen;
      httpd_ws_send_frame(req, &out);
      break;
    default:
      break;  // CONTROL_EVENT/EVAL_QUEUED/IGNORED need no reply here
  }
  return ESP_OK;
}

// --- console static files ---------------------------------------------------
//
// The console bundle (firmware/main/data/console/) is embedded into the app
// binary via EMBED_FILES (firmware/main/CMakeLists.txt), same mechanism as
// the vendored Fennel compiler (see main.cpp's fennel_lua_start). ESP-IDF
// names each blob's symbols after the file's base name with non-alnum
// characters replaced by '_' -- e.g. "ws-client.js" -> _binary_ws_client_js_*.

extern "C" const uint8_t index_html_start[]        asm("_binary_index_html_start");
extern "C" const uint8_t index_html_end[]          asm("_binary_index_html_end");
extern "C" const uint8_t app_js_start[]             asm("_binary_app_js_start");
extern "C" const uint8_t app_js_end[]               asm("_binary_app_js_end");
extern "C" const uint8_t styles_css_start[]         asm("_binary_styles_css_start");
extern "C" const uint8_t styles_css_end[]           asm("_binary_styles_css_end");
extern "C" const uint8_t ws_client_js_start[]       asm("_binary_ws_client_js_start");
extern "C" const uint8_t ws_client_js_end[]         asm("_binary_ws_client_js_end");
extern "C" const uint8_t script_panel_js_start[]    asm("_binary_script_panel_js_start");
extern "C" const uint8_t script_panel_js_end[]      asm("_binary_script_panel_js_end");
extern "C" const uint8_t fennel_editor_js_start[]   asm("_binary_fennel_editor_js_start");
extern "C" const uint8_t fennel_editor_js_end[]     asm("_binary_fennel_editor_js_end");
extern "C" const uint8_t preact_mjs_start[]         asm("_binary_preact_mjs_start");
extern "C" const uint8_t preact_mjs_end[]           asm("_binary_preact_mjs_end");
extern "C" const uint8_t preact_hooks_mjs_start[]   asm("_binary_preact_hooks_mjs_start");
extern "C" const uint8_t preact_hooks_mjs_end[]     asm("_binary_preact_hooks_mjs_end");
extern "C" const uint8_t htm_mjs_start[]            asm("_binary_htm_mjs_start");
extern "C" const uint8_t htm_mjs_end[]              asm("_binary_htm_mjs_end");
extern "C" const uint8_t editor_bundle_mjs_start[]  asm("_binary_editor_bundle_mjs_start");
extern "C" const uint8_t editor_bundle_mjs_end[]    asm("_binary_editor_bundle_mjs_end");

namespace {
struct StaticFile {
  const char* uri;
  const uint8_t* start;
  const uint8_t* end;
  const char* contentType;
};
}  // namespace

static const StaticFile kStaticFiles[] = {
  {"/",                        index_html_start,      index_html_end,      "text/html"},
  {"/index.html",              index_html_start,      index_html_end,      "text/html"},
  {"/app.js",                  app_js_start,           app_js_end,          "application/javascript"},
  {"/styles.css",              styles_css_start,       styles_css_end,      "text/css"},
  {"/ws-client.js",            ws_client_js_start,     ws_client_js_end,    "application/javascript"},
  {"/script-panel.js",         script_panel_js_start,  script_panel_js_end, "application/javascript"},
  {"/shared/fennel-editor.js", fennel_editor_js_start, fennel_editor_js_end, "application/javascript"},
  {"/vendor/preact.mjs",       preact_mjs_start,       preact_mjs_end,       "application/javascript"},
  {"/vendor/preact-hooks.mjs", preact_hooks_mjs_start, preact_hooks_mjs_end, "application/javascript"},
  {"/vendor/htm.mjs",          htm_mjs_start,          htm_mjs_end,          "application/javascript"},
  {"/vendor/editor-bundle.mjs", editor_bundle_mjs_start, editor_bundle_mjs_end, "application/javascript"},
};
constexpr size_t kNumStaticFiles = sizeof(kStaticFiles) / sizeof(kStaticFiles[0]);

static esp_err_t static_file_handler(httpd_req_t* req) {
  const StaticFile* f = static_cast<const StaticFile*>(req->user_ctx);
  httpd_resp_set_type(req, f->contentType);
  httpd_resp_send(req, reinterpret_cast<const char*>(f->start),
                  static_cast<ssize_t>(f->end - f->start));
  return ESP_OK;
}

// --- server bring-up ---------------------------------------------------------

void web_server_task(void* /*ctx*/) {
  if (g_wsClientsMutex == nullptr) {
    g_wsClientsMutex = xSemaphoreCreateMutex();
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // Every URI registered below is an exact path (no wildcards needed), so
  // this stays at IDF's default single-worker, exact-match config --
  // handlers above rely on running one at a time (see kWsBufCap). +1 for
  // /ws, +1 for /ota (F5 -- see ota_manager.h; reuses this same server
  // rather than starting a second one).
  config.max_uri_handlers = kNumStaticFiles + 2;
  // OTA uploads (up to a full ota_0/ota_1 partition, several MB) can take
  // a while over a venue's WiFi; the default recv timeout is tuned for
  // small console requests, not that.
  config.recv_wait_timeout = 30;

  if (httpd_start(&g_server, &config) != ESP_OK) {
    ESP_LOGE(TAG, "httpd_start failed");
    return;
  }

  httpd_uri_t wsUri = {};
  wsUri.uri = "/ws";
  wsUri.method = HTTP_GET;
  wsUri.handler = ws_handler;
  wsUri.is_websocket = true;
  httpd_register_uri_handler(g_server, &wsUri);

  for (size_t i = 0; i < kNumStaticFiles; ++i) {
    httpd_uri_t uri = {};
    uri.uri = kStaticFiles[i].uri;
    uri.method = HTTP_GET;
    uri.handler = static_file_handler;
    uri.user_ctx = const_cast<void*>(static_cast<const void*>(&kStaticFiles[i]));
    httpd_register_uri_handler(g_server, &uri);
  }

  ota_register_handlers(g_server);

  ESP_LOGI(TAG, "web console + /ws + /ota ready");
}

#endif  // ESP_PLATFORM
