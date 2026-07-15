// test_artnet_router.cpp — host tests for the portable Art-Net packet
// building + per-universe routing (artnet_router.h/.cpp). This is the host
// gate for Wave 3's "not optional" ArtSync + per-destination routing --
// see FORMAT.md's "Art-Net Wire Universe & Destination Routing" and the
// Wave 3 task spec's "Host tests (Phase 1)" list, which this file covers
// directly (the device-only socket wrapper, artnet_sink.cpp, is HIL-only;
// see tests/hil/test_l2_artnet.py).
#include "artnet_router.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

struct RecordedPacket {
  uint32_t ip;
  uint16_t port;
  std::vector<uint8_t> bytes;
};

// Records every sendTo() call in order, verbatim -- exactly what a real
// socket would have been asked to send.
class MockTransport : public IArtNetTransport {
public:
  void sendTo(uint32_t ip, uint16_t port, const uint8_t* data, uint16_t len) override {
    RecordedPacket p;
    p.ip = ip;
    p.port = port;
    p.bytes.assign(data, data + len);
    calls.push_back(p);
  }
  std::vector<RecordedPacket> calls;
};

static uint16_t opcodeOf(const RecordedPacket& p) {
  return static_cast<uint16_t>(p.bytes[8]) | (static_cast<uint16_t>(p.bytes[9]) << 8);
}

static uint16_t wireUniverseOf(const RecordedPacket& p) {
  // SubUni (low byte) + Net (high 7 bits), big-endian split -- see
  // buildArtDmxPacket.
  return static_cast<uint16_t>(p.bytes[14]) | (static_cast<uint16_t>(p.bytes[15] & 0x7F) << 8);
}

static uint8_t sequenceOf(const RecordedPacket& p) { return p.bytes[12]; }

static std::vector<uint8_t> makeUniverseData(uint8_t fill) {
  return std::vector<uint8_t>(DMX_UNIVERSE_SIZE, fill);
}

static void test_send_stamps_wire_universe_not_internal_index() {
  printf("Test: send() stamps the routed wire universe, not the internal index\n");
  ArtNetRouter router(/*fallbackIp=*/0);
  ArtNetDest dest{};
  dest.ip = 0xC0A80132;    // 192.168.1.50
  dest.wireUniverse = 7;   // deliberately different from the universe index below
  router.setDest(/*universeIndex=*/2, dest);

  MockTransport transport;
  auto data = makeUniverseData(0xAB);
  router.send(2, data.data(), static_cast<uint16_t>(data.size()), transport);

  CHECK(transport.calls.size() == 1);
  CHECK(transport.calls[0].ip == 0xC0A80132);
  CHECK(opcodeOf(transport.calls[0]) == ARTNET_OP_DMX);
  CHECK(wireUniverseOf(transport.calls[0]) == 7);
  CHECK(wireUniverseOf(transport.calls[0]) != 2);
  CHECK(transport.calls[0].bytes.size() == 18 + DMX_UNIVERSE_SIZE);
  CHECK(std::memcmp(transport.calls[0].bytes.data() + 18, data.data(), data.size()) == 0);
}

