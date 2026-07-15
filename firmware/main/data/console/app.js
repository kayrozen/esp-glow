//
// app.js — Phase 2 console UI.
//
// Rendered data-driven from the Phase-1 WsClient config callback. Vanilla
// ESM + Preact + htm; no build step. Served from ESP32 flash as static
// files alongside the vendor/ libs.
//
// Layout:
//   ┌──────────────────────────────────────────────────────────────┐
//   │ status indicator + URL                                       │
//   ├──────────────────────────────────────────────────────────────┤
//   │ cue grid (flash = momentary, toggle = latched)               │
//   ├──────────────────────────────────────────────────────────────┤
//   │ scene row (cue-set triggers)                                 │
//   ├──────────────────────────────────────────────────────────────┤
//   │ master fader (grandmaster)                                   │
//   └──────────────────────────────────────────────────────────────┘
//
// Interaction model (mirrors LiveControl binding semantics):
//   - flash cues: pointerdown -> sendCue(id, true), pointerup/cancel/leave
//     -> sendCue(id, false). Local state mirrors pressed for instant
//     visual feedback; device never echoes back.
//   - toggle cues: tap -> sendCue(id, true). Local state flips latched
//     optimistic; device never echoes back for MVP.
//   - scenes: same flash/toggle split as cues, dispatched through
//     LiveControl's SceneGo / SceneToggle bindings.
//   - master: pointer drag -> sendMaster(value) at ~30 Hz; clamped to
//     [0,1] on the client and again on the device.
//
// Reconnection: handled by WsClient. On reconnect, WsClient auto-sends
// `hello` and the device re-pushes `config`. Local latched state is
// preserved across reconnects so the operator doesn't see flicker.
//

import { h, render } from "./vendor/preact.mjs";
import { useState, useEffect, useRef, useCallback } from "./vendor/preact-hooks.mjs";
import htm from "./vendor/htm.mjs";
import { WsClient, Status } from "./ws-client.js";
import { ScriptPanel } from "./script-panel.js";

const html = htm.bind(h);

// --- tiny helpers --------------------------------------------------------

function classList(...parts) {
  return parts.filter(Boolean).join(" ");
}

// Active-cue set stored as a Set<number> for O(1) membership.
function toggleInSet(set, id) {
  const next = new Set(set);
  if (next.has(id)) next.delete(id);
  else next.add(id);
  return next;
}

// P1.3: reconciles the local (optimistic) active-cue set against a
// device-pushed `state` message. Exported as a pure function (no Preact,
// no DOM) so it's unit-testable on its own -- see test-reconcile.mjs.
//
// Cue ids (< SCENE_MARKER_BASE) are authoritative from the device: the
// returned set's cue-id membership is exactly `deviceIds`, full stop --
// this is what corrects a client whose optimistic guess drifted (dropped
// a WS message, another console/MIDI/OSC fired the same cue, etc.).
//
// Scene markers (id >= SCENE_MARKER_BASE, see onScenePointerDown) are
// local-only and preserved untouched: the device has no "active scene"
// concept to push in the first place (a scene is just "go every member
// cue" -- see SceneRow's comment), so a `state` message was never going
// to mention them, and wiping them on every reconcile would make scene
// highlighting flicker off on every unrelated cue-state change.
export const SCENE_MARKER_BASE = 0x10000;

export function reconcileActiveCues(prev, deviceIds) {
  const next = new Set(deviceIds);
  for (const id of prev) {
    if (id >= SCENE_MARKER_BASE) next.add(id);
  }
  return next;
}

// --- top-level App component --------------------------------------------

