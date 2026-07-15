//
// ws-client.js — Phase 1 WebSocket client + command layer.
//
// Pure ESM module, no framework, no dependencies. Connects to the device's
// WebSocket endpoint, reconnects with exponential backoff, parses incoming
// `config`/`state` JSON messages, and exposes typed send helpers for
// `cue`/`scene`/`master`/`hello` commands.
//
// The UI layer (app.js) registers callbacks via onConfig/onState/onStatus,
// then calls sendCue/sendScene/sendMaster. No visual code lives here.
//
// Protocol (pinned in the project plan, mirrored by web_protocol.cpp):
//
//   Device -> UI:
//     { type: "config", cues: [{id,label,color,mode}], scenes: [{id,label}], hasMaster }
//     { type: "state",  active: [0,3,5], master: 0.75 }
//     // P1.3: `state` is the device's source of truth for which cues are
//     // active and the current grandmaster level -- pushed on change, and
//     // in full to a client right after it connects. See app.js's onState
//     // handler for how the UI reconciles its own optimistic guesses
//     // against it.
//     { type: "eval_result", seq: 7, ok: true|false, err?: "..." }
//     { type: "scripts", names: ["boot","verse"] }
//     { type: "script",  name: "verse", src: "..." }
//     { type: "fx_error", effect: "verse#1", err: "..." }  // unsolicited
//
//   UI -> device:
//     { type: "cue",    id: 0, pressed: true }
//     { type: "scene",  id: 0, pressed: true }
//     { type: "master", value: 0.5 }
//     { type: "hello" }
//     { type: "eval", src: "(glow.cue.go :chorus)", seq: 7 }
//     { type: "script_list" }
//     { type: "script_load",   name: "verse" }
//     { type: "script_save",   name: "verse", src: "..." }
//     { type: "script_delete", name: "verse" }
//

const DEFAULT_URL = (() => {
  if (typeof location === "undefined") return "ws://localhost/";
  // Default to the same host as the page, default device port 80, /ws path.
  return `ws://${location.host}/ws`;
})();

// Status values surfaced via onStatus.
export const Status = Object.freeze({
  Connecting: "connecting",
  Open:       "open",
  Closed:     "closed",
  Error:      "error",
});

// Sentinel returned by parseFrame when a message is well-formed JSON but
// not one of the protocol's device->UI types. The UI ignores these.
const UNKNOWN_MSG = Symbol("unknown");

function parseFrame(data) {
  // The device sends text frames only. Binary frames are ignored.
  if (typeof data !== "string") return UNKNOWN_MSG;
  let msg;
  try {
    msg = JSON.parse(data);
  } catch {
    return UNKNOWN_MSG;
  }
  if (typeof msg !== "object" || msg === null) return UNKNOWN_MSG;
  if (typeof msg.type !== "string") return UNKNOWN_MSG;
  return msg;
}

function clamp01(v) {
  if (typeof v !== "number" || !isFinite(v)) return 0;
  if (v < 0) return 0;
  if (v > 1) return 1;
  return v;
}

export class WsClient {
  // `url` may be omitted; defaults to ws://<page-host>/ws.
  // `opts` is optional and may carry:
  //   backoffMinMs (default 250), backoffMaxMs (default 8000),
  //   pingSec (default 0 = disabled) — sends hello on connect, so no
  //   app-level ping is needed for v1.
  constructor(url, opts = {}) {
    this.url = url || DEFAULT_URL;
    this.opts = opts;
    this.ws = null;
    this.closedByUser = false;
    this.backoff = {
      min: opts.backoffMinMs ?? 250,
      max: opts.backoffMaxMs ?? 8000,
      cur: opts.backoffMinMs ?? 250,
    };
    this.reconnectTimer = null;

    // Callbacks. Single-callback per channel — UI layer manages its own
    // listener fan-out if needed.
    this.cb = {
      config:     null,  // (cfg)  -> void
      state:      null,  // (activeIds, masterLevel|null) -> void -- P1.3: masterLevel
                         // is null only if the device omitted the field entirely
                         // (never happens from a real device; defensive for hand-built test frames)
      status:     null,  // (status, info?) -> void
      evalResult: null,  // ({seq, ok, err}) -> void
      scripts:    null,  // (names: string[]) -> void
      script:     null,  // ({name, src}) -> void
      fxError:    null,  // ({effect, err}) -> void
    };
  }

  // --- callback registration --------------------------------------------

  onConfig(fn)     { this.cb.config     = fn; return this; }
  onState(fn)      { this.cb.state      = fn; return this; }
  onStatus(fn)     { this.cb.status     = fn; return this; }
  onEvalResult(fn) { this.cb.evalResult = fn; return this; }
  onScripts(fn)    { this.cb.scripts    = fn; return this; }
  onScript(fn)     { this.cb.script     = fn; return this; }
  onFxError(fn)    { this.cb.fxError    = fn; return this; }

  // --- lifecycle ---------------------------------------------------------

  connect() {
    if (this.ws && (this.ws.readyState === WebSocket.OPEN ||
                    this.ws.readyState === WebSocket.CONNECTING)) {
      return;
    }
    this.closedByUser = false;
    this._setStatus(Status.Connecting);
    try {
      this.ws = new WebSocket(this.url);
    } catch (err) {
      this._setStatus(Status.Error, String(err));
      this._scheduleReconnect();
      return;
    }
    this.ws.binaryType = "arraybuffer";

    this.ws.onopen = () => {
      this.backoff.cur = this.backoff.min;
      this._setStatus(Status.Open);
      // On (re)connect, request the current config from the device.
      this._send({ type: "hello" });
    };

    this.ws.onmessage = (ev) => this._onMessage(ev);

    this.ws.onerror = () => {
      // The browser gives no detail; the follow-up onclose handles reconnect.
      this._setStatus(Status.Error);
    };

    this.ws.onclose = () => {
      this._setStatus(Status.Closed);
      if (!this.closedByUser) this._scheduleReconnect();
    };
  }

