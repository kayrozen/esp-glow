# Web Console (Phase 1 + 2)

A browser-based cue console that pilots the ESP32-S3 live over WebSocket.
Served from the device's flash alongside the firmware. The UI is "dumb": it
emits button-press/release and fader-value events; the device's
`LiveControl` binding table decides flash-vs-toggle.

## Layout

```
web/
  console/
    index.html        # shell — loads styles.css and app.js
    styles.css        # venue-ready dark theme (large targets, no tap delay)
    app.js            # Preact + htm UI; data-driven from config
    ws-client.js      # Phase 1: connect, reconnect, parse/send
    vendor/
      preact.mjs      # vendored 10.24.0 (no build step)
      htm.mjs         # vendored 3.1.1
  dev-server.js       # mock device + static server for offline iteration
```

No build step. The bundle is plain static files. On-device (F4), they're
embedded into the app binary via `EMBED_FILES` (`firmware/main/CMakeLists.txt`)
and served with `httpd` — no filesystem partition, matching the raw `show`
partition's no-filesystem approach (see README_FIRMWARE.md).

## Protocol

Pinned in the project plan and implemented by `web_protocol.h/.cpp` on the
device side. WebSocket text frames, JSON.

### Device → UI on connect
```json
{ "type": "config",
  "cues":   [ { "id": 0, "label": "Blue wash", "color": "#3060ff", "mode": "toggle" } ],
  "scenes": [ { "id": 0, "label": "Verse" } ],
  "hasMaster": true }
```

`mode` is `"flash"` (momentary) or `"toggle"` (latching); derived on the
device from the binding's `ActionKind` (`CueFlash`/`SceneGo` → `"flash"`,
`CueToggle`/`SceneToggle` → `"toggle"`).

### UI → device
```json
{ "type": "cue",   "id": 0, "pressed": true }    // flash: down=true, up=false; toggle: send true only
{ "type": "scene", "id": 0, "pressed": true }
{ "type": "master","value": 0.5 }
{ "type": "hello" }                               // request config (auto on connect)
```

### Device → UI feedback (Phase 4 -- P1.3: the device's real push, source of truth)
```json
{ "type": "state", "active": [0, 3, 5], "master": 0.75 }
```

Broadcast from the render task, post-render, whenever the active-cue set or
master level actually changes (change-detection, same discipline
`led_feedback.h` uses for MIDI LED output -- a static show emits nothing
after its first push) and rate-limited to at most 20/sec. A newly-connected
client gets one unconditionally, bypassing both gates, so it never starts
out guessing. This is the SAME `ShowController::isActive`/`activeCueIds`
snapshot MIDI LED feedback reads -- firing a cue from any surface (this
console, another one, MIDI, OSC, a live-coded `glow.cue.go`) updates the
controller's pad LEDs and every connected console together, never one
without the other. See `main.cpp`'s `broadcast_state_if_changed`.

## Phase 1 — `ws-client.js`

Pure ESM module. No framework, no dependencies. Exposes:

```js
import { WsClient, Status } from "./ws-client.js";

const client = new WsClient();  // defaults to ws://<page-host>/ws
client
  .onConfig((cfg) => { /* { cues, scenes, hasMaster } */ })
  .onState((activeIds, masterLevel) => { /* number[], number|null -- P1.3 */ })
  .onStatus((status) => { /* "connecting" | "open" | "closed" | "error" */ })
  .connect();

client.sendCue(0, true);     // flash press
client.sendCue(0, false);    // flash release
client.sendScene(0, true);
client.sendMaster(0.5);
```

Features:
- Exponential backoff with full jitter (`backoffMinMs`/`backoffMaxMs` opts).
- Auto-sends `hello` on (re)connect.
- Validates id range (0..65535) and clamps master to `[0,1]` before sending.
- Normalizes incoming `config`/`state` so the UI never sees undefined arrays.
- Silently ignores malformed, non-JSON, non-text, and unknown-type frames.
- Single callback per channel; UI manages its own listener fan-out if needed.

