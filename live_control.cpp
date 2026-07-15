#include "live_control.h"
#include "mdef.h"
#include "show_control.h"

bool parseMidi(const uint8_t* msg, size_t len, ControlEvent& out) {
  if (len < 1) return false;

  uint8_t status = msg[0] & 0xF0;
  if (status == 0xF0) return false;  // System/Realtime -- not channel-voice, routed elsewhere

  uint8_t channel = msg[0] & 0x0F;

  // Program Change and Channel Pressure are 2-byte messages (status + one
  // data byte) -- every other channel-voice status is 3 bytes. Validate
  // against the length THIS status needs, never a blanket "len < 3": that
  // used to reject valid 2-byte messages outright.
  bool twoByte = (status == 0xC0 || status == 0xD0);
  size_t need = twoByte ? 2 : 3;
  if (len < need) return false;

  uint8_t data1 = msg[1];
  uint8_t data2 = twoByte ? 0 : msg[2];

  switch (status) {
    case 0x80:  // Note Off
      out.type = ControlType::Button;
      out.channel = channel;
      out.id = data1;
      out.pressed = false;
      out.value = 0.0f;
      return true;
    case 0x90:  // Note On (velocity 0 is a note-off in disguise)
      out.type = ControlType::Button;
      out.channel = channel;
      out.id = data1;
      out.pressed = (data2 > 0);
      out.value = 0.0f;
      return true;
    case 0xA0:  // Polyphonic Key Pressure (per-note aftertouch)
      out.type = ControlType::Aftertouch;
      out.channel = channel;
      out.id = data1;
      out.pressed = false;
      out.value = data2 / 127.0f;
      return true;
    case 0xB0:  // Control Change
      out.type = ControlType::Fader;
      out.channel = channel;
      out.id = 128 + data1;
      out.pressed = false;
      out.value = data2 / 127.0f;
      return true;
    case 0xC0:  // Program Change
      out.type = ControlType::Program;
      out.channel = channel;
      out.id = data1;
      out.pressed = false;
      out.value = 0.0f;
      return true;
    case 0xD0:  // Channel Pressure (whole-channel aftertouch, no note id)
      out.type = ControlType::Aftertouch;
      out.channel = channel;
      out.id = 0;
      out.pressed = false;
      out.value = data1 / 127.0f;
      return true;
    case 0xE0: {  // Pitch Bend: 14-bit, LSB then MSB, center 0x2000 -> 0.5
      uint16_t bend14 = static_cast<uint16_t>(data1) | (static_cast<uint16_t>(data2) << 7);
      out.type = ControlType::PitchBend;
      out.channel = channel;
      out.id = 0;
      out.pressed = false;
      out.value = bend14 / 16383.0f;
      return true;
    }
    default:
      return false;
  }
}

uint16_t packChannelControlId(uint8_t channel, uint16_t id) {
  return static_cast<uint16_t>((static_cast<uint16_t>(channel) << 8) | id);
}

uint16_t resolveFaderBindingId(const MidiControllerProfile* profile, uint8_t cc) {
  uint16_t id = static_cast<uint16_t>(128 + cc);
  if (profile == nullptr) return id;

  const MdefFaderRange* range = findFaderChannelRange(*profile, cc);
  if (range == nullptr) return id;

  // glow.bind.fader has no channel/coordinate argument; for a multi-channel
  // fader range, resolve to the range's base channel.
  return packChannelControlId(range->channelFrom, id);
}

LiveControl::LiveControl(ShowController& ctrl) : ctrl_(ctrl) {}

void LiveControl::bindButton(uint16_t controlId, ActionKind action, uint16_t targetId) {
  bindings_.push_back({ControlType::Button, controlId, action, targetId, false});
}

void LiveControl::bindFader(uint16_t controlId, ActionKind action, uint16_t targetId) {
  bindContinuous(ControlType::Fader, controlId, action, targetId);
}

void LiveControl::bindPitchBend(ActionKind action, uint16_t targetId) {
  bindContinuous(ControlType::PitchBend, 0, action, targetId);
}

void LiveControl::bindPressure(ActionKind action, uint16_t targetId) {
  bindContinuous(ControlType::Aftertouch, 0, action, targetId);
}

