#include "artnet_discovery.h"
#include <cstring>

namespace {

// Copies a possibly-non-NUL-terminated fixed-width field (`srcLen` bytes)
// into a NUL-terminated destination buffer of `dstCap` bytes (dstCap must
// be srcLen+1). Never reads past srcLen, never writes past dstCap.
void copyFixedField(const uint8_t* src, size_t srcLen, char* dst, size_t dstCap) {
  size_t n = (srcLen < dstCap - 1) ? srcLen : dstCap - 1;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
  // A field that fills its entire wire width has no guaranteed NUL in the
  // source bytes -- stop at the first embedded NUL if there is one, same
  // as any other NUL-padded wire string in this codebase (CFG1's
  // wifiSsid/wifiPass, PFX1/MDF1 name blobs).
  for (size_t i = 0; i < n; ++i) {
    if (src[i] == '\0') {
      dst[i] = '\0';
      break;
    }
  }
}

}  // namespace

bool parseArtPollReply(const uint8_t* data, size_t len, ArtNetPollReply& out) {
  if (!data || len < ARTNET_POLL_REPLY_MIN_LEN) return false;
  if (std::memcmp(data, "Art-Net", 7) != 0 || data[7] != 0) return false;

  uint16_t opcode = static_cast<uint16_t>(data[8]) | (static_cast<uint16_t>(data[9]) << 8);
  if (opcode != ARTNET_OP_POLL_REPLY) return false;

  out = ArtNetPollReply();

  // IP (packet offset 10..13, 4 raw bytes -- no endian ambiguity, packed
  // host-byte-order the same way CFG1/ArtNetDest.ip already is).
  out.ip = (static_cast<uint32_t>(data[10]) << 24) | (static_cast<uint32_t>(data[11]) << 16) |
           (static_cast<uint32_t>(data[12]) << 8) | static_cast<uint32_t>(data[13]);

  // NetSwitch/SubSwitch (offsets 18, 19 -- single bytes, no endian issue).
  out.netSwitch = data[18] & 0x7F;
  out.subSwitch = data[19] & 0x0F;

  // ShortName (offset 26, 18 bytes), LongName (offset 44, 64 bytes).
  copyFixedField(data + 26, ARTNET_SHORT_NAME_LEN, out.shortName, sizeof(out.shortName));
  copyFixedField(data + 44, ARTNET_LONG_NAME_LEN, out.longName, sizeof(out.longName));

  // NumPorts is a 2-byte [Hi, Lo] field at offset 172-173; every real node
  // reports <= 4, so the Hi byte is always 0 in practice, but we only ever
  // read the Lo byte (offset 173) to sidestep having to interpret it as
  // one endianness or another.
  uint8_t portCount = data[173];
  if (portCount > ARTNET_MAX_REPLY_PORTS) return false;
  out.portCount = portCount;

  // SwOut[4] at offset 190..193 -- the low nibble of each byte is that
  // output port's Universe field; combined with NetSwitch/SubSwitch this
  // is exactly the same 15-bit Net:SubNet:Universe composition ArtDMX
  // sends (see artnet_router.cpp's buildArtDmxPacket).
  for (uint8_t i = 0; i < out.portCount; ++i) {
    uint8_t swOut = data[190 + i] & 0x0F;
    out.wireUniverse[i] = (static_cast<uint16_t>(out.netSwitch) << 8) |
                          (static_cast<uint16_t>(out.subSwitch) << 4) |
                          static_cast<uint16_t>(swOut);
  }

  return true;
}

uint16_t buildArtPollPacket(uint8_t* out) {
  static const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
  std::memcpy(out, kId, 8);
  out[8]  = static_cast<uint8_t>(ARTNET_OP_POLL & 0xFF);
  out[9]  = static_cast<uint8_t>((ARTNET_OP_POLL >> 8) & 0xFF);
  out[10] = 0;   // ProtVerHi (protocol version 14, big-endian -- see ARTNET_PROTOCOL_VERSION)
  out[11] = 14;  // ProtVerLo
  out[12] = 0;   // TalkToMe -- no unsolicited updates; we poll periodically instead
  out[13] = 0;   // Priority (diagnostics filter; unused, we don't request diagnostics)
  return static_cast<uint16_t>(ARTNET_POLL_PACKET_SIZE);
}

ArtNetDiscovery::ArtNetDiscovery(float timeoutSec) : timeoutSec_(timeoutSec) {}

void ArtNetDiscovery::onReply(const ArtNetPollReply& reply, float nowSec) {
  for (size_t i = 0; i < count_; ++i) {
    if (nodes_[i].ip == reply.ip) {
      DiscoveredNode& n = nodes_[i];
      std::memcpy(n.shortName, reply.shortName, sizeof(n.shortName));
      std::memcpy(n.longName, reply.longName, sizeof(n.longName));
      std::memcpy(n.wireUniverse, reply.wireUniverse, sizeof(n.wireUniverse));
      n.portCount = reply.portCount;
      n.lastSeenSec = nowSec;
      return;
    }
  }
  if (count_ >= kMaxNodes) return;  // table full -- drop silently, not a crash

  DiscoveredNode& n = nodes_[count_++];
  n.ip = reply.ip;
  std::memcpy(n.shortName, reply.shortName, sizeof(n.shortName));
  std::memcpy(n.longName, reply.longName, sizeof(n.longName));
  std::memcpy(n.wireUniverse, reply.wireUniverse, sizeof(n.wireUniverse));
  n.portCount = reply.portCount;
  n.lastSeenSec = nowSec;
}

void ArtNetDiscovery::expire(float nowSec) {
  size_t w = 0;
  for (size_t r = 0; r < count_; ++r) {
    if (nowSec - nodes_[r].lastSeenSec <= timeoutSec_) {
      if (w != r) nodes_[w] = nodes_[r];
      ++w;
    }
  }
  count_ = w;
}

const DiscoveredNode* ArtNetDiscovery::node(size_t i) const {
  if (i >= count_) return nullptr;
  return &nodes_[i];
}

const DiscoveredNode* ArtNetDiscovery::findByWireUniverse(uint16_t wireUniverse) const {
  for (size_t i = 0; i < count_; ++i) {
    for (uint8_t p = 0; p < nodes_[i].portCount; ++p) {
      if (nodes_[i].wireUniverse[p] == wireUniverse) return &nodes_[i];
    }
  }
  return nullptr;
}

void resolveDiscoveredDests(const ArtNetDest showDest[], uint8_t universeCount,
                             const ArtNetDiscovery& discovery, ArtNetDest resolved[]) {
  for (uint8_t u = 0; u < universeCount && u < MAX_UNIVERSES; ++u) {
    resolved[u] = showDest[u];
    if (showDest[u].ip != 0) continue;  // explicit .show route always wins

    const DiscoveredNode* n = discovery.findByWireUniverse(showDest[u].wireUniverse);
    resolved[u].ip = n ? n->ip : 0;  // 0 -> fallback/broadcast, never darkness
  }
}
