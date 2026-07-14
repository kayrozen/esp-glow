// mdef_parser.h — parse .mdef controller definition files.
//
// The .mdef format describes MIDI hardware: pads, faders, encoders, and
// their LED feedback capabilities. This is the twin of .fdef (fixture
// definitions) — it models the controller, not the show. Bindings are
// show-specific and live in Fennel; this file models what exists.
//
// Grammar (line-oriented, like .fdef):
//   CONTROLLER Akai APC40 mkII
//   MIDI_CHANNEL 1                  # 0 = any (default)
//
//   # Inputs
//   PAD  53 92                      # contiguous block of pads: note 53..92
//   PAD  0                          # single pad at note 0
//   FADER CC 48 55                  # faders on CC 48..55
//   FADER CC 7   master             # named single fader
//   ENCODER CC 16 23 relative-2c    # relative encoders (two's complement)
//
//   # LED feedback
//   LED NOTE 53 92 velocity         # pads 53..92: LED colour = note-on velocity
//     COLOR off    0
//     COLOR green  1
//     COLOR red    3
//     COLOR amber  5
//     COLOR blink-green 2
//   LED CC 48 55 value              # fader LED rings driven by CC value 0..127
//
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace glow {
namespace mdef {

// Encoder mode: how relative encoders report deltas.
enum class EncoderMode : uint8_t {
  Absolute = 0,      // sends absolute values (degrades to fader)
  Relative2C = 1,    // two's complement delta encoding
  RelativeSignMag = 2, // sign-magnitude delta encoding
  Relative6365 = 3,  // 63=down, 65=up (common vendor scheme)
};

// Control type declared in the .mdef.
enum class ControlType : uint8_t {
  Pad = 0,
  Fader = 1,
  Encoder = 2,
};

// A single control element (pad, fader, or encoder).
struct ControlElement {
  ControlType type;
  uint16_t startId;  // note number or CC number
  uint16_t endId;    // inclusive; == startId for single elements
  std::string name;  // optional name (e.g. "master" for a fader)
  EncoderMode encoderMode = EncoderMode::Absolute;  // only for encoders
};

// How an LED is driven: by note-on velocity (colour index) or by CC value (level).
enum class LedSemantic : uint8_t {
  Velocity = 0,  // data byte = colour index into palette
  Value = 1,     // data byte = level 0..127
};

// LED definition for a range of controls.
struct LedDefinition {
  bool isNote;         // true = NOTE messages, false = CC messages
  uint16_t startId;    // starting note/CC
  uint16_t endId;      // ending note/CC (inclusive)
  LedSemantic semantic;
  std::unordered_map<std::string, uint8_t> colorPalette;  // name -> value
};

// Parsed .mdef controller definition.
struct ControllerDef {
  std::string name;
  uint8_t midiChannel = 0;  // 0 = any channel
  std::vector<ControlElement> controls;
  std::vector<LedDefinition> leds;
};

// Parse a .mdef source string. Returns false and fills errOut on error.
// Strict: unknown tokens, out-of-range values, or malformed lines fail.
bool parseMdef(const char* src, size_t len, ControllerDef& out, 
               char* errOut, size_t errCap);

// Look up a control element by its ID (note or CC number).
// Returns nullptr if not found.
const ControlElement* findControlById(const ControllerDef& def, uint16_t id);

// Look up an LED definition that covers the given ID.
// Returns nullptr if no LED covers this ID.
const LedDefinition* findLedById(const ControllerDef& def, uint16_t id);

// Resolve a color name to its value from the LED's palette.
// Returns false if the color name is not in the palette.
bool resolveColor(const LedDefinition& led, const std::string& name, uint8_t& valueOut);

}  // namespace mdef
}  // namespace glow
