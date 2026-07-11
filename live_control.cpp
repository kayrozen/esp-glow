<<<<<<< ours
#include "live_control.h"
#include "show_control.h"

bool parseMidi(const uint8_t* msg, size_t len, ControlEvent& out) {
  if (len < 3) return false;

  uint8_t status = msg[0] & 0xF0;
  uint8_t data1 = msg[1];
  uint8_t data2 = msg[2];

  if (status == 0x90) {
    out.type = ControlType::Button;
    out.id = data1;
    out.pressed = (data2 > 0);
    out.value = 0.0f;
    return true;
  } else if (status == 0x80) {
    out.type = ControlType::Button;
    out.id = data1;
    out.pressed = false;
    out.value = 0.0f;
    return true;
  } else if (status == 0xB0) {
    out.type = ControlType::Fader;
    out.id = 128 + data1;
    out.value = data2 / 127.0f;
    out.pressed = false;
    return true;
  }

  return false;
}

LiveControl::LiveControl(ShowController& ctrl) : ctrl_(ctrl) {}

void LiveControl::bindButton(uint16_t controlId, ActionKind action, uint16_t targetId) {
  bindings_.push_back({ControlType::Button, controlId, action, targetId, false});
}

void LiveControl::bindFader(uint16_t controlId, ActionKind action) {
  bindings_.push_back({ControlType::Fader, controlId, action, 0, false});
}

LiveControl::Binding* LiveControl::find(ControlType type, uint16_t controlId) {
  for (auto& b : bindings_) {
    if (b.type == type && b.controlId == controlId) {
      return &b;
    }
  }
  return nullptr;
}

void LiveControl::handle(const ControlEvent& ev, float t) {
  Binding* binding = find(ev.type, ev.id);
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
=======
// live_control.cpp — input-to-ShowController dispatch.
#include "live_control.h"
#include <cstring>
#include <algorithm>

LiveControl::LiveControl(ShowController& controller) : controller_(controller) {}

void LiveControl::bindMidiNote(uint8_t channel, uint8_t note, uint16_t cueId) {
  midi_.push_back({channel, note, cueId, /*isCC*/ false});
}
void LiveControl::bindMidiCC(uint8_t channel, uint8_t cc, uint16_t cueId) {
  midi_.push_back({channel, cc, cueId, /*isCC*/ true});
}
void LiveControl::bindOsc(const char* address, uint16_t cueId, bool releaseOnZero) {
  OscBinding b{};
  std::strncpy(b.address, address ? address : "", sizeof(b.address) - 1);
  b.cueId = cueId;
  b.releaseOnZero = releaseOnZero;
  osc_.push_back(b);
}
void LiveControl::bindWebButton(uint8_t buttonId, uint16_t cueId, const char* label) {
  web_.push_back({buttonId, cueId, label});
}

void LiveControl::handleMidi(const MidiEvent& ev, float t) {
  if (ev.type == MidiEvent::NoteOn) {
    for (const auto& b : midi_) {
      if (!b.isCC && b.channel == ev.channel && b.index == ev.data1) {
        if (ev.data2 > 0) controller_.go(b.cueId, t);
        else              controller_.release(b.cueId, t);  // velocity 0 = off
        return;
      }
    }
  } else if (ev.type == MidiEvent::NoteOff) {
    for (const auto& b : midi_) {
      if (!b.isCC && b.channel == ev.channel && b.index == ev.data1) {
        controller_.release(b.cueId, t);
        return;
      }
    }
  } else if (ev.type == MidiEvent::ControlChange) {
    for (const auto& b : midi_) {
      if (b.isCC && b.channel == ev.channel && b.index == ev.data1) {
        if (ev.data2 > 0) controller_.go(b.cueId, t);
        else              controller_.release(b.cueId, t);
        return;
      }
    }
  }
}

void LiveControl::handleOsc(const char* address, float arg, bool hasArg, float t) {
  if (!address) return;
  for (const auto& b : osc_) {
    if (std::strncmp(b.address, address, sizeof(b.address)) == 0) {
      if (b.releaseOnZero && hasArg && arg <= 0.0f) {
        controller_.release(b.cueId, t);
      } else {
        controller_.go(b.cueId, t);
      }
      return;
>>>>>>> theirs
    }
  }
}

<<<<<<< ours
float LiveControl::masterLevel() const {
  return master_;
=======
void LiveControl::handleWebCueGo(uint16_t cueId, float t)      { controller_.go(cueId, t); }
void LiveControl::handleWebCueRelease(uint16_t cueId, float t)  { controller_.release(cueId, t); }
void LiveControl::handleWebScene(uint16_t sceneId, float t)     { controller_.goScene(sceneId, t); }

void LiveControl::handleWebButton(uint8_t buttonId, float t) {
  for (const auto& b : web_) {
    if (b.id == buttonId) {
      controller_.go(b.cueId, t);
      return;
    }
  }
>>>>>>> theirs
}
