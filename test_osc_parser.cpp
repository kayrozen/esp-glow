#include "osc_parser.h"
#include "live_control.h"

#include <cstring>
#include <cstdio>
#include <cstdint>
#include <vector>

static int g_failCount = 0;

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failCount++; \
    } \
  } while (0)

#define TEST(name) printf("Test: %s\n", name)

// Helper: build a minimal OSC packet and parse it.
// addr: address string (e.g., "/cue/1")
// argType: 'f' for float, 'i' for int32, ' ' for no argument
// argValue: the value to encode (as float or int32)
static std::vector<uint8_t> buildOscPacket(const char* addr, char argType,
                                            uint32_t argValue) {
  std::vector<uint8_t> pkt;
  size_t addrLen = std::strlen(addr);

  // Address + NUL
  pkt.insert(pkt.end(), (uint8_t*)addr, (uint8_t*)addr + addrLen + 1);

  // Address padding to 4-byte boundary
  size_t pad = (4 - ((addrLen + 1) % 4)) % 4;
  for (size_t i = 0; i < pad; i++) pkt.push_back(0);

  // Type tag: ",f", ",i", or ","
  if (argType == 'f') {
    pkt.push_back(',');
    pkt.push_back('f');
    pkt.push_back(0);
    pkt.push_back(0);
  } else if (argType == 'i') {
    pkt.push_back(',');
    pkt.push_back('i');
    pkt.push_back(0);
    pkt.push_back(0);
  } else {
    pkt.push_back(',');
    pkt.push_back(0);
    pkt.push_back(0);
    pkt.push_back(0);
  }

  // Argument (if any)
  if (argType != ' ') {
    pkt.push_back((argValue >> 24) & 0xFF);
    pkt.push_back((argValue >> 16) & 0xFF);
    pkt.push_back((argValue >> 8) & 0xFF);
    pkt.push_back((argValue >> 0) & 0xFF);
  }

  return pkt;
}

// ---------------------------------------------------------------------------
// parseOsc — valid packets
// ---------------------------------------------------------------------------

void test_parse_osc_button_float_pressed() {
  TEST("parse: OSC button with float 1.0 (pressed)");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/cue/0", 'f', 0x3F800000u);  // 1.0 in IEEE 754
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), map, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 10);
  CHECK(ev.pressed == true);
}

void test_parse_osc_button_float_released() {
  TEST("parse: OSC button with float 0.0 (released)");

  OscAddressBinding bindings[] = {
    {"/cue/1", ControlType::Button, 11}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/cue/1", 'f', 0x00000000u);  // 0.0
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), map, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 11);
  CHECK(ev.pressed == false);
}

void test_parse_osc_button_int_pressed() {
  TEST("parse: OSC button with int32 127 (pressed)");

  OscAddressBinding bindings[] = {
    {"/scene/2", ControlType::Button, 20}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/scene/2", 'i', 0x0000007Fu);  // 127
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), map, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 20);
  CHECK(ev.pressed == true);  // 127/127 = 1.0 > 0.5
}

void test_parse_osc_button_int_released() {
  TEST("parse: OSC button with int32 0 (released)");

  OscAddressBinding bindings[] = {
    {"/scene/3", ControlType::Button, 21}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/scene/3", 'i', 0x00000000u);  // 0
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), map, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 21);
  CHECK(ev.pressed == false);
}

void test_parse_osc_fader_float() {
  TEST("parse: OSC fader with float 0.5");

  OscAddressBinding bindings[] = {
    {"/fader/0", ControlType::Fader, 30}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/fader/0", 'f', 0x3F000000u);  // 0.5 in IEEE 754
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), map, ev));
  CHECK(ev.type == ControlType::Fader);
  CHECK(ev.id == 30);
  CHECK(ev.pressed == false);
  // Value should be approximately 0.5
  CHECK(ev.value >= 0.49f && ev.value <= 0.51f);
}

void test_parse_osc_fader_int() {
  TEST("parse: OSC fader with int32 64");

  OscAddressBinding bindings[] = {
    {"/fader/1", ControlType::Fader, 31}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/fader/1", 'i', 0x00000040u);  // 64
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), map, ev));
  CHECK(ev.type == ControlType::Fader);
  CHECK(ev.id == 31);
  CHECK(ev.pressed == false);
  // 64/127 ~= 0.504
  CHECK(ev.value >= 0.50f && ev.value <= 0.51f);
}

void test_parse_osc_button_no_arg() {
  TEST("parse: OSC button with no argument (defaults to 1.0)");

  OscAddressBinding bindings[] = {
    {"/cue/9", ControlType::Button, 19}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/cue/9", ' ', 0);  // no argument
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), map, ev));
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 19);
  CHECK(ev.pressed == true);  // default 1.0 > 0.5
}

void test_parse_osc_multiple_bindings() {
  TEST("parse: OSC packet with multiple bindings in map");

  OscAddressBinding bindings[] = {
    {"/cue/1", ControlType::Button, 100},
    {"/cue/2", ControlType::Button, 101},
    {"/fader/1", ControlType::Fader, 200}
  };
  OscAddressMap map{bindings, 3};

  auto pkt = buildOscPacket("/fader/1", 'f', 0x3FA00000u);  // 1.25 (clamped later)
  ControlEvent ev;
  CHECK(parseOsc(pkt.data(), pkt.size(), map, ev));
  CHECK(ev.type == ControlType::Fader);
  CHECK(ev.id == 200);
}