static void test_sequence_numbers_advance_independently_per_universe() {
  printf("Test: sequence numbers advance independently per universe (interleaved)\n");
  ArtNetRouter router(/*fallbackIp=*/0xC0A80101);
  router.setDest(0, ArtNetDest{0xC0A80150, 0});
  router.setDest(1, ArtNetDest{0xC0A80150, 1});  // same node, second output

  MockTransport transport;
  auto data = makeUniverseData(0x11);

  // Interleave: u0, u1, u0, u1, u0 -- each universe's own sequence must
  // advance by exactly 1 per call to that universe, unaffected by calls to
  // the other.
  router.send(0, data.data(), (uint16_t)data.size(), transport);  // u0 seq 0
  router.send(1, data.data(), (uint16_t)data.size(), transport);  // u1 seq 0
  router.send(0, data.data(), (uint16_t)data.size(), transport);  // u0 seq 1
  router.send(1, data.data(), (uint16_t)data.size(), transport);  // u1 seq 1
  router.send(0, data.data(), (uint16_t)data.size(), transport);  // u0 seq 2

  CHECK(transport.calls.size() == 5);
  CHECK(sequenceOf(transport.calls[0]) == 0);  // u0
  CHECK(sequenceOf(transport.calls[1]) == 0);  // u1
  CHECK(sequenceOf(transport.calls[2]) == 1);  // u0
  CHECK(sequenceOf(transport.calls[3]) == 1);  // u1
  CHECK(sequenceOf(transport.calls[4]) == 2);  // u0

  // Same IP, different wire universes: both destinations are the "same
  // node, second output" case -- both must actually go to that IP.
  CHECK(transport.calls[0].ip == 0xC0A80150);
  CHECK(transport.calls[1].ip == 0xC0A80150);
  CHECK(wireUniverseOf(transport.calls[0]) == 0);
  CHECK(wireUniverseOf(transport.calls[1]) == 1);
}

static void test_frameEnd_emits_exactly_one_artsync_after_data() {
  printf("Test: frameEnd() emits exactly one ArtSync, after the data packets\n");
  ArtNetRouter router(/*fallbackIp=*/0xC0A80101);
  router.setDest(0, ArtNetDest{0xC0A80150, 0});
  router.setDest(1, ArtNetDest{0xC0A80151, 0});

  MockTransport transport;
  auto data = makeUniverseData(0x22);
  router.send(0, data.data(), (uint16_t)data.size(), transport);
  router.send(1, data.data(), (uint16_t)data.size(), transport);
  router.frameEnd(transport);

  CHECK(transport.calls.size() == 3);
  CHECK(opcodeOf(transport.calls[0]) == ARTNET_OP_DMX);
  CHECK(opcodeOf(transport.calls[1]) == ARTNET_OP_DMX);
  CHECK(opcodeOf(transport.calls[2]) == ARTNET_OP_SYNC);
  CHECK(transport.calls[2].bytes.size() == ARTNET_SYNC_PACKET_SIZE);
  // ArtSync always broadcasts -- it must reach every node regardless of
  // each universe's own (possibly unicast) destination.
  CHECK(transport.calls[2].ip == 0xFFFFFFFFu);

  int syncCount = 0;
  for (const auto& p : transport.calls) {
    if (opcodeOf(p) == ARTNET_OP_SYNC) syncCount++;
  }
  CHECK(syncCount == 1);
}

static void test_dest_ip_zero_falls_back_to_configured_fallback() {
  printf("Test: destination ip==0 resolves to the router's fallback IP\n");
  ArtNetRouter router(/*fallbackIp=*/0x0A000005);
  router.setDest(3, ArtNetDest{0, 9});  // no explicit ip -- use the fallback

  MockTransport transport;
  auto data = makeUniverseData(0x33);
  router.send(3, data.data(), (uint16_t)data.size(), transport);

  CHECK(transport.calls.size() == 1);
  CHECK(transport.calls[0].ip == 0x0A000005);
  CHECK(wireUniverseOf(transport.calls[0]) == 9);
}

static void test_dest_and_fallback_both_zero_broadcasts() {
  printf("Test: ip==0 and fallback==0 -> broadcast (precedence's last tier)\n");
  ArtNetRouter router(/*fallbackIp=*/0);
  router.setDest(0, ArtNetDest{0, 0});

  MockTransport transport;
  auto data = makeUniverseData(0x44);
  router.send(0, data.data(), (uint16_t)data.size(), transport);

  CHECK(transport.calls.size() == 1);
  CHECK(transport.calls[0].ip == 0xFFFFFFFFu);
}

