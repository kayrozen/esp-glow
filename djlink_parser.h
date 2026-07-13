// djlink_parser.h — passive Pro DJ Link packet parsing: the host-testable
// core for the DJ-Link transport (djlink_input.cpp).
//
// ⚠️ Every byte offset in this file is taken directly from Deep Symmetry's
// dysentery protocol analysis (the same reverse-engineering project this
// codebase's own author -- Afterglow's team -- comes from):
//   https://github.com/Deep-Symmetry/dysentery
//   doc/modules/ROOT/pages/beats.adoc  (Beat packet, port 50001)
//   doc/modules/ROOT/pages/vcdj.adoc   (CDJ Status packet, port 50002)
//   doc/modules/ROOT/pages/sync.adoc   (tempo master flag semantics)
// Nothing here is guessed or reconstructed from memory -- do not add
// fields to these parsers without checking that documentation first.
//
// SCOPE (deliberately passive-only, per the design doc's T3)
//   This is NOT a Virtual CDJ. It never sends a packet, never announces a
//   device, never claims a channel number. It only listens.
//   - Beat packets (port 50001, kind 0x28): give BPM, pitch, and
//     beat-within-bar (1-4) on every beat -- exactly what BeatClock's
//     onBeat wants. This is the primary/only source of BeatEvents.
//   - CDJ Status packets (port 50002, kind 0x0a): a large (0xd0-0x200
//     byte), richly-detailed packet (track metadata, waveforms, USB
//     activity...). This parser reads exactly ONE bit out of it -- the
//     tempo-master flag (byte 0x89, bit 5) -- and nothing else. Parsing
//     the rest of that packet is out of scope (track metadata / waveform
//     sync is explicitly out of scope for this feature; see the design
//     doc). Mixer status packets (kind 0x29) also carry a master flag (at
//     a DIFFERENT offset, byte 0x27) but are intentionally not parsed
//     here -- see djlink_master_tracker.h for the fallback this implies.
//
#pragma once

#include <cstddef>
#include <cstdint>

#include "beat_clock.h"  // glow::BeatEvent

namespace glow {

// The fixed 10-byte sequence every Pro DJ Link packet starts with
// ("Qspt1WmJOL" in ASCII): 51 73 70 74 31 57 6d 4a 4f 4c.
constexpr uint8_t kDjLinkMagic[10] = {0x51, 0x73, 0x70, 0x74, 0x31,
                                      0x57, 0x6d, 0x4a, 0x4f, 0x4c};

constexpr uint8_t kDjLinkKindBeat = 0x28;       // port 50001
constexpr uint8_t kDjLinkKindCdjStatus = 0x0a;  // port 50002

struct DjLinkBeatPacket {
  BeatEvent event;       // tUs is filled from the caller-supplied arrival time
  uint8_t   deviceNumber;  // sender's player number (byte 0x21)
};

// Parses a Beat packet (port 50001, kind 0x28, documented as exactly 0x60
// = 96 bytes -- see beats.adoc). `tUs` is the caller's own monotonic
// arrival timestamp; this protocol carries no timestamp of its own (only
// millisecond countdowns to upcoming beats, which are not used here --
// the PACKET'S ARRIVAL is the beat, same as the design doc's "even the
// arrival of the packet is interesting information" observation).
// Returns false (leaving `out` unmodified) if `pkt` is too short or
// doesn't start with the DJ Link magic + beat kind byte.
bool parseDjLinkBeatPacket(const uint8_t* pkt, size_t len, uint64_t tUs, DjLinkBeatPacket& out);

// Reads ONLY the tempo-master flag out of a CDJ Status packet (port
// 50002, kind 0x0a): device number (byte 0x21) and whether bit 5 of the
// status flags byte F (byte 0x89) is set. Returns false if `pkt` is too
// short to safely contain byte 0x89 or doesn't start with the DJ Link
// magic + CDJ-status kind byte.
bool parseDjLinkMasterFlag(const uint8_t* pkt, size_t len, uint8_t& deviceNumberOut, bool& isMasterOut);

}  // namespace glow
