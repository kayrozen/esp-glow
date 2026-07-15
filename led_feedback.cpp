#include "led_feedback.h"

#include "show_control.h"

LedFeedback::LedFeedback(const MidiControllerProfile& profile, IMidiOutput* output,
                         float maxMsgsPerSec)
    : profile_(profile), output_(output), maxMsgsPerSec_(maxMsgsPerSec) {}

LedFeedback::LedState& LedFeedback::stateFor(uint8_t addr, uint8_t stateChannel) {
  for (auto& st : states_) {
    if (st.addr == addr && st.stateChannel == stateChannel) return st;
  }
  states_.push_back(LedState{});
  states_.back().addr = addr;
  states_.back().stateChannel = stateChannel;
  return states_.back();
}

void LedFeedback::applyDesired(uint8_t addr, uint8_t channel, const char* colorName) {
  const MdefLedRange* range = findLedRangeForNote(profile_, addr);
  LedMsgType msgType = LedMsgType::Note;
  if (range == nullptr) {
    range = findLedRangeForCc(profile_, addr);
    msgType = LedMsgType::Cc;
  }
  if (range == nullptr) return;  // no LED declared for this address -- no-op

  uint8_t value;
  if (!ledColorValueByName(profile_, *range, colorName, value)) return;  // unknown colour -- no-op

  LedState& st = stateFor(addr, channel);
  st.msgType = msgType;
  // The LED-output channel rule: only honour `channel` if the matched LED
  // range itself is channel-significant (see mdef.h's MdefLedRange comment
  // -- this is independent of, and can be narrower than, the PAD/FADER
  // range's own significance). Otherwise fall back to the MIDI_CHANNEL
  // default at send time, same as the plain (kChannelAgnostic) overload.
  st.sendChannel = (range->channelFrom != kChannelAgnostic && channel != kChannelAgnostic)
                      ? channel
                      : kChannelAgnostic;
  st.hasDesired = true;
  st.desiredValue = value;
}

void LedFeedback::set(uint8_t addr, const char* colorName) {
  applyDesired(addr, kChannelAgnostic, colorName);
}

void LedFeedback::set(uint8_t addr, uint8_t channel, const char* colorName) {
  applyDesired(addr, channel, colorName);
}

void LedFeedback::setAuto(uint8_t addr, uint16_t cueId, std::string activeColorName,
                          std::string inactiveColorName) {
  setAuto(addr, kChannelAgnostic, cueId, std::move(activeColorName), std::move(inactiveColorName));
}

void LedFeedback::setAuto(uint8_t addr, uint8_t channel, uint16_t cueId, std::string activeColorName,
                          std::string inactiveColorName) {
  for (auto& b : autoBindings_) {
    if (b.addr == addr && b.channel == channel) {
      b.cueId = cueId;
      b.activeColorName = std::move(activeColorName);
      b.inactiveColorName = std::move(inactiveColorName);
      return;
    }
  }
  autoBindings_.push_back({addr, channel, cueId, std::move(activeColorName), std::move(inactiveColorName)});
}

void LedFeedback::clearAuto() {
  autoBindings_.clear();
}

void LedFeedback::sendFor(const LedState& st) {
  if (output_ == nullptr) return;
  uint8_t nibble;
  if (st.sendChannel != kChannelAgnostic) {
    nibble = st.sendChannel;
  } else {
    // midiChannel: 0 = any/unset -> send on channel 1 (nibble 0); 1..16 ->
    // nibble = value-1 (grammar's MIDI_CHANNEL is 1-indexed, MIDI status
    // nibbles are 0-indexed).
    nibble = (profile_.midiChannel == 0) ? 0 : static_cast<uint8_t>(profile_.midiChannel - 1);
  }
  uint8_t status = static_cast<uint8_t>((st.msgType == LedMsgType::Note ? 0x90 : 0xB0) | (nibble & 0x0F));
  output_->send3(status, st.addr, st.desiredValue);
}

void LedFeedback::pump(float t) {
  if (lastPumpT_ < 0.0f) {
    // First refresh ever: start with a full bucket so an initial LED paint
    // (every pad's starting colour on boot) can go out in one burst, up to
    // the configured rate cap.
    tokens_ = maxMsgsPerSec_;
  } else {
    float dt = t - lastPumpT_;
    if (dt > 0.0f) {
      tokens_ += dt * maxMsgsPerSec_;
      if (tokens_ > maxMsgsPerSec_) tokens_ = maxMsgsPerSec_;
    }
  }
  lastPumpT_ = t;

  for (auto& st : states_) {
    if (!st.hasDesired) continue;
    if (st.hasLastSent && st.desiredValue == st.lastSentValue) continue;  // unchanged: no send
    if (tokens_ < 1.0f) continue;  // rate limited this frame; retried next refresh()

    sendFor(st);
    tokens_ -= 1.0f;
    st.lastSentValue = st.desiredValue;
    st.hasLastSent = true;
  }
}

void LedFeedback::refresh(const ShowController& show, float t) {
  for (const auto& b : autoBindings_) {
    bool active = show.isActive(b.cueId);
    const std::string& name = active ? b.activeColorName : b.inactiveColorName;
    applyDesired(b.addr, b.channel, name.c_str());
  }
  pump(t);
}

size_t LedFeedback::pendingCount() const {
  size_t n = 0;
  for (const auto& st : states_) {
    if (st.hasDesired && (!st.hasLastSent || st.desiredValue != st.lastSentValue)) ++n;
  }
  return n;
}
