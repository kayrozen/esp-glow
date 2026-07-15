// test_device_config.cpp — CFG1 flash-time device config: parser +
// CRC32 round-trip tests. Same discipline as test_fixture_profile.cpp/
// test_mdef.cpp: a strict, bounds-checked parser is a security boundary,
// so every rejection path (bad magic, bad version, bad CRC, truncated,
// all-0xFF erased flash) gets its own test, plus a full-field round trip
// and a single-bit-flip CRC check.

#include "device_config.h"
#include "device_config_encoder.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_failCount = 0;

#define CHECK(cond)                                           \
  do {                                                        \
    if (!(cond)) {                                            \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++;                                          \
    }                                                          \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

namespace {

DeviceConfig makeConfig() {
  DeviceConfig cfg;
  cfg.usbMidiHost = true;
  cfg.skipWifi = false;
  std::snprintf(cfg.wifiSsid, sizeof(cfg.wifiSsid), "TestNet");
  std::snprintf(cfg.wifiPass, sizeof(cfg.wifiPass), "hunter2pass");
  cfg.artnetFallbackIp = 0xC0A80105u;  // 192.168.1.5
  cfg.artnetPort = 6454;
  cfg.dmxTxGpio = 17;
  cfg.dmxRxGpio = 18;
  cfg.dmxRtsGpio = 8;
  cfg.ledGpio = 2;
  return cfg;
}

}  // namespace

void test_roundtrip_all_fields() {
  TEST("Round-trip: every field survives encode -> parse");

  DeviceConfig cfg = makeConfig();
  std::vector<uint8_t> blob = encodeDeviceConfig(cfg);
  CHECK(blob.size() == DEVCFG_BLOB_SIZE);
  CHECK(blob[0] == 'C' && blob[1] == 'F' && blob[2] == 'G' && blob[3] == '1');
  CHECK(blob[4] == 1);  // version

  DeviceConfig out;
  CHECK(parseDeviceConfig(blob.data(), blob.size(), out));

  CHECK(out.usbMidiHost == true);
  CHECK(out.skipWifi == false);
  CHECK(std::strcmp(out.wifiSsid, "TestNet") == 0);
  CHECK(std::strcmp(out.wifiPass, "hunter2pass") == 0);
  CHECK(out.artnetFallbackIp == 0xC0A80105u);
  CHECK(out.artnetPort == 6454);
  CHECK(out.dmxTxGpio == 17);
  CHECK(out.dmxRxGpio == 18);
  CHECK(out.dmxRtsGpio == 8);
  CHECK(out.ledGpio == 2);
}

void test_flags_both_bits() {
  TEST("skipWifi and usbMidiHost are independent flag bits");

  DeviceConfig cfg = makeConfig();
  cfg.usbMidiHost = false;
  cfg.skipWifi = true;
  std::vector<uint8_t> blob = encodeDeviceConfig(cfg);

  DeviceConfig out;
  CHECK(parseDeviceConfig(blob.data(), blob.size(), out));
  CHECK(out.usbMidiHost == false);
  CHECK(out.skipWifi == true);
}

void test_broadcast_fallback_ip_zero() {
  TEST("artnetFallbackIp = 0 round-trips as 0 (broadcast)");

  DeviceConfig cfg = makeConfig();
  cfg.artnetFallbackIp = 0;
  std::vector<uint8_t> blob = encodeDeviceConfig(cfg);

  DeviceConfig out;
  CHECK(parseDeviceConfig(blob.data(), blob.size(), out));
  CHECK(out.artnetFallbackIp == 0);
}

void test_max_length_strings_no_overrun() {
  TEST("SSID/password filling the full field width still NUL-terminates cleanly");

  DeviceConfig cfg = makeConfig();
  std::memset(cfg.wifiSsid, 'A', sizeof(cfg.wifiSsid) - 1);
  cfg.wifiSsid[sizeof(cfg.wifiSsid) - 1] = '\0';  // 32 'A's, exactly DEVCFG_SSID_MAX
  std::memset(cfg.wifiPass, 'B', sizeof(cfg.wifiPass) - 1);
  cfg.wifiPass[sizeof(cfg.wifiPass) - 1] = '\0';  // 64 'B's, exactly DEVCFG_PASS_MAX

  std::vector<uint8_t> blob = encodeDeviceConfig(cfg);
  DeviceConfig out;
  CHECK(parseDeviceConfig(blob.data(), blob.size(), out));
  CHECK(std::strlen(out.wifiSsid) == DEVCFG_SSID_MAX);
  CHECK(std::strlen(out.wifiPass) == DEVCFG_PASS_MAX);
}

void test_bad_magic() {
  TEST("Bad magic is rejected");

  std::vector<uint8_t> blob = encodeDeviceConfig(makeConfig());
  blob[0] = 'X';
  DeviceConfig out;
  CHECK(!parseDeviceConfig(blob.data(), blob.size(), out));
}

void test_bad_version() {
  TEST("Unsupported version is rejected");

  std::vector<uint8_t> blob = encodeDeviceConfig(makeConfig());
  blob[4] = 2;
  DeviceConfig out;
  CHECK(!parseDeviceConfig(blob.data(), blob.size(), out));
}

void test_bad_crc() {
  TEST("Corrupted CRC field is rejected");

  std::vector<uint8_t> blob = encodeDeviceConfig(makeConfig());
  blob[DEVCFG_CRC_OFFSET] ^= 0xFF;
  DeviceConfig out;
  CHECK(!parseDeviceConfig(blob.data(), blob.size(), out));
}

void test_truncated_buffer() {
  TEST("Truncated buffer (even with a valid-looking prefix) is rejected, no OOB read");

  std::vector<uint8_t> blob = encodeDeviceConfig(makeConfig());
  for (size_t len : {size_t(0), size_t(1), size_t(4), size_t(8), size_t(100), DEVCFG_BLOB_SIZE - 1}) {
    DeviceConfig out;
    CHECK(!parseDeviceConfig(blob.data(), len, out));
  }
}

void test_null_data() {
  TEST("Null data pointer is rejected, not dereferenced");

  DeviceConfig out;
  CHECK(!parseDeviceConfig(nullptr, 0, out));
}

void test_erased_flash_all_0xff() {
  TEST("All-0xFF blob (erased flash, the common blank-board case) is rejected cleanly, "
       "not read as a config with GPIO 255");

  std::vector<uint8_t> blob(DEVCFG_BLOB_SIZE, 0xFF);
  DeviceConfig out;
  CHECK(!parseDeviceConfig(blob.data(), blob.size(), out));
}

void test_all_zero_blob_rejected() {
  TEST("All-zero blob (half-written / freshly-erased-then-zeroed) is rejected: bad magic");

  std::vector<uint8_t> blob(DEVCFG_BLOB_SIZE, 0x00);
  DeviceConfig out;
  CHECK(!parseDeviceConfig(blob.data(), blob.size(), out));
}

void test_single_byte_flip_anywhere_rejected() {
  TEST("Flipping any single byte of a valid blob makes the CRC reject it");

  std::vector<uint8_t> golden = encodeDeviceConfig(makeConfig());
  int rejected = 0;
  for (size_t i = 0; i < golden.size(); i++) {
    std::vector<uint8_t> blob = golden;
    blob[i] ^= 0x01;  // flip the low bit -- smallest possible corruption
    DeviceConfig out;
    if (!parseDeviceConfig(blob.data(), blob.size(), out)) {
      rejected++;
    }
  }
  // Every byte in the blob participates in either the magic/version check
  // or the CRC (the CRC itself covers offsets [0, DEVCFG_CRC_OFFSET), and
  // the stored CRC field's own bytes changing also breaks the comparison)
  // -- so every single-byte flip must be caught.
  CHECK(rejected == static_cast<int>(golden.size()));
}

void test_parse_failure_does_not_modify_output() {
  TEST("A rejected parse leaves the output struct untouched");

  DeviceConfig out;
  out.dmxTxGpio = 99;  // sentinel
  std::vector<uint8_t> blob = encodeDeviceConfig(makeConfig());
  blob[0] = 'X';  // corrupt magic
  CHECK(!parseDeviceConfig(blob.data(), blob.size(), out));
  CHECK(out.dmxTxGpio == 99);  // untouched
}

int main() {
  test_roundtrip_all_fields();
  test_flags_both_bits();
  test_broadcast_fallback_ip_zero();
  test_max_length_strings_no_overrun();
  test_bad_magic();
  test_bad_version();
  test_bad_crc();
  test_truncated_buffer();
  test_null_data();
  test_erased_flash_all_0xff();
  test_all_zero_blob_rejected();
  test_single_byte_flip_anywhere_rejected();
  test_parse_failure_does_not_modify_output();

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
