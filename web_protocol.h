#pragma once

#include <cstdint>
#include <cstddef>

#include "live_control.h"  // ControlEvent, ActionKind

//
// Web protocol — testable core for the ESP32-S3 web console.
//
// Mirrors the Phase-0 protocol pinned in the project plan:
//
//   Device -> UI on connect:
//     { "type":"config",
//       "cues":[ {"id":0,"label":"Blue wash","color":"#3060ff","mode":"toggle"} ],
//       "scenes":[ {"id":0,"label":"Verse"} ],
//       "hasMaster": true }
//
//   UI -> device:
//     { "type":"cue",   "id":0, "pressed":true }   // flash: down/up; toggle: send true only
//     { "type":"scene", "id":0, "pressed":true }
//     { "type":"master","value":0.5 }
//     { "type":"hello" }                           // request config
//
//   Device -> UI feedback (Phase 4 -- P1.3 makes this the device's real
//   push, not just a stub):
//     { "type":"state", "active":[0,3,5], "master":0.75 }
//
// `state` is the device's source of truth for which cues are active and
// the current grandmaster level -- broadcast on change (post-render, from
// ShowController::activeCueIds()/LiveControl::masterLevel(), the same
// snapshot MIDI LED feedback reads -- see led_feedback.h/main.cpp), and
// sent in full to a newly-connected client so it never starts out wrong.
// `master` is always present (LiveControl::masterLevel() always has a
// value, default 1.0) even on a device with no master fader bound -- the
// UI only displays it when config.hasMaster is true.
//
// `mode` is "flash" or "toggle" and is derived from the binding's ActionKind:
//   CueFlash / SceneGo  -> "flash"
//   CueToggle / SceneToggle -> "toggle"
//
// The device setup code maintains a parallel metadata array (label/color per
// cue, label per scene) at the same time it calls LiveControl::bindButton /
// bindFader. This keeps LiveControl's binding table private and avoids
// modifying an existing module.
//

// Cue metadata for config emission. `action` mirrors the ActionKind passed to
// LiveControl::bindButton for the same controlId; buildConfigJson derives
// `mode` from it.
struct WebCueInfo {
  uint16_t id;            // controlId (matches LiveControl::bindButton first arg)
  const char* label;      // may be nullptr -> "Cue N"
  const char* color;      // may be nullptr -> "#3060ff" default
  ActionKind action;      // CueFlash or CueToggle (SceneGo/SceneToggle forbidden)
};

struct WebSceneInfo {
  uint16_t id;
  const char* label;      // may be nullptr -> "Scene N"
};

// Parse a UI->device JSON message into a ControlEvent.
//
// Returns:
//   true  - parsed a cue/scene/master message; `out` is populated.
//   false - message was `hello`, malformed, unknown type, or out of range.
//           `out` is left untouched.
//
// `out.type` mapping:
//   cue/scene -> ControlType::Button, out.id = controlId, out.pressed = pressed
//   master    -> ControlType::Fader, out.id = 0 (single grandmaster), out.value = value
//
// The device scaffold is responsible for the `hello` path (send config back).
bool parseWebCommand(const char* json, size_t len, ControlEvent& out);

// Returns true iff the frame is the `hello` handshake (device replies with config).
bool isHelloCommand(const char* json, size_t len);

// Build a `config` JSON message into `buf`. Returns the number of bytes
// written (excluding the terminating NUL). If the buffer is too small,
// writes a truncated-but-NUL-terminated prefix and returns the number of
// bytes that *would* have been written (>= bufLen means truncated).
//
// `cues`/`scenes` arrays may be nullptr only if the corresponding count is 0.
size_t buildConfigJson(const WebCueInfo* cues, size_t nCues,
                       const WebSceneInfo* scenes, size_t nScenes,
                       bool hasMaster,
                       char* buf, size_t bufLen);

// Build a `state` JSON message (Phase 4 feedback, P1.3) into `buf`.
// `activeIds` may be nullptr only if nActive is 0. `masterLevel` is
// written as the `master` field unconditionally (see this header's
// protocol comment on why the UI decides whether to show it).
size_t buildStateJson(const uint16_t* activeIds, size_t nActive, float masterLevel,
                      char* buf, size_t bufLen);

