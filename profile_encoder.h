#pragma once

#include "fixture_profile.h"
#include <vector>
#include <string>

struct ProfileBuilder {
  uint8_t footprint = 0;
  std::string name;
  std::vector<ChannelMap> caps;

  ProfileBuilder& setFootprint(uint8_t f);
  ProfileBuilder& add(Capability cap, uint8_t coarse, uint8_t fine = 0xFF,
                      uint8_t def = 0, bool inverted = false);
  std::vector<uint8_t> encode() const;
};
