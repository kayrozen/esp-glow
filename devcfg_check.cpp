// devcfg_check.cpp -- host CLI used by web/shared/test-devcfg.mjs as the
// round-trip oracle proving the JS CFG1 encoder (web/shared/devcfg.js)
// produces bytes the real device parser (device_config.cpp) accepts, and
// with the exact fields intended. Mirrors fdef_check.cpp's role for
// .fdef/PFX1: same C++ source the firmware links (device_config.cpp),
// compiled for the host instead of the device.
//
// Usage: devcfg_check <blob.cfg1>
// Prints {"ok":true,...fields...} or {"ok":false,"error":"..."} as JSON.

#include "device_config.h"

#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

static std::vector<uint8_t> readFileFromDisk(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static void printJsonString(const char* s) {
  putchar('"');
  for (const char* p = s; *p; p++) {
    unsigned char c = static_cast<unsigned char>(*p);
    if (c == '"' || c == '\\') { putchar('\\'); putchar(c); }
    else if (c < 0x20) { printf("\\u%04x", c); }
    else putchar(c);
  }
  putchar('"');
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    fprintf(stderr, "Usage: %s <blob.cfg1>\n", argv[0]);
    return 1;
  }

  std::vector<uint8_t> bytes = readFileFromDisk(argv[1]);
  if (bytes.empty()) {
    printf("{\"ok\":false,\"error\":\"cannot read %s or file is empty\"}\n", argv[1]);
    return 1;
  }

  DeviceConfig cfg;
  if (!parseDeviceConfig(bytes.data(), bytes.size(), cfg)) {
    printf("{\"ok\":false,\"error\":\"parseDeviceConfig rejected the blob\"}\n");
    return 1;
  }

  printf("{\"ok\":true,");
  printf("\"usbMidiHost\":%s,", cfg.usbMidiHost ? "true" : "false");
  printf("\"skipWifi\":%s,", cfg.skipWifi ? "true" : "false");
  printf("\"wifiSsid\":"); printJsonString(cfg.wifiSsid); printf(",");
  printf("\"wifiPass\":"); printJsonString(cfg.wifiPass); printf(",");
  printf("\"artnetFallbackIp\":%u,", (unsigned)cfg.artnetFallbackIp);
  printf("\"artnetPort\":%u,", (unsigned)cfg.artnetPort);
  printf("\"dmxTxGpio\":%u,", (unsigned)cfg.dmxTxGpio);
  printf("\"dmxRxGpio\":%u,", (unsigned)cfg.dmxRxGpio);
  printf("\"dmxRtsGpio\":%u,", (unsigned)cfg.dmxRtsGpio);
  printf("\"ledGpio\":%u", (unsigned)cfg.ledGpio);
  printf("}\n");
  return 0;
}