  disconnect() {
    this.closedByUser = true;
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      try { this.ws.close(); } catch { /* ignore */ }
      this.ws = null;
    }
    this._setStatus(Status.Closed);
  }

  // --- send helpers ------------------------------------------------------

  // cue: flash -> send pressed=true on pointer down, pressed=false on up.
  //       toggle -> send pressed=true only on tap.
  sendCue(id, pressed) {
    if (!Number.isInteger(id) || id < 0 || id > 65535) return false;
    if (typeof pressed !== "boolean") return false;
    return this._send({ type: "cue", id, pressed });
  }

  sendScene(id, pressed) {
    if (!Number.isInteger(id) || id < 0 || id > 65535) return false;
    if (typeof pressed !== "boolean") return false;
    return this._send({ type: "scene", id, pressed });
  }

  sendMaster(value) {
    return this._send({ type: "master", value: clamp01(value) });
  }

  // Evaluates a Fennel form. `seq` is an opaque id the caller picks (e.g.
  // an increasing counter) so the eventual (asynchronous, possibly
  // out-of-order) eval_result can be matched back to this request.
  sendEval(src, seq) {
    if (typeof src !== "string") return false;
    return this._send({ type: "eval", src, seq });
  }

  sendScriptList() {
    return this._send({ type: "script_list" });
  }

  sendScriptLoad(name) {
    if (typeof name !== "string" || name.length === 0) return false;
    return this._send({ type: "script_load", name });
  }

  sendScriptSave(name, src) {
    if (typeof name !== "string" || name.length === 0) return false;
    if (typeof src !== "string") return false;
    return this._send({ type: "script_save", name, src });
  }

  sendScriptDelete(name) {
    if (typeof name !== "string" || name.length === 0) return false;
    return this._send({ type: "script_delete", name });
  }

  // --- internal ----------------------------------------------------------

  _send(obj) {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) return false;
    try {
      this.ws.send(JSON.stringify(obj));
      return true;
    } catch {
      return false;
    }
  }

  _onMessage(ev) {
    const msg = parseFrame(ev.data);
    if (msg === UNKNOWN_MSG) return;

    if (msg.type === "config") {
      // Normalize so the UI never sees undefined arrays.
      const cfg = {
        cues:    Array.isArray(msg.cues)    ? msg.cues.map(normalizeCue)    : [],
        scenes:  Array.isArray(msg.scenes)  ? msg.scenes.map(normalizeScene) : [],
        hasMaster: msg.hasMaster === true,
      };
      if (this.cb.config) this.cb.config(cfg);
      return;
    }
    if (msg.type === "state") {
      const active = Array.isArray(msg.active)
        ? msg.active.filter((x) => Number.isInteger(x) && x >= 0 && x <= 65535)
        : [];
      const master = typeof msg.master === "number" && isFinite(msg.master)
        ? clamp01(msg.master)
        : null;
      if (this.cb.state) this.cb.state(active, master);
      return;
    }
    if (msg.type === "eval_result") {
      if (this.cb.evalResult) {
        this.cb.evalResult({
          seq: Number.isInteger(msg.seq) ? msg.seq : 0,
          ok: msg.ok === true,
          err: typeof msg.err === "string" ? msg.err : null,
        });
      }
      return;
    }
    if (msg.type === "scripts") {
      const names = Array.isArray(msg.names) ? msg.names.filter((n) => typeof n === "string") : [];
      if (this.cb.scripts) this.cb.scripts(names);
      return;
    }
    if (msg.type === "script") {
      if (typeof msg.name !== "string") return;
      if (this.cb.script) this.cb.script({ name: msg.name, src: typeof msg.src === "string" ? msg.src : "" });
      return;
    }
    if (msg.type === "fx_error") {
      if (typeof msg.effect !== "string") return;
      if (this.cb.fxError) this.cb.fxError({ effect: msg.effect, err: typeof msg.err === "string" ? msg.err : "" });
      return;
    }
    // Unknown device->UI message — ignore silently.
  }

  _setStatus(status, info) {
    if (this.cb.status) this.cb.status(status, info);
  }

  _scheduleReconnect() {
    if (this.reconnectTimer !== null) return;
    const delay = this.backoff.cur;
    // Exponential backoff with full jitter on the next round.
    this.backoff.cur = Math.min(this.backoff.cur * 2, this.backoff.max);
    this.reconnectTimer = setTimeout(() => {
      this.reconnectTimer = null;
      this.connect();
    }, delay);
  }
}

function normalizeCue(c) {
  return {
    id:    Number.isInteger(c?.id) ? c.id : 0,
    label: typeof c?.label === "string" ? c.label : `Cue ${c?.id ?? 0}`,
    color: typeof c?.color === "string" ? c.color : "#3060ff",
    mode:  c?.mode === "flash" ? "flash" : "toggle",
  };
}

function normalizeScene(s) {
  return {
    id:    Number.isInteger(s?.id) ? s.id : 0,
    label: typeof s?.label === "string" ? s.label : `Scene ${s?.id ?? 0}`,
  };
}
