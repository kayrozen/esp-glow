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
  router.send(0, data.data(), (uint16_t)data.size(), transport);  // u0 seq 1
  router.send(1, data.data(), (uint16_t)data.size(), transport);  // u1 seq 1
  router.send(0, data.data(), (uint16_t)data.size(), transport);  // u0 seq 2
  router.send(1, data.data(), (uint16_t)data.size(), transport);  // u1 seq 2
  router.send(0, data.data(), (uint16_t)data.size(), transport);  // u0 seq 3

  CHECK(transport.calls.size() == 5);
  CHECK(sequenceOf(transport.calls[0]) == 1);  // u0
  CHECK(sequenceOf(transport.calls[1]) == 1);  // u1
  CHECK(sequenceOf(transport.calls[2]) == 2);  // u0
  CHECK(sequenceOf(transport.calls[3]) == 2);  // u1
  CHECK(sequenceOf(transport.calls[4]) == 3);  // u0

  // Same IP, different wire universes: both destinations are the "same
  // node, second output" case -- both must actually go to that IP.
  CHECK(transport.calls[0].ip == 0xC0A80150);
  CHECK(transport.calls[1].ip == 0xC0A80150);
  CHECK(wireUniverseOf(transport.calls[0]) == 0);
  CHECK(wireUniverseOf(transport.calls[1]) == 1);

  ArtNetRouter wrapRouter(/*fallbackIp=*/0xC0A80101);
  MockTransport wrapTransport;
  for (int i = 0; i < 256; ++i) {
    wrapRouter.send(0, data.data(), (uint16_t)data.size(), wrapTransport);
  }
  CHECK(wrapTransport.calls.size() == 256);
  CHECK(sequenceOf(wrapTransport.calls[0]) == 1);
  CHECK(sequenceOf(wrapTransport.calls[254]) == 255);
  CHECK(sequenceOf(wrapTransport.calls[255]) == 1);
}

static void test_frameEnd_emits_nothing_when_nothing_routed() {
  printf("Test: frameEnd() sends no ArtSync at all when no universe resolves to a destination\n");
  ArtNetRouter router(/*fallbackIp=*/0);  // no fallback -- every universe is unrouted
  MockTransport transport;
  router.frameEnd(transport);
  CHECK(transport.calls.empty());
}

static void test_frameEnd_sends_one_unicast_artsync_per_distinct_ip() {
  printf("Test: frameEnd() sends one unicast ArtSync per distinct routed IP, deduplicated\n");
  ArtNetRouter router(/*fallbackIp=*/0);
  router.setDest(0, ArtNetDest{0x0A000001, 0});
  router.setDest(1, ArtNetDest{0x0A000001, 1});  // same node, second output -- dedup target
  router.setDest(2, ArtNetDest{0x0A000002, 0});  // a different node

  MockTransport transport;
  auto data = makeUniverseData(0x33);
  // frameEnd() targets only universes actually send() for THIS frame, not
  // every setDest() call ever made -- mirrors the real per-frame call
  // pattern (send() for every active universe, then frameEnd() once).
  router.send(0, data.data(), (uint16_t)data.size(), transport);
  router.send(1, data.data(), (uint16_t)data.size(), transport);
  router.send(2, data.data(), (uint16_t)data.size(), transport);
  transport.calls.clear();  // isolate frameEnd()'s own output from the sends above

  router.frameEnd(transport);

  CHECK(transport.calls.size() == 2);
  for (const auto& p : transport.calls) {
    CHECK(opcodeOf(p) == ARTNET_OP_SYNC);
    CHECK(p.ip == 0x0A000001u || p.ip == 0x0A000002u);
  }
  CHECK(transport.calls[0].ip != transport.calls[1].ip);
}

static void test_frameEnd_ignores_universes_never_sent_this_frame() {
  // Regression test for the bug this design point exists to prevent: a
  // universe left at its default {ip=0, wireUniverse=index} slot (never
  // setDest()'d, never send()'d -- e.g. a .show that only uses 2 of the
  // 8 MAX_UNIVERSES slots) must not resolve through a non-zero fallbackIp_
  // into a phantom ArtSync target. Only send()-touched universes count.
  printf("Test: frameEnd() never targets a universe that had no send() this frame, "
         "even with a non-zero fallback\n");
  ArtNetRouter router(/*fallbackIp=*/0x0A000099);  // non-zero: every unused slot would
                                                    // resolve through this if not for
                                                    // the activeThisFrame_ tracking
  router.setDest(0, ArtNetDest{0x0A000001, 0});

  MockTransport transport;
  auto data = makeUniverseData(0x11);
  router.send(0, data.data(), (uint16_t)data.size(), transport);  // only universe 0 is active
  transport.calls.clear();

  router.frameEnd(transport);

  CHECK(transport.calls.size() == 1);
  CHECK(transport.calls[0].ip == 0x0A000001u);  // never 0x0A000099 (the fallback)
}

