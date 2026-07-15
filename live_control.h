#pragma once

#include <cstddef>  // size_t
#include <cstdint>
#include <string>
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

// P1.2: split by control SHAPE, not per-event-type -- CueFlash/CueToggle/
// SceneGo/SceneToggle are discrete (Button-driven, and Program as a scene
// selector -- see LiveControl::handle); Master/CueLevel/ParamSet are
// continuous (fed by any of Fader/PitchBend/Aftertouch, all of which
// report a normalized value -- see bindContinuous below). This is what
// lets a controller's wheel, pressure, or program buttons drive real
// actions without a per-controller/per-event-type special case: an
// APC40's fader and a keyboard's pitch wheel both just feed whichever
// continuous ActionKind they're bound to.
//   CueLevel  -- hold a cue at a manual weight (ShowController::setManualLevel)
//   ParamSet  -- drive a named effect parameter (LiveControl::paramValue,
//                read from Fennel via glow.param.get)
enum class ActionKind : uint8_t { CueFlash, CueToggle, SceneGo, SceneToggle, Master, CueLevel, ParamSet };

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

// Shared binding/lookup key helpers for channel-significant controls.
uint16_t packChannelControlId(uint8_t channel, uint16_t id);
uint16_t resolveFaderBindingId(const MidiControllerProfile* profile, uint8_t cc);

class LiveControl {
public:
  explicit LiveControl(ShowController& ctrl);

  // Optional: a channel-significant .mdef (e.g. the APC40's clip grid/track
  // faders, see FORMAT.md's "Per-range channel significance"). When set,
  // incoming events addressing a channel-significant pad/fader range are
  // looked up (and must have been bound) by the packed id
  // packChannelControlId(channel, id) -- see effectiveId() below and glow.bind.pad-xy
  // (glow_lua_api.cpp), which binds using that same packed id. nullptr (the
  // default) keeps every controller's existing channel-agnostic behavior
  // byte-for-byte: every id is looked up unpacked, exactly as before this
  // field existed.
  void setControllerProfile(const MidiControllerProfile* profile) { profile_ = profile; }

  void bindButton(uint16_t controlId, ActionKind action, uint16_t targetId);

  // targetId is ignored for ActionKind::Master (the only kind this had
  // before P1.2) and meaningful for CueLevel (a cue id) / ParamSet (a
  // param slot index -- see internParam below). Defaults to 0 so every
  // existing 2-argument call site (glow.bind.fader's :master path) keeps
  // compiling unchanged.
  void bindFader(uint16_t controlId, ActionKind action, uint16_t targetId = 0);

  // P1.2: the wheel and channel pressure are each a single physical
  // control (parseMidi always reports PitchBend/channel-Aftertouch with
  // id=0 -- see live_control.cpp/FORMAT.md's MIDI Parsing table), so
  // these take no controlId, unlike bindButton/bindFader which address a
  // note/CC number. A controller that never sends the underlying MIDI
  // message just never triggers the bound action -- no error, no special
  // case (FORMAT.md/A1's graceful-degradation rule, extended to sources
  // instead of just addresses).
  void bindPitchBend(ActionKind action, uint16_t targetId = 0);
  void bindPressure(ActionKind action, uint16_t targetId = 0);

  // P1.2: the one binding path every continuous source above funnels
  // through (bindFader/bindPitchBend/bindPressure are thin wrappers around
  // this). Exposed directly so a test -- or a future continuous source --
  // doesn't need a new per-type convenience wrapper to exercise it.
  void bindContinuous(ControlType type, uint16_t controlId, ActionKind action, uint16_t targetId = 0);

  // P1.2: Program Change as a scene selector -- "program N -> scene N",
  // exactly what program-change exists for (see handle()'s Program case).
  // Unlike every other bind* call, this takes no controlId/targetId: a
  // Program event's own `id` (the program number) IS the target scene id,
  // so there is nothing to pre-register per value. No-op (not an error)
  // on a controller that never sends Program Change.
  void bindProgram();

  // P1.2 (ActionKind::ParamSet): finds or creates the named parameter slot
  // and returns its index -- pass straight through to bindContinuous's
  // targetId. Interning is idempotent: binding two different continuous
  // sources to the same name (e.g. both a fader and the pitch wheel to
  // "hue") makes them both drive the one slot, last-write-wins per frame,
  // same convention as everything else in this dispatch table.
  uint16_t internParam(const std::string& name);

  // glow.param.get's read side: the named slot's last-set value, or 0.0f
  // if that name was never bound to (or never received) a ParamSet event
  // -- a typo'd or not-yet-bound name is a silent 0, not an error, same
  // contract as every other "controller doesn't have this" case here.
  float paramValue(const std::string& name) const;

  // Wipe every binding (glow.bind.clear) -- e.g. before a live-coded
  // re-bind pass so stale bindings from a previous version of boot.fnl
  // don't linger alongside the new ones. Does not touch master_: that's a
  // fader *value*, not a binding, and clearing it would drop the operator's
  // current grandmaster level for no reason. Also resets every ParamSet
  // slot to 0.0f and disables the Program->scene selector -- both are
  // binding-shaped state, unlike master_.
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

  struct ParamSlot {
    std::string name;
    float value = 0.0f;
  };

  Binding* find(ControlType type, uint16_t controlId);

  // Resolves an incoming event's raw (type, id, channel) to the id it was
  // (or should have been) bound under: unpacked, unless profile_ is set AND
  // the event's raw id falls in one of its channel-significant PAD/FADER
  // ranges (mdef.h's findPadChannelRange/findFaderChannelRange), in
  // which case it's packChannelControlId(channel, id) -- the same packing glow.bind.pad-xy
  // uses (glow_lua_api.cpp). Button ids are notes (0..127); Fader ids already
  // carry parseMidi's +128 offset, so the fader lookup strips it before
  // checking the profile's CC ranges and re-adds it after packing. Every
  // other continuous/discrete type (PitchBend, Aftertouch, Program) is
  // channel-agnostic today -- returned unpacked, unconditionally.
  uint16_t effectiveId(const ControlEvent& ev) const;

  // P1.2: the shared continuous-action dispatch (Master/CueLevel/
  // ParamSet) -- called from handle() for every ControlType that reports a
  // normalized value (Fader, PitchBend, Aftertouch). A discrete ActionKind
  // bound to a continuous source (a binding-table mismatch that shouldn't
  // happen given how the bind* wrappers are typed, but the dispatch stays
  // defensive) is a no-op, not a crash.
  void handleContinuous(Binding& binding, float value);

  ShowController& ctrl_;
  std::vector<Binding> bindings_;
  std::vector<ParamSlot> params_;
  float master_ = 1.0f;
  bool programSceneEnabled_ = false;
  const MidiControllerProfile* profile_ = nullptr;
};
