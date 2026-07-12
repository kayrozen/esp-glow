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
// Hardware wiring (httpd ws endpoint, console file serving) is left as
// `// TODO` for the same reasons as midi_input.cpp and osc_input.cpp: it
// cannot be verified without hardware.
//

#include "web_protocol.h"
#include "control_queue.h"   // IControlEventQueue, ControlEvent (transitively)
#include "eval_queue.h"      // IEvalSubmissionQueue, EvalSubmission
#include "glow_lua_api.h"    // GlowLuaApi::pollNewlyDisabledEffects (web_input_poll_fx_error)
#include "live_control.h"    // ControlType (for parseWebCommand's out param)
#include "scripts_storage.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// --- borrowed state set up at init time ----------------------------------

static IControlEventQueue*   g_controlQueue = nullptr;
static IEvalSubmissionQueue* g_evalQueue    = nullptr;
static const WebCueInfo*     g_cues         = nullptr;
static size_t                g_nCues        = 0;
static const WebSceneInfo*   g_scenes       = nullptr;
static size_t                g_nScenes      = 0;
static bool                  g_hasMaster    = false;

// TODO: add an httpd_handle_t / socket set here when the real server lands.
// static httpd_handle_t g_server = nullptr;

extern "C" {

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

// What the caller should do after web_input_handle_text_frame returns.
enum WebInputAction {
  WEB_INPUT_IGNORED       = 0,  // malformed/unknown frame; nothing to do
  WEB_INPUT_CONTROL_EVENT = 1,  // cue/scene/master; already pushed to the control queue
  WEB_INPUT_SEND_HELLO    = 2,  // caller should send `config` to this client
  WEB_INPUT_EVAL_QUEUED   = 3,  // already pushed to the eval queue; eval_result comes later
                                 // (via glow::pumpEvalSubmissions on the render task)
  WEB_INPUT_REPLY         = 4,  // outBuf/outLen holds a message; send to THIS client only
};

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
// that just broke. Returns true and fills buf/outLen with an `fx_error`
// message if one is pending; the caller's contract is to broadcast it to
// every connected client (not just one), since any of them may be running
// the live REPL that needs to see it. Returns false (buf untouched) when
// there is nothing new to report this frame.
bool web_input_poll_fx_error(GlowLuaApi& api, char* buf, size_t bufLen, size_t* outLen) {
  std::vector<std::pair<std::string, std::string>> notifications;
  api.pollNewlyDisabledEffects(notifications);
  if (notifications.empty()) return false;
  // Only the first is reported this frame; the rest wait for the next
  // frame's poll rather than growing this function's stack usage or the
  // message size unboundedly on a show that broke badly all at once.
  *outLen = buildFxErrorJson(notifications[0].first.c_str(), notifications[0].second.c_str(),
                             buf, bufLen);
  return true;
}

// --- WebSocket server task (hardware-specific, scaffold only) -------------

void web_server_task() {
  // TODO: Start an ESP-IDF httpd instance with a WebSocket handler on a
  // known URI (e.g. "/ws"). For each inbound text frame:
  //
  //   char reply[EVAL_SRC_MAX + 128];
  //   size_t replyLen = 0;
  //   WebInputAction action = web_input_handle_text_frame(frame, len, reply, sizeof(reply), &replyLen);
  //   switch (action) {
  //     case WEB_INPUT_SEND_HELLO: {
  //       char buf[512];
  //       size_t n = web_input_build_config(buf, sizeof(buf));
  //       httpd_ws_send_frame(req, buf, n, TEXT);
  //       break;
  //     }
  //     case WEB_INPUT_REPLY:
  //       httpd_ws_send_frame(req, reply, replyLen, TEXT);
  //       break;
  //     default:
  //       break;  // CONTROL_EVENT/EVAL_QUEUED/IGNORED need no reply here
  //   }
  //
  // On a new client connection (before any message), also push config.
  //
  // Optionally (Phase 4) push `state` whenever the show controller's
  // active-cue set changes; the controller already knows this set.
  //
  // The render task's frame-slack hook should also call
  // web_input_poll_fx_error(glow::glowLuaApi(), ...) once per frame and,
  // if it returns true, broadcast the result to every connected client.
  //
  // The static files for the console bundle (index.html, app.js,
  // styles.css, vendor/) are embedded into the app binary via EMBED_FILES
  // (firmware/main/CMakeLists.txt) and served from a separate httpd URI
  // handler that maps "/" -> the embedded index.html, etc. No filesystem
  // partition is involved.
}

}  // extern "C"

#endif  // ESP_PLATFORM
