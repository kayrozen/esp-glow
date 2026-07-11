// test_osc_parser.cpp — host test for the OSC packet parser.
#include "osc_parser.h"

#include <cstdio>
#include <cstdint>
#include <cstring>

static int g_fail = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

// Helper: encode a null-padded OSC string into buf at offset; return new offset.
static size_t putStr(uint8_t* buf, size_t off, const char* s) {
  size_t n = strlen(s);
  memcpy(buf + off, s, n);
  off += n;
  buf[off++] = 0;
  while (off % 4 != 0) buf[off++] = 0;
  return off;
}
static size_t putBE32(uint8_t* buf, size_t off, uint32_t v) {
  buf[off++] = (v >> 24) & 0xFF;
  buf[off++] = (v >> 16) & 0xFF;
  buf[off++] = (v >> 8) & 0xFF;
  buf[off++] = v & 0xFF;
  return off;
}

static void test_simple_message_float() {
  printf("Test: /cue/1/go with float 1.0\n");
  uint8_t buf[64];
  size_t off = 0;
  off = putStr(buf, off, "/cue/1/go");
  off = putStr(buf, off, ",f");
  // 1.0f big-endian = 0x3F800000
  off = putBE32(buf, off, 0x3F800000u);

  OscMessage m;
  CHECK(parseOsc(buf, off, m));
  CHECK(m.valid);
  CHECK(m.addressLen == 9);
  CHECK(memcmp(m.address, "/cue/1/go", 9) == 0);
  CHECK(m.arg.type == OscArg::Float32);
  CHECK(m.arg.f == 1.0f);
}

static void test_int_arg() {
  printf("Test: /cue/2/go with int 1\n");
  uint8_t buf[64];
  size_t off = 0;
  off = putStr(buf, off, "/cue/2/go");
  off = putStr(buf, off, ",i");
  off = putBE32(buf, off, 1u);
  OscMessage m;
  CHECK(parseOsc(buf, off, m));
  CHECK(m.arg.type == OscArg::Int32);
  CHECK(m.arg.i == 1);
}

static void test_no_args() {
  printf("Test: address with no args\n");
  uint8_t buf[64];
  size_t off = 0;
  off = putStr(buf, off, "/esp-glow/blackout");
  off = putStr(buf, off, ",");
  OscMessage m;
  CHECK(parseOsc(buf, off, m));
  CHECK(m.valid);
  CHECK(m.arg.type == OscArg::None);
}

static void test_first_numeric_wins() {
  printf("Test: first numeric arg is extracted, later ones skipped\n");
  uint8_t buf[64];
  size_t off = 0;
  off = putStr(buf, off, "/x");
  off = putStr(buf, off, ",sif");  // string, int, float
  off = putStr(buf, off, "hi");
  off = putBE32(buf, off, 42u);          // int
  off = putBE32(buf, off, 0x40000000u);  // float 2.0
  OscMessage m;
  CHECK(parseOsc(buf, off, m));
  CHECK(m.arg.type == OscArg::Int32);
  CHECK(m.arg.i == 42);
}

static void test_bundle_first_element() {
  printf("Test: bundle unwraps to first element\n");
  uint8_t buf[128];
  size_t off = 0;
  // "#bundle\0" + 8-byte time tag (zeros)
  memcpy(buf + off, "#bundle\0", 8); off += 8;
  memset(buf + off, 0, 8); off += 8;
  // Element size (BE32) of the inner message:
  size_t sizePos = off; off += 4;
  size_t innerStart = off;
  off = putStr(buf, off, "/cue/3/go");
  off = putStr(buf, off, ",f");
  off = putBE32(buf, off, 0x3F000000u);  // 0.5f
  size_t innerLen = off - innerStart;
  putBE32(buf, sizePos, (uint32_t)innerLen);

  OscMessage m;
  CHECK(parseOsc(buf, off, m));
  CHECK(m.valid);
  CHECK(memcmp(m.address, "/cue/3/go", 9) == 0);
  CHECK(m.arg.type == OscArg::Float32);
  CHECK(m.arg.f == 0.5f);
}

static void test_too_short_rejected() {
  printf("Test: too-short buffer rejected\n");
  uint8_t buf[3] = {1, 2, 3};
  OscMessage m;
  CHECK(!parseOsc(buf, 3, m));
  CHECK(!m.valid);
}

static void test_missing_comma_rejected() {
  printf("Test: typetag missing leading comma rejected\n");
  uint8_t buf[64];
  size_t off = 0;
  off = putStr(buf, off, "/x");
  off = putStr(buf, off, "f");  // no comma
  OscMessage m;
  CHECK(!parseOsc(buf, off, m));
}

static void test_bool_tags_no_data() {
  printf("Test: T/F/N/I tags consume no data bytes\n");
  uint8_t buf[64];
  size_t off = 0;
  off = putStr(buf, off, "/x");
  off = putStr(buf, off, ",T");  // True, no data
  OscMessage m;
  CHECK(parseOsc(buf, off, m));
  CHECK(m.arg.type == OscArg::None);  // no numeric arg
}

int main() {
  test_simple_message_float();
  test_int_arg();
  test_no_args();
  test_first_numeric_wins();
  test_bundle_first_element();
  test_too_short_rejected();
  test_missing_comma_rejected();
  test_bool_tags_no_data();
  if (g_fail == 0) {
    printf("All osc_parser tests passed!\n");
    return 0;
  }
  printf("%d osc_parser tests FAILED\n", g_fail);
  return 1;
}