Node tests: `node scripts/test-ws-client.mjs` (mocks `WebSocket`, exercises
parse/send/reconnect paths, including P1.3's `state`/`master` handling) and
`node web/console/test-reconcile.mjs` (app.js's `reconcileActiveCues`, the
optimistic-UI merge). Both run via `make test-console`.

## Phase 2 — `app.js` + `styles.css`

Preact + htm, no build. Rendered data-driven from the Phase-1 `onConfig`
callback. Layout:

```
┌──────────────────────────────────────────────────────────────┐
│ status indicator + URL                                       │
├──────────────────────────────────────────────────────────────┤
│ cue grid (flash = momentary, toggle = latched)               │
├──────────────────────────────────────────────────────────────┤
│ scene row (cue-set triggers)                                 │
├──────────────────────────────────────────────────────────────┤
│ master fader (grandmaster)                                   │
└──────────────────────────────────────────────────────────────┘
```

Interaction model mirrors `LiveControl` binding semantics:

| Mode   | Pointer down | Pointer up / leave / cancel | Keyboard |
|--------|--------------|------------------------------|----------|
| flash  | `sendCue(id, true)`;  local active = true  | `sendCue(id, false)`; local active = false | Enter/Space = momentary press |
| toggle | `sendCue(id, true)`;  local latched flips  | (no release sent)                          | Enter/Space = single press |

- **Pointer capture** on every cue button so a finger drag off the button
  still releases reliably on touch devices.
- **`touch-action: none`** on every interactive element to kill the 300ms
  tap delay and prevent the browser from hijacking the gesture as a scroll.
- **Optimistic local state, device-reconciled (P1.3)**: the UI flips
  active/latched immediately for zero-latency feedback (a tapped pad lights
  before the round trip completes), then the device's `state` push corrects
  it -- standard optimistic-UI reconciliation (`reconcileActiveCues` in
  `app.js`). Cue ids are authoritative from the device; scene highlights
  stay local-only (the device has no "active scene" concept to push -- a
  scene is just "go every member cue") and survive each reconcile
  untouched. Multiple consoles open at once all converge on the device's
  state.
- **Master fader** is drag-friendly (pointer capture, vertical or
  horizontal drag both work) and arrow-key navigable.

Visual design (venue-ready):
- Dark theme, high contrast, legible in low light.
- Active flash cues glow in their bound color with a soft halo.
- Latched toggle cues show a persistent left rail in their bound color so
  the operator can tell latched-on from held-on at a glance.
- Responsive: 2-3 columns on phones, 6-8 on tablets, capped at 1280px.
- `prefers-reduced-motion` respected.

## Local iteration

```bash
node web/dev-server.js              # http://localhost:8080/
node web/dev-server.js --port 3000  # custom port
node web/dev-server.js --no-echo    # disable Phase-4 state echo-back
```

The dev server hosts the static console bundle and a mock WebSocket at
`/ws` that:
- Sends the sample `config` on connect (8 cues, 3 scenes, master on).
- Logs every UI→device message to stdout.
- Optionally echoes a `state` message back when a toggle cue fires, so you
  can exercise the Phase-4 highlight path without real hardware.

Open `http://localhost:8080/?url=ws://localhost:8080/ws` to point the UI
at a non-default WebSocket URL.

## Device integration (Phase 3)

The device side is in `web_protocol.h/.cpp` (testable core, host-tested)
and `web_input.cpp` (device-only scaffold, `#ifdef ESP_PLATFORM`). The
core is wired into `make test` as `test_web_protocol`.

Setup pattern (device firmware):

```cpp
LiveControl live(showController);

// 1. Bind controls in LiveControl (as you already do).
live.bindButton(0, ActionKind::CueToggle, cueId0);
live.bindButton(4, ActionKind::CueFlash,  cueId4);
live.bindFader(0, ActionKind::Master);   // controlId 0 = grandmaster

// 2. Keep a parallel metadata array for the UI. Labels/colors here are
//    UI-only; LiveControl's binding table is the source of truth for
//    behavior.
static const WebCueInfo cues[] = {
  { 0, "Blue wash",  "#3060ff", ActionKind::CueToggle },
  { 4, "Solo spot",  "#ffdf30", ActionKind::CueFlash  },
};
static const WebSceneInfo scenes[] = {
  { 0, "Verse" },
};

// 3. Init the web input layer with borrowed pointers.
web_input_init(live, cues, /*nCues=*/2, scenes, /*nScenes=*/1,
               /*hasMaster=*/true);

// 4. Start the httpd ws server (TODO in web_input.cpp).
web_server_task();
```

`web_input_handle_text_frame(json, len, now)` is called per inbound
WebSocket text frame. It runs the testable parser, then:
- `cue`/`scene`/`master` → `LiveControl::handle(ev, now)`
- `hello` → caller responds by pushing `config` back to this client

`web_input_build_config(buf, bufLen)` serializes the current config into
the buffer; `web_input_build_state(activeIds, n, masterLevel, buf, bufLen)`
does the same for Phase-4 (P1.3) state pushes -- called from `main.cpp`'s
`broadcast_state_if_changed` (render task, post-render), which owns the
change-detection and rate-limiting, not `web_input.cpp` itself.
`web_input_note_new_client()`/`web_input_take_forced_state_broadcast()` are
the one bit of new cross-task signaling this needed: the WS handshake
handler (`ws_handler`'s `HTTP_GET` branch, off the render task) marks that
a client just connected with no idea what's active yet, and the render
task's next per-frame check sends a full `state` unconditionally, bypassing
its own change-detection, before going back to normal.

The Phase-2 console bundle (`web/console/**`) is served by a separate
`httpd` URI handler that maps `/` → `/spiffs/index.html` and so on. The
bundle is plain static files — no preprocessing, no gzip required (total
size ≈ 35 KB uncompressed including the vendored Preact).

## Fennel editor + REPL (Script tab)

A second tab (`web/console/script-panel.js`, mounted from `app.js`'s
`Header` tab switch) turns the console into a live-coding surface against
the running rig. It only works here — served same-origin from the device
over `ws://` — never in the HTTPS-served static provisioner; see
README_PROVISIONER.md's mixed-content note for why.

- **Editor**: CodeMirror 6 with bracket matching, auto-close, Parinfer
  (structural paren balancing on every edit), and Clojure-mode syntax
  highlighting (close enough for Fennel). Shared with the provisioner —
  `web/shared/fennel-editor.js`, bundled from `web/vendor/editor-bundle.mjs`
  (see `scripts/vendor_editor_bundle.sh`).
- **REPL**: multi-line input (Enter evaluates, Shift+Enter newlines), a
  scrolling transcript, and ↑/↓ history persisted to `localStorage`. Each
  eval carries a `seq` so an out-of-order `eval_result` (they're queued and
  drained on the render task — see `eval_queue.h`) still matches its input.
- **Script sidebar**: lists scripts from `script_list`; `boot` is badged —
  it's the one `scripts_storage_read_boot` runs at startup. Load/Save/New/
  Delete round-trip through `script_load`/`script_save`/`script_delete`.
- **Snippets**: a static, always-available panel of insertable forms for
  `glow.set`/`aim`/`cue.define`/`fx.*`/`matrix.pattern` — nobody memorizes
  an API from a README mid-set.
- **Safety UX**: `fx_error` (an unsolicited push when a `LuaEffect` throws
  and gets disabled — see `README_LUA_FENNEL.md`'s error policy) surfaces
  as a persistent banner naming the effect (`"<cue>#<index>"`, since Lua
  function values have no reliable name of their own —
  `GlowLuaApi::pollNewlyDisabledEffects`) plus an inline REPL transcript
  entry, not a quiet log line. A PANIC button is always reachable in the
  toolbar (client-orchestrated for v1: drops the master fader to 0 and logs
  it, using only the existing `master` message — no new device-side
  protocol). A cheap regex lint flags the two documented real-time
  footguns (string concatenation, `while` loops) as a hint, not a gate.

Protocol additions (`web_protocol.h/.cpp`, host-tested): `eval`/
`eval_result` (the `seq` field, not `id` — distinct from cue/scene ids),
`script_list`/`script_load`/`script_save`/`script_delete`/`scripts`/
`script`, and `fx_error`. Script CRUD hits `scripts_storage.h` (LittleFS)
synchronously on the WS task — unlike eval, it never touches the single
Lua VM, so it doesn't need the eval queue's single-owner discipline.

## Out of scope

- **Manual cue crossfade** — would require `setManualLevel(cueId, level)`
  on `ShowController`; explicitly deferred to v2. The master fader is a
  grandmaster only.
- **Auth on the device console** — it's a LAN device; noted, not solved.
- **Build step / bundler** for the console's own files — intentionally
  absent, small enough to serve from flash as-is. `web/vendor/*.mjs` are
  themselves committed *build outputs* of a one-time vendoring step (see
  `scripts/vendor_editor_bundle.sh`), same as `preact.mjs`/`htm.mjs`.
