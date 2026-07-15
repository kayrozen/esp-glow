# Live Control Layer: MIDI/OSC/Web → Cues

The live control layer transforms incoming control input (MIDI, OSC, WebSocket/HTTP) into real-time cue and scene actions via the `ShowController`.

See also: **FORMAT.md**'s "MDF1 Controller Definition Format" section, which
describes MIDI *hardware* (pads/faders/encoders/LEDs) so that `glow.led.*`
(below) has something to address. `.mdef` never contains bindings -- those
stay here, in Fennel, live-editable per show (`glow.bind.*`).

`parseMidi` reports every message's MIDI channel (see "MIDI Parsing" below),
and `.mdef`'s per-range channel significance (FORMAT.md) plus
`glow.bind.pad-xy` let a channel-multiplexed controller like the Akai APC40
work correctly: all 40 clip-launch grid pads (`samples/apc40.mdef`,
`samples/apc40-original.mdef`) are independently bindable and independently
lit, even though they share only 5 MIDI note numbers -- the track is carried
on the channel nibble, not the note.

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
enum class ControlType : uint8_t { Button, Fader, Aftertouch, PitchBend, Program };

struct ControlEvent {
  ControlType type;
  uint16_t id;       // opaque logical id (0..65535) -- note/CC(+128)/program, see parseMidi
  uint8_t channel;   // 0..15, low nibble of the MIDI status byte. ALWAYS set by
                     // parseMidi; OSC/web sources always report 0 (no MIDI channel
                     // concept). Only consulted for binding/LED lookups on a
                     // controller whose .mdef marks the address channel-significant
                     // (FORMAT.md's "Per-Range Channel Significance") -- see
                     // LiveControl::effectiveId below.
  bool pressed;      // Button: true = down/on, false = up/off
  float value;       // Fader/Aftertouch/PitchBend: [0,1]
};
```

Events are type-checked during dispatch: a Button event sent to a Fader binding (or vice versa) is silently ignored. `Aftertouch`/`PitchBend`/`Program` events are parsed and queued like any other `ControlEvent`, but `LiveControl` has no built-in `ActionKind` binding for them yet -- they're reported for a future binding to consume (e.g. via `glow.bind.*`), not silently dropped at the parser.

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

## Fennel API: `glow.bind.*` / `glow.led.*`

`LiveControl::bindButton`/`bindFader`/`clear` are also reachable from Fennel
(see `glow_lua_api.h`), so a show's bindings can be defined live in
`boot.fnl` instead of hardcoded C++:

```fennel
(glow.bind.pad 53 :flash :chorus)      ; note 53 -> momentary cue (CueFlash)
(glow.bind.pad 54 :toggle :verse)      ; note 54 -> latching cue (CueToggle)
(glow.bind.pad-xy 0 0 :flash :chorus)  ; grid (col,row) -> (note,channel) via the
                                        ; wired .mdef's channel-significant PAD
                                        ; declarations (FORMAT.md's resolvePadXY) --
                                        ; the APC40's clip-launch grid, e.g.
(glow.bind.fader 48 :master)           ; CC 48 -> grandmaster level
(glow.bind.clear)                       ; wipe every binding (and every
                                         ; glow.led.auto tracker -- see below)
```

`glow.bind.pad`'s cue name must already be defined (`glow.cue.define`); an
unknown name is a Lua error, same as `glow.cue.go`. `glow.bind.fader` only
supports `:master` today (the only fader `ActionKind` `LiveControl` has).

`glow.bind.pad-xy` is a no-op (not an error) when there's no `.mdef`/LED
capability wired at all, or the `(col, row)` coordinate is out of range for
the loaded controller's declared grid -- same graceful-degradation contract
as `glow.led.*` below. It resolves through the same channel-significant PAD
ranges `LiveControl::effectiveId` uses to match incoming events, so a
`glow.bind.pad-xy`-bound cue and the physical pad it names always agree on
which packed `(channel << 8) | note` id to use -- this is what makes two
pads sharing a note (different tracks on the APC40's clip grid) independently
bindable instead of collapsing onto the same cue.

### LED feedback: `glow.led.*` (needs a `.mdef`)

```fennel
(glow.led.set 53 :red)                  ; by colour name, from the pad's
                                        ;  own LED range palette (.mdef)
(glow.led.set 53 :off)
(glow.led.auto 53 :chorus :green :off)  ; pad 53 tracks cue :chorus: green
                                        ;  while active, off while not
```

`glow.led.auto` is the one that makes a controller feel alive: register it
once per pad and it stays in sync with the show with no further scripting.
Internally it re-evaluates every render frame against `ShowController`
(`LedFeedback::refresh`, see `led_feedback.h`) and only ever sends MIDI when
a pad's colour actually changes -- a static show emits zero ongoing MIDI
traffic, and a burst of simultaneous changes (a scene cut touching 40 pads)
is rate-limited (default 100 msg/sec) rather than flooding the DIN link.

Both `glow.led.*` calls are **no-ops**, not errors, when:
- the device has no `LedFeedback` wired up at all (no `.mdef` was embedded
  in the SHW1 bundle, or the device build has no MIDI OUT transport), or
- the addressed note/CC has no `LED` range in the loaded `.mdef`, or
- the colour name isn't in that LED range's palette.

This is the same graceful-degradation contract as `glow.set` on a fixture
capability the patched fixture doesn't have: a typo or an unsupported
controller shouldn't take down the whole show.

## MIDI Parsing

```cpp
bool parseMidi(const uint8_t* msg, size_t len, ControlEvent& out);
```

Converts one complete, already-framed MIDI channel-voice message (status
byte `0xSn`, `n` = channel 0..15) to a `ControlEvent`. All seven
channel-voice message types are handled, each validated against its OWN
wire length (2 bytes for Program Change/Channel Pressure, 3 for everything
else) -- not a blanket "len < 3":

| Status | Bytes | → ControlEvent | Notes |
|--------|-------|---|---|
| Note On `0x9n` | data1=note, data2=velocity | Button id=note, channel=n, pressed=(velocity>0) | velocity 0 → released |
| Note Off `0x8n` | data1=note, data2=any | Button id=note, channel=n, pressed=false | |
| Poly Aftertouch `0xAn` | data1=note, data2=pressure | Aftertouch id=note, channel=n, value=pressure/127.0 | |
| Control Change `0xBn` | data1=CC#, data2=value | Fader id=128+CC#, channel=n, value=data2/127.0 | |
| Program Change `0xCn` | data1=program | Program id=program, channel=n | **2 bytes**, not 3 |
| Channel Pressure `0xDn` | data1=pressure | Aftertouch id=0, channel=n, value=pressure/127.0 | **2 bytes**, not 3 |
| Pitch Bend `0xEn` | data1=LSB, data2=MSB | PitchBend id=0, channel=n, value=((MSB\<\<7)\|LSB)/16383.0 | 14-bit; center 0x2000 → ~0.5 |

- `channel` (the status byte's low nibble) is **always** reported, for every
  message type -- whether a binding cares is a per-controller `.mdef`
  decision (`FORMAT.md`'s "Per-Range Channel Significance"), not `parseMidi`'s.
  Before this table existed, `parseMidi` discarded the channel entirely,
  which is why two APC40 pads sharing a note (different tracks) used to
  collapse onto the same binding -- see `glow.bind.pad-xy` above.
- Returns `false` when the buffer is shorter than the status's own required
  length (2 or 3 bytes, see above), or the status is `>= 0xF0`
  (System/Realtime -- not a channel-voice message at all; that's
  `MidiByteReader`'s job, `midi_realtime.h`).
- `msg` may be longer than the message needs (e.g. USB-MIDI's fixed 3-byte,
  zero-padded packets calling this with `len=3` even for a 2-byte Program
  Change) -- only *too short*, never *too long*, is rejected.
- Framing (running status, Realtime-byte interleaving) is out of scope here
  -- that's `MidiByteReader` (`midi_realtime.h`), which feeds this function
  a complete message.

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
- Bidirectional web state reflection — input-only for now (MIDI LED feedback
  IS implemented, see `glow.led.*` above and FORMAT.md's MDF1 section)
- Fader crossfaders for manual cue mixing — requires `setManualLevel(cueId, level)` on ShowController
- MIDI clock, beat sync, tap tempo — separate concern
- SysEx (device inquiry, the APC40's mode-switch handshake) — see FORMAT.md's
  MDF1 "Out of Scope" for the follow-up flag on the APC40 specifically.
- MPE, 14-bit CC (RPN/NRPN), running status (that's `MidiByteReader`,
  `midi_realtime.h`) — not this parser's job.
- `Aftertouch`/`PitchBend`/`Program` events are parsed but have no built-in
  `ActionKind`/binding yet (see "Control Types and Events" above) — a future
  `glow.bind.*` addition, not implemented here.
- Modifying `ShowController` or other existing modules
- USB-MIDI host — implemented (`usb_midi_input.cpp`), gated behind
  `CONFIG_GLOW_USB_MIDI_HOST` (Kconfig.projbuild), OFF by default. `.mdef`/
  LED feedback/MIDI OUT still only work over the DIN transport -- USB-MIDI
  is input-only here, structurally identical to `midi_input.cpp`: strip
  USB-MIDI event packets to raw MIDI bytes, call the existing host-tested
  `parseMidi`, push onto the same control queue. Nothing else. Enabling it
  is still a board decision, not just a firmware flag: USB host mode means
  the ESP32 must supply 5V VBUS to the controller (a USB-A receptacle, a
  power path able to source a few hundred mA, ideally with over-current
  protection) -- a board wired for DIN-MIDI only has no VBUS path to offer
  a controller. A USB-host-to-DIN adapter (~$20) remains the lower-risk
  option if that board change isn't worth it yet.

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
5. MIDI parsing: all seven channel-voice types, correct per-status lengths
   (2-byte Program Change/Channel Pressure accepted, not rejected), channel
   always reported, `status >= 0xF0` rejected, truncated buffers rejected
   with no OOB read
6. End-to-end MIDI → dispatch → cue activation
7. Channel-significant pads (a channel-multiplexed `.mdef` wired via
   `LiveControl::setControllerProfile`): same note + different channel fire
   independently -- the regression test for the bug this format addition
   fixes (`test_channel_significant_pads_fire_independently`)
8. `glow.bind.pad-xy` grid resolution and graceful degradation
   (`test_glow_lua_api.cpp`)

All tests link against the real `ShowController` with proper fixture profiles and effect evaluation.
