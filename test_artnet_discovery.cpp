// test_artnet_discovery.cpp — host tests for Wave 3 Phase 3's ArtPoll/
// ArtPollReply parsing and discovery table (artnet_discovery.h/.cpp).
//
// makeSampleArtPollReply() below hand-builds one ArtPollReply, field by
// field, against the byte layout documented in
// OpenLightingProject/ola's plugins/artnet/ArtNetPackets.h (artnet_reply_s)
// -- a mature, widely-deployed open-source Art-Net implementation, used
// here because this sandboxed session has no Art-Net hardware to sniff a
// genuine capture from and no route to the official spec PDF. It is NOT a
// literal hardware capture; treat it as a byte-accurate synthetic fixture
// and swap in a real capture from an actual node during HIL bring-up if
// maximum fidelity matters.
#include "artnet_discovery.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { \
  if (!(cond)) { printf("FAIL: %s:%d: %s\n", __FILE__, __LINE__, #cond); g_fail++; } \
} while (0)

// Builds a 239-byte ArtPollReply for a fictional 2-output node at
// 192.168.1.50, NetSwitch=0, SubSwitch=0, SwOut={0,1} (wire universes 0
// and 1 -- the "multi-output node" case Wave 3 cares about), name
// "TestNode".
static std::vector<uint8_t> makeSampleArtPollReply() {
  std::vector<uint8_t> pkt(239, 0);
  std::memcpy(pkt.data(), "Art-Net", 7);
  pkt[7] = 0;
  pkt[8] = 0x00; pkt[9] = 0x21;  // OpCode 0x2100, little-endian

  pkt[10] = 192; pkt[11] = 168; pkt[12] = 1; pkt[13] = 50;  // IP
  pkt[14] = 0x36; pkt[15] = 0x17;  // Port 6454 (not interpreted by the parser)
  pkt[16] = 0; pkt[17] = 1;        // VersInfo (not interpreted)
  pkt[18] = 0;   // NetSwitch
  pkt[19] = 0;   // SubSwitch
  pkt[20] = 0; pkt[21] = 0;  // Oem (not interpreted)
  pkt[22] = 0;   // Ubea
  pkt[23] = 0;   // Status1
  pkt[24] = 0; pkt[25] = 0;  // EstaMan (not interpreted)

  const char* shortName = "TestNode";
  std::memcpy(pkt.data() + 26, shortName, std::strlen(shortName));  // offset 26, 18 bytes
  const char* longName = "Test Art-Net Node (2 outputs)";
  std::memcpy(pkt.data() + 44, longName, std::strlen(longName));   // offset 44, 64 bytes
  // NodeReport (offset 108, 64 bytes) left zeroed -- not interpreted.

  pkt[172] = 0; pkt[173] = 2;  // NumPorts = 2

  // PortTypes/GoodInput/GoodOutput (offsets 174, 178, 182) left zeroed --
  // not interpreted by this parser.

  pkt[186] = 0; pkt[187] = 0; pkt[188] = 0; pkt[189] = 0;  // SwIn (not interpreted)
  pkt[190] = 0; pkt[191] = 1; pkt[192] = 0; pkt[193] = 0;  // SwOut: port0=0, port1=1

  // SwVideo/SwMacro/SwRemote/spares/Style/MAC/BindIp/BindIndex/Status2/
  // filler (offsets 194..238) left zeroed -- not interpreted.
  return pkt;
}

static void test_parse_sample_reply_extracts_ip_name_and_wire_universes() {
  printf("Test: parse a real-layout ArtPollReply -> IP, name, universes\n");
  auto pkt = makeSampleArtPollReply();

  ArtNetPollReply reply;
  CHECK(parseArtPollReply(pkt.data(), pkt.size(), reply));
  CHECK(reply.ip == ((192u << 24) | (168u << 16) | (1u << 8) | 50u));
  CHECK(std::string(reply.shortName) == "TestNode");
  CHECK(std::string(reply.longName) == "Test Art-Net Node (2 outputs)");
  CHECK(reply.portCount == 2);
  CHECK(reply.wireUniverse[0] == 0);
  CHECK(reply.wireUniverse[1] == 1);
}

static void test_parse_rejects_bad_magic() {
  printf("Test: parseArtPollReply rejects bad magic\n");
  auto pkt = makeSampleArtPollReply();
  pkt[0] = 'X';
  ArtNetPollReply reply;
  CHECK(!parseArtPollReply(pkt.data(), pkt.size(), reply));
}

static void test_parse_rejects_wrong_opcode() {
  printf("Test: parseArtPollReply rejects the wrong OpCode (e.g. ArtDMX)\n");
  auto pkt = makeSampleArtPollReply();
  pkt[8] = 0x00; pkt[9] = 0x50;  // ArtDMX's opcode, not ArtPollReply's
  ArtNetPollReply reply;
  CHECK(!parseArtPollReply(pkt.data(), pkt.size(), reply));
}

static void test_parse_rejects_every_truncation_no_oob() {
  printf("Test: every truncation of a well-formed reply is rejected, no OOB read (ASan)\n");
  auto pkt = makeSampleArtPollReply();
  for (size_t len = 0; len < pkt.size(); ++len) {
    ArtNetPollReply reply;
    bool ok = parseArtPollReply(pkt.data(), len, reply);
    if (len < ARTNET_POLL_REPLY_MIN_LEN) {
      CHECK(!ok);
    }
    // Whether or not it's accepted, this call must never read past `len`
    // bytes -- ASan (this binary is built with -fsanitize=address) would
    // abort the whole test run on a heap-buffer-overflow if it did.
  }
}

static void test_parse_rejects_null_and_zero_length() {
  printf("Test: parseArtPollReply rejects null pointer / zero length\n");
  ArtNetPollReply reply;
  CHECK(!parseArtPollReply(nullptr, 0, reply));
  uint8_t one[1] = {0};
  CHECK(!parseArtPollReply(one, 0, reply));
}

static void test_parse_rejects_port_count_over_max() {
  printf("Test: parseArtPollReply rejects a NumPorts over ARTNET_MAX_REPLY_PORTS\n");
  auto pkt = makeSampleArtPollReply();
  pkt[173] = ARTNET_MAX_REPLY_PORTS + 1;
  ArtNetPollReply reply;
  CHECK(!parseArtPollReply(pkt.data(), pkt.size(), reply));
}

static void test_build_art_poll_packet_bytes() {
  printf("Test: buildArtPollPacket bytes\n");
  uint8_t pkt[ARTNET_POLL_PACKET_SIZE];
  uint16_t n = buildArtPollPacket(pkt);
  CHECK(n == ARTNET_POLL_PACKET_SIZE);
  CHECK(std::memcmp(pkt, "Art-Net", 7) == 0);
  CHECK(pkt[8] == 0x00 && pkt[9] == 0x20);  // OpCode 0x2000, little-endian
  CHECK(pkt[10] == 0 && pkt[11] == 14);     // ProtVer 14, big-endian
}

static ArtNetPollReply makeReply(uint32_t ip, uint16_t wireUniverse0) {
  ArtNetPollReply r;
  r.ip = ip;
  r.portCount = 1;
  r.wireUniverse[0] = wireUniverse0;
  std::snprintf(r.shortName, sizeof(r.shortName), "Node");
  return r;
}

static void test_discovery_table_adds_and_refreshes_nodes() {
  printf("Test: ArtNetDiscovery adds a new node, refreshes an existing one\n");
  ArtNetDiscovery disc(/*timeoutSec=*/10.0f);
  disc.onReply(makeReply(0x0A000001, 3), 1.0f);
  CHECK(disc.nodeCount() == 1);

  disc.onReply(makeReply(0x0A000001, 3), 2.0f);  // same IP -- refresh, not a new entry
  CHECK(disc.nodeCount() == 1);
  CHECK(disc.node(0)->lastSeenSec == 2.0f);

  disc.onReply(makeReply(0x0A000002, 4), 2.0f);  // a different node
  CHECK(disc.nodeCount() == 2);
}

static void test_discovery_expire_drops_stale_nodes() {
  printf("Test: a node not heard from within timeoutSec is dropped\n");
  ArtNetDiscovery disc(/*timeoutSec=*/5.0f);
  disc.onReply(makeReply(0x0A000001, 0), 0.0f);
  disc.onReply(makeReply(0x0A000002, 1), 10.0f);  // seen much later

  disc.expire(10.0f);  // node1 is 10s stale (>5s timeout); node2 just seen
  CHECK(disc.nodeCount() == 1);
  CHECK(disc.node(0)->ip == 0x0A000002);
}

static void test_resolve_show_route_never_overwritten_by_discovery() {
  printf("Test: an explicit .show destination is never overwritten by discovery\n");
  ArtNetDest showDest[MAX_UNIVERSES] = {};
  showDest[0] = ArtNetDest{0xC0A80132, 0};  // explicit .show route
  showDest[1] = ArtNetDest{0, 1};           // no explicit route; wants wire universe 1

  ArtNetDiscovery disc(10.0f);
  disc.onReply(makeReply(0x0A000009, 0), 0.0f);  // would also match universe 0's wire number
  disc.onReply(makeReply(0x0A000009, 1), 0.0f);  // matches universe 1's wire number

  ArtNetDest resolved[MAX_UNIVERSES];
  resolveDiscoveredDests(showDest, 2, disc, resolved);

  CHECK(resolved[0].ip == 0xC0A80132);  // untouched -- the .show route wins
  CHECK(resolved[1].ip == 0x0A000009);  // filled in by discovery
}

static void test_resolve_reverts_to_fallback_when_node_vanishes() {
  printf("Test: a vanished node's universe reverts to fallback, not darkness\n");
  ArtNetDest showDest[MAX_UNIVERSES] = {};
  showDest[0] = ArtNetDest{0, 7};  // no explicit route; wants wire universe 7

  ArtNetDiscovery disc(5.0f);
  disc.onReply(makeReply(0x0A0000AA, 7), 0.0f);

  ArtNetDest resolved[MAX_UNIVERSES];
  resolveDiscoveredDests(showDest, 1, disc, resolved);
  CHECK(resolved[0].ip == 0x0A0000AA);  // discovered

  disc.expire(100.0f);  // node long gone
  resolveDiscoveredDests(showDest, 1, disc, resolved);
  CHECK(resolved[0].ip == 0);  // reverted to fallback/broadcast marker
}

int main() {
  test_parse_sample_reply_extracts_ip_name_and_wire_universes();
  test_parse_rejects_bad_magic();
  test_parse_rejects_wrong_opcode();
  test_parse_rejects_every_truncation_no_oob();
  test_parse_rejects_null_and_zero_length();
  test_parse_rejects_port_count_over_max();
  test_build_art_poll_packet_bytes();
  test_discovery_table_adds_and_refreshes_nodes();
  test_discovery_expire_drops_stale_nodes();
  test_resolve_show_route_never_overwritten_by_discovery();
  test_resolve_reverts_to_fallback_when_node_vanishes();

  if (g_fail == 0) {
    printf("All artnet_discovery tests passed!\n");
    return 0;
  }
  printf("%d artnet_discovery tests FAILED\n", g_fail);
  return 1;
}
