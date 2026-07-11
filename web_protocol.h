<<<<<<< ours
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
//   Device -> UI feedback (optional, Phase 4):
//     { "type":"state", "active":[0,3,5] }
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

// Build a `state` JSON message (Phase 4 feedback) into `buf`.
// `activeIds` may be nullptr only if nActive is 0.
size_t buildStateJson(const uint16_t* activeIds, size_t nActive,
                      char* buf, size_t bufLen);
=======
// web_protocol.h — JSON message schema for the Preact <-> firmware WebSocket.
//
// The console (firmware/main/data/console/) and web_input.cpp exchange tiny
// JSON text frames. The shapes are fixed and small, so we parse them with a
// hand-rolled scanner (no cJSON dependency) in web_input_handler.cpp.
//
// Inbound (browser -> device):
//   {"type":"go","cue":N}        trigger cue N
//   {"type":"release","cue":N}   release cue N
//   {"type":"scene","id":N}      go scene N
//   {"type":"button","id":N}     fire web-button binding N
//
// Outbound (device -> browser), sent on connect and on state change:
//   {"type":"config","cues":[{"id":N,"label":"..."}],"buttons":[{"id":N,"cue":N,"label":"..."}]}
//   {"type":"state","active":[N,M,...]}
#pragma once

#include <cstdint>

// Inbound message types (matched as substrings by the handler).
#define WEB_TYPE_GO      "go"
#define WEB_TYPE_RELEASE "release"
#define WEB_TYPE_SCENE   "scene"
#define WEB_TYPE_BUTTON  "button"

// Outbound message type strings.
#define WEB_OUT_CONFIG "config"
#define WEB_OUT_STATE  "state"
>>>>>>> theirs
