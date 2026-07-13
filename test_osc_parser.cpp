#include "osc_parser.h"

#include <cmath>
#include <cstdio>
#include <cstring>
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

// --- packet builder --------------------------------------------------------

static void padTo4(std::vector<uint8_t>& v) {
  while (v.size() % 4 != 0) v.push_back(0);
}

// Builds a well-formed OSC packet: address, NUL-padded to 4; type tag
// ("," + typeChar), NUL-padded to 4; then the 4-byte big-endian arg.
static std::vector<uint8_t> buildOsc(const char* address, char typeChar, uint32_t argBits) {
  std::vector<uint8_t> pkt(address, address + std::strlen(address));
  pkt.push_back(0);
  padTo4(pkt);

  pkt.push_back(',');
  pkt.push_back(static_cast<uint8_t>(typeChar));
  pkt.push_back(0);
  padTo4(pkt);

  pkt.push_back(static_cast<uint8_t>((argBits >> 24) & 0xFF));
  pkt.push_back(static_cast<uint8_t>((argBits >> 16) & 0xFF));
  pkt.push_back(static_cast<uint8_t>((argBits >> 8) & 0xFF));
  pkt.push_back(static_cast<uint8_t>(argBits & 0xFF));
  return pkt;
}

static uint32_t floatBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

static const OscBinding kBindings[] = {
  {"/cue/1", ControlType::Button, 1},
  {"/fader/0", ControlType::Fader, 0},
};
static const OscAddressMap kMap{kBindings, 2};

// Wraps `elements` (each already a complete OSC message) into a bundle:
// "#bundle\0" + 8-byte timetag (arbitrary, ignored by parseOscPacket) +
// one 4-byte big-endian size prefix per element.
static std::vector<uint8_t> buildBundle(const std::vector<std::vector<uint8_t>>& elements,
                                        uint64_t timetag = 1) {
  std::vector<uint8_t> pkt = {'#', 'b', 'u', 'n', 'd', 'l', 'e', 0};
  for (int shift = 56; shift >= 0; shift -= 8) {
    pkt.push_back(static_cast<uint8_t>((timetag >> shift) & 0xFF));
  }
  for (const auto& elem : elements) {
    uint32_t size = static_cast<uint32_t>(elem.size());
    pkt.push_back(static_cast<uint8_t>((size >> 24) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((size >> 16) & 0xFF));
    pkt.push_back(static_cast<uint8_t>((size >> 8) & 0xFF));
    pkt.push_back(static_cast<uint8_t>(size & 0xFF));
    pkt.insert(pkt.end(), elem.begin(), elem.end());
  }
  return pkt;
}

struct CollectedEvents {
  std::vector<ControlEvent> events;
};

static void collectEvent(void* ctx, const ControlEvent& ev) {
  static_cast<CollectedEvents*>(ctx)->events.push_back(ev);
}

// --- valid packets -----------------------------------------------------------

void test_button_pressed() {
  TEST("parseOsc: button address, float arg 1.0 -> pressed=true");
  auto pkt = buildOsc("/cue/1", 'f', floatBits(1.0f));
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == true);
  CHECK(ev.type == ControlType::Button);
  CHECK(ev.id == 1);
  CHECK(ev.pressed == true);
}

void test_button_released() {
  TEST("parseOsc: button address, float arg 0.0 -> pressed=false");
  auto pkt = buildOsc("/cue/1", 'f', floatBits(0.0f));
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == true);
  CHECK(ev.pressed == false);
}

void test_fader_float() {
  TEST("parseOsc: fader address, float arg -> value passed through");
  auto pkt = buildOsc("/fader/0", 'f', floatBits(0.75f));
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == true);
  CHECK(ev.type == ControlType::Fader);
  CHECK(ev.id == 0);
  CHECK(std::fabs(ev.value - 0.75f) < 1e-6f);
  CHECK(ev.pressed == false);
}

void test_fader_int_normalized() {
  TEST("parseOsc: fader address, int32 arg -> normalized by /127");
  auto pkt = buildOsc("/fader/0", 'i', 64u);
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == true);
  CHECK(std::fabs(ev.value - (64.0f / 127.0f)) < 1e-6f);
}

void test_button_int_arg() {
  TEST("parseOsc: button address, int32 arg nonzero -> pressed=true");
  auto pkt = buildOsc("/cue/1", 'i', 127u);
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == true);
  CHECK(ev.pressed == true);
}

// --- rejections --------------------------------------------------------------

void test_unmapped_address() {
  TEST("parseOsc: address not in the map -> false");
  auto pkt = buildOsc("/nope/9", 'f', floatBits(1.0f));
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == false);
}

void test_unsupported_type_tag() {
  TEST("parseOsc: type tag other than f/i -> false");
  auto pkt = buildOsc("/cue/1", 's', 0);
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == false);
}