static void test_unset_universe_defaults_to_internal_index_never_crashes() {
  printf("Test: a universe with no setDest() call defaults to {ip=0, wireUniverse=index}\n");
  ArtNetRouter router(/*fallbackIp=*/0x0A000009);
  // Deliberately never call setDest for universe 5.

  MockTransport transport;
  auto data = makeUniverseData(0x55);
  router.send(5, data.data(), (uint16_t)data.size(), transport);

  CHECK(transport.calls.size() == 1);
  CHECK(transport.calls[0].ip == 0x0A000009);  // fell back, no crash
  CHECK(wireUniverseOf(transport.calls[0]) == 5);  // defaulted to its own index
}

static void test_out_of_range_universe_is_a_safe_no_op() {
  printf("Test: universeIndex >= MAX_UNIVERSES is a no-op, not a crash\n");
  ArtNetRouter router(/*fallbackIp=*/0);
  MockTransport transport;
  auto data = makeUniverseData(0x66);
  router.setDest(200, ArtNetDest{0x0A000001, 3});  // out of range -- ignored
  router.send(200, data.data(), (uint16_t)data.size(), transport);
  CHECK(transport.calls.empty());
}

static void test_build_art_dmx_packet_header_bytes() {
  printf("Test: buildArtDmxPacket header bytes (ID, opcode, protover, length)\n");
  uint8_t pkt[ARTNET_DMX_PACKET_MAX];
  uint8_t data[3] = {1, 2, 3};  // odd length -> must be padded to even
  uint16_t n = buildArtDmxPacket(pkt, /*wireUniverse=*/300, /*sequence=*/42, data, 3);

  CHECK(std::memcmp(pkt, "Art-Net", 7) == 0);
  CHECK(pkt[7] == 0);
  CHECK(pkt[8] == 0x00 && pkt[9] == 0x50);  // OpCode 0x5000, little-endian
  CHECK(pkt[10] == 0 && pkt[11] == 14);     // ProtVer 14, big-endian
  CHECK(pkt[12] == 42);                     // sequence, as given
  CHECK(pkt[13] == 0);                      // physical
  // wireUniverse=300 = 0x012C -> SubUni=0x2C, Net=0x01
  CHECK(pkt[14] == 0x2C);
  CHECK(pkt[15] == 0x01);
  CHECK(pkt[16] == 0 && pkt[17] == 4);  // length 3 padded to 4, big-endian
  CHECK(n == 18 + 4);
  CHECK(pkt[18] == 1 && pkt[19] == 2 && pkt[20] == 3 && pkt[21] == 0);  // pad byte
}

static void test_build_art_sync_packet_bytes() {
  printf("Test: buildArtSyncPacket bytes\n");
  uint8_t pkt[ARTNET_SYNC_PACKET_SIZE];
  uint16_t n = buildArtSyncPacket(pkt);
  CHECK(n == 14);
  CHECK(std::memcmp(pkt, "Art-Net", 7) == 0);
  CHECK(pkt[8] == 0x00 && pkt[9] == 0x52);  // OpCode 0x5200, little-endian
  CHECK(pkt[10] == 0 && pkt[11] == 14);     // ProtVer 14
  CHECK(pkt[12] == 0 && pkt[13] == 0);      // Aux1/Aux2
}

int main() {
  test_send_stamps_wire_universe_not_internal_index();
  test_sequence_numbers_advance_independently_per_universe();
  test_frameEnd_emits_exactly_one_artsync_after_data();
  test_dest_ip_zero_falls_back_to_configured_fallback();
  test_dest_and_fallback_both_zero_broadcasts();
  test_unset_universe_defaults_to_internal_index_never_crashes();
  test_out_of_range_universe_is_a_safe_no_op();
  test_build_art_dmx_packet_header_bytes();
  test_build_art_sync_packet_bytes();

  if (g_fail == 0) {
    printf("All artnet_router tests passed!\n");
    return 0;
  }
  printf("%d artnet_router tests FAILED\n", g_fail);
  return 1;
}
