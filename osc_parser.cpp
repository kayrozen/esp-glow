#include "osc_parser.h"

#include <cstring>

namespace {

// Consumes one OSC string starting at `pkt[start]`: scans for the
// mandatory NUL terminator (never past `pkt[len-1]`), then verifies the
// 0-3 padding bytes that follow it (up to the next 4-byte boundary) are
// actually NUL -- a packet with garbage in the padding is malformed, not
// something to silently tolerate. On success, `*contentLen` is the
// string's length excluding the NUL and `*end` is the absolute offset of
// the byte following the padding (i.e. where the next field starts).
bool readOscString(const uint8_t* pkt, size_t len, size_t start,
                    size_t* contentLen, size_t* end) {
  if (start >= len) return false;

  size_t i = start;
  while (i < len && pkt[i] != 0) ++i;
  if (i >= len) return false;  // no terminator within bounds -- truncated

  size_t afterNul = i + 1;
  size_t aligned = ((afterNul + 3) / 4) * 4;
  if (aligned > len) return false;  // padding would run past the buffer

  for (size_t p = afterNul; p < aligned; ++p) {
    if (pkt[p] != 0) return false;  // bad padding
  }

  *contentLen = i - start;
  *end = aligned;
  return true;
}

}  // namespace

bool parseOsc(const uint8_t* pkt, size_t len, const OscAddressMap& map, ControlEvent& out) {
  if (pkt == nullptr || len == 0) return false;

  size_t addrLen = 0, afterAddr = 0;
  if (!readOscString(pkt, len, 0, &addrLen, &afterAddr)) return false;
  if (addrLen == 0 || pkt[0] != '/') return false;

  size_t ttLen = 0, afterTt = 0;
  if (!readOscString(pkt, len, afterAddr, &ttLen, &afterTt)) return false;

  const char* typeTag = reinterpret_cast<const char*>(pkt + afterAddr);
  if (ttLen != 2 || typeTag[0] != ',') return false;  // "," + exactly one type char
  char typeChar = typeTag[1];
  if (typeChar != 'f' && typeChar != 'i') return false;

  if (afterTt + 4 > len) return false;  // truncated before the 4-byte arg

  uint32_t raw = (static_cast<uint32_t>(pkt[afterTt]) << 24) |
                 (static_cast<uint32_t>(pkt[afterTt + 1]) << 16) |
                 (static_cast<uint32_t>(pkt[afterTt + 2]) << 8) |
                 static_cast<uint32_t>(pkt[afterTt + 3]);

  float value;
  if (typeChar == 'f') {
    std::memcpy(&value, &raw, sizeof(value));
  } else {
    int32_t iv;
    std::memcpy(&iv, &raw, sizeof(iv));
    value = static_cast<float>(iv) / 127.0f;  // matches parseMidi's controller-value convention
  }

  // `address` is NUL-terminated within `pkt` (readOscString just verified
  // pkt[addrLen] == 0), so strcmp against caller-owned binding strings is
  // bounds-safe.
  const char* address = reinterpret_cast<const char*>(pkt);
  for (size_t i = 0; i < map.count; ++i) {
    const OscBinding& b = map.bindings[i];
    if (std::strcmp(address, b.address) != 0) continue;
    out.type = b.type;
    out.id = b.id;
    if (b.type == ControlType::Button) {
      out.pressed = (value != 0.0f);
      out.value = 0.0f;
    } else {
      out.pressed = false;
      out.value = value;
    }
    return true;
  }
  return false;  // address not in the map
}
