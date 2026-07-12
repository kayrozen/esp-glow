#pragma once

#include <cstddef>
#include <cstdint>

#include "live_control.h"  // ControlType, ControlEvent

//
// Minimal OSC packet parser — host-testable core for the OSC transport
// (osc_input.cpp). Mirrors parseMidi (live_control.h) and parseWebCommand
// (web_protocol.h): a pure function over untrusted bytes, no device
// dependency, so it can be fuzzed/ASan-tested on the host before it ever
// touches a real UDP socket.
//
// Supported subset: one address pattern, one optional float or int32 arg.
// OSC bundles, multi-argument messages, and the full type-tag vocabulary
// are out of scope — this drives cue/scene/fader control, not a general
// OSC endpoint.
//
// Wire layout (OSC 1.0):
//   address   NUL-terminated ASCII starting with '/', padded with NULs to
//             a 4-byte boundary (including its own NUL).
//   type tag  ","  followed by one type char ('f' or 'i'), NUL-terminated,
//             padded the same way.
//   arg       4 bytes, big-endian (IEEE-754 float for 'f', two's-complement
//             int32 for 'i').
//

// One address -> logical control binding. `address` is caller-owned and
// must outlive any parseOsc call using this map (typically a static table
// built once in main.cpp, matching LiveControl's own bindButton/bindFader
// calls).
struct OscBinding {
  const char* address;
  ControlType type;
  uint16_t id;
};

struct OscAddressMap {
  const OscBinding* bindings;
  size_t count;
};

// Parse one OSC packet (`pkt`, `len` bytes, untrusted/off-the-wire) into a
// ControlEvent. Returns false — leaving `out` unmodified — if the packet is
// truncated, malformed (bad padding, missing/unsupported type tag), or its
// address isn't in `map`. Never reads past `pkt[len - 1]`.
//
// On a match: a Button binding yields {pressed = (arg != 0)}; a Fader
// binding yields {value = arg} (int32 args are normalized to 0..1 by
// dividing by 127, matching parseMidi's controller-value convention).
bool parseOsc(const uint8_t* pkt, size_t len, const OscAddressMap& map, ControlEvent& out);
