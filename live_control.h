#pragma once

#include <cstddef>  // size_t
#include <cstdint>
#include <vector>

class ShowController;

enum class ControlType : uint8_t { Button, Fader };

struct ControlEvent {
  ControlType type;
  uint16_t id;
  bool pressed;
  float value;
};

enum class ActionKind : uint8_t { CueFlash, CueToggle, SceneGo, SceneToggle, Master };

bool parseMidi(const uint8_t* msg, size_t len, ControlEvent& out);

class LiveControl {
public:
  explicit LiveControl(ShowController& ctrl);

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

  ShowController& ctrl_;
  std::vector<Binding> bindings_;
  float master_ = 1.0f;
};
