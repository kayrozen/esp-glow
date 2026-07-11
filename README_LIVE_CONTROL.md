# Live Control Layer: MIDI/OSC/Web → Cues

The live control layer transforms incoming control input (MIDI, OSC, WebSocket/HTTP) into real-time cue and scene actions via the `ShowController`.

## Architecture

The layer splits into two parts:

### Core (Testable, Host-Only)
- **`live_control.h/cpp`**: Pure event dispatch engine + MIDI parser
  - `LiveControl`: bind controls to actions, dispatch `ControlEvent`s to `ShowController` methods
  - `parseMidi`: pure function converting raw MIDI bytes to `ControlEvent`

No heap allocation in the dispatch path (`handle`). No third-party dependencies. C++17, `-Wall -Wextra -Werror` clean.

### Transports (Device-Only Scaffolds, Excluded from Host Tests)
- **`midi_input.cpp`** (ESP_PLATFORM only): reads MIDI bytes from UART/USB, calls `parseMidi`, dispatches via `LiveControl::handle`
- **`osc_input.cpp`** (ESP_PLATFORM only): receives UDP OSC packets (address + one float/int arg), maps to `ControlEvent`, dispatches
- **`web_input.cpp`** (ESP_PLATFORM only): WebSocket/HTTP command handler, transforms commands to `ControlEvent`s

Each transport is a stub with `TODO` comments marking where hardware I/O actually happens. They do not run in host tests.

## Control Types and Events

```cpp
enum class ControlType : uint8_t { Button, Fader };

struct ControlEvent {
  ControlType type;
  uint16_t id;      // opaque logical id (0..65535)
  bool pressed;     // Button: true = down/on, false = up/off
  float value;      // Fader: [0,1]
};
```

Events are type-checked during dispatch: a Button event sent to a Fader binding (or vice versa) is silently ignored.

## Action Types

```cpp
enum class ActionKind : uint8_t { CueFlash, CueToggle, SceneGo, SceneToggle, Master };
```

### CueFlash (Button-only)
Momentary trigger:
- **Press** → `ShowController::go(cueId, t)` (activate immediately)
- **Release** → `ShowController::release(cueId, t)` (begin fade-out)

The cue is active only while the button is held.

### CueToggle (Button-only)
Latch toggle:
- **Press (unlatched)** → `go(cueId, t)`, set latched=true
- **Press (latched)** → `release(cueId, t)`, set latched=false
- **Release** → ignored (no-op)

The engine tracks its own `latched` state per binding, independent of the cue's fade state.

### SceneGo (Button-only)
Momentary scene trigger:
- **Press** → `ShowController::goScene(sceneId, t)` (activate all cues in scene)
- **Release** → `ShowController::releaseScene(sceneId, t)` (fade out all)

### SceneToggle (Button-only)
Latch scene toggle:
- **Press (unlatched)** → `goScene(sceneId, t)`, set latched=true
- **Press (latched)** → `releaseScene(sceneId, t)`, set latched=false
- **Release** → ignored (no-op)

### Master (Fader-only)
Grandmaster level:
- **Fader change** → store `value` clamped to [0,1]
- **Read** → `LiveControl::masterLevel()` returns the last stored level

The control layer only tracks the master level; the firmware applies it (e.g., as a global brightness scale).

## Binding Model

```cpp
LiveControl live(showController);

// Button binding
live.bindButton(controlId, ActionKind::CueFlash, targetCueId);
live.bindButton(controlId, ActionKind::CueToggle, targetCueId);
live.bindButton(controlId, ActionKind::SceneGo, targetSceneId);
live.bindButton(controlId, ActionKind::SceneToggle, targetSceneId);

// Fader binding (only Master supported here)
live.bindFader(controlId, ActionKind::Master);

// Dispatch
ControlEvent ev = {...};
live.handle(ev, timeInSeconds);

// Query
float level = live.masterLevel();
```

Bindings are stored in a vector that grows at setup time only. No heap allocation during dispatch. Unbound ids and type mismatches are silently ignored.

## MIDI Parsing

```cpp
bool parseMidi(const uint8_t* msg, size_t len, ControlEvent& out);
```

Converts 3-byte MIDI channel messages to `ControlEvent`:

| Status | Bytes | → ControlEvent | Notes |
|--------|-------|---|---|
| Note On `0x9n` | data1=note, data2=velocity | Button id=note, pressed=(velocity>0) | velocity 0 → released |
| Note Off `0x8n` | data1=note, data2=any | Button id=note, pressed=false | |
| Control Change `0xBn` | data1=CC#, data2=value | Fader id=128+CC#, value=data2/127.0 | |

- Channel nibble (low 4 bits of status) is ignored; all channels map alike.
- Returns `false` on buffer too short (<3 bytes) or unsupported status.
- Only handles standard 3-byte messages. SysEx, Running Status, and other extended formats are not supported.

## Example Workflow

1. **Bind controls at startup:**
   ```cpp
   LiveControl live(controller);
   live.bindButton(60, ActionKind::CueFlash, cueId1);
   live.bindButton(61, ActionKind::CueToggle, cueId2);
   live.bindFader(7, ActionKind::Master);
   ```

2. **MIDI transport receives bytes, parses, and dispatches:**
   ```cpp
   uint8_t midiMsg[] = {0x90, 60, 100};  // Note On, note 60, velocity 100
   ControlEvent ev;
   if (parseMidi(midiMsg, 3, ev)) {
     live.handle(ev, getCurrentTime());  // → calls go(cueId1, t)
   }
   ```

3. **Firmware reads master level in its render loop:**
   ```cpp
   float masterBrightness = live.masterLevel();  // [0,1]
   // Apply to output, e.g.: outputLevel = baseLevel * masterBrightness
   ```

## Out of Scope

- Web UI frontend (HTML/CSS/JS) — separate project
- Full OSC spec (bundles, all type tags, timetags) — address + one scalar arg only
- Bidirectional feedback (MIDI LED updates, web state reflection) — input-only for now
- Fader crossfaders for manual cue mixing — requires `setManualLevel(cueId, level)` on ShowController
- MIDI clock, beat sync, tap tempo — separate concern
- Modifying `ShowController` or other existing modules

## Testing

Tests are in `test_live_control.cpp`. Run:
```bash
make test
```

The test suite covers:
1. Button actions (Flash, Toggle)
2. Scene actions (Go, Toggle)
3. Master fader clamping
4. Unbound and type-mismatch events
5. MIDI parsing (Note On/Off, CC, channel masking, boundary cases)
6. End-to-end MIDI → dispatch → cue activation

All tests link against the real `ShowController` with proper fixture profiles and effect evaluation.
