#pragma once

#include <cstdint>
#include <cstddef>

// MDF1 controller-definition format (see FORMAT.md's "MDF1" section). The
// MIDI twin of PFX1/fixture_profile.h: a .mdef describes MIDI *hardware* --
// which pads/faders/encoders exist and at what note/CC, and how each one's
// LED is driven -- so that LED feedback (led_feedback.h) has something to
// address. It deliberately carries no cue/scene bindings: those are the
// show, live-edited in Fennel (glow.bind.*), not baked into a shareable
// controller-library file.
//
// Unlike PFX1 there is no capability enum: pads/faders/encoders are
// addressed directly by MIDI note/CC number, matching parseMidi's existing
// id scheme (live_control.h) -- glow.bind.pad/fader bind straight to that
// number and need no .mdef at all. Only LED feedback needs this file.
//
// This header is device-facing (like fixture_profile.h): fixed-size arrays,
// no heap, no std::string. The text grammar parser and MDF1 encoder
// (provision.h's ControllerDef/parseControllerDef, controller_encoder.h's
// ControllerBuilder) are host-tool-only and never linked into firmware.

static constexpr int MDEF_MAX_PADS = 32;
static constexpr int MDEF_MAX_FADERS = 16;
static constexpr int MDEF_MAX_ENCODERS = 16;
static constexpr int MDEF_MAX_LED_RANGES = 8;
static constexpr int MDEF_MAX_COLORS = 96;
static constexpr int MDEF_MAX_NAME_BLOB = 1024;

// A3: relative encoders send deltas, not absolute positions, and the
// encoding is vendor-specific. Absolute is the safe default (degrades to a
// fader) when unsure -- see decodeEncoderDelta below.
enum class EncoderMode : uint8_t { Absolute = 0, Relative2C = 1, RelativeSignMag = 2 };

enum class LedMsgType : uint8_t { Note = 0, Cc = 1 };
enum class LedSemantic : uint8_t { Velocity = 0, Value = 1 };

// A contiguous block of pads (a single pad has noteFrom == noteTo).
struct MdefPadRange {
  uint8_t noteFrom = 0;
  uint8_t noteTo = 0;  // inclusive
};

struct MdefFaderRange {
  uint8_t ccFrom = 0;
  uint8_t ccTo = 0;      // inclusive
  uint16_t nameOff = 0xFFFF;  // offset into nameBlob, or 0xFFFF if unnamed
};

struct MdefEncoderRange {
  uint8_t ccFrom = 0;
  uint8_t ccTo = 0;      // inclusive
  EncoderMode mode = EncoderMode::Absolute;
};

// One entry in an LED range's colour palette: sending `value` as the data
// byte of that range's message type selects the colour named at nameOff.
// Always named -- COLOR lines in the grammar always give a name.
struct MdefColorEntry {
  uint16_t nameOff = 0;
  uint8_t value = 0;
};

// Declares how a block of pads/faders lights up: the message type and
// address range to send on, what the data byte means (a colour index vs. a
// raw level), and the slice of MidiControllerProfile::colors[] that is this
// range's palette (COLOR lines under the matching LED line).
struct MdefLedRange {
  LedMsgType msgType = LedMsgType::Note;
  uint8_t addrFrom = 0;
  uint8_t addrTo = 0;   // inclusive: note number range or CC number range
  LedSemantic semantic = LedSemantic::Velocity;
  uint16_t colorOffset = 0;  // index into MidiControllerProfile::colors[]
  uint8_t colorCount = 0;
};

struct MidiControllerProfile {
  uint8_t midiChannel = 0;  // 0 = any (parseMidi already ignores channel)

  uint8_t padCount = 0;
  MdefPadRange pads[MDEF_MAX_PADS];

  uint8_t faderCount = 0;
  MdefFaderRange faders[MDEF_MAX_FADERS];

  uint8_t encoderCount = 0;
  MdefEncoderRange encoders[MDEF_MAX_ENCODERS];

  uint8_t ledCount = 0;
  MdefLedRange leds[MDEF_MAX_LED_RANGES];

  uint8_t colorCount = 0;
  MdefColorEntry colors[MDEF_MAX_COLORS];

  // NUL-separated UTF-8 strings (fader names + colour names), indexed by
  // MdefFaderRange::nameOff / MdefColorEntry::nameOff. Copied out of the
  // parsed blob -- parseMidiController never keeps a pointer into the
  // caller's buffer, since MidiControllerProfile is copied by value (e.g.
  // into LoadedShow::controllers, show_bundle.h).
  uint16_t nameBlobLen = 0;
  uint8_t nameBlob[MDEF_MAX_NAME_BLOB];
};

// Parse an MDF1 blob into `out`. Strict, same security-boundary contract as
// parseProfile (fixture_profile.h): returns false (out left unspecified,
// never read out of bounds) on:
//  - buffer shorter than the 13-byte header, or shorter than the declared
//    name/table sizes
//  - bad magic, version != 1, flags != 0
//  - any of padCount/faderCount/encoderCount/ledCount/colorCount exceeding
//    its MDEF_MAX_*
//  - a pad/fader/encoder/led addrFrom > addrTo, or an address/mode/msgType/
//    semantic value out of its valid range
//  - a fader or colour nameOff that isn't 0xFFFF (faders only) and doesn't
//    land on a NUL-terminated string within the trailing name blob
//  - an LED range's colorOffset/colorCount that doesn't fit within the
//    global colour table
bool parseMidiController(const uint8_t* data, size_t len, MidiControllerProfile& out);

// Find the LED range (if any) covering `note`/`cc`. Returns nullptr if the
// controller has no LED declared for that address -- the "no .mdef LED for
// this pad" case glow.led.* must no-op on, not error on.
const MdefLedRange* findLedRangeForNote(const MidiControllerProfile& p, uint8_t note);
const MdefLedRange* findLedRangeForCc(const MidiControllerProfile& p, uint8_t cc);

// Look up a named colour within one LED range's own palette. Returns false
// (valueOut untouched) if `name` isn't in that range's COLOR list -- an
// unknown colour name is a no-op, not an error (see FORMAT.md/A1).
bool ledColorValueByName(const MidiControllerProfile& p, const MdefLedRange& range,
                        const char* name, uint8_t& valueOut);

// Decode a relative-encoder MIDI data byte (0..127, the third byte of a CC
// message) into a signed delta, per `mode`. Getting the wrong mode makes an
// encoder jump wildly or move backwards (A3) -- this is the one place that
// distinction matters. Absolute mode isn't a delta at all (the raw value IS
// the position, like a fader); returns 0 unconditionally for it.
int8_t decodeEncoderDelta(EncoderMode mode, uint8_t data2);
