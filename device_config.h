#pragma once

#include <cstdint>
#include <cstddef>

// CFG1 device config format: little-endian, fixed-size, like every other
// format here. Read once at boot from the raw "devcfg" partition
// (partitions.csv) -- simplicity beats compactness. See FORMAT.md for the
// full writeup, including why `artnetFallbackIp` is named/scoped the way
// it is (Wave 3 will add per-universe Art-Net routing to the .show; this
// field is deliberately only ever the fallback for what the .show does not
// route explicitly).
//
// Header (8 bytes):
//   magic          4 bytes "CFG1"
//   version        u8  = 1
//   flags          u8   (bit0: usbMidiHost, bit1: skipWifi)
//   reserved       u16 = 0
// Network (102 bytes):
//   wifiSsid       32 bytes, NUL-padded
//   wifiPass       64 bytes, NUL-padded
//   artnetFallbackIp    u32   (packed host-byte-order IPv4; 0 = broadcast)
//   artnetPort     u16
// Pins (4 bytes):
//   dmxTxGpio      u8
//   dmxRxGpio      u8
//   dmxRtsGpio     u8
//   ledGpio        u8
// Tail:
//   reserved2      32 bytes = 0   (room for the next few options; no
//                                  version bump needed to use them)
//   crc32          u32 over every byte above (offsets 0..145)
//
// Total blob size: 150 bytes. The "devcfg" partition is 4 KB, so there is
// plenty of headroom beyond this for a future version bump too -- 150
// bytes is just what CFG1 v1 currently defines.
constexpr size_t DEVCFG_BLOB_SIZE = 150;

// Byte offset of the crc32 field; also the number of bytes the CRC covers.
constexpr size_t DEVCFG_CRC_OFFSET = 146;

constexpr uint8_t DEVCFG_FLAG_USB_MIDI_HOST = 0x01;
constexpr uint8_t DEVCFG_FLAG_SKIP_WIFI = 0x02;

constexpr size_t DEVCFG_SSID_MAX = 32;
constexpr size_t DEVCFG_PASS_MAX = 64;

// Label of the raw `data` partition holding the CFG1 blob (partitions.csv,
// subtype 0x40, same "opaque blob, no filesystem" pattern as the "show"
// partition -- see storage_manager.h/device_config_web.h, both of which
// look this partition up by this same label).
#define DEVCFG_PARTITION_LABEL "devcfg"

// Standard CRC-32 (IEEE 802.3 / zlib / PNG): poly 0xEDB88320 (reflected),
// init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Exposed so the JS encoder
// (web/shared/devcfg.js) and the host-only C++ encoder
// (device_config_encoder.h) can be proven to compute byte-identical CRCs
// against this same reference (see test_device_config.cpp's round-trip
// test and web/shared/test-devcfg.mjs's golden-blob check).
uint32_t devcfgCrc32(const uint8_t* data, size_t len);

// Runtime-friendly parsed form. NUL-terminated C strings, not the raw
// fixed-width padded fields -- callers (main.cpp, the /devcfg console
// handler) want to hand wifiSsid/wifiPass straight to wifi_start_sta and
// friends without re-parsing padding.
struct DeviceConfig {
  bool usbMidiHost = false;
  bool skipWifi = false;

  char wifiSsid[DEVCFG_SSID_MAX + 1] = {};
  char wifiPass[DEVCFG_PASS_MAX + 1] = {};

  uint32_t artnetFallbackIp = 0;  // 0 = broadcast
  uint16_t artnetPort = 6454;

  uint8_t dmxTxGpio = 0;
  uint8_t dmxRxGpio = 0;
  uint8_t dmxRtsGpio = 0;
  uint8_t ledGpio = 0;
};

// Parse a CFG1 blob from `data`/`len`. Strict, bounds-checked, CRC-verified
// -- same discipline as parseProfile/loadShow (never reads out of bounds,
// never partially fills `out` on failure). Rejects:
//   - buffers shorter than DEVCFG_BLOB_SIZE
//   - bad magic ("CFG1") -- this alone rejects an erased/all-0xFF
//     partition (a blank board's common case) and a half-written one,
//     long before any GPIO field would ever be read as 0xFF/255
//   - unsupported version (only 1 today)
//   - CRC mismatch (catches a single flipped bit anywhere in the blob,
//     including a magic/version that happens to survive but corrupted
//     payload bytes)
// Returns true and fills `out` only on full success.
bool parseDeviceConfig(const uint8_t* data, size_t len, DeviceConfig& out);
