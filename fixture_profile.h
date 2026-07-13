#pragma once

#include <cstdint>
#include <cstddef>

// PFX1 Format Versions 1 and 2 (see FORMAT.md)
// 16-bit capability encoding: coarse channel holds MSB (norm01 * 65535) >> 8
// fine channel holds LSB (norm01 * 65535) & 0xFF. Inverted flag applied after scaling.
//
// v2 adds function ranges: named discrete slots ("gobo 2", "red") and
// continuous sub-ranges ("strobe 1->10 Hz") carved out of a capability's raw
// DMX byte range. v1 blobs (no ranges, every capability linear) still parse
// -- see parseProfile.

enum class Capability : uint8_t {
  Dimmer = 0, Red, Green, Blue, White, Amber, Uv,
  Cyan, Magenta, Yellow,
  Pan, Tilt,
  ShutterStrobe, Gobo, Focus, Zoom,
  Fog, Fan,
  // v2 additions (appended, never renumbered -- existing PFX1 blobs must
  // still parse with the same enum values for 0..17 above).
  ColorWheel, GoboRotation, Prism, PrismRotation, Frost, Iris, CTO,
  AnimationWheel, Macro,
  Generic = 255
};

static constexpr int MAX_CAPS = 24;

// v2 function-range table limits. Compact and fixed-size (no heap) since
// FixtureProfile is a value type copied around freely (see show_bundle.cpp's
// PatchEntry). ~25 ranges (a 12-slot colour wheel + 10-slot gobo wheel) is
// the worked example in the design doc; MAX_RANGES gives headroom above that.
static constexpr int MAX_RANGES = 64;
static constexpr int MAX_RANGE_NAME_BLOB = 512;

struct ChannelMap {
  Capability cap;
  uint8_t coarse;
  uint8_t fine;         // 0xFF = 8-bit only
  uint8_t defaultValue;
  uint8_t flags;        // bit0 = inverted
};

// A named slice of a capability's raw DMX byte range [dmxFrom, dmxTo]
// (inclusive). Discrete: selecting it snaps to the centre (the safe value).
// Continuous: selecting it maps a [0,1] value linearly across the slice.
// capIndex indexes FixtureProfile::channels (which ChannelMap this range
// narrows). semantic is reserved for v3 (e.g. "nearest red" colour-wheel
// mapping) -- always 0 today, unused by every apply/introspection function.
struct FunctionRange {
  uint8_t capIndex;
  uint8_t dmxFrom;
  uint8_t dmxTo;         // inclusive
  bool continuous;
  uint16_t nameOff;      // offset into FixtureProfile::rangeNameBlob, or 0xFFFF if unnamed
  uint8_t semantic;      // reserved, currently always 0
};

struct FixtureProfile {
  uint8_t footprint = 0;
  uint8_t channelCount = 0;
  ChannelMap channels[MAX_CAPS];

  uint16_t rangeCount = 0;
  FunctionRange ranges[MAX_RANGES];

  // NUL-separated UTF-8 range names, indexed by FunctionRange::nameOff.
  // Copied out of the parsed blob (parseProfile never keeps a pointer into
  // the caller's buffer -- FixtureProfile is copied by value, e.g. into
  // LoadedShow::fixtures).
  uint16_t rangeNameBlobLen = 0;
  uint8_t rangeNameBlob[MAX_RANGE_NAME_BLOB];
};

// Parse a blob into `out`. Returns false (and leaves out unspecified) on:
//  - buffer shorter than the header, or shorter than the declared total size
//  - bad magic, version not in {1, 2}, flags != 0
//  - capCount > MAX_CAPS
//  - any capability whose coarse offset is >= footprint, or fine offset (if != 0xFF) >= footprint
//  - (v2 only) rangeCount > MAX_RANGES, name blob too large for MAX_RANGE_NAME_BLOB
//  - (v2 only) any range whose capIndex >= capCount, or dmxFrom > dmxTo, or
//    whose nameOff is neither 0xFFFF nor a valid NUL-terminated offset within the name blob
// Never reads out of bounds. This is the security boundary -- be strict.
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
// Unchanged by v2: a capability with no function ranges behaves exactly as
// it always has.
void applyCapability(const FixtureProfile& p, Capability cap, float norm01,
                     uint8_t* universeBuf, uint16_t base);

// Write every capability's defaultValue into the buffer (8-bit coarse only;
// for 16-bit channels write defaultValue to coarse and 0 to fine). Used to set
// idle state (shutter open, etc.) before effects run.
void applyDefaults(const FixtureProfile& p, uint8_t* universeBuf, uint16_t base);

// --- v2: function ranges ----------------------------------------------------
//
// Ranges are a coarse-channel concept: on a 16-bit capability, only the
// coarse byte is written; the fine byte is left untouched.
//
// Both applyRangeBy* functions return false (buffer untouched) when `cap` is
// absent from the fixture, or the name/index doesn't match a range on that
// capability's channel -- the same "emit a capability the fixture lacks ->
// no-op" rule as applyCapability, so a cue written for one fixture degrades
// gracefully on another.

// Discrete slot -> writes the centre of [dmxFrom, dmxTo]: (dmxFrom+dmxTo)/2,
// integer division (floors -- e.g. 32..63 writes 47, not 48). value01 is
// ignored.
// Continuous sub-range -> writes dmxFrom + round(clamp01(value01) * (dmxTo-dmxFrom)).
bool applyRangeByName(const FixtureProfile& p, Capability cap, const char* name,
                      float value01, uint8_t* universeBuf, uint16_t base);
bool applyRangeByIndex(const FixtureProfile& p, Capability cap, uint8_t rangeIdx,
                       float value01, uint8_t* universeBuf, uint16_t base);

// Introspection (Lua glow.ranges / UI). rangeIdx is 0-based within just this
// capability's own ranges (not the profile-wide range table).
size_t rangeCount(const FixtureProfile& p, Capability cap);
const char* rangeName(const FixtureProfile& p, Capability cap, uint8_t rangeIdx);  // nullptr if unnamed or absent
bool rangeIsContinuous(const FixtureProfile& p, Capability cap, uint8_t rangeIdx);  // false if absent
