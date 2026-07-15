#pragma once

#include <cstddef>  // size_t
#include <cstdint>
#include <vector>

class ShowController;
struct MidiControllerProfile;

// Button/Fader: unchanged since the original 3-message parser (Note On/Off,
// CC). Aftertouch/PitchBend/Program are new -- see parseMidi below and
// FORMAT.md/README_LIVE_CONTROL.md for the full seven-message-type table.
enum class ControlType : uint8_t { Button, Fader, Aftertouch, PitchBend, Program };

struct ControlEvent {
  ControlType type;
  uint16_t id;        // note / CC (128+cc, unchanged) / program -- see parseMidi
  uint8_t channel = 0; // 0..15, low nibble of the MIDI status byte. ALWAYS
                       // set by parseMidi, for every message type -- whether
                       // a binding cares is a per-controller/.mdef decision
                       // (see mdef.h's channel-significant pad/fader ranges
                       // and LiveControl::effectiveId below), not parseMidi's.
  bool pressed;
  float value;
};

enum class ActionKind : uint8_t { CueFlash, CueToggle, SceneGo, SceneToggle, Master };

// Parses one complete, already-framed MIDI channel-voice message (status +
// its data bytes) into a ControlEvent. Framing (running status, Realtime-byte
// interleaving) is MidiByteReader's job (midi_realtime.h) -- this function
// has no opinion on it and never reads past `len`.
//
// All seven MIDI channel-voice message types are handled, each validated
// against its OWN wire length (2 bytes for Program Change/Channel Pressure,
// 3 for everything else) -- never a blanket "len < 3":
//
//   status  message            bytes  data1        data2
//   0x80    Note Off             3    note         velocity
//   0x90    Note On              3    note         velocity (0 => pressed=false)
//   0xA0    Poly Aftertouch      3    note          pressure
//   0xB0    Control Change       3    controller   value
//   0xC0    Program Change       2    program      --
//   0xD0    Channel Pressure     2    pressure     --
//   0xE0    Pitch Bend           3    LSB          MSB (14-bit)
//
// `msg` may be longer than the message needs (e.g. USB-MIDI's fixed 3-byte,
// zero-padded packets) -- only `status >= 0xF0` (System/Realtime, not a
// channel-voice message at all -- routed elsewhere, see midi_realtime.h) and
// a buffer shorter than the status's own required length are rejected.
// Bounds-safe: never reads `msg[i]` for `i >= len`.
bool parseMidi(const uint8_t* msg, size_t len, ControlEvent& out);

class LiveControl {
public:
  explicit LiveControl(ShowController& ctrl);

  // Optional: a channel-significant .mdef (e.g. the APC40's clip grid/track
  // faders, see FORMAT.md's "Per-range channel significance"). When set,
  // incoming events addressing a channel-significant pad/fader range are
  // looked up (and must have been bound) by the packed id
  // (channel << 8) | id -- see effectiveId() below and glow.bind.pad-xy
  // (glow_lua_api.cpp), which binds using that same packed id. nullptr (the
  // default) keeps every controller's existing channel-agnostic behavior
  // byte-for-byte: every id is looked up unpacked, exactly as before this
  // field existed.
  void setControllerProfile(const MidiControllerProfile* profile) { profile_ = profile; }

  void bindButton(uint16_t controlId, ActionKind action, uint16_t targetId);
  void bindFader(uint16_t controlId, ActionKind action);

  // Wipe every binding (glow.bind.clear) -- e.g. before a live-coded
  // re-bind pass so stale bindings from a previous version of boot.fnl
  // don't linger alongside the new ones. Does not touch master_: that's a
  // fader *value*, not a binding, and clearing it would drop the operator's
  // current grandmaster level for no reason.
  void clear();

  void handle(const ControlEvent& ev, float t);

  float masterLevel() const;

private:
  struct Binding {
    ControlType type;
    uint16_t controlId;
    ActionKind action;
    uint16_t targetId;
    bool latched = false;
  };

  Binding* find(ControlType type, uint16_t controlId);

  // Resolves an incoming event's raw (type, id, channel) to the id it was
  // (or should have been) bound under: unpacked, unless profile_ is set AND
  // the event's raw id falls in one of its channel-significant PAD/FADER
  // ranges (mdef.h's isPadChannelSignificant/isFaderChannelSignificant), in
  // which case it's (channel << 8) | id -- the same packing glow.bind.pad-xy
  // uses (glow_lua_api.cpp). Button ids are notes (0..127); Fader ids already
  // carry parseMidi's +128 offset, so the fader lookup strips it before
  // checking the profile's CC ranges and re-adds it after packing.
  uint16_t effectiveId(const ControlEvent& ev) const;

  ShowController& ctrl_;
  std::vector<Binding> bindings_;
  float master_ = 1.0f;
  const MidiControllerProfile* profile_ = nullptr;
};
