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
// MIDI_CHANNEL <0-16>                     # optional, default 0 (any; the whole-controller LED-output channel default -- see led_feedback.cpp)
// PAD  <note> [<note2>] [CH <lo> <hi>]     # a contiguous block of pads (a single pad if note2 omitted)
// FADER CC <from> [<to>] [<name...>] [CH <lo> <hi>]  # faders on CC <from>..<to>, optionally named
// ENCODER CC <from> [<to>] [absolute|relative-2c|relative-signmag]  # default: absolute
// LED NOTE|CC <from> <to> velocity|value [CH <lo> <hi>]  # how a block of pads/faders lights up
//   COLOR <name> <value>                  # indented under the preceding LED line
//
// PAD/FADER/ENCODER declare what exists; LED+COLOR (attaches to the most
// recent LED line, like SLOT/RANGE attach to the preceding CAP in .fdef)
// declare how it lights up. A controller with no LED lines still works --
// glow.led.* is a no-op without them.
//
// CH <lo> <hi> (0..15, 0-indexed -- NOT MIDI_CHANNEL's 1-indexed "any"
// scheme) marks a PAD/FADER/LED range as channel-significant: the same
// note/CC number is multiplexed across channels <lo>..<hi> to address
// several distinct physical controls (see FORMAT.md's "Per-range channel
// significance" and samples/apc40.mdef). Omitting CH (every existing .mdef)
// keeps that range channel-agnostic, unchanged from before CH existed.

// Parse a .mdef text and populate `out`. Returns false on any parse error.
// On failure, sets `err` to a non-empty error message.
bool parseControllerDef(const std::string& text, ControllerBuilder& out, std::string& err);

// Build an MDF1 blob from a ControllerBuilder. Returns an empty vector and
// sets `err` on failure (a count over one of mdef.h's MDEF_MAX_* limits, or
// too much fader/colour name text -- see controller_encoder.h).
std::vector<uint8_t> encodeController(const ControllerBuilder& def, std::string& err);

// .show grammar (the patch), format version 2 -- fully 1-indexed and
// human-facing (write the number printed on the fixture/console, not a
// memory offset). Internally everything still stores 0-indexed values
// (PatchEntry.base, MatrixMap.startChannel, the SHW1 bundle) -- the -1
// conversion happens exactly once, here in the compiler.
//
// SHOW 2                              # REQUIRED first non-comment line. No SHOW 2 header ->
//                                     # hard parse error (see compileShow) -- there is no v1
//                                     # fallback, because silently reinterpreting an old
//                                     # 0-indexed .show as 1-indexed would shift every address
//                                     # by one channel without any error.
// UNIVERSE <idx> <DMX|ARTNET|SACN> [<ip>] [<wireUniverse>]
//                                     # sets transport for universe idx (1..8, 1-indexed).
//                                     # Stored internally as idx-1. NOTE: this is the .show
//                                     # text convention only -- Art-Net universes are 0-indexed
//                                     # on the wire, so UNIVERSE 1 ARTNET is internal index 0,
//                                     # which the Art-Net sink sends as wire universe 0 by
//                                     # default (see FORMAT.md / README_PROVISION.md for the
//                                     # worked mapping).
//                                     # <ip> and <wireUniverse> (ARTNET only, both optional) are
//                                     # Wave 3's explicit per-node routing: <ip> (dotted-quad)
//                                     # names the destination Art-Net node -- omitted, it falls
//                                     # back to CFG1's artnetFallbackIp, or broadcast if that's
//                                     # 0 too. <wireUniverse> (0..32767) is the 15-bit Art-Net
//                                     # Port-Address to send as -- omitted (but <ip> given), it
//                                     # defaults to the internal index and the compiler warns.
//                                     # Two UNIVERSE ARTNET lines resolving to the same (ip,
//                                     # wireUniverse) is a compile error (same ip, different
//                                     # wireUniverse is fine -- that's one node's second output).
// FIXTURE  <deffile> <universe> <address> # patch an instance; deffile resolved via callback.
//                                     # <address> is the 1-indexed DMX address printed on the
//                                     # fixture's own display (1..512), stored as base=address-1.
// POS      <x> <y> <z>               # head only: modifies the most recent FIXTURE
// ROT      <yaw> <pitch> <roll>      # head only: degrees, most recent FIXTURE
// CENTER   <panNorm> <tiltNorm>      # head only, optional (default 0.5 0.5)
// INVERT   <0|1> <0|1>               # head only, optional (default 0 0)
// MATRIX   <startUniverse> <startAddress> <w> <h> <SERP|PROG> <H|V> <ORDER>
//                                     # startUniverse and startAddress are 1-indexed, same
//                                     # convention as UNIVERSE/FIXTURE above.
// CONTROLLER <deffile>                # embed a .mdef controller definition; deffile resolved via callback
//
// After parsing, the compiler validates every DMX address (1..512) and every
// fixture's footprint against the end of its universe (a fixture never
// spans universes; a MATRIX's w*h*3 channels may legitimately roll over
// into the next universe(s), matching PixelMatrix's own on-device
// behavior). It then checks for address collisions: any two
// fixtures/matrices whose occupied-channel ranges overlap in the same
// universe fail the compile, naming both and the exact overlapping range
// (see compileShow). This catches the single most common real-world
// patching mistake at compile time instead of on stage.

struct CompileResult {
  bool ok = false;
  std::vector<uint8_t> bundle;
  std::string err;
  // Non-fatal notices (e.g. unused trailing channels in a patched
  // universe). Always empty when !ok.
  std::vector<std::string> warnings;
};

// readFile(name) returns the text of a referenced .fdef, or "" if not found.
// Compile a .show and produce a SHW1 bundle.
// De-duplicates identical profiles (two FIXTURE lines using the same .fdef share one profile-table entry).
CompileResult compileShow(const std::string& showText,
                          const std::function<std::string(const std::string&)>& readFile);