function App() {
  // Connection + protocol state.
  const [status, setStatus] = useState(Status.Closed);
  const [config, setConfig] = useState(null);  // null = no config yet
  const [tab, setTab] = useState("cues");  // "cues" | "script"
  // Per-cue local state. For flash cues: boolean "pressed". For toggle
  // cues: boolean "latched". Both stored under the same map keyed by id
  // so the cue grid can render a single "active" boolean per button.
  const [activeCues, setActiveCues] = useState(() => new Set());
  // Master value is local-only; the device clamps too.
  const [master, setMaster] = useState(1.0);

  // Refs that survive re-renders.
  const clientRef = useRef(null);
  if (clientRef.current === null) {
    // Allow ?url=ws://host:port/ws override for development; otherwise
    // WsClient defaults to ws://<page-host>/ws.
    const params = new URLSearchParams(location.search);
    const url = params.get("url") || undefined;
    clientRef.current = new WsClient(url);
  }
  const client = clientRef.current;

  // Wire WsClient callbacks once.
  useEffect(() => {
    client
      .onStatus((s) => setStatus(s))
      .onConfig((cfg) => {
        setConfig(cfg);
        // On a fresh config, drop any local latched state — the device's
        // binding table is authoritative about which cues exist.
        setActiveCues(new Set());
        if (typeof cfg.hasMaster === "boolean" && !cfg.hasMaster) {
          setMaster(1.0);
        }
      })
      .onState((ids, masterLevel) => {
        // P1.3: the device is source of truth. Standard optimistic-UI
        // reconciliation (reconcileActiveCues, above) -- a tapped pad
        // already lit locally (onCuePointerDown's optimistic setActiveCues)
        // the instant it was pressed; this push (arriving shortly after,
        // whether it originated from THIS console, another one, MIDI, OSC,
        // or a live-coded glow.cue.go) corrects any drift instead of
        // trusting the local guess forever.
        setActiveCues((prev) => reconcileActiveCues(prev, ids));
        if (masterLevel !== null) setMaster(masterLevel);
      });
    client.connect();
    return () => client.disconnect();
  }, [client]);

  // --- command senders (memoized; stable across renders) ---------------

  const sendCue = useCallback((id, pressed) => client.sendCue(id, pressed),
                              [client]);
  const sendScene = useCallback((id, pressed) => client.sendScene(id, pressed),
                                [client]);
  const sendMaster = useCallback((v) => client.sendMaster(v), [client]);

  // --- cue button handlers --------------------------------------------

  const onCuePointerDown = useCallback((cue) => {
    if (cue.mode === "flash") {
      setActiveCues((s) => new Set(s).add(cue.id));
      sendCue(cue.id, true);
    } else {
      // toggle: optimistic flip + send true only
      setActiveCues((s) => toggleInSet(s, cue.id));
      sendCue(cue.id, true);
    }
  }, [sendCue]);

  const onCuePointerUp = useCallback((cue) => {
    if (cue.mode === "flash") {
      setActiveCues((s) => {
        const next = new Set(s);
        next.delete(cue.id);
        return next;
      });
      sendCue(cue.id, false);
    }
    // toggle: no release event sent
  }, [sendCue]);

  const onScenePointerDown = useCallback((scene, mode) => {
    if (mode === "flash") {
      // Use the scene id as the local-active key; the device knows which
      // cues to fire. We don't try to track individual scene member cues
      // locally — the device's state push (Phase 4) is the source of truth.
      setActiveCues((s) => new Set(s).add(SCENE_MARKER_BASE + scene.id));
      sendScene(scene.id, true);
    } else {
      setActiveCues((s) => toggleInSet(s, SCENE_MARKER_BASE + scene.id));
      sendScene(scene.id, true);
    }
  }, [sendScene]);

  const onScenePointerUp = useCallback((scene, mode) => {
    if (mode === "flash") {
      setActiveCues((s) => {
        const next = new Set(s);
        next.delete(SCENE_MARKER_BASE + scene.id);
        return next;
      });
      sendScene(scene.id, false);
    }
  }, [sendScene]);

  // --- render -----------------------------------------------------------

  return html`
    <div class="console">
      <${Header} status=${status} hasConfig=${config !== null} tab=${tab} onTab=${setTab} />
      ${tab === "cues"
        ? (config === null
            ? html`<div class="empty">Waiting for config…</div>`
            : html`
                <${CueGrid} cues=${config.cues} activeCues=${activeCues}
                            onPointerDown=${onCuePointerDown}
                            onPointerUp=${onCuePointerUp} />
                ${config.scenes.length > 0 && html`
                  <${SceneRow} scenes=${config.scenes} activeCues=${activeCues}
                                onPointerDown=${onScenePointerDown}
                                onPointerUp=${onScenePointerUp} />
                `}
                ${config.hasMaster && html`
                  <${MasterFader} value=${master}
                                  onChange=${(v) => { setMaster(v); sendMaster(v); }} />
                `}
              `)
        : html`<${ScriptPanel} client=${client} status=${status} />`}
    </div>
  `;
}

