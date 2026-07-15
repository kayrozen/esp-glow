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

// P1.1: opaque SysEx init messages (`INIT SYSEX <hex bytes>`, MDF1 v3 --
// see FORMAT.md). A handful of short messages is the realistic shape (a
// mode-set/LED-enable handshake, not a firmware dump), so these stay small
// and fixed-size like everything else in this file -- no heap.
static constexpr int MDEF_MAX_INIT_BLOBS = 8;
static constexpr int MDEF_MAX_INIT_BLOB_BYTES = 64;

// A3: relative encoders send deltas, not absolute positions, and the
// encoding is vendor-specific. Absolute is the safe default (degrades to a
// fader) when unsure -- see decodeEncoderDelta below.
enum class EncoderMode : uint8_t { Absolute = 0, Relative2C = 1, RelativeSignMag = 2 };

enum class LedMsgType : uint8_t { Note = 0, Cc = 1 };
enum class LedSemantic : uint8_t { Velocity = 0, Value = 1 };

// Channel significance is a per-range flag, not a per-controller one (see
// FORMAT.md's "Per-range channel significance"): most controllers are
// entirely channel-agnostic (channelFrom == channelTo == kChannelAgnostic on
// every range, the v1-compatible default), but on a controller like the
// APC40, ONE note or CC number is multiplexed across several MIDI channels
// to address several distinct physical controls (e.g. all 40 clip-launch
// pads share 5 note numbers, one per scene row, with the channel nibble
// selecting the track/column). channelFrom/channelTo (0..15, inclusive) name
// that channel range; kChannelAgnostic (0xFF) in BOTH fields means "channel
// carries no addressing meaning here" -- the id alone identifies the
// control, exactly like every controller before this field existed.
static constexpr uint8_t kChannelAgnostic = 0xFF;

// A contiguous block of pads (a single pad has noteFrom == noteTo).
struct MdefPadRange {
  uint8_t noteFrom = 0;
  uint8_t noteTo = 0;  // inclusive
  uint8_t channelFrom = kChannelAgnostic;
  uint8_t channelTo = kChannelAgnostic;  // inclusive, 0..15
};

struct MdefFaderRange {
  uint8_t ccFrom = 0;
  uint8_t ccTo = 0;      // inclusive
  uint16_t nameOff = 0xFFFF;  // offset into nameBlob, or 0xFFFF if unnamed
  uint8_t channelFrom = kChannelAgnostic;
  uint8_t channelTo = kChannelAgnostic;  // inclusive, 0..15
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
//
// channelFrom/channelTo is the LED-OUTPUT side of channel significance, and
// is independent of the matching PAD/FADER range's own flag (A2 in
// FORMAT.md): on the APC40, input channel-significance covers a wider note
// span than LED-output channel-significance does (Akai's own protocol doc:
// buttons 0x30-0x49 use the channel for track, but LEDs only honour it on
// 0x30-0x39) -- so this flag lives on the LED record, not inherited from the
// PAD/FADER record it happens to overlap.
struct MdefLedRange {
  LedMsgType msgType = LedMsgType::Note;
  uint8_t addrFrom = 0;
  uint8_t addrTo = 0;   // inclusive: note number range or CC number range
  LedSemantic semantic = LedSemantic::Velocity;
  uint16_t colorOffset = 0;  // index into MidiControllerProfile::colors[]
  uint8_t colorCount = 0;
  uint8_t channelFrom = kChannelAgnostic;
  uint8_t channelTo = kChannelAgnostic;  // inclusive, 0..15
};

// One opaque outbound SysEx message from `INIT SYSEX <hex bytes>` -- the
// firmware never interprets `data`, only sends it verbatim, in declaration
// order, once the controller's MIDI-out path is up (controller_init.h).
struct MdefInitBlob {
  uint8_t len = 0;
  uint8_t data[MDEF_MAX_INIT_BLOB_BYTES] = {};
};

struct MidiControllerProfile {
  // 0 = any/unset, 1..16 = a fixed channel (1-indexed, unlike parseMidi's
  // 0-indexed ControlEvent::channel or this file's own 0-indexed
  // MdefPadRange/MdefFaderRange/MdefLedRange channelFrom/channelTo). Used
  // today only as LedFeedback's default outgoing LED channel when the
  // matched LED range is NOT itself channel-significant (led_feedback.cpp) --
  // it does not filter parseMidi's *input*, which reports every message's
  // channel regardless (live_control.h).
  uint8_t midiChannel = 0;

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