void LiveControl::bindContinuous(ControlType type, uint16_t controlId, ActionKind action, uint16_t targetId) {
  bindings_.push_back({type, controlId, action, targetId, false});
}

void LiveControl::bindProgram() {
  programSceneEnabled_ = true;
}

uint16_t LiveControl::internParam(const std::string& name) {
  for (size_t i = 0; i < params_.size(); ++i) {
    if (params_[i].name == name) return static_cast<uint16_t>(i);
  }
  params_.push_back({name, 0.0f});
  return static_cast<uint16_t>(params_.size() - 1);
}

float LiveControl::paramValue(const std::string& name) const {
  for (const auto& p : params_) {
    if (p.name == name) return p.value;
  }
  return 0.0f;
}

void LiveControl::clear() {
  bindings_.clear();
  params_.clear();
  programSceneEnabled_ = false;
}

LiveControl::Binding* LiveControl::find(ControlType type, uint16_t controlId) {
  for (auto& b : bindings_) {
    if (b.type == type && b.controlId == controlId) {
      return &b;
    }
  }
  return nullptr;
}

uint16_t LiveControl::effectiveId(const ControlEvent& ev) const {
  if (profile_ == nullptr) return ev.id;  // no .mdef -- unpacked, unchanged

  if (ev.type == ControlType::Button) {
    if (findPadChannelRange(*profile_, static_cast<uint8_t>(ev.id)) == nullptr) return ev.id;
    return packChannelControlId(ev.channel, ev.id);
  }
  if (ev.type == ControlType::Fader && ev.id >= 128) {
    uint8_t cc = static_cast<uint8_t>(ev.id - 128);
    if (findFaderChannelRange(*profile_, cc) == nullptr) return ev.id;
    return packChannelControlId(ev.channel, ev.id);
  }
  return ev.id;
}

namespace {
bool isContinuousType(ControlType type) {
  return type == ControlType::Fader || type == ControlType::PitchBend || type == ControlType::Aftertouch;
}
}  // namespace

void LiveControl::handle(const ControlEvent& ev, float t) {
  // P1.2: Program Change is a scene SELECTOR, not a (controlId -> fixed
  // target) binding like everything else here -- the event's own `id`
  // (the program number) names which scene to go to, so there is no
  // per-value Binding to look up. See bindProgram()'s header comment.
  if (ev.type == ControlType::Program) {
    if (programSceneEnabled_) ctrl_.goScene(ev.id, t);
    return;
  }

  Binding* binding = find(ev.type, effectiveId(ev));
  if (!binding) return;

  if (ev.type == ControlType::Button) {
    if (binding->action == ActionKind::CueFlash) {
      if (ev.pressed) {
        ctrl_.go(binding->targetId, t);
      } else {
        ctrl_.release(binding->targetId, t);
      }
    } else if (binding->action == ActionKind::CueToggle) {
      if (ev.pressed) {
        if (binding->latched) {
          ctrl_.release(binding->targetId, t);
          binding->latched = false;
        } else {
          ctrl_.go(binding->targetId, t);
          binding->latched = true;
        }
      }
    } else if (binding->action == ActionKind::SceneGo) {
      if (ev.pressed) {
        ctrl_.goScene(binding->targetId, t);
      } else {
        ctrl_.releaseScene(binding->targetId, t);
      }
    } else if (binding->action == ActionKind::SceneToggle) {
      if (ev.pressed) {
        if (binding->latched) {
          ctrl_.releaseScene(binding->targetId, t);
          binding->latched = false;
        } else {
          ctrl_.goScene(binding->targetId, t);
          binding->latched = true;
        }
      }
    }
  } else if (isContinuousType(ev.type)) {
    handleContinuous(*binding, ev.value);
  }
}

void LiveControl::handleContinuous(Binding& binding, float value) {
  float v = value;
  if (v < 0.0f) v = 0.0f;
  if (v > 1.0f) v = 1.0f;

  switch (binding.action) {
    case ActionKind::Master:
      master_ = v;
      break;
    case ActionKind::CueLevel:
      ctrl_.setManualLevel(binding.targetId, v);
      break;
    case ActionKind::ParamSet:
      if (binding.targetId < params_.size()) params_[binding.targetId].value = v;
      break;
    default:
      break;  // a discrete ActionKind bound to a continuous source: no-op
  }
}

float LiveControl::masterLevel() const {
  return master_;
}
