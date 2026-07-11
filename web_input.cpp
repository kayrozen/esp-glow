#ifdef ESP_PLATFORM

#include "live_control.h"
#include <cstdint>

static LiveControl* g_liveControl = nullptr;

void web_input_init(LiveControl& live) {
  g_liveControl = &live;
}

void web_input_handle_command(const char* command, float value) {
  if (g_liveControl == nullptr) return;

  // TODO: Parse WebSocket/HTTP command into ControlEvent
  // Example command format:
  //   "cue/go/60" → CueFlash on
  //   "cue/toggle/60" → CueToggle
  //   "scene/go/1" → SceneGo on
  //   "fader/master" with value → Master fader
  //
  // This maps application-level commands to ControlEvents.
  // Full WebSocket/HTTP protocol handling is out of scope.

  ControlEvent ev;
  uint16_t controlId = 0;

  if (std::sscanf(command, "cue/go/%hu", &controlId) == 1) {
    ev.type = ControlType::Button;
    ev.id = controlId;
    ev.pressed = (value > 0.0f);
    ev.value = 0.0f;
  } else if (std::sscanf(command, "scene/go/%hu", &controlId) == 1) {
    ev.type = ControlType::Button;
    ev.id = 1000 + controlId;
    ev.pressed = (value > 0.0f);
    ev.value = 0.0f;
  } else if (std::strcmp(command, "fader/master") == 0) {
    ev.type = ControlType::Fader;
    ev.id = 200;
    ev.pressed = false;
    ev.value = value;
  } else {
    return;
  }

  float now = 0.0f;
  g_liveControl->handle(ev, now);
}

void web_server_task() {
  // TODO: Start HTTP/WebSocket server (hardware-specific)
  // for each incoming command message:
  //   web_input_handle_command(cmdStr, value);
  //
  // The HTML/JS frontend is a separate project.
}

#endif