// --- Header (status indicator) -----------------------------------------

function Header({ status, hasConfig, tab, onTab }) {
  const dotClass = classList(
    "status-dot",
    status === Status.Open       && "status-open",
    status === Status.Connecting && "status-connecting",
    status === Status.Closed     && "status-closed",
    status === Status.Error      && "status-error",
  );
  const label = status === Status.Open && hasConfig ? "Live"
              : status === Status.Open ? "No config"
              : status === Status.Connecting ? "Connecting…"
              : status === Status.Error ? "Error — retrying"
              : "Disconnected — retrying";
  return html`
    <header class="header">
      <span class=${dotClass}></span>
      <span class="status-label">${label}</span>
      <span class="brand">esp-glow</span>
      <nav class="tab-switch">
        <button class=${classList("tab-btn", tab === "cues" && "tab-btn-active")}
                onClick=${() => onTab("cues")}>Cues</button>
        <button class=${classList("tab-btn", tab === "script" && "tab-btn-active")}
                onClick=${() => onTab("script")}>Script</button>
      </nav>
    </header>
  `;
}

// --- CueGrid ------------------------------------------------------------

function CueGrid({ cues, activeCues, onPointerDown, onPointerUp }) {
  if (cues.length === 0) {
    return html`<section class="cue-grid empty-cues"><p>No cues bound.</p></section>`;
  }
  return html`
    <section class="cue-grid" style=${{ "--cue-count": cues.length }}>
      ${cues.map((cue) => html`
        <${CueButton} key=${cue.id} cue=${cue}
                      active=${activeCues.has(cue.id)}
                      onPointerDown=${() => onPointerDown(cue)}
                      onPointerUp=${() => onPointerUp(cue)} />
      `)}
    </section>
  `;
}

function CueButton({ cue, active, onPointerDown, onPointerUp }) {
  // pointerdown fires the press; pointerup / pointerleave / pointercancel
  // all release. We use pointer capture so a finger drag off the button
  // still releases reliably on touch devices.
  const handleDown = (e) => {
    e.currentTarget.setPointerCapture?.(e.pointerId);
    onPointerDown();
  };
  const handleUp = (e) => {
    e.currentTarget.releasePointerCapture?.(e.pointerId);
    onPointerUp();
  };
  // Keyboard: Enter/Space act as a momentary press for both modes; for
  // toggle we treat Enter as a single press (no held state).
  const handleKey = (e) => {
    if (e.key === "Enter" || e.key === " ") {
      e.preventDefault();
      onPointerDown();
      if (cue.mode === "flash") {
        // Release on keyup; for toggle it's a one-shot.
        const release = () => {
          onPointerUp();
          e.currentTarget.removeEventListener("keyup", release);
        };
        e.currentTarget.addEventListener("keyup", release);
      }
    }
  };

  const style = { "--cue-color": cue.color };
  const classes = classList(
    "cue-btn",
    `cue-${cue.mode}`,
    active && "cue-active",
  );
  return html`
    <button class=${classes} style=${style}
            onPointerDown=${handleDown}
            onPointerUp=${handleUp}
            onPointerLeave=${handleUp}
            onPointerCancel=${handleUp}
            onKeyDown=${handleKey}
            aria-pressed=${active ? "true" : "false"}
            aria-label=${cue.label}>
      <span class="cue-swatch" style=${{ background: cue.color }}></span>
      <span class="cue-label">${cue.label}</span>
      <span class="cue-mode">${cue.mode}</span>
    </button>
  `;
}

