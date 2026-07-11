#pragma once

#include "fixture_profile.h"
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// .fdef grammar: one fixture type per file
// FIXTURE  <name...>              # rest of line is the name
// FOOTPRINT <n>                   # 1..255, required
// HEAD                            # optional flag: this is a moving head
// PANRANGE  <deg>                 # head only, e.g. 540
// TILTRANGE <deg>                 # head only, e.g. 270
// CAP <Name> <coarse> [<fine>|-] [<default>] [inv]

struct FixtureDef {
  std::string name;
  uint8_t footprint = 0;
  bool isHead = false;
  float panRangeDeg = 540.0f;
  float tiltRangeDeg = 270.0f;
  std::vector<ChannelMap> caps;
};

// Parse a .fdef text and populate `out`. Returns false on any parse error.
// On failure, sets `err` to a non-empty error message.
bool parseFixtureDef(const std::string& text, FixtureDef& out, std::string& err);

// Map a capability name (exact string) to its enum value.
// Returns true and sets `out` on success; false otherwise.
bool capFromName(const std::string& name, Capability& out);

// Build a PFX1 blob from a FixtureDef using ProfileBuilder.
std::vector<uint8_t> encodeProfile(const FixtureDef& def);

// .show grammar (the patch):
// UNIVERSE <idx> <DMX|ARTNET|SACN>   # sets transport for that universe
// FIXTURE  <deffile> <universe> <base> # patch an instance; deffile resolved via callback
// POS      <x> <y> <z>               # head only: modifies the most recent FIXTURE
// ROT      <yaw> <pitch> <roll>      # head only: degrees, most recent FIXTURE
// CENTER   <panNorm> <tiltNorm>      # head only, optional (default 0.5 0.5)
// INVERT   <0|1> <0|1>               # head only, optional (default 0 0)
// MATRIX   <startUniverse> <startChannel> <w> <h> <SERP|PROG> <H|V> <ORDER>

struct CompileResult {
  bool ok = false;
  std::vector<uint8_t> bundle;
  std::string err;
};

// readFile(name) returns the text of a referenced .fdef, or "" if not found.
// Compile a .show and produce a SHW1 bundle.
// De-duplicates identical profiles (two FIXTURE lines using the same .fdef share one profile-table entry).
CompileResult compileShow(const std::string& showText,
                          const std::function<std::string(const std::string&)>& readFile);
