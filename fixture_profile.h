#pragma once

#include <cstdint>
#include <cstddef>

// PFX1 Format Version 1
// 16-bit capability encoding: coarse channel holds MSB (norm01 * 65535) >> 8
// fine channel holds LSB (norm01 * 65535) & 0xFF. Inverted flag applied after scaling.

enum class Capability : uint8_t {
  Dimmer = 0, Red, Green, Blue, White, Amber, Uv,
  Cyan, Magenta, Yellow,
  Pan, Tilt,
  ShutterStrobe, Gobo, Focus, Zoom,
  Fog, Fan,
  Generic = 255
};

static constexpr int MAX_CAPS = 24;

struct ChannelMap {
  Capability cap;
  uint8_t coarse;
  uint8_t fine;         // 0xFF = 8-bit only
  uint8_t defaultValue;
  uint8_t flags;        // bit0 = inverted
};

struct FixtureProfile {
  uint8_t footprint = 0;
  uint8_t channelCount = 0;
  ChannelMap channels[MAX_CAPS];
};

// Parse a blob into `out`. Returns false (and leaves out unspecified) on:
//  - buffer shorter than the header, or shorter than the declared total size
//  - bad magic, version != 1, flags != 0
//  - capCount > MAX_CAPS
//  - any capability whose coarse offset is >= footprint, or fine offset (if != 0xFF) >= footprint
// Never reads out of bounds. This is the security boundary — be strict.
bool parseProfile(const uint8_t* data, size_t len, FixtureProfile& out);

// Return the first ChannelMap matching `cap`, or nullptr if absent.
const ChannelMap* findCapability(const FixtureProfile& p, Capability cap);

bool hasCapability(const FixtureProfile& p, Capability cap);

// Write a normalized value [0..1] for `cap` into a DMX universe buffer.
// - base is the fixture's start channel (0-based) within `universeBuf`.
// - Clamp norm01 to [0,1] first.
// - 16-bit (fine != 0xFF): v16 = round(norm01 * 65535); coarse = v16>>8; fine = v16&0xFF.
// - 8-bit: v8 = round(norm01 * 255).
// - If inverted flag set: invert AFTER scaling (65535-v16 or 255-v8).
// - If cap absent: no-op.
// - Assume universeBuf has at least base+footprint valid bytes (caller guarantees);
//   still, never write past base+footprint-1.
void applyCapability(const FixtureProfile& p, Capability cap, float norm01,
                     uint8_t* universeBuf, uint16_t base);

// Write every capability's defaultValue into the buffer (8-bit coarse only;
// for 16-bit channels write defaultValue to coarse and 0 to fine). Used to set
// idle state (shutter open, etc.) before effects run.
void applyDefaults(const FixtureProfile& p, uint8_t* universeBuf, uint16_t base);
