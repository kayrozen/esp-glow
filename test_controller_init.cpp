// test_controller_init.cpp — P1.1: sendControllerInit walks a
// MidiControllerProfile's init blobs and sends each verbatim via
// IRawMidiOutput, in order, exactly once. The mock sink below stands in
// for DIN MIDI OUT (a UART write) or a USB-MIDI OUT endpoint -- see
// controller_init.h's header comment for why this is a distinct seam from
// led_feedback.h's fixed-3-byte IMidiOutput.

#include "controller_init.h"
#include "controller_encoder.h"

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

// Records each sendRaw() call as a distinct byte vector, in call order --
// exactly what "a mock MIDI-out sink receives the exact init bytes on
// connect" needs to assert on.
class RecordingSink : public IRawMidiOutput {
public:
  void sendRaw(const uint8_t* bytes, size_t len) override {
    calls.emplace_back(bytes, bytes + len);
  }
  std::vector<std::vector<uint8_t>> calls;
};

}  // namespace

void test_init_blobs_sent_in_order_exact_bytes() {
  TEST("sendControllerInit sends each INIT blob, in order, byte-exact");

  ControllerBuilder b;
  b.name = "Test";
  b.initBlobs.push_back({0xF0, 0x47, 0x7F, 0x29, 0x60, 0x00, 0x04, 0x40, 0x00, 0x00, 0x00, 0xF7});
  b.initBlobs.push_back({0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(err.empty());

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));

  RecordingSink sink;
  sendControllerInit(p, sink);

  CHECK(sink.calls.size() == 2);
  CHECK(sink.calls[0] == b.initBlobs[0]);
  CHECK(sink.calls[1] == b.initBlobs[1]);
}

void test_no_init_blobs_sends_nothing() {
  TEST("No INIT SYSEX in the .mdef -> sendControllerInit sends nothing");

  ControllerBuilder b;
  b.name = "No Init";
  b.pads.push_back({53, 60});
  std::string err;
  std::vector<uint8_t> blob = b.encode(err);
  CHECK(blob[4] == 1);  // no CH, no INIT -> version 1

  MidiControllerProfile p;
  CHECK(parseMidiController(blob.data(), blob.size(), p));
  CHECK(p.initCount == 0);

  RecordingSink sink;
  sendControllerInit(p, sink);
  CHECK(sink.calls.empty());
}

void test_old_v1_v2_profile_sends_nothing() {
  TEST("An old (pre-INIT) v1/v2 profile sends nothing, same as no INIT line");

  MidiControllerProfile p;  // default-constructed, as if loaded from a v1/v2 blob
  CHECK(p.initCount == 0);

  RecordingSink sink;
  sendControllerInit(p, sink);
  CHECK(sink.calls.empty());
}

int main() {
  test_init_blobs_sent_in_order_exact_bytes();
  test_no_init_blobs_sends_nothing();
  test_old_v1_v2_profile_sends_nothing();

  if (g_failCount == 0) {
    printf("\nAll tests passed!\n");
    return 0;
  }
  printf("\n%d test(s) FAILED\n", g_failCount);
  return 1;
}
