#pragma once

#include <cstddef>
#include <cstdint>

#include "web_protocol.h"  // WebCueInfo, WebSceneInfo

class IControlEventQueue;
class IEvalSubmissionQueue;
class GlowLuaApi;

//
// Web console input — device scaffold (see web_input.cpp for the full
// rationale). This header exists so main.cpp has something to include to
// call into it; without it, the render task has no way to reach
// web_input_poll_fx_error, and a Lua effect throwing mid-show gets disabled
// with nobody ever finding out.
//
// Plain C++ linkage: every caller and callee here is C++ (unlike
// scripts_storage.h, which is a real C-callable surface), and several
// signatures take C++ reference/class types, so extern "C" would buy
// nothing but an unusual reading experience.
//

// Initialize the web input layer with the queues it pushes to, plus the
// cue/scene metadata used to build `config` messages. All pointers are
// borrowed (not copied); they must outlive the server.
void web_input_init(IControlEventQueue& controlQueue, IEvalSubmissionQueue& evalQueue,
                    const WebCueInfo* cues, size_t nCues,
                    const WebSceneInfo* scenes, size_t nScenes,
                    bool hasMaster);

// Build a `config` message into `buf`. Returns bytes written (excluding the
// NUL) or the would-be length if `bufLen` is too small. Caller may pass
// buf=nullptr,bufLen=0 to measure.
size_t web_input_build_config(char* buf, size_t bufLen);

// Build a `state` message into `buf` listing currently-active cue ids.
// `activeIds` may be nullptr if nActive == 0.
size_t web_input_build_state(const uint16_t* activeIds, size_t nActive,
                             char* buf, size_t bufLen);

// What the caller should do after web_input_handle_text_frame returns.
enum WebInputAction {
  WEB_INPUT_IGNORED       = 0,  // malformed/unknown frame; nothing to do
  WEB_INPUT_CONTROL_EVENT = 1,  // cue/scene/master; already pushed to the control queue
  WEB_INPUT_SEND_HELLO    = 2,  // caller should send `config` to this client
  WEB_INPUT_EVAL_QUEUED   = 3,  // already pushed to the eval queue; eval_result comes later
                                 // (via glow::pumpEvalSubmissions on the render task)
  WEB_INPUT_REPLY         = 4,  // outBuf/outLen holds a message; send to THIS client only
};

// Handle one inbound WebSocket text frame. `outBuf`/`outBufCap` are scratch
// space for WEB_INPUT_REPLY; `*outLen` is set to the reply's length when
// that's returned.
WebInputAction web_input_handle_text_frame(const char* json, size_t len,
                                           char* outBuf, size_t outBufCap, size_t* outLen);

// Called once per frame (render task frame slack, alongside
// pumpControlEvents/pumpEvalSubmissions/gcStepSlack) to check for effects
// that just broke. May invoke onFxError more than once per call (one newly-
// disabled effect per invocation) — see web_input.cpp's header comment on
// web_input_poll_fx_error for why a single-message version would silently
// drop effects. Returns the number of notifications delivered this call.
using FxErrorReplyFn = void (*)(void* ctx, const char* json, size_t len);
int web_input_poll_fx_error(GlowLuaApi& api, FxErrorReplyFn onFxError, void* ctx);

// Starts the httpd server (console static files + the /ws WebSocket
// endpoint) and returns once it's listening. Unlike midi_uart_task/
// osc_server_task, this is NOT meant to be handed to xTaskCreate: httpd
// spins up its own internal worker task to handle requests, so call this
// directly from app_main (the `ctx` parameter exists only so its shape
// matches the other transports' entry points; it's unused).
void web_server_task(void* ctx);

// Sends `json` (`len` bytes) to every currently-connected WS client.
// Called from the render task (post-render, alongside gcStepSlack/
// web_input_poll_fx_error) for fx_error and eval_result -- both need every
// client to see them, not just whichever one triggered them, since any
// connected client's REPL may be waiting on a result or watching for
// errors. Safe to call before the server has started (a no-op then). Drops
// any fd whose send fails rather than retrying -- see web_input.cpp.
void web_ws_broadcast(const char* json, size_t len);
