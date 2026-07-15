#pragma once

#include "mdef.h"

#include <cstdint>
#include <string>
#include <vector>

class ShowController;

//
// LED feedback engine (A5/A6 of the .mdef design) -- the payoff of the
// whole format: `MidiControllerProfile` says which pads have an LED and how
// to drive it; this turns "cue :chorus is active" into the right MIDI
// message on the right address, with two invariants a naive
// "resend everything every frame" implementation would violate:
//
//  - CHANGE DETECTION: a static show (no cue transitions) emits zero MIDI
//    traffic after the first refresh -- every LED's last-sent value is
//    tracked, and refresh() only ever sends when the desired value differs.
//  - RATE LIMIT: even when many LEDs change in the same frame (the initial
//    paint on boot, a scene change touching 40 pads at once), sends are
//    capped via a token bucket (default 100 msg/sec -- see IMidiOutput) so
//    a DIN link (31250 baud =~ 1000 msg/sec) never gets flooded. Delayed,
//    not dropped: an unsent change stays pending and is retried on the next
//    refresh() once a token is available.
//
// No-op, not an error, when the controller has no LED declared for an
// address or the requested colour name isn't in that LED's palette -- same
// graceful-degradation rule as glow.set on a missing capability (see
// mdef.h's findLedRangeForNote/Cc, ledColorValueByName). Pure C++, no
// hardware -- host-tested like LiveControl.
//

// Sends raw 3-byte MIDI messages (status, data1, data2). Device wiring
// implements this over the DIN UART's TX pin (midi_input.cpp) or, later, a
// USB-MIDI IN endpoint; host tests use a small recording fake. LedFeedback
// never touches a UART/USB driver directly -- same transport-agnostic seam
// as parseMidi/IControlEventQueue elsewhere in this project.
class IMidiOutput {
public:
  virtual ~IMidiOutput() = default;
  virtual void send3(uint8_t status, uint8_t data1, uint8_t data2) = 0;
};

class LedFeedback {
public:
  // `profile` and `output` are borrowed and must outlive this object.
  // `output` may be nullptr (no MIDI OUT transport wired) -- every refresh
  // still runs change-detection/rate-limiting bookkeeping, it just has
  // nowhere to send, matching glow.led.*'s "no capability -> no-op"
  // contract without every call site needing its own null check.
  // maxMsgsPerSec caps the token bucket (A5's suggested default: 100);
  // also the size of the initial burst allowed on the very first refresh
  // (e.g. painting every LED's starting state on boot).
  explicit LedFeedback(const MidiControllerProfile& profile, IMidiOutput* output = nullptr,
                       float maxMsgsPerSec = 100.0f);

  // glow.led.set: stage `addr` (a pad note or fader CC -- whichever the
  // .mdef declared an LED for) to show `colorName` from that LED range's
  // palette. Takes effect on the next refresh(), same frame-batched
  // discipline as glow.set. No-op if `addr` has no LED, or `colorName`
  // isn't in its palette.
  void set(uint8_t addr, const char* colorName);

  // Channel-aware glow.led.set, for a channel-multiplexed pad (the APC40's
  // clip grid): `channel` picks which of the several physical controls
  // sharing `addr` this call addresses, so each channel gets its own
  // tracked state instead of stomping the others. At send time, the
  // channel nibble actually emitted follows the LED-OUTPUT rule declared on
  // the matched LED range (MdefLedRange::channelFrom/channelTo,
  // independent of the PAD range's own flag -- FORMAT.md's "Per-range
  // channel significance"): if that range is LED-channel-significant,
  // `channel` is used; if not (e.g. the APC40's input-only-significant
  // ranges), `channel` is ignored and the ordinary MIDI_CHANNEL-derived
  // nibble is sent instead, exactly like the plain overload above.
  void set(uint8_t addr, uint8_t channel, const char* colorName);

  // glow.led.auto: `addr` continuously tracks whether `cueId` is active in
  // the ShowController passed to refresh() -- `activeColorName` while
  // active, `inactiveColorName` while not. Re-evaluated every refresh();
  // registering another auto binding for the same `addr` replaces the
  // earlier one (same "redefine wins" convention as glow.cue.define).
  void setAuto(uint8_t addr, uint16_t cueId, std::string activeColorName,
              std::string inactiveColorName);

  // Channel-aware glow.led.auto -- see the channel-aware set() above for
  // what `channel` means. Registering another auto binding for the same
  // (addr, channel) pair replaces the earlier one; a plain (addr)-only auto
  // binding and a (addr, channel) one are tracked independently.
  void setAuto(uint8_t addr, uint8_t channel, uint16_t cueId, std::string activeColorName,
              std::string inactiveColorName);

  // glow.bind.clear also calls this: stop tracking every auto binding (a
  // fresh boot.fnl pass is about to redefine the cue ids they reference).
  // Does not touch already-lit LEDs or last-sent tracking -- an LED that
  // isn't driven by anything anymore just stays as it last was.
  void clearAuto();

  // Evaluate every auto binding against `show`'s current cue state, then
  // flush pending (changed, not yet sent) LED updates through the rate
  // limiter. Call once per frame with the frame's `t` (seconds) -- same
  // discipline as ShowController::evaluate.
  void refresh(const ShowController& show, float t);

  // Test/introspection: how many addresses have a pending update (changed
  // since last actually sent, e.g. still waiting on the rate limiter).
  size_t pendingCount() const;

  // The .mdef this feedback engine was built against -- glow.bind.pad-xy
  // (glow_lua_api.cpp) reads it to resolve a grid (col, row) via
  // resolvePadXY (mdef.h). Always valid: `profile` is a required
  // constructor argument, borrowed, and must outlive this object.
  const MidiControllerProfile& profile() const { return profile_; }

private:
  struct AutoBinding {
    uint8_t addr;
    uint8_t channel;  // kChannelAgnostic (mdef.h) for the plain (addr)-only overload
    uint16_t cueId;
    std::string activeColorName;
    std::string inactiveColorName;
  };

  struct LedState {
    uint8_t addr = 0;
    uint8_t stateChannel = kChannelAgnostic;  // storage key's channel component (kChannelAgnostic = the legacy, addr-only slot)
    LedMsgType msgType = LedMsgType::Note;
    uint8_t sendChannel = kChannelAgnostic;   // channel nibble to emit IF the matched LED range is channel-significant; else the MIDI_CHANNEL default is used
    bool hasDesired = false;
    uint8_t desiredValue = 0;
    bool hasLastSent = false;
    uint8_t lastSentValue = 0;
  };

  void applyDesired(uint8_t addr, uint8_t channel, const char* colorName);
  LedState& stateFor(uint8_t addr, uint8_t stateChannel);
  void pump(float t);
  void sendFor(const LedState& st);

  const MidiControllerProfile& profile_;
  IMidiOutput* output_;
  float maxMsgsPerSec_;
  float tokens_ = 0.0f;
  float lastPumpT_ = -1.0f;  // < 0 => never pumped yet

  std::vector<AutoBinding> autoBindings_;
  std::vector<LedState> states_;  // one per addr ever touched; small N, linear scan
};
