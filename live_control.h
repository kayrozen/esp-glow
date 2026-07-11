<<<<<<< ours
#pragma once

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
=======
// live_control.h — input-to-ShowController binding layer.
//
// Maps triggers from the three input transports (MIDI, OSC, web) onto
// ShowController actions (go/release cue, go scene). Host-compilable (no device
// deps) so the binding dispatch is exercised by test_web_input_handler.cpp.
//
// Bindings are registered at boot from a config (the bundle doesn't encode
// them yet; F4 loads them from /littlefs/live.json — see web_input.cpp). The
// device transports call the handle* methods with the current show time t.
#pragma once

#include "show_control.h"
#include "midi_parser.h"
#include <cstdint>
#include <vector>

class LiveControl {
public:
  explicit LiveControl(ShowController& controller);

  // --- Bindings ---
  // Note On (channel,note) -> go(cue); Note Off -> release(cue). Velocity 0
  // on a Note On is treated as Note Off by the caller (the parser is literal).
  void bindMidiNote(uint8_t channel, uint8_t note, uint16_t cueId);
  // CC (channel,cc): value > 0 -> go(cue), value == 0 -> release(cue).
  void bindMidiCC(uint8_t channel, uint8_t cc, uint16_t cueId);
  // OSC address -> go(cue) on any message to that address; if releaseOnZero
  // is set, a 0 arg releases the cue instead.
  void bindOsc(const char* address, uint16_t cueId, bool releaseOnZero = false);
  // Web button id -> go(cue). (The console sends {"type":"button","id":N}.)
  void bindWebButton(uint8_t buttonId, uint16_t cueId, const char* label = "");

  // --- Handlers (device transports call these with show time t in seconds) ---
  void handleMidi(const MidiEvent& ev, float t);
  void handleOsc(const char* address, float arg, bool hasArg, float t);
  void handleWebCueGo(uint16_t cueId, float t);
  void handleWebCueRelease(uint16_t cueId, float t);
  void handleWebScene(uint16_t sceneId, float t);
  void handleWebButton(uint8_t buttonId, float t);

  // --- Config snapshot for the web console (built by web_input.cpp) ---
  struct ButtonInfo { uint8_t id; uint16_t cueId; const char* label; };
  const std::vector<ButtonInfo>& buttons() const { return web_; }

private:
  ShowController& controller_;

  struct MidiBinding {
    uint8_t channel;
    uint8_t index;     // note or cc
    uint16_t cueId;
    bool isCC;
  };
  struct OscBinding {
    char address[64];
    uint16_t cueId;
    bool releaseOnZero;
  };
  // ButtonInfo carries id, cueId, label.
  std::vector<MidiBinding> midi_;
  std::vector<OscBinding>  osc_;
  std::vector<ButtonInfo>  web_;
>>>>>>> theirs
};
