// web_input_handler.h — pure JSON text-frame -> LiveControl dispatch.
//
// The firmware's web_input.cpp receives WebSocket text frames and passes them
// here. This parses the tiny JSON (a type + a number) and calls the right
// LiveControl method. Pure logic, host-tested.
//
// We do NOT pull in a JSON library: the messages are fixed-shape and small,
// so a hand-rolled scanner is lighter and avoids a cJSON dependency in
// -fno-exceptions/-fno-rtti firmware.
#pragma once

#include "live_control.h"
#include <cstdint>

// Handle one WebSocket text frame. Returns true if the frame was recognised
// and dispatched; false on malformed/unrecognised input (caller may log).
bool web_input_handle_text_frame(const char* text, LiveControl& lc, float t);

// Build the outbound `config` JSON snapshot into `out` (caller-provided
// buffer). Returns the number of bytes written (0 on truncation).
size_t web_input_build_config(const LiveControl& lc, char* out, size_t cap);
