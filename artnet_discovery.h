// artnet_discovery.h — Wave 3 Phase 3: ArtPoll / ArtPollReply discovery.
//
// Portable (no ESP_PLATFORM dependency), fully host-testable: parsing a real
// ArtPollReply's bytes, building an ArtPoll request, and maintaining the
// small "which nodes are alive right now, and what universes do they carry"
// table that feeds the same setDest() routing table Phase 1 built (see
// artnet_router.h) -- discovery only fills in what the .show left
// unspecified (ArtNetDest.ip == 0); an explicit .show route is never
// overwritten (see resolveDiscoveredDests).
//
// Byte offsets below are cross-checked against a real, mature open-source
// Art-Net implementation (OpenLightingProject/ola,
// plugins/artnet/ArtNetPackets.h's artnet_reply_s), not reconstructed from
// memory alone -- this codebase has already lost days once to a hallucinated
// API, and Art-Net is a published spec, not something to guess at. Only the
// fields Phase 3 actually needs are interpreted (source IP, ShortName,
// LongName, NetSwitch/SubSwitch, NumPorts, SwOut[]); every other documented
// field (PortTypes/GoodInput/GoodOutput/Status1/Status2/MAC/BindIp/etc.) is
// skipped over, not guessed at, to keep the parsed surface honest.
#pragma once

#include "show.h"
#include <cstdint>
#include <cstddef>

constexpr uint16_t ARTNET_OP_POLL = 0x2000;
constexpr uint16_t ARTNET_OP_POLL_REPLY = 0x2100;

constexpr size_t ARTNET_POLL_PACKET_SIZE = 14;
constexpr size_t ARTNET_SHORT_NAME_LEN = 18;
constexpr size_t ARTNET_LONG_NAME_LEN = 64;
constexpr uint8_t ARTNET_MAX_REPLY_PORTS = 4;

// The minimum ArtPollReply length this parser needs: through SwOut[4] at
// packet offset 190..193 inclusive (offset 10 = the 10-byte ID+OpCode
// header; see artnet_reply_s in ola's ArtNetPackets.h for the full,
// 239-byte fixed layout -- this parser simply never reads past the fields
// it uses, whether or not a given node sends the full 239).
constexpr size_t ARTNET_POLL_REPLY_MIN_LEN = 194;

// One parsed ArtPollReply -- just the fields Wave 3 Phase 3 routes on.
struct ArtNetPollReply {
  uint32_t ip = 0;  // packed host-byte-order IPv4 (same convention as
                     // ArtNetDest.ip/CFG1's artnetFallbackIp), from the
                     // reply body's own IP field.
  char shortName[ARTNET_SHORT_NAME_LEN + 1] = {};  // NUL-terminated
  char longName[ARTNET_LONG_NAME_LEN + 1] = {};    // NUL-terminated
  uint8_t netSwitch = 0;     // bits 0-6 of the 15-bit Port-Address
  uint8_t subSwitch = 0;     // bits 4-7 (the SubNet nibble)
  uint8_t portCount = 0;     // 0..ARTNET_MAX_REPLY_PORTS
  // Resolved 15-bit wire universe per output port (netSwitch:subSwitch:
  // SwOut[i], the same Net/SubNet/Universe composition ArtDMX sends --
  // see artnet_router.cpp's buildArtDmxPacket). Only the first portCount
  // entries are meaningful.
  uint16_t wireUniverse[ARTNET_MAX_REPLY_PORTS] = {};
};

// Parses an ArtPollReply (OpCode 0x2100) from `data`/`len` into `out`.
// Strict, bounds-checked, never reads past `len` regardless of what the
// packet claims -- same discipline as loadShow/parseProfile/parseDeviceConfig.
// Rejects: bad magic, wrong OpCode, a buffer shorter than
// ARTNET_POLL_REPLY_MIN_LEN, or portCount > ARTNET_MAX_REPLY_PORTS.
bool parseArtPollReply(const uint8_t* data, size_t len, ArtNetPollReply& out);

// Builds an ArtPoll packet (OpCode 0x2000) into `out` (must have room for
// at least ARTNET_POLL_PACKET_SIZE bytes). TalkToMe/Priority are both sent
// as 0 (no unsolicited updates requested; the caller polls periodically
// instead). Returns the packet length.
uint16_t buildArtPollPacket(uint8_t* out);

// One discovered node, as tracked by ArtNetDiscovery.
struct DiscoveredNode {
  uint32_t ip = 0;
  char shortName[ARTNET_SHORT_NAME_LEN + 1] = {};
  char longName[ARTNET_LONG_NAME_LEN + 1] = {};
  uint16_t wireUniverse[ARTNET_MAX_REPLY_PORTS] = {};
  uint8_t portCount = 0;
  float lastSeenSec = 0.0f;
};

// Tracks which Art-Net nodes are currently alive, from ArtPollReply
// traffic. A node not heard from within `timeoutSec` is dropped -- the
// caller (resolveDiscoveredDests) then naturally reverts that node's
// universes to fallback/broadcast instead of leaving them pointed at a
// vanished IP.
class ArtNetDiscovery {
public:
  explicit ArtNetDiscovery(float timeoutSec = 10.0f);

  // Feed one already-parsed reply, observed at time `nowSec`. Adds the
  // node if new, otherwise refreshes its fields and lastSeenSec. Silently
  // ignored if the table is full (kMaxNodes) and this is a genuinely new IP.
  void onReply(const ArtNetPollReply& reply, float nowSec);

  // Drops every node whose lastSeenSec is more than timeoutSec behind
  // nowSec. Call this once per poll cycle, before resolving destinations.
  void expire(float nowSec);

  size_t nodeCount() const { return count_; }
  const DiscoveredNode* node(size_t i) const;
  const DiscoveredNode* findByWireUniverse(uint16_t wireUniverse) const;

private:
  static constexpr size_t kMaxNodes = 16;
  DiscoveredNode nodes_[kMaxNodes];
  size_t count_ = 0;
  float timeoutSec_;
};

// Wave 3 Phase 3's precedence rule, as pure data-in/data-out logic: an
// explicit .show route (showDest[u].ip != 0) always wins and is copied
// through unchanged. Otherwise, if `discovery` currently has a node
// advertising showDest[u].wireUniverse, that node's IP is used; if no
// node advertises it (never discovered, or the node just expired),
// resolved[u].ip comes back 0 -- fallback/broadcast, never darkness.
void resolveDiscoveredDests(const ArtNetDest showDest[], uint8_t universeCount,
                             const ArtNetDiscovery& discovery, ArtNetDest resolved[]);
