#include "device_config.h"

#include <cstring>

namespace {

uint16_t readU16LE(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t readU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Copies a fixed-width, NUL-padded field out of the blob into a
// NUL-terminated C string. Always writes a terminator itself (at
// `fieldLen` in `out`, which must be sized fieldLen+1) rather than trusting
// the blob to contain one -- a malformed/adversarial blob with no NUL in
// the field must not read past `fieldLen` bytes.
void readPaddedString(const uint8_t* field, size_t fieldLen, char* out) {
  std::memcpy(out, field, fieldLen);
  out[fieldLen] = '\0';
}

}  // namespace

uint32_t devcfgCrc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      uint32_t mask = -(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return ~crc;
}

bool parseDeviceConfig(const uint8_t* data, size_t len, DeviceConfig& out) {
  if (!data || len < DEVCFG_BLOB_SIZE) return false;

  // Magic check first: an erased (all-0xFF) or half-written partition
  // fails here, long before any field would be read as garbage (a GPIO
  // byte of 0xFF/255, etc.) -- see this file's header comment.
  if (data[0] != 'C' || data[1] != 'F' || data[2] != 'G' || data[3] != '1') {
    return false;
  }

  uint8_t version = data[4];
  if (version != 1) return false;

  uint8_t flags = data[5];
  // reserved u16 at [6..7] -- parsed over, not validated (forward-
  // compatible: a future minor addition can use it without every old
  // parser needing to reject it).

  uint32_t storedCrc = readU32LE(data + DEVCFG_CRC_OFFSET);
  uint32_t computedCrc = devcfgCrc32(data, DEVCFG_CRC_OFFSET);
  if (storedCrc != computedCrc) return false;

  DeviceConfig parsed;
  parsed.usbMidiHost = (flags & DEVCFG_FLAG_USB_MIDI_HOST) != 0;
  parsed.skipWifi = (flags & DEVCFG_FLAG_SKIP_WIFI) != 0;

  size_t off = 8;
  readPaddedString(data + off, DEVCFG_SSID_MAX, parsed.wifiSsid);
  off += DEVCFG_SSID_MAX;
  readPaddedString(data + off, DEVCFG_PASS_MAX, parsed.wifiPass);
  off += DEVCFG_PASS_MAX;

  parsed.artnetFallbackIp = readU32LE(data + off);
  off += 4;
  parsed.artnetPort = readU16LE(data + off);
  off += 2;

  parsed.dmxTxGpio = data[off++];
  parsed.dmxRxGpio = data[off++];
  parsed.dmxRtsGpio = data[off++];
  parsed.ledGpio = data[off++];

  // off now points at reserved2 (32 bytes); nothing to read from it yet.

  out = parsed;
  return true;
}
