#include "djlink_parser.h"

#include <cstring>

namespace glow {

namespace {

bool hasDjLinkMagicAndKind(const uint8_t* pkt, size_t len, uint8_t kind) {
  // Need at least through byte 0x0a (the kind byte) to check anything.
  if (pkt == nullptr || len < 11) return false;
  if (std::memcmp(pkt, kDjLinkMagic, sizeof(kDjLinkMagic)) != 0) return false;
  return pkt[0x0a] == kind;
}

}  // namespace

bool parseDjLinkBeatPacket(const uint8_t* pkt, size_t len, uint64_t tUs, DjLinkBeatPacket& out) {
  if (!hasDjLinkMagicAndKind(pkt, len, kDjLinkKindBeat)) return false;
  // Documented as exactly 0x60 (96) bytes; require at least that much to
  // safely read every field this parser uses (up to byte 0x5c).
  if (len < 0x60) return false;

  out.deviceNumber = pkt[0x21];

  // BPM: 2-byte big-endian, one hundred times the track's BPM (beats.adoc).
  uint16_t bpmRaw = (static_cast<uint16_t>(pkt[0x5a]) << 8) | pkt[0x5b];

  // Pitch: the low 3 bytes of the 4-byte field at 0x54-0x57 (byte 0x54 is
  // the unused high byte); 0x100000 (1048576) == no adjustment (beats.adoc).
  uint32_t pitchRaw = (static_cast<uint32_t>(pkt[0x55]) << 16) |
                      (static_cast<uint32_t>(pkt[0x56]) << 8) | pkt[0x57];

  // Effective (pitch-adjusted) BPM, matching beats.adoc's combined formula
  // exactly: (bpmRaw / 100) * (pitchRaw / 0x100000).
  float effectiveBpm = (static_cast<float>(bpmRaw) / 100.0f) *
                       (static_cast<float>(pitchRaw) / 1048576.0f);

  uint8_t beatInBar = pkt[0x5c];
  if (beatInBar < 1 || beatInBar > 4) beatInBar = 0;  // malformed/unknown -- not a hard parse failure

  out.event.tUs = tUs;
  out.event.bpm = effectiveBpm;
  out.event.beatInBar = beatInBar;
  out.event.isDownbeat = (beatInBar == 1);
  return true;
}

bool parseDjLinkMasterFlag(const uint8_t* pkt, size_t len, uint8_t& deviceNumberOut, bool& isMasterOut) {
  if (!hasDjLinkMagicAndKind(pkt, len, kDjLinkKindCdjStatus)) return false;
  // Need through byte 0x89 (the status flags byte F); the shortest
  // documented CDJ status packet is 0xd0 (208) bytes, comfortably more,
  // but this parser only requires what it actually reads.
  if (len < 0x8a) return false;

  deviceNumberOut = pkt[0x21];
  isMasterOut = (pkt[0x89] & 0x20) != 0;  // bit 5 == Master (vcdj.adoc's cdj-status-flag-bits)
  return true;
}

}  // namespace glow
