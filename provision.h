#pragma once

#include "fixture_profile.h"
#include "controller_encoder.h"
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
//   SLOT <from> <to> <name...>    # discrete slot, attaches to preceding CAP
//   RANGE <from> <to> <name...>   # continuous sub-range, attaches to preceding CAP
//
// SLOT/RANGE lines are indented under a CAP and select a named function
// range within that capability's raw DMX byte range [0,255] (e.g. a colour
// wheel's "red" slot at DMX 10-19). A CAP with no SLOT/RANGE lines stays
// linear -- full backward compatibility with every existing .fdef.

struct FdefRange {
  uint8_t capIndex;   // index into FixtureDef.caps this range narrows
  uint8_t dmxFrom;
  uint8_t dmxTo;      // inclusive
  bool continuous;    // RANGE = true, SLOT = false
  std::string name;   // rest of the line; may be empty
};

struct FixtureDef {
  std::string name;
  uint8_t footprint = 0;
  bool isHead = false;
  float panRangeDeg = 540.0f;
  float tiltRangeDeg = 270.0f;
  std::vector<ChannelMap> caps;
  std::vector<FdefRange> ranges;
};

// Parse a .fdef text and populate `out`. Returns false on any parse error.
// On failure, sets `err` to a non-empty error message.
bool parseFixtureDef(const std::string& text, FixtureDef& out, std::string& err);

// Map a capability name (exact string) to its enum value.
// Returns true and sets `out` on success; false otherwise.
bool capFromName(const std::string& name, Capability& out);

// Build a PFX1 blob from a FixtureDef using ProfileBuilder.
std::vector<uint8_t> encodeProfile(const FixtureDef& def);

// .mdef grammar: one controller (MIDI hardware) definition per file (see
// FORMAT.md's "MDF1" section). The MIDI twin of .fdef: describes the
// hardware (pads/faders/encoders/LEDs), never a cue/scene binding -- those
// belong in Fennel (glow.bind.*/glow.led.*, glow_lua_api.h), live-editable
// per show, not baked into a shareable controller-library file.
// CONTROLLER <name...>                    # rest of line is the name
// MIDI_CHANNEL <0-16>                     # optional, default 0 (any; parseMidi already ignores channel)
// PAD  <note> [<note2>]                   # a contiguous block of pads (a single pad if note2 omitted)
// FADER CC <from> [<to>] [<name...>]      # faders on CC <from>..<to>, optionally named
// ENCODER CC <from> [<to>] [absolute|relative-2c|relative-signmag]  # default: absolute
// LED NOTE|CC <from> <to> velocity|value  # how a block of pads/faders lights up
//   COLOR <name> <value>                  # indented under the preceding LED line
//
// PAD/FADER/ENCODER declare what exists; LED+COLOR (attaches to the most
// recent LED line, like SLOT/RANGE attach to the preceding CAP in .fdef)
// declare how it lights up. A controller with no LED lines still works --
// glow.led.* is a no-op without them.

// Parse a .mdef text and populate `out`. Returns false on any parse error.
// On failure, sets `err` to a non-empty error message.
bool parseControllerDef(const std::string& text, ControllerBuilder& out, std::string& err);

// Build an MDF1 blob from a ControllerBuilder. Returns an empty vector and
// sets `err` on failure (a count over one of mdef.h's MDEF_MAX_* limits, or
// too much fader/colour name text -- see controller_encoder.h).
std::vector<uint8_t> encodeController(const ControllerBuilder& def, std::string& err);

// .show grammar (the patch):
// UNIVERSE <idx> <DMX|ARTNET|SACN>   # sets transport for that universe
// FIXTURE  <deffile> <universe> <base> # patch an instance; deffile resolved via callback
// POS      <x> <y> <z>               # head only: modifies the most recent FIXTURE
// ROT      <yaw> <pitch> <roll>      # head only: degrees, most recent FIXTURE
// CENTER   <panNorm> <tiltNorm>      # head only, optional (default 0.5 0.5)
// INVERT   <0|1> <0|1>               # head only, optional (default 0 0)
// MATRIX   <startUniverse> <startChannel> <w> <h> <SERP|PROG> <H|V> <ORDER>
// CONTROLLER <deffile>                # embed a .mdef controller definition; deffile resolved via callback

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
