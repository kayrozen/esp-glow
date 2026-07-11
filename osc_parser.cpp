#include "osc_parser.h"

#include <cstring>
#include <cstdint>

// Helper: convert big-endian 32-bit bytes to float.
static float bytesToFloat(const uint8_t* p) {
  uint32_t bits = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                  ((uint32_t)p[2] << 8) | (uint32_t)p[3];
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// Helper: convert big-endian 32-bit bytes to int32.
static int32_t bytesToInt32(const uint8_t* p) {
  return ((int32_t)p[0] << 24) | ((int32_t)p[1] << 16) |
         ((int32_t)p[2] << 8) | (int32_t)p[3];
}

// Helper: compute padding to 4-byte boundary after a block of length `len`.
static size_t paddingTo4(size_t len) {
  size_t pad = (4 - (len % 4)) % 4;
  return pad;
}

bool parseOsc(const uint8_t* pkt, size_t len,
              const OscAddressMap& map, ControlEvent& out) {
  if (!pkt || len == 0 || !map.bindings || map.count == 0) {
    return false;
  }

  // Parse address: NUL-terminated string.
  const char* address = reinterpret_cast<const char*>(pkt);
  size_t addressLen = 0;
  for (addressLen = 0; addressLen < len; addressLen++) {
    if (address[addressLen] == '\0') break;
  }

  // Must have found a NUL and have room for padding.
  if (addressLen >= len) return false;

  // Bounds check: address + NUL + padding.
  size_t pad = paddingTo4(addressLen + 1);
  size_t typeTagOffset = addressLen + 1 + pad;
  if (typeTagOffset >= len) return false;

  // Parse type tag: must start with comma, NUL-terminated.
  const char* typeTag = address + typeTagOffset;
  if (typeTag[0] != ',') return false;

  size_t typeTagLen = 0;
  for (typeTagLen = 0; typeTagLen < len - typeTagOffset; typeTagLen++) {
    if (typeTag[typeTagLen] == '\0') break;
  }
  if (typeTagLen >= len - typeTagOffset) return false;

  // Bounds check: type tag + NUL + padding.
  pad = paddingTo4(typeTagLen + 1);
  size_t argOffset = typeTagOffset + typeTagLen + 1 + pad;

  // Parse argument based on type tag.
  float value = 0.0f;
  char argType = typeTag[1];

  if (argType == '\0') {
    // No argument, default to 1.0.
    value = 1.0f;
  } else if (argType == 'f') {
    if (argOffset + 4 > len) return false;  // Not enough bytes for float
    value = bytesToFloat(pkt + argOffset);
  } else if (argType == 'i') {
    if (argOffset + 4 > len) return false;  // Not enough bytes for int32
    int32_t intVal = bytesToInt32(pkt + argOffset);
    value = static_cast<float>(intVal) / 127.0f;
  } else {
    return false;  // Unknown type.
  }

  // Look up address in the map.
  for (size_t i = 0; i < map.count; i++) {
    if (std::strcmp(map.bindings[i].address, address) == 0) {
      out.type = map.bindings[i].type;
      out.id = map.bindings[i].controlId;

      if (out.type == ControlType::Button) {
        out.pressed = (value > 0.5f);
        out.value = 0.0f;
      } else {
        out.pressed = false;
        out.value = value;
      }

      return true;
    }
  }

  return false;  // Address not in map.
}
