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

//
// OSC bundles — minimal support (low priority; see design doc's T6).
// Bundles give atomic multi-message delivery; timetags give scheduled
// execution. Neither is load-bearing for lighting control (most OSC
// senders just send plain messages), so this deliberately does NOT build
// a scheduler: every contained message is dispatched immediately,
// regardless of its bundle's timetag, as if it said "now" -- exactly the
// scope the design doc calls for.
//
// Wire layout (OSC 1.0): the 8-byte ASCII marker "#bundle\0", an 8-byte
// NTP timetag (ignored), then zero or more elements, each a 4-byte
// big-endian size prefix followed by that many bytes -- either a plain
// OSC message or a nested bundle (bundles can contain bundles).
//

// True if `pkt` starts with the OSC bundle marker "#bundle\0".
bool isOscBundle(const uint8_t* pkt, size_t len);

// Called once per successfully-parsed, address-matched message found
// while walking a packet (see parseOscPacket).
using OscEventFn = void (*)(void* ctx, const ControlEvent& ev);

// Parses one OSC packet that may be either a single message OR a bundle
// (arbitrarily nested) containing multiple messages, calling onEvent for
// each match it finds, in wire order. A plain (non-bundle) packet is
// handled the same as calling parseOsc once. Malformed bundle framing
// (a size prefix that would run past the buffer, or excessive nesting)
// stops walking the rest of that bundle rather than reading out of
// bounds; whatever was already dispatched is not undone. Returns the
// number of events dispatched.
size_t parseOscPacket(const uint8_t* pkt, size_t len, const OscAddressMap& map,
                      OscEventFn onEvent, void* ctx);
