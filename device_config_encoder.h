#pragma once

#include "device_config.h"
#include <vector>
#include <cstdint>

// CFG1 encoder -- the C++ twin of web/shared/devcfg.js's
// encodeDeviceConfig. Unlike profile_encoder.h/controller_encoder.h (whose
// job is compiling a host-only TEXT GRAMMAR the device never sees), CFG1
// has no text form -- it's just a fixed-field struct -- so this encoder is
// useful on-device too: device_config_web.cpp uses it to serialize the
// currently-EFFECTIVE config (whether sourced from devcfg or Kconfig
// defaults) for the device console's GET /devcfg (see its header comment).
// It also exists for host tests (test_device_config.cpp) and
// devcfg_check.cpp's golden-blob round trip against the JS encoder
// (web/shared/test-devcfg.mjs). Linked into glow_core alongside the parser.

// Encodes `cfg` into a DEVCFG_BLOB_SIZE-byte CFG1 blob, computing the CRC32
// itself. Truncates wifiSsid/wifiPass to DEVCFG_SSID_MAX/DEVCFG_PASS_MAX
// bytes if longer (callers building test fixtures should keep them within
// bounds; this never overflows the fixed-width fields regardless).
std::vector<uint8_t> encodeDeviceConfig(const DeviceConfig& cfg);