void test_missing_comma() {
  TEST("parseOsc: type tag field not starting with ',' -> false");
  auto pkt = buildOsc("/cue/1", 'f', floatBits(1.0f));
  // Overwrite the type tag's leading ',' (first byte after the padded
  // address) with something else.
  size_t addrSlot = 8;  // "/cue/1\0" is 7 bytes, padded to 8
  pkt[addrSlot] = 'x';
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == false);
}

void test_null_and_empty() {
  TEST("parseOsc: null pointer / zero length -> false, never crashes");
  ControlEvent ev{};
  CHECK(parseOsc(nullptr, 0, kMap, ev) == false);
  uint8_t dummy = 0;
  CHECK(parseOsc(&dummy, 0, kMap, ev) == false);
}

void test_bad_padding() {
  TEST("parseOsc: non-zero padding byte after the address's NUL -> false");
  auto pkt = buildOsc("/cue/1", 'f', floatBits(1.0f));
  // "/cue/1\0" is 7 bytes; padded to 8 with one extra pad byte at index 7.
  CHECK(pkt.size() > 7);
  pkt[7] = 0xAA;  // corrupt the padding byte -- should be NUL
  ControlEvent ev{};
  CHECK(parseOsc(pkt.data(), pkt.size(), kMap, ev) == false);
}

// Truncated packet: allocate a heap buffer sized EXACTLY to the truncation
// point (like test_web_protocol.cpp's isHelloCommand overread regression
// test) so ASan catches any read one byte past the end.
void test_truncated_no_overread() {
  TEST("parseOsc: truncated packet -> false, no overread (ASan-checked)");
  auto full = buildOsc("/cue/1", 'f', floatBits(1.0f));

  for (size_t n = 0; n < full.size(); ++n) {
    uint8_t* buf = new uint8_t[n];
    for (size_t i = 0; i < n; ++i) buf[i] = full[i];
    ControlEvent ev{};
    bool ok = parseOsc(buf, n, kMap, ev);
    CHECK(ok == false);  // every truncation of a well-formed packet must fail
    delete[] buf;
  }
}

void test_no_terminator_in_buffer() {
  TEST("parseOsc: address with no NUL anywhere in buffer -> false, no overread");
  std::vector<uint8_t> pkt = {'/', 'c', 'u', 'e'};  // no terminator at all
  uint8_t* buf = new uint8_t[pkt.size()];
  for (size_t i = 0; i < pkt.size(); ++i) buf[i] = pkt[i];
  ControlEvent ev{};
  CHECK(parseOsc(buf, pkt.size(), kMap, ev) == false);
  delete[] buf;
}

// --- OSC bundles -------------------------------------------------------------

void test_is_osc_bundle_detection() {
  TEST("isOscBundle: recognizes the marker, rejects a plain message and short buffers");
  auto bundle = buildBundle({});
  CHECK(isOscBundle(bundle.data(), bundle.size()) == true);

  auto msg = buildOsc("/cue/1", 'f', floatBits(1.0f));
  CHECK(isOscBundle(msg.data(), msg.size()) == false);

  CHECK(isOscBundle(nullptr, 0) == false);
  std::vector<uint8_t> tooShort = {'#', 'b', 'u', 'n'};
  CHECK(isOscBundle(tooShort.data(), tooShort.size()) == false);
}

void test_parse_osc_packet_plain_message_same_as_parse_osc() {
  TEST("parseOscPacket: a plain (non-bundle) message behaves exactly like parseOsc");
  auto pkt = buildOsc("/cue/1", 'f', floatBits(1.0f));
  CollectedEvents got;
  size_t n = parseOscPacket(pkt.data(), pkt.size(), kMap, &collectEvent, &got);
  CHECK(n == 1);
  CHECK(got.events.size() == 1);
  CHECK(got.events[0].type == ControlType::Button);
  CHECK(got.events[0].id == 1);
  CHECK(got.events[0].pressed == true);
}

void test_parse_osc_packet_bundle_dispatches_all_messages_in_order() {
  TEST("parseOscPacket: a bundle of two messages dispatches both, in wire order");
  auto m1 = buildOsc("/cue/1", 'f', floatBits(1.0f));
  auto m2 = buildOsc("/fader/0", 'f', floatBits(0.75f));
  auto bundle = buildBundle({m1, m2});

  CollectedEvents got;
  size_t n = parseOscPacket(bundle.data(), bundle.size(), kMap, &collectEvent, &got);
  CHECK(n == 2);
  CHECK(got.events.size() == 2);
  CHECK(got.events[0].type == ControlType::Button);
  CHECK(got.events[0].id == 1);
  CHECK(got.events[1].type == ControlType::Fader);
  CHECK(std::fabs(got.events[1].value - 0.75f) < 1e-6f);
}

