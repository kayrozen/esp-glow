#pragma once

#include "live_control.h"  // ControlEvent, ControlType
#include <cstdint>
#include <cstddef>

//
// OSC packet parser — minimal implementation for button/fader control.
//
// Parses a minimal OSC packet (address string + optional single float/int32 arg)
// into a ControlEvent, using a caller-supplied address→logical-id map.
//
// Layout: NUL-terminated address padded to 4 bytes, then a type-tag string
// (`,f`/`,i`/`,`), padded to 4 bytes, then the arg (big-endian float/int32).
//
// Button address → ControlEvent{Button, id, pressed=(arg!=0)}.
// Fader address → {Fader, id, value}.
//
// Returns false on a malformed packet or an unmapped address. Bounds-checked;
// no overread on truncated input (this is untrusted UDP).
//

// Maps an OSC address to a control ID and type.
struct OscAddressBinding {
  const char* address;   // OSC address string (e.g., "/cue/1")
  ControlType type;      // Button or Fader
  uint16_t controlId;    // logical control ID
};

struct OscAddressMap {
  const OscAddressBinding* bindings;
  size_t count;
};

// Parse a minimal OSC packet into a ControlEvent using the provided address map.
// Returns false on a malformed packet or an unmapped address; true if a valid
// event was parsed into `out`.
bool parseOsc(const uint8_t* pkt, size_t len,
              const OscAddressMap& map, ControlEvent& out);