static void test_frameEnd_broadcast_mode_always_sends_one_broadcast() {
  printf("Test: syncBroadcast=true -> exactly one broadcast ArtSync, regardless of routing\n");
  ArtNetRouter router(/*fallbackIp=*/0, ARTNET_PORT, /*syncBroadcast=*/true);
  router.setDest(0, ArtNetDest{0x0A000001, 0});
  router.setDest(1, ArtNetDest{0x0A000002, 0});

  MockTransport transport;
  router.frameEnd(transport);

  CHECK(transport.calls.size() == 1);
  CHECK(transport.calls[0].ip == 0xFFFFFFFFu);
  CHECK(opcodeOf(transport.calls[0]) == ARTNET_OP_SYNC);

  // Even with nothing routed at all, broadcast mode still sends its one
  // unconditional ArtSync -- this is the spec-literal behavior, kept
  // available for a node that requires it.
  ArtNetRouter emptyRouter(/*fallbackIp=*/0, ARTNET_PORT, /*syncBroadcast=*/true);
  MockTransport emptyTransport;
  emptyRouter.frameEnd(emptyTransport);
  CHECK(emptyTransport.calls.size() == 1);
  CHECK(emptyTransport.calls[0].ip == 0xFFFFFFFFu);
}

static void test_frameEnd_emits_targeted_artsync_after_data() {
  // Was test_frameEnd_emits_exactly_one_artsync_after_data, asserting
  // ArtSync always broadcasts unconditionally. That is exactly the other
  // half of the root cause this test file guards against (see PR1's
  // rewrite of test_dest_and_fallback_both_zero_broadcasts above): ~44
  // unconditional broadcasts/s on top of the per-universe ones, with no
  // node needing a broadcast when its unicast address is already known.
  // Rewritten to assert the fixed default (syncBroadcast=false): one
  // ArtSync per distinct routed IP, not one broadcast for all of them.
  // See test_frameEnd_broadcast_mode_always_sends_one_broadcast above for
  // the still-available opt-in spec-literal broadcast behavior.
  printf("Test: frameEnd() emits one targeted ArtSync per distinct IP, after the data packets\n");
  ArtNetRouter router(/*fallbackIp=*/0xC0A80101);
  router.setDest(0, ArtNetDest{0xC0A80150, 0});
  router.setDest(1, ArtNetDest{0xC0A80151, 0});

  MockTransport transport;
  auto data = makeUniverseData(0x22);
  router.send(0, data.data(), (uint16_t)data.size(), transport);
  router.send(1, data.data(), (uint16_t)data.size(), transport);
  router.frameEnd(transport);

  CHECK(transport.calls.size() == 4);
  CHECK(opcodeOf(transport.calls[0]) == ARTNET_OP_DMX);
  CHECK(opcodeOf(transport.calls[1]) == ARTNET_OP_DMX);
  CHECK(opcodeOf(transport.calls[2]) == ARTNET_OP_SYNC);
  CHECK(opcodeOf(transport.calls[3]) == ARTNET_OP_SYNC);
  CHECK(transport.calls[2].bytes.size() == ARTNET_SYNC_PACKET_SIZE);
  CHECK(transport.calls[3].bytes.size() == ARTNET_SYNC_PACKET_SIZE);
  // Each node's own unicast address, not a shared broadcast -- and never
  // 0xFFFFFFFF, since every universe here has an explicit unicast route.
  CHECK(transport.calls[2].ip == 0xC0A80150u);
  CHECK(transport.calls[3].ip == 0xC0A80151u);

  int syncCount = 0;
  for (const auto& p : transport.calls) {
    if (opcodeOf(p) == ARTNET_OP_SYNC) syncCount++;
  }
  CHECK(syncCount == 2);
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

static void test_dest_and_fallback_both_zero_drops_no_broadcast() {
  // Was test_dest_and_fallback_both_zero_broadcasts, asserting the old
  // implicit-broadcast-by-default behavior. That behavior is exactly the
  // root cause of the network-wide memory-leak/ENOMEM bug this test file
  // now guards against (see FORMAT.md / the PR description): an unrouted
  // universe with no fallback configured broadcast unconditionally,
  // flooding the WiFi driver's static TX buffer pool at frame rate.
  // Rewritten to assert the fixed contract -- ip==0 and fallback==0 now
  // means "no destination," so the universe is dropped, not broadcast.
  printf("Test: ip==0 and fallback==0 -> dropped, not broadcast\n");
  ArtNetRouter router(/*fallbackIp=*/0);
  router.setDest(0, ArtNetDest{0, 0});

  MockTransport transport;
  auto data = makeUniverseData(0x44);
  CHECK(router.unroutedDropCount() == 0);
  router.send(0, data.data(), (uint16_t)data.size(), transport);

  CHECK(transport.calls.empty());
  CHECK(router.unroutedDropCount() == 1);
}

static void test_explicit_broadcast_fallback_still_broadcasts() {
  printf("Test: fallbackIp explicitly 0xFFFFFFFF -> broadcast is honored, not dropped\n");
  ArtNetRouter router(/*fallbackIp=*/0xFFFFFFFFu);
  router.setDest(0, ArtNetDest{0, 4});

  MockTransport transport;
  auto data = makeUniverseData(0x77);
  router.send(0, data.data(), (uint16_t)data.size(), transport);

  CHECK(transport.calls.size() == 1);
  CHECK(transport.calls[0].ip == 0xFFFFFFFFu);
  CHECK(router.unroutedDropCount() == 0);
}

static void test_routed_universe_unaffected_by_drop_fix() {
  printf("Test: a universe with an explicit unicast route is never dropped\n");
  ArtNetRouter router(/*fallbackIp=*/0);
  router.setDest(1, ArtNetDest{0x0A000005, 2});

  MockTransport transport;
  auto data = makeUniverseData(0x88);
  router.send(1, data.data(), (uint16_t)data.size(), transport);

  CHECK(transport.calls.size() == 1);
  CHECK(transport.calls[0].ip == 0x0A000005);
  CHECK(router.unroutedDropCount() == 0);
}

static void test_sequence_does_not_advance_on_drop() {
  printf("Test: sequence number does not advance for a dropped (unrouted) universe\n");
  ArtNetRouter router(/*fallbackIp=*/0);
  MockTransport transport;
  auto data = makeUniverseData(0x99);

  // Universe 0 has no route and no fallback -- every send() drops.
  router.send(0, data.data(), (uint16_t)data.size(), transport);
  router.send(0, data.data(), (uint16_t)data.size(), transport);
  router.send(0, data.data(), (uint16_t)data.size(), transport);
  CHECK(transport.calls.empty());
  CHECK(router.unroutedDropCount() == 3);

  // Route now appears (mirrors ArtPoll discovery arriving mid-show): the
  // very first packet sent must carry sequence 1, not 4 -- a jump here
  // would look like 3 dropped frames to the node, when really none were
  // ever sent under this route.
  router.setDest(0, ArtNetDest{0x0A000001, 0});
  router.send(0, data.data(), (uint16_t)data.size(), transport);
  CHECK(transport.calls.size() == 1);
  CHECK(sequenceOf(transport.calls[0]) == 1);
}

static void test_unrouted_drop_count_only_counts_drops() {
  printf("Test: unroutedDropCount() only increments on drops, not on successful sends\n");
  ArtNetRouter router(/*fallbackIp=*/0x0A000010);
  MockTransport transport;
  auto data = makeUniverseData(0xAA);

  router.send(0, data.data(), (uint16_t)data.size(), transport);  // resolves via fallback
  CHECK(router.unroutedDropCount() == 0);
  CHECK(transport.calls.size() == 1);
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
  test_frameEnd_emits_nothing_when_nothing_routed();
  test_frameEnd_sends_one_unicast_artsync_per_distinct_ip();
  test_frameEnd_ignores_universes_never_sent_this_frame();
  test_frameEnd_broadcast_mode_always_sends_one_broadcast();
  test_frameEnd_emits_targeted_artsync_after_data();
  test_dest_ip_zero_falls_back_to_configured_fallback();
  test_dest_and_fallback_both_zero_drops_no_broadcast();
  test_explicit_broadcast_fallback_still_broadcasts();
  test_routed_universe_unaffected_by_drop_fix();
  test_sequence_does_not_advance_on_drop();
  test_unrouted_drop_count_only_counts_drops();
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