void test_parse_osc_packet_bundle_skips_unmatched_and_malformed_elements() {
  TEST("parseOscPacket: a bundle mixing a matched, an unmapped, and a malformed element only dispatches the matched one");
  auto matched = buildOsc("/cue/1", 'f', floatBits(1.0f));
  auto unmapped = buildOsc("/nope/9", 'f', floatBits(1.0f));
  std::vector<uint8_t> garbage = {'x', 'y', 'z', 0};  // not a valid OSC message
  auto bundle = buildBundle({matched, unmapped, garbage});

  CollectedEvents got;
  size_t n = parseOscPacket(bundle.data(), bundle.size(), kMap, &collectEvent, &got);
  CHECK(n == 1);
  CHECK(got.events.size() == 1);
  CHECK(got.events[0].id == 1);
}

void test_parse_osc_packet_nested_bundle() {
  TEST("parseOscPacket: a bundle containing a nested bundle dispatches messages from both levels");
  auto inner = buildBundle({buildOsc("/fader/0", 'f', floatBits(0.5f))});
  auto outer = buildBundle({buildOsc("/cue/1", 'f', floatBits(1.0f)), inner});

  CollectedEvents got;
  size_t n = parseOscPacket(outer.data(), outer.size(), kMap, &collectEvent, &got);
  CHECK(n == 2);
  CHECK(got.events.size() == 2);
  CHECK(got.events[0].type == ControlType::Button);
  CHECK(got.events[1].type == ControlType::Fader);
}

void test_parse_osc_packet_empty_bundle_dispatches_nothing() {
  TEST("parseOscPacket: an empty bundle (no elements) dispatches nothing, doesn't crash");
  auto bundle = buildBundle({});
  CollectedEvents got;
  size_t n = parseOscPacket(bundle.data(), bundle.size(), kMap, &collectEvent, &got);
  CHECK(n == 0);
  CHECK(got.events.empty());
}

void test_parse_osc_packet_truncated_bundle_no_overread() {
  TEST("parseOscPacket: every truncation of a well-formed bundle fails safely, no overread (ASan-checked)");
  auto m1 = buildOsc("/cue/1", 'f', floatBits(1.0f));
  auto m2 = buildOsc("/fader/0", 'f', floatBits(0.75f));
  auto full = buildBundle({m1, m2});

  for (size_t n = 0; n < full.size(); ++n) {
    uint8_t* buf = new uint8_t[n];
    for (size_t i = 0; i < n; ++i) buf[i] = full[i];
    CollectedEvents got;
    // No assertion on the count -- a truncated bundle may still yield a
    // valid prefix of complete elements. The only contract under test is
    // "never reads past buf[n-1]", which ASan enforces.
    parseOscPacket(buf, n, kMap, &collectEvent, &got);
    delete[] buf;
  }
}

void test_parse_osc_packet_size_prefix_past_buffer_stops_safely() {
  TEST("parseOscPacket: an element size prefix claiming more bytes than remain stops walking, no overread");
  auto m1 = buildOsc("/cue/1", 'f', floatBits(1.0f));
  auto bundle = buildBundle({m1});
  // Corrupt the first (only) element's size prefix to claim far more
  // bytes than the buffer actually has.
  size_t sizeOffset = 16;  // right after the 8-byte marker + 8-byte timetag
  bundle[sizeOffset] = 0x7F;
  bundle[sizeOffset + 1] = 0xFF;
  bundle[sizeOffset + 2] = 0xFF;
  bundle[sizeOffset + 3] = 0xFF;

  CollectedEvents got;
  size_t n = parseOscPacket(bundle.data(), bundle.size(), kMap, &collectEvent, &got);
  CHECK(n == 0);  // the corrupted element is never read
  CHECK(got.events.empty());
}

void test_parse_osc_packet_null_and_empty() {
  TEST("parseOscPacket: null pointer / zero length -> 0 events, never crashes");
  CollectedEvents got;
  CHECK(parseOscPacket(nullptr, 0, kMap, &collectEvent, &got) == 0);
  uint8_t dummy = 0;
  CHECK(parseOscPacket(&dummy, 0, kMap, &collectEvent, &got) == 0);
  CHECK(got.events.empty());
}

int main() {
  test_button_pressed();
  test_button_released();
  test_fader_float();
  test_fader_int_normalized();
  test_button_int_arg();

  test_unmapped_address();
  test_unsupported_type_tag();
  test_missing_comma();
  test_null_and_empty();
  test_bad_padding();
  test_truncated_no_overread();
  test_no_terminator_in_buffer();

  test_is_osc_bundle_detection();
  test_parse_osc_packet_plain_message_same_as_parse_osc();
  test_parse_osc_packet_bundle_dispatches_all_messages_in_order();
  test_parse_osc_packet_bundle_skips_unmatched_and_malformed_elements();
  test_parse_osc_packet_nested_bundle();
  test_parse_osc_packet_empty_bundle_dispatches_nothing();
  test_parse_osc_packet_truncated_bundle_no_overread();
  test_parse_osc_packet_size_prefix_past_buffer_stops_safely();
  test_parse_osc_packet_null_and_empty();

  if (g_failCount == 0) {
    printf("\nAll tests passed.\n");
    return 0;
  } else {
    printf("\n%d test(s) failed.\n", g_failCount);
    return 1;
  }
}
