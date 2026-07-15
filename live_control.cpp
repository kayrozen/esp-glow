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

LiveControl::LiveControl(ShowController& ctrl) : ctrl_(ctrl) {}

void LiveControl::bindButton(uint16_t controlId, ActionKind action, uint16_t targetId) {
  bindings_.push_back({ControlType::Button, controlId, action, targetId, false});
}

void LiveControl::bindFader(uint16_t controlId, ActionKind action) {
  bindings_.push_back({ControlType::Fader, controlId, action, 0, false});
}

void LiveControl::clear() {
  bindings_.clear();
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
    return static_cast<uint16_t>((static_cast<uint16_t>(ev.channel) << 8) | ev.id);
  }
  if (ev.type == ControlType::Fader && ev.id >= 128) {
    uint8_t cc = static_cast<uint8_t>(ev.id - 128);
    if (findFaderChannelRange(*profile_, cc) == nullptr) return ev.id;
    return static_cast<uint16_t>((static_cast<uint16_t>(ev.channel) << 8) | ev.id);
  }
  return ev.id;
}

void LiveControl::handle(const ControlEvent& ev, float t) {
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
  } else if (ev.type == ControlType::Fader) {
    if (binding->action == ActionKind::Master) {
      master_ = ev.value;
      if (master_ < 0.0f) master_ = 0.0f;
      if (master_ > 1.0f) master_ = 1.0f;
    }
  }
}

float LiveControl::masterLevel() const {
  return master_;
}
