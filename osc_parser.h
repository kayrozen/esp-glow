// osc_parser.h — pure OSC packet parser (host-tested).
//
// The firmware's OSC input is a UDP datagram; the parsing — OSC address
// pattern, type tag string, 4-byte alignment, big-endian ints/floats — is pure
// logic and lives here so `make test` covers it.
//
// Scope: the lighting use case is "address + one numeric arg" (cue go/release
// triggered by an OSC path like /esp-glow/cue/3/go with a float level). This
// parser extracts the address (zero-copy, pointing into the input buffer) and
// the FIRST numeric argument (int32 or float32). Bundles are unwrapped to
// their first element. Strings/blob args beyond the first are skipped.
#pragma once

#include <cstdint>
#include <cstddef>

struct OscArg {
  enum Type : uint8_t { None = 0, Int32, Float32 };
  Type    type = None;
  int32_t i = 0;
  float   f = 0.0f;
};

struct OscMessage {
  bool        valid = false;
  const char* address = nullptr;   // zero-copy into the input buffer
  size_t      addressLen = 0;
  OscArg      arg;                 // first numeric arg, if any
};

// Parse a UDP datagram as an OSC message or bundle.
//   - For a message, fills `out` with address + first numeric arg.
//   - For a bundle (#bundle), parses the first contained message.
// Returns false on malformed input (too short, bad alignment, etc.).
bool parseOsc(const uint8_t* data, size_t len, OscMessage& out);