// ---------------------------------------------------------------------------
// parseOsc — invalid/edge cases
// ---------------------------------------------------------------------------

void test_parse_osc_truncated_no_address_nul() {
  TEST("parse: OSC packet truncated, no address NUL (no overread)");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  uint8_t pkt[] = {0x2F, 0x63, 0x75, 0x65};  // "/cue" without NUL
  ControlEvent ev;
  CHECK(!parseOsc(pkt, 4, map, ev));
}

void test_parse_osc_truncated_before_type_tag() {
  TEST("parse: OSC packet truncated, type tag out of bounds");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  // Address "/cue/0" (6 bytes) + NUL + no padding (already 7, need 8) + truncated
  uint8_t pkt[] = {0x2F, 0x63, 0x75, 0x65, 0x2F, 0x30, 0x00, 0x00};  // only 8 bytes
  ControlEvent ev;
  CHECK(!parseOsc(pkt, 8, map, ev));
}

void test_parse_osc_truncated_before_argument() {
  TEST("parse: OSC packet truncated, argument incomplete");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  uint8_t pkt[] = {
    0x2F, 0x63, 0x75, 0x65,  // "/cue"
    0x2F, 0x30, 0x00, 0x00,  // "/0\0\0"
    0x2C, 0x66, 0x00, 0x00,  // ",f\0\0"
    0x3F, 0x80               // incomplete float (only 2 bytes of 4)
  };
  ControlEvent ev;
  CHECK(!parseOsc(pkt, sizeof(pkt), map, ev));
}

void test_parse_osc_empty_packet() {
  TEST("parse: OSC packet empty");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  ControlEvent ev;
  CHECK(!parseOsc(nullptr, 0, map, ev));
}

void test_parse_osc_unmapped_address() {
  TEST("parse: OSC packet with unmapped address");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/scene/99", 'f', 0x3F800000u);
  ControlEvent ev;
  CHECK(!parseOsc(pkt.data(), pkt.size(), map, ev));
}

void test_parse_osc_missing_comma_in_type_tag() {
  TEST("parse: OSC packet with type tag missing comma");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  uint8_t pkt[] = {
    0x2F, 0x63, 0x75, 0x65,  // "/cue"
    0x2F, 0x30, 0x00, 0x00,  // "/0\0\0"
    0x66, 0x00, 0x00, 0x00,  // "f\0\0\0" (no comma)
    0x3F, 0x80, 0x00, 0x00   // 1.0
  };
  ControlEvent ev;
  CHECK(!parseOsc(pkt, sizeof(pkt), map, ev));
}

void test_parse_osc_unknown_type_tag() {
  TEST("parse: OSC packet with unknown type tag");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  uint8_t pkt[] = {
    0x2F, 0x63, 0x75, 0x65,  // "/cue"
    0x2F, 0x30, 0x00, 0x00,  // "/0\0\0"
    0x2C, 0x78, 0x00, 0x00,  // ",x\0\0" (unknown type 'x')
    0x3F, 0x80, 0x00, 0x00   // 1.0
  };
  ControlEvent ev;
  CHECK(!parseOsc(pkt, sizeof(pkt), map, ev));
}

// Test with heap buffer sized exactly (ASan will catch overread)
void test_parse_osc_exact_buffer_size() {
  TEST("parse: OSC packet in exact-size heap buffer (ASan check)");

  OscAddressBinding bindings[] = {
    {"/cue/0", ControlType::Button, 10}
  };
  OscAddressMap map{bindings, 1};

  auto pkt = buildOscPacket("/cue/0", 'f', 0x3F800000u);
  size_t exactLen = pkt.size();

  uint8_t* heap = new uint8_t[exactLen];
  std::memcpy(heap, pkt.data(), exactLen);

  ControlEvent ev;
  bool ok = parseOsc(heap, exactLen, map, ev);
  CHECK(ok);
  CHECK(ev.id == 10);

  delete[] heap;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
  printf("=== OSC Parser Tests ===\n");

  // Valid packets
  test_parse_osc_button_float_pressed();
  test_parse_osc_button_float_released();
  test_parse_osc_button_int_pressed();
  test_parse_osc_button_int_released();
  test_parse_osc_fader_float();
  test_parse_osc_fader_int();
  test_parse_osc_button_no_arg();
  test_parse_osc_multiple_bindings();

  // Invalid/edge cases
  test_parse_osc_truncated_no_address_nul();
  test_parse_osc_truncated_before_type_tag();
  test_parse_osc_truncated_before_argument();
  test_parse_osc_empty_packet();
  test_parse_osc_unmapped_address();
  test_parse_osc_missing_comma_in_type_tag();
  test_parse_osc_unknown_type_tag();
  test_parse_osc_exact_buffer_size();

  printf("=== Results ===\n");
  if (g_failCount == 0) {
    printf("All tests passed.\n");
    return 0;
  } else {
    printf("%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
