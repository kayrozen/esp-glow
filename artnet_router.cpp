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

ArtNetRouter::ArtNetRouter(uint32_t fallbackIp, uint16_t port)
    : fallbackIp_(fallbackIp), port_(port) {
  for (uint8_t i = 0; i < MAX_UNIVERSES; ++i) {
    dest_[i].ip = 0;
    dest_[i].wireUniverse = i;
  }
}

void ArtNetRouter::setDest(uint8_t universeIndex, const ArtNetDest& d) {
  if (universeIndex >= MAX_UNIVERSES) return;
  dest_[universeIndex] = d;
}

const ArtNetDest& ArtNetRouter::destFor(uint8_t universeIndex) const {
  static const ArtNetDest kDefault{};
  if (universeIndex >= MAX_UNIVERSES) return kDefault;
  return dest_[universeIndex];
}

uint32_t ArtNetRouter::resolveIp(const ArtNetDest& d) const {
  if (d.ip != 0) return d.ip;
  if (fallbackIp_ != 0) return fallbackIp_;
  return 0xFFFFFFFFu;
}

void ArtNetRouter::send(uint8_t universeIndex, const uint8_t* data, uint16_t len,
                         IArtNetTransport& transport) {
  if (universeIndex >= MAX_UNIVERSES) return;

  uint8_t pkt[ARTNET_DMX_PACKET_MAX];
  uint16_t pktLen = buildArtDmxPacket(pkt, dest_[universeIndex].wireUniverse,
                                      seq_[universeIndex], data, len);
  seq_[universeIndex]++;  // wraps at 255; per-universe, never shared

  transport.sendTo(resolveIp(dest_[universeIndex]), port_, pkt, pktLen);
}

void ArtNetRouter::frameEnd(IArtNetTransport& transport) {
  uint8_t pkt[ARTNET_SYNC_PACKET_SIZE];
  uint16_t pktLen = buildArtSyncPacket(pkt);
  transport.sendTo(0xFFFFFFFFu, port_, pkt, pktLen);
}
