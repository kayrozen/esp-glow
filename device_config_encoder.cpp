#include "device_config_encoder.h"

#include <cstring>
#include <algorithm>

namespace {

void writeU16LE(std::vector<uint8_t>& b, size_t off, uint16_t v) {
  b[off] = static_cast<uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
}

void writeU32LE(std::vector<uint8_t>& b, size_t off, uint32_t v) {
  b[off] = static_cast<uint8_t>(v & 0xFF);
  b[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  b[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  b[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
}

void writePaddedString(std::vector<uint8_t>& b, size_t off, size_t fieldLen, const char* s) {
  size_t n = std::min(std::strlen(s), fieldLen);
  std::memcpy(b.data() + off, s, n);
  // Remainder (including at least one byte if s exactly fills fieldLen)
  // stays zero-initialized from the vector's construction.
}

}  // namespace

std::vector<uint8_t> encodeDeviceConfig(const DeviceConfig& cfg) {
  std::vector<uint8_t> b(DEVCFG_BLOB_SIZE, 0);

  b[0] = 'C';
  b[1] = 'F';
  b[2] = 'G';
  b[3] = '1';
  b[4] = 1;  // version

  uint8_t flags = 0;
  if (cfg.usbMidiHost) flags |= DEVCFG_FLAG_USB_MIDI_HOST;
  if (cfg.skipWifi) flags |= DEVCFG_FLAG_SKIP_WIFI;
  b[5] = flags;
  writeU16LE(b, 6, 0);  // reserved

  size_t off = 8;
  writePaddedString(b, off, DEVCFG_SSID_MAX, cfg.wifiSsid);
  off += DEVCFG_SSID_MAX;
  writePaddedString(b, off, DEVCFG_PASS_MAX, cfg.wifiPass);
  off += DEVCFG_PASS_MAX;

  writeU32LE(b, off, cfg.artnetFallbackIp);
  off += 4;
  writeU16LE(b, off, cfg.artnetPort);
  off += 2;

  b[off++] = cfg.dmxTxGpio;
  b[off++] = cfg.dmxRxGpio;
  b[off++] = cfg.dmxRtsGpio;
  b[off++] = cfg.ledGpio;

  // reserved2 (32 bytes) stays zero.

  uint32_t crc = devcfgCrc32(b.data(), DEVCFG_CRC_OFFSET);
  writeU32LE(b, DEVCFG_CRC_OFFSET, crc);

  return b;
}
