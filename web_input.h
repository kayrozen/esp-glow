// web_input.h — device-only WebSocket + static-file server.
//
// F4 device transport for the web console. Serves the Preact console bundle
// from LittleFS (/littlefs/console/) over HTTP, and exposes a WebSocket
// endpoint at /ws that:
//   - on connect, sends the `config` JSON snapshot (cues/buttons) built by
//     web_input_build_config()
//   - on each inbound text frame, dispatches via the host-tested
//     web_input_handle_text_frame() to LiveControl
//
// The JSON parsing/building is host-tested; this file only owns the HTTP/WS
// transport. A single connected client is tracked for state broadcasts.
#pragma once

#include "live_control.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct WebInputConfig {
  LiveControl* live;   // borrowed
  uint16_t port;       // typically 80
};

// Start the HTTP + WebSocket server. Returns true on success.
bool web_input_start(const struct WebInputConfig* cfg);

// Stop the server.
void web_input_stop(void);

// Broadcast a JSON state string to the most-recent connected WS client.
// Used by a state-poller in main.cpp (e.g. sending active cue ids every 500ms).
// No-op if no client is connected.
void web_input_broadcast_state(const char* json);

#ifdef __cplusplus
}
#endif
