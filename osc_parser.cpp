// osc_parser.cpp — pure OSC parser implementation.
#include "osc_parser.h"
#include <cstring>

// OSC strings are null-terminated and padded to a 4-byte boundary. Returns the
// total byte length including padding, or 0 if the buffer is too short / the
// string runs past the end.
static size_t oscStringLen(const uint8_t* data, size_t len, size_t* outStrLen) {
  // Find the null terminator.
  size_t i = 0;
  while (i < len && data[i] != 0) ++i;
  if (i >= len) return 0;  // no null terminator
  *outStrLen = i;
  // Pad to 4-byte boundary: total = i+1 padded up to multiple of 4.
  size_t total = i + 1;
  total = (total + 3) & ~static_cast<size_t>(3);
  if (total > len) return 0;
  return total;
}

static uint32_t readBE32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8)  |
         (static_cast<uint32_t>(p[3]));
}

// Parse a single OSC message (not a bundle) starting at `data`.
static bool parseMessage(const uint8_t* data, size_t len, OscMessage& out) {
  out = OscMessage();
  if (len < 4) return false;

  // Address pattern (string).
  size_t addrStrLen = 0;
  size_t addrTotal = oscStringLen(data, len, &addrStrLen);
  if (addrTotal == 0) return false;
  out.address = reinterpret_cast<const char*>(data);
  out.addressLen = addrStrLen;

  // If the address starts with '#', it's a bundle indicator (shouldn't happen
  // here; the top-level handles bundles). Reject to be safe.
  if (addrStrLen > 0 && out.address[0] == '#') return false;

  // Type tag string, must start with ','.
  size_t off = addrTotal;
  if (off >= len) return false;
  if (data[off] != ',') return false;
  size_t tagStrLen = 0;
  size_t tagTotal = oscStringLen(data + off, len - off, &tagStrLen);
  if (tagTotal == 0) return false;
  // Tags live at data[off+1 .. off+tagStrLen).
  size_t argOff = off + tagTotal;

  // Walk tags; extract the first int32 / float32; skip others.
  for (size_t t = 1; t < tagStrLen; ++t) {
    char tag = static_cast<char>(data[off + t]);
    if (tag == 'i') {
      if (argOff + 4 > len) return false;
      if (out.arg.type == OscArg::None) {
        out.arg.type = OscArg::Int32;
        out.arg.i = static_cast<int32_t>(readBE32(data + argOff));
      }
      argOff += 4;
    } else if (tag == 'f') {
      if (argOff + 4 > len) return false;
      if (out.arg.type == OscArg::None) {
        uint32_t bits = readBE32(data + argOff);
        float f;
        std::memcpy(&f, &bits, sizeof(float));
        out.arg.type = OscArg::Float32;
        out.arg.f = f;
      }
      argOff += 4;
    } else if (tag == 's' || tag == 'S') {
      size_t sl = 0;
      size_t st = oscStringLen(data + argOff, len - argOff, &sl);
      if (st == 0) return false;
      argOff += st;
    } else if (tag == 'b') {
      // blob: int32 size + data padded to 4
      if (argOff + 4 > len) return false;
      uint32_t sz = readBE32(data + argOff);
      argOff += 4;
      if (argOff + sz > len) return false;
      argOff += sz;
      argOff = (argOff + 3) & ~static_cast<size_t>(3);
    } else if (tag == 'T' || tag == 'F' || tag == 'N' || tag == 'I') {
      // No data bytes.
    } else if (tag == 'h' || tag == 'd' || tag == 't') {
      // 64-bit: skip 8 bytes.
      argOff += 8;
    } else {
      // Unknown tag: stop parsing safely.
      break;
    }
    if (argOff > len) return false;
  }

  out.valid = true;
  return true;
}

bool parseOsc(const uint8_t* data, size_t len, OscMessage& out) {
  out = OscMessage();
  if (!data || len < 4) return false;

  // Bundle? Address starts with "#bundle".
  if (data[0] == '#') {
    if (len < 16) return false;
    if (std::memcmp(data, "#bundle\0", 8) != 0) return false;
    // Skip 8-byte time tag, then a sequence of {int32 size, element}.
    size_t off = 16;
    if (off + 4 > len) return false;
    uint32_t elemSize = readBE32(data + off);
    off += 4;
    if (elemSize == 0 || off + elemSize > len) return false;
    // Parse the first element as a message (it could itself be a bundle;
    // recurse once via parseMessage which rejects '#'-prefixed addresses —
    // for simplicity we only unwrap one bundle level, which covers the
    // common lighting case).
    return parseMessage(data + off, elemSize, out);
  }

  return parseMessage(data, len, out);
}
