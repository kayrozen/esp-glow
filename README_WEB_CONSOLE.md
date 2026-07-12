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

### Device → UI feedback (optional, Phase 4)
```json
{ "type": "state", "active": [0, 3, 5] }
```

## Phase 1 — `ws-client.js`

Pure ESM module. No framework, no dependencies. Exposes:

```js
import { WsClient, Status } from "./ws-client.js";

const client = new WsClient();  // defaults to ws://<page-host>/ws
client
  .onConfig((cfg) => { /* { cues, scenes, hasMaster } */ })
  .onState((activeIds) => { /* number[] */ })
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
parse/send/reconnect paths).

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
- **Optimistic local state**: the UI flips active/latched immediately. The
  device never echoes back for MVP. Phase-4 `state` messages, when wired,
  override local state.
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
the buffer; `web_input_build_state(activeIds, n, buf, bufLen)` does the
same for Phase-4 state pushes.

The Phase-2 console bundle (`web/console/**`) is served by a separate
`httpd` URI handler that maps `/` → `/spiffs/index.html` and so on. The
bundle is plain static files — no preprocessing, no gzip required (total
size ≈ 35 KB uncompressed including the vendored Preact).

## Out of scope

- **Manual cue crossfade** — would require `setManualLevel(cueId, level)`
  on `ShowController`; explicitly deferred to v2. The master fader is a
  grandmaster only.
- **Provisioning UI** — a separate, larger project (Phase 5). Not part of
  this MVP.
- **Build step / bundler** — intentionally absent. The bundle is small
  enough to serve from flash as-is.
