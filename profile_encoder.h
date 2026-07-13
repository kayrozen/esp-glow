#pragma once

#include "fixture_profile.h"
#include <vector>
#include <string>

// A function range attached to one of ProfileBuilder::caps by index (mirrors
// FunctionRange's on-disk capIndex field -- see FORMAT.md's PFX2 section).
struct RangeSpec {
  uint8_t capIndex;
  uint8_t dmxFrom;
  uint8_t dmxTo;
  bool continuous;
  std::string name;  // empty = unnamed
};

struct ProfileBuilder {
  uint8_t footprint = 0;
  std::string name;
  std::vector<ChannelMap> caps;
  std::vector<RangeSpec> ranges;

  ProfileBuilder& setFootprint(uint8_t f);
  ProfileBuilder& add(Capability cap, uint8_t coarse, uint8_t fine = 0xFF,
                      uint8_t def = 0, bool inverted = false);
  // capIndex is the index into `caps` (in the order added) this range narrows.
  ProfileBuilder& addRange(uint8_t capIndex, uint8_t dmxFrom, uint8_t dmxTo,
                           bool continuous, const std::string& rangeName = "");

  // Emits PFX1 v1 bytes when `ranges` is empty (byte-identical to the
  // pre-v2 encoder, so every existing non-range .fdef compiles to the exact
  // same blob it always has). Emits PFX2 (version=2, with a range table and
  // trailing name blob) when any range has been added.
  std::vector<uint8_t> encode() const;
};