  // v3 only (0 on a v1/v2 blob -- "no INIT line" and "old MDF1 version"
  // are the same no-op case; see FORMAT.md's "INIT Blob Table (v3)").
  uint8_t initCount = 0;
  MdefInitBlob initBlobs[MDEF_MAX_INIT_BLOBS];

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
//  - bad magic, version not 1, 2, or 3, flags != 0
//  - any of padCount/faderCount/encoderCount/ledCount/colorCount exceeding
//    its MDEF_MAX_*
//  - a pad/fader/encoder/led addrFrom > addrTo, or an address/mode/msgType/
//    semantic value out of its valid range
//  - a fader or colour nameOff that isn't 0xFFFF (faders only) and doesn't
//    land on a NUL-terminated string within the trailing name blob
//  - an LED range's colorOffset/colorCount that doesn't fit within the
//    global colour table
//  - (version 2+ only) a pad/fader/LED channelFrom/channelTo that isn't the
//    kChannelAgnostic sentinel in both fields, and isn't a valid 0..15 range
//    with channelFrom <= channelTo
//  - (version 3 only) initCount exceeding MDEF_MAX_INIT_BLOBS, or any init
//    blob's declared length exceeding MDEF_MAX_INIT_BLOB_BYTES or running
//    past the end of the buffer
//
// Version 1 blobs (no channel fields, no init blobs -- every existing
// committed .mdef, and every profile built by a ControllerBuilder with no
// CH ranges or INIT lines) parse with every pad/fader/LED range's channel
// fields set to kChannelAgnostic and initCount 0, byte-for-byte the same
// runtime behavior as before version 2/3 existed. Version 2 blobs (channel
// ranges, no INIT lines) parse with initCount 0 the same way.
bool parseMidiController(const uint8_t* data, size_t len, MidiControllerProfile& out);

// Find the LED range (if any) covering `note`/`cc`. Returns nullptr if the
// controller has no LED declared for that address -- the "no .mdef LED for
// this pad" case glow.led.* must no-op on, not error on.
const MdefLedRange* findLedRangeForNote(const MidiControllerProfile& p, uint8_t note);
const MdefLedRange* findLedRangeForCc(const MidiControllerProfile& p, uint8_t cc);

// Find the PAD/FADER range covering `note`/`cc`, but ONLY if that range is
// channel-significant (see kChannelAgnostic above) -- nullptr both when no
// range covers the address at all AND when the covering range is ordinary
// (channel-agnostic). Used by LiveControl::effectiveId (live_control.h) to
// decide whether an incoming event's binding lookup key needs the
// (channel << 8) | id packing, and by glow.bind.pad-xy (glow_lua_api.cpp) to
// validate/resolve a grid column against the range's channel span.
const MdefPadRange* findPadChannelRange(const MidiControllerProfile& p, uint8_t note);
const MdefFaderRange* findFaderChannelRange(const MidiControllerProfile& p, uint8_t cc);

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

// glow.bind.pad-xy's grid resolver: maps a (col, row) grid coordinate to a
// (note, channel) pair via the profile's declared PAD ranges, for a
// controller whose grid is a stack of channel-significant single-note PAD
// declarations in row order (e.g. the APC40's 5 clip-launch note rows, each
// "PAD <note> CH 0 7", one physical control per channel/track) -- exactly
// the shape samples/apc40.mdef declares. `row` selects the row-th such
// declaration (in .mdef declaration order); `col` must land within that
// row's channelFrom..channelTo. Returns false (outputs untouched) if `row`
// is out of range or `col` isn't in that row's channel span -- a no-op, not
// an error, so glow.bind.pad-xy degrades gracefully on a controller with a
// smaller (or no) grid, per FORMAT.md/A1.
bool resolvePadXY(const MidiControllerProfile& p, int col, int row, uint8_t& noteOut, uint8_t& channelOut);
