#include "artnet_router.h"
#include <cstring>

uint16_t buildArtDmxPacket(uint8_t* out, uint16_t wireUniverse, uint8_t sequence,
                           const uint8_t* data, uint16_t len) {
  if (len > DMX_UNIVERSE_SIZE) len = DMX_UNIVERSE_SIZE;

  static const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
  std::memcpy(out, kId, 8);
  out[8]  = static_cast<uint8_t>(ARTNET_OP_DMX & 0xFF);
  out[9]  = static_cast<uint8_t>((ARTNET_OP_DMX >> 8) & 0xFF);
  out[10] = static_cast<uint8_t>((ARTNET_PROTOCOL_VERSION >> 8) & 0xFF);
  out[11] = static_cast<uint8_t>(ARTNET_PROTOCOL_VERSION & 0xFF);
  out[12] = sequence;
  out[13] = 0;  // Physical input (unused; we're a controller, not a node)

  // Wire universe: Art-Net's 15-bit Port-Address, transmitted as SubUni
  // (low byte) + Net (high 7 bits) -- see FORMAT.md's Net/SubNet writeup.
  out[14] = wireUniverse & 0xFF;
  out[15] = (wireUniverse >> 8) & 0x7F;

  uint16_t payload = len;
  if (payload & 1) payload += 1;  // Art-Net length must be even
  out[16] = (payload >> 8) & 0xFF;
  out[17] = payload & 0xFF;

  std::memcpy(out + 18, data, len);
  if (payload > len) out[18 + len] = 0;  // pad byte

  return static_cast<uint16_t>(18 + payload);
}

uint16_t buildArtSyncPacket(uint8_t* out) {
  static const char kId[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
  std::memcpy(out, kId, 8);
  out[8]  = static_cast<uint8_t>(ARTNET_OP_SYNC & 0xFF);
  out[9]  = static_cast<uint8_t>((ARTNET_OP_SYNC >> 8) & 0xFF);
  out[10] = static_cast<uint8_t>((ARTNET_PROTOCOL_VERSION >> 8) & 0xFF);
  out[11] = static_cast<uint8_t>(ARTNET_PROTOCOL_VERSION & 0xFF);
  out[12] = 0;  // Aux1, unused
  out[13] = 0;  // Aux2, unused
  return static_cast<uint16_t>(ARTNET_SYNC_PACKET_SIZE);
}

ArtNetRouter::ArtNetRouter(uint32_t fallbackIp, uint16_t port, bool syncBroadcast)
    : fallbackIp_(fallbackIp), port_(port), syncBroadcast_(syncBroadcast) {
  for (uint8_t i = 0; i < MAX_UNIVERSES; ++i) {
    dest_[i].ip = 0;
    dest_[i].wireUniverse = i;
    seq_[i] = 1;
  }
}

void ArtNetRouter::setDest(uint8_t universeIndex, const ArtNetDest& d) {
  if (universeIndex >= MAX_UNIVERSES) return;
  portENTER_CRITICAL(&destMux_);
  dest_[universeIndex] = d;
  portEXIT_CRITICAL(&destMux_);
}

ArtNetDest ArtNetRouter::destFor(uint8_t universeIndex) const {
  if (universeIndex >= MAX_UNIVERSES) return ArtNetDest{};
  portENTER_CRITICAL(&destMux_);
  ArtNetDest d = dest_[universeIndex];
  portEXIT_CRITICAL(&destMux_);
  return d;
}

uint32_t ArtNetRouter::resolveIp(const ArtNetDest& d) const {
  if (d.ip != 0) return d.ip;
  return fallbackIp_;  // 0 here means "no destination" -- see ctor's doc.
}

void ArtNetRouter::send(uint8_t universeIndex, const uint8_t* data, uint16_t len,
                         IArtNetTransport& transport) {
  if (universeIndex >= MAX_UNIVERSES) return;

  portENTER_CRITICAL(&destMux_);
  ArtNetDest dest = dest_[universeIndex];
  portEXIT_CRITICAL(&destMux_);

  uint32_t ip = resolveIp(dest);
  if (ip == 0) {
    // No route and no fallback: drop rather than broadcast. Do not touch
    // the sequence counter -- it must not advance for a packet that was
    // never sent (see send()'s doc). Also not "active" for frameEnd()'s
    // ArtSync targeting -- nothing was sent for this universe.
    ++unroutedDrops_;
    activeThisFrame_[universeIndex] = false;
    return;
  }

  uint8_t pkt[ARTNET_DMX_PACKET_MAX];
  uint16_t pktLen = buildArtDmxPacket(pkt, dest.wireUniverse,
                                      seq_[universeIndex], data, len);
  if (++seq_[universeIndex] == 0) seq_[universeIndex] = 1;

  activeThisFrame_[universeIndex] = true;
  transport.sendTo(ip, port_, pkt, pktLen);
}

void ArtNetRouter::frameEnd(IArtNetTransport& transport) {
  uint8_t pkt[ARTNET_SYNC_PACKET_SIZE];
  uint16_t pktLen = buildArtSyncPacket(pkt);

  if (syncBroadcast_) {
    transport.sendTo(0xFFFFFFFFu, port_, pkt, pktLen);
    return;
  }

  // Snapshot the routing table once under the lock, then dedup outside it
  // -- keeps the critical section as short as send()'s (no I/O or O(n^2)
  // work while it's held).
  portENTER_CRITICAL(&destMux_);
  ArtNetDest snapshot[MAX_UNIVERSES];
  std::memcpy(snapshot, dest_, sizeof(dest_));
  portEXIT_CRITICAL(&destMux_);

  // No dynamic allocation: MAX_UNIVERSES is small (8) and fixed, so a
  // linear-scan dedup into a stack array costs nothing this doesn't
  // already pay for per-frame elsewhere. Only universes send() actually
  // sent data for THIS frame are considered -- see activeThisFrame_'s doc
  // for why a fixed-size-array-wide scan would have been wrong.
  uint32_t distinctIps[MAX_UNIVERSES];
  uint8_t distinctCount = 0;
  for (uint8_t i = 0; i < MAX_UNIVERSES; ++i) {
    if (!activeThisFrame_[i]) continue;
    uint32_t ip = resolveIp(snapshot[i]);
    if (ip == 0) continue;  // defensive; send() already excludes this case
    bool seen = false;
    for (uint8_t j = 0; j < distinctCount; ++j) {
      if (distinctIps[j] == ip) { seen = true; break; }
    }
    if (!seen) distinctIps[distinctCount++] = ip;
  }
  std::memset(activeThisFrame_, 0, sizeof(activeThisFrame_));

  for (uint8_t i = 0; i < distinctCount; ++i) {
    transport.sendTo(distinctIps[i], port_, pkt, pktLen);
  }
}