// --- SceneRow -----------------------------------------------------------

function SceneRow({ scenes, activeCues, onPointerDown, onPointerUp }) {
  // Scenes don't carry their own mode in the protocol; we treat them as
  // momentary (SceneGo) for v1. The device's binding table decides
  // flash-vs-toggle on its side; the UI's job is to send press/release
  // events. If the device wants toggle behavior, it ignores the release.
  const mode = "flash";
  return html`
    <section class="scene-row">
      <h2 class="section-title">Scenes</h2>
      <div class="scene-buttons">
        ${scenes.map((scene) => html`
          <button key=${scene.id}
                  class=${classList("scene-btn",
                                    activeCues.has(SCENE_MARKER_BASE + scene.id) && "scene-active")}
                  onPointerDown=${() => onPointerDown(scene, mode)}
                  onPointerUp=${() => onPointerUp(scene, mode)}
                  onPointerLeave=${() => onPointerUp(scene, mode)}
                  onPointerCancel=${() => onPointerUp(scene, mode)}
                  aria-label=${scene.label}>
            ${scene.label}
          </button>
        `)}
      </div>
    </section>
  `;
}

// --- MasterFader --------------------------------------------------------

function MasterFader({ value, onChange }) {
  const trackRef = useRef(null);
  const draggingRef = useRef(false);

  const valueFromEvent = useCallback((clientX) => {
    const el = trackRef.current;
    if (!el) return null;
    const rect = el.getBoundingClientRect();
    // Vertical fader: top = 1.0, bottom = 0.0. Drag-right also raises,
    // which helps on phones held in landscape.
    let v = 1.0 - (clientX - rect.left) / rect.width;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    return v;
  }, []);

  const handlePointerDown = (e) => {
    e.currentTarget.setPointerCapture?.(e.pointerId);
    draggingRef.current = true;
    const v = valueFromEvent(e.clientX);
    if (v !== null) onChange(v);
  };
  const handlePointerMove = (e) => {
    if (!draggingRef.current) return;
    const v = valueFromEvent(e.clientX);
    if (v !== null) onChange(v);
  };
  const handlePointerUp = (e) => {
    e.currentTarget.releasePointerCapture?.(e.pointerId);
    draggingRef.current = false;
  };

  // Render the fill as a horizontal proportion.
  const pct = Math.round(value * 100);

  return html`
    <section class="master">
      <h2 class="section-title">Master</h2>
      <div class="master-row">
        <div class="master-track"
             ref=${trackRef}
             role="slider"
             aria-valuemin="0"
             aria-valuemax="100"
             aria-valuenow=${pct}
             aria-label="Master fader"
             tabindex="0"
             onPointerDown=${handlePointerDown}
             onPointerMove=${handlePointerMove}
             onPointerUp=${handlePointerUp}
             onPointerCancel=${handlePointerUp}
             onKeyDown=${(e) => {
               if (e.key === "ArrowLeft" || e.key === "ArrowDown") {
                 e.preventDefault();
                 onChange(Math.max(0, value - 0.05));
               } else if (e.key === "ArrowRight" || e.key === "ArrowUp") {
                 e.preventDefault();
                 onChange(Math.min(1, value + 0.05));
               }
             }}>
          <div class="master-fill" style=${{ width: pct + "%" }}></div>
          <div class="master-thumb" style=${{ left: pct + "%" }}></div>
          <div class="master-value">${pct}</div>
        </div>
      </div>
    </section>
  `;
}

// --- mount --------------------------------------------------------------

// Guarded so this module can be imported from a plain Node test (see
// test-reconcile.mjs, which imports reconcileActiveCues/SCENE_MARKER_BASE
// above) without a DOM to mount into.
if (typeof document !== "undefined") {
  render(html`<${App} />`, document.getElementById("app"));
}