//
// Live-coding eval channel + script CRUD (README_LUA_FENNEL.md;
// the Fennel scripting UI's device-console protocol additions):
//
//   UI -> device:
//     { "type":"eval",          "src":"(glow.cue.go :chorus)", "seq":7 }
//     { "type":"script_list" }
//     { "type":"script_load",   "name":"verse" }
//     { "type":"script_save",   "name":"verse", "src":"(fn breathe [t] ...)" }
//     { "type":"script_delete", "name":"verse" }
//
//   Device -> UI:
//     { "type":"eval_result", "seq":7, "ok":true }
//     { "type":"eval_result", "seq":7, "ok":false, "err":"...:1: unexpected symbol" }
//     { "type":"scripts",     "names":["boot","verse","chorus"] }
//     { "type":"script",      "name":"verse", "src":"..." }
//     { "type":"fx_error",    "effect":"breathe", "err":"attempt to index nil value" }
//
// `seq` is an opaque request id the UI picks (e.g. an increasing counter) so
// out-of-order replies can be matched back to their request (evals are
// queued and drained on the render task; results are asynchronous); it is
// not a cue/scene id. `src` is arbitrary Fennel source text, so — unlike
// every other string in this protocol — it needs real JSON escape handling
// (parseWebCommand's strings deliberately reject backslash escapes because
// labels/names never need them; script source does). Script CRUD hits
// LittleFS directly on the WS task (scripts_storage.h) -- unlike eval, it
// never touches the Lua VM, so it doesn't go through the eval queue.
//
// `fx_error` is unsolicited: the device pushes it (to every connected
// client) whenever a running LuaEffect throws and gets permanently disabled
// (see lua_effect.h). It is not a reply to any UI->device message.
//

// Parse a UI->device `{"type":"eval", ...}` message. On success, the
// unescaped source is written into srcBuf (NUL-terminated if it fits) and
// outSrcLen is its length; outRequestId is the `seq` field (0 if absent).
// Returns false if this is not an `eval` message, srcBuf is too small for
// the unescaped source, or the message is malformed.
bool parseEvalCommand(const char* json, size_t len, uint32_t& outRequestId,
                      char* srcBuf, size_t srcBufCap, size_t& outSrcLen);

// Build an `eval_result` JSON message into `buf`. `err` is ignored when ok
// is true; may be nullptr when ok is false (omits the "err" field).
size_t buildEvalResultJson(uint32_t requestId, bool ok, const char* err,
                          char* buf, size_t bufLen);

// Returns true iff the frame is the `script_list` message (no other fields).
bool isScriptListCommand(const char* json, size_t len);

// Parse a UI->device `{"type":"script_load"|"script_delete", "name":"..."}`
// message (both share this shape: just a name, using parseStringRaw's plain
// rules -- script filenames, like cue labels, never need JSON escapes).
// outType receives "load" or "delete" as a 0-terminated pointer into a
// static string (safe to compare with ==, not to free). Returns false if
// this isn't a script_load/script_delete message, the name doesn't fit
// nameBuf, or the message is malformed.
bool parseScriptNameCommand(const char* json, size_t len,
                            const char** outType,
                            char* nameBuf, size_t nameBufCap);

// Parse a UI->device `{"type":"script_save", "name":"...", "src":"..."}`
// message. `src` gets full escape handling (see parseEvalCommand); `name`
// does not (see parseScriptNameCommand). Returns false if this isn't a
// script_save message, name/src don't fit their buffers, or malformed.
bool parseScriptSaveCommand(const char* json, size_t len,
                            char* nameBuf, size_t nameBufCap,
                            char* srcBuf, size_t srcBufCap, size_t& outSrcLen);

// Build a `scripts` JSON message (the script_list reply) into `buf`. `names`
// may be nullptr only if nNames is 0.
size_t buildScriptsJson(const char* const* names, size_t nNames,
                        char* buf, size_t bufLen);

// Build a `script` JSON message (the script_load reply) into `buf`. `src`
// may be nullptr (treated as empty).
size_t buildScriptJson(const char* name, const char* src, size_t srcLen,
                      char* buf, size_t bufLen);

// Build an unsolicited `fx_error` JSON message into `buf`.
size_t buildFxErrorJson(const char* effectName, const char* err,
                        char* buf, size_t bufLen);

//
// F5 robustness: safe blackout + OTA status. Both are unsolicited,
// broadcast to every connected client the same way fx_error is (see
// web_ws_broadcast) -- a blackout or an in-flight OTA is exactly the kind
// of state any connected console needs to see, not just whoever triggered
// it.
//
//   Device -> UI (unsolicited):
//     { "type":"blackout", "reason":"show partition: bad magic" }
//     { "type":"ota", "phase":"receiving", "message":"...", "percent":42 }
//

// Build an unsolicited `blackout` JSON message into `buf`. `reason` should
// be a short, specific, human-readable diagnosis (see glow_safe_blackout in
// main.cpp) -- a blackout with no reason is a debugging nightmare at 2am.
size_t buildBlackoutJson(const char* reason, char* buf, size_t bufLen);

// Build an unsolicited `ota` status JSON message into `buf`. `phase` is a
// short machine-readable tag ("receiving", "validating", "done", "error",
// "refused", ...); `message` is human-readable detail (may be nullptr to
// omit); `percent` is upload progress in [0,100], or a negative value to
// omit the field entirely (progress isn't always knowable, e.g. before the
// upload's Content-Length is known).
size_t buildOtaStatusJson(const char* phase, const char* message, int percent,
                          char* buf, size_t bufLen);
