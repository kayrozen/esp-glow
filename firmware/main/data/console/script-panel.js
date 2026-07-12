//
// script-panel.js — the device console's live editor + REPL (Part A of the
// Fennel scripting UI plan). Talks to the device over the same WsClient
// instance app.js already owns; this module only adds UI, no new transport.
//
// Layout:
//   ┌───────────┬──────────────────────────────┬──────────────┐
//   │ scripts   │ editor (CodeMirror, shared    │ snippets     │
//   │ sidebar   │ with the provisioner) +        │ (collapsible)│
//   │           │ Load-into-rig / Save           │              │
//   ├───────────┴──────────────────────────────┴──────────────┤
//   │ fx_error / disabled-effects banner (always visible if any)│
//   ├────────────────────────────────────────────────────────┤
//   │ REPL transcript (scrolling)                               │
//   ├────────────────────────────────────────────────────────┤
//   │ REPL input (multi-line; Enter=eval, Shift+Enter=newline)  │
//   └────────────────────────────────────────────────────────┘
//

import { h } from "./vendor/preact.mjs";
import { useState, useEffect, useRef, useCallback } from "./vendor/preact-hooks.mjs";
import htm from "./vendor/htm.mjs";
import { createFennelEditor, setEditorDoc, getEditorDoc, lintFootguns } from "../shared/fennel-editor.js";

const html = htm.bind(h);

const HISTORY_KEY = "esp-glow-repl-history";
const HISTORY_MAX = 200;

function loadHistory() {
  try {
    const raw = localStorage.getItem(HISTORY_KEY);
    const arr = raw ? JSON.parse(raw) : [];
    return Array.isArray(arr) ? arr.filter((x) => typeof x === "string") : [];
  } catch {
    return [];
  }
}

function saveHistory(list) {
  try {
    localStorage.setItem(HISTORY_KEY, JSON.stringify(list.slice(-HISTORY_MAX)));
  } catch {
    // localStorage unavailable (private mode, quota) -- history just won't persist.
  }
}

// Snippets: cheap API discoverability (Part A4). Each inserts its `src` at
// the editor's cursor.
const SNIPPETS = [
  { label: "glow.set", src: "(glow.set 1 :dimmer 1.0)" },
  { label: "glow.aim (point)", src: "(glow.aim 1 [0 2 5])" },
  { label: "glow.cue.define", src: "(glow.cue.define :verse\n  {:effects [my-effect]\n   :fade-in 2.0 :fade-out 1.0 :priority 0})" },
  { label: "glow.cue.go / release", src: "(glow.cue.go :verse)\n(glow.cue.release :verse)" },
  { label: "glow.scene.define / go", src: "(glow.scene.define :chorus [:verse-wash :strobe-hit])\n(glow.scene.go :chorus)" },
  { label: "glow.fx.hue-rotate", src: "(glow.fx.hue-rotate [2 3] {:period 4.0})" },
  { label: "glow.fx.chase", src: "(glow.fx.chase [1 2 3] {:period 1.0})" },
  { label: "glow.fx.sweep", src: "(glow.fx.sweep [1 2 3] {:period 2.0})" },
  { label: "glow.matrix.pattern", src: "(glow.matrix.pattern 0 :plasma {:speed 0.5 :scale 0.2})" },
  { label: "glow.matrix.brightness", src: "(glow.matrix.brightness 0 0.8)" },
  { label: "effect skeleton", src: "(fn my-effect [t]\n  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2))))))" },
];

let seqCounter = 1;
function nextSeq() { return seqCounter++; }

function classList(...parts) { return parts.filter(Boolean).join(" "); }

export function ScriptPanel({ client, status }) {
  const [scripts, setScripts] = useState([]);
  const [currentName, setCurrentName] = useState(null);
  const [dirty, setDirty] = useState(false);
  const [lintWarnings, setLintWarnings] = useState([]);
  const [snippetsOpen, setSnippetsOpen] = useState(false);
  const [sidebarOpen, setSidebarOpen] = useState(true);

  const [transcript, setTranscript] = useState([]);  // {id, kind, text}
  const [inputValue, setInputValue] = useState("");
  const [history, setHistoryState] = useState(() => loadHistory());
  const [historyPos, setHistoryPos] = useState(null);  // index into history while browsing, null = not browsing

  // fx_error / disabled effects. Keyed by effect name so re-triggering the
  // same effect updates in place instead of piling up duplicates.
  const [disabledEffects, setDisabledEffects] = useState(() => new Map());

  const editorHostRef = useRef(null);
  const editorViewRef = useRef(null);
  const transcriptRef = useRef(null);
  const inputRef = useRef(null);
  const pendingSeqRef = useRef(new Map());  // seq -> transcript entry id

  // --- mount the shared CodeMirror editor once ---------------------------
  useEffect(() => {
    if (!editorHostRef.current) return undefined;
    const view = createFennelEditor({
      parent: editorHostRef.current,
      doc: "; select a script, or start typing\n",
      onChange: (text) => {
        setDirty(true);
        setLintWarnings(lintFootguns(text));
      },
    });
    editorViewRef.current = view;
    return () => view.destroy();
  }, []);

  // --- wire WsClient callbacks --------------------------------------------
  useEffect(() => {
    if (!client) return undefined;
    client.onScripts((names) => setScripts(names));
    client.onScript(({ name, src }) => {
      setCurrentName(name);
      setDirty(false);
      setLintWarnings(lintFootguns(src));
      if (editorViewRef.current) setEditorDoc(editorViewRef.current, src);
    });
    client.onEvalResult(({ seq, ok, err }) => {
      const entryId = pendingSeqRef.current.get(seq);
      pendingSeqRef.current.delete(seq);
      setTranscript((t) => t.map((e) => {
        if (entryId != null && e.id === entryId) {
          return { ...e, kind: ok ? "result-ok" : "result-err", text: ok ? "=> ok" : err || "(error)" };
        }
        return e;
      }));
    });
    client.onFxError(({ effect, err }) => {
      setDisabledEffects((m) => {
        const next = new Map(m);
        next.set(effect, err);
        return next;
      });
      setTranscript((t) => [...t, { id: `fx-${Date.now()}-${effect}`, kind: "fx-error", text: `${effect}: ${err}` }]);
    });
    // Ask for the script list up front so the sidebar isn't empty on load.
    client.sendScriptList();
    return undefined;
  }, [client]);

  // Auto-scroll the transcript to the newest entry.
  useEffect(() => {
    if (transcriptRef.current) {
      transcriptRef.current.scrollTop = transcriptRef.current.scrollHeight;
    }
  }, [transcript]);

  // --- actions -------------------------------------------------------------

  const evalSource = useCallback((src) => {
    if (!client || src.trim().length === 0) return;
    const seq = nextSeq();
    const inputId = `in-${seq}`;
    const pendingId = `pend-${seq}`;
    setTranscript((t) => [
      ...t,
      { id: inputId, kind: "input", text: src },
      { id: pendingId, kind: "pending", text: "…" },
    ]);
    pendingSeqRef.current.set(seq, pendingId);
    client.sendEval(src, seq);
  }, [client]);

  const submitReplInput = useCallback(() => {
    const src = inputValue;
    if (src.trim().length === 0) return;
    evalSource(src);
    setHistoryState((h) => {
      const next = [...h, src];
      saveHistory(next);
      return next;
    });
    setHistoryPos(null);
    setInputValue("");
  }, [inputValue, evalSource]);

  const handleReplKeyDown = useCallback((e) => {
    if (e.key === "Enter" && !e.shiftKey) {
      e.preventDefault();
      submitReplInput();
      return;
    }
    if (e.key === "ArrowUp" && e.currentTarget.selectionStart === 0) {
      e.preventDefault();
      setHistoryPos((pos) => {
        const nextPos = pos === null ? history.length - 1 : Math.max(0, pos - 1);
        if (history[nextPos] !== undefined) setInputValue(history[nextPos]);
        return nextPos;
      });
      return;
    }
    if (e.key === "ArrowDown" && e.currentTarget.selectionStart === e.currentTarget.value.length) {
      e.preventDefault();
      setHistoryPos((pos) => {
        if (pos === null) return null;
        const nextPos = pos + 1;
        if (nextPos >= history.length) {
          setInputValue("");
          return null;
        }
        setInputValue(history[nextPos]);
        return nextPos;
      });
    }
  }, [history, submitReplInput]);

  const loadIntoRig = useCallback(() => {
    if (!editorViewRef.current) return;
    evalSource(getEditorDoc(editorViewRef.current));
  }, [evalSource]);

  const saveScript = useCallback(() => {
    if (!editorViewRef.current || !client) return;
    const name = currentName || window.prompt("Save as (filename, e.g. verse.fnl):", "verse.fnl");
    if (!name) return;
    client.sendScriptSave(name, getEditorDoc(editorViewRef.current));
    setCurrentName(name);
    setDirty(false);
  }, [client, currentName]);

  const loadScript = useCallback((name) => {
    if (!client) return;
    client.sendScriptLoad(name);
  }, [client]);

  const deleteScript = useCallback((name) => {
    if (!client) return;
    if (!window.confirm(`Delete "${name}"? This cannot be undone.`)) return;
    client.sendScriptDelete(name);
    if (name === currentName) setCurrentName(null);
  }, [client, currentName]);

  const newScript = useCallback(() => {
    setCurrentName(null);
    setDirty(false);
    if (editorViewRef.current) setEditorDoc(editorViewRef.current, ";; new script\n");
  }, []);

  const insertSnippet = useCallback((src) => {
    const view = editorViewRef.current;
    if (!view) return;
    const pos = view.state.selection.main.head;
    view.dispatch({ changes: { from: pos, insert: (pos > 0 ? "\n" : "") + src + "\n" } });
    view.focus();
  }, []);

  const panic = useCallback(() => {
    if (!client) return;
    // Client-orchestrated blackout: drop the master fader to 0 (an
    // immediate, guaranteed-present kill) and let the operator know it
    // fired. Full per-cue release lives in the main cue console (Header
    // has no notion of the current cue/scene active set on this tab).
    client.sendMaster(0);
    setTranscript((t) => [...t, { id: `panic-${Date.now()}`, kind: "panic", text: "PANIC — master fader set to 0" }]);
  }, [client]);

  const dismissDisabled = useCallback((name) => {
    setDisabledEffects((m) => {
      const next = new Map(m);
      next.delete(name);
      return next;
    });
  }, []);

  const disabledList = Array.from(disabledEffects.entries());

  return html`
    <div class="script-panel">
      <div class="script-toolbar">
        <button class="sp-btn" onClick=${() => setSidebarOpen((v) => !v)}>${sidebarOpen ? "◀ scripts" : "▶ scripts"}</button>
        <span class="sp-title">${currentName || "(untitled)"}${dirty ? " •" : ""}</span>
        <button class="sp-btn sp-btn-primary" onClick=${loadIntoRig} disabled=${status !== "open"}>Load into rig</button>
        <button class="sp-btn" onClick=${saveScript} disabled=${status !== "open"}>Save</button>
        <button class="sp-btn" onClick=${newScript}>New</button>
        <button class="sp-btn" onClick=${() => setSnippetsOpen((v) => !v)}>${snippetsOpen ? "snippets ▲" : "snippets ▼"}</button>
        <button class="sp-btn sp-panic" onClick=${panic} disabled=${status !== "open"} title="Release / blackout">⏻ PANIC</button>
      </div>

      ${disabledList.length > 0 && html`
        <div class="fx-error-banner">
          ${disabledList.map(([name, err]) => html`
            <div class="fx-error-row" key=${name}>
              <strong>fx_error</strong> — effect <code>${name}</code> disabled: ${err}
              <button class="sp-btn sp-btn-tiny" onClick=${() => dismissDisabled(name)}>dismiss</button>
            </div>
          `)}
        </div>
      `}

      <div class="script-body">
        ${sidebarOpen && html`
          <div class="script-sidebar">
            ${scripts.length === 0 && html`<p class="sp-empty">No scripts on device.</p>`}
            <ul class="script-list">
              ${scripts.map((name) => html`
                <li key=${name} class=${classList("script-list-item", name === currentName && "script-list-item-active")}>
                  <button class=${classList("script-name-btn", name === "boot" && "script-name-boot")}
                          onClick=${() => loadScript(name)}>
                    ${name === "boot" && html`<span class="boot-badge" title="runs at startup">BOOT</span>`}
                    ${name}
                  </button>
                  <button class="sp-btn sp-btn-tiny" onClick=${() => deleteScript(name)}>✕</button>
                </li>
              `)}
            </ul>
          </div>
        `}

        <div class="script-editor-col">
          <div class="script-editor-host" ref=${editorHostRef}></div>
          ${lintWarnings.length > 0 && html`
            <div class="lint-hints">
              ${lintWarnings.map((w) => html`<div class="lint-hint" key=${w.line}>line ${w.line}: ${w.message}</div>`)}
            </div>
          `}
        </div>

        ${snippetsOpen && html`
          <div class="snippets-panel">
            <h3 class="section-title">Snippets</h3>
            ${SNIPPETS.map((s) => html`
              <button class="snippet-btn" key=${s.label} onClick=${() => insertSnippet(s.src)}>${s.label}</button>
            `)}
          </div>
        `}
      </div>

      <div class="repl">
        <div class="repl-transcript" ref=${transcriptRef}>
          ${transcript.map((entry) => html`
            <div key=${entry.id} class=${classList("repl-entry", `repl-entry-${entry.kind}`)}>
              ${entry.kind === "input" && html`<span class="repl-prompt">›</span>`}
              <pre class="repl-text">${entry.text}</pre>
            </div>
          `)}
        </div>
        <div class="repl-input-row">
          <textarea
            ref=${inputRef}
            class="repl-input"
            rows="2"
            placeholder="(glow.cue.go :chorus) — Enter to evaluate, Shift+Enter for a new line"
            value=${inputValue}
            onInput=${(e) => setInputValue(e.currentTarget.value)}
            onKeyDown=${handleReplKeyDown}
            disabled=${status !== "open"}
          ></textarea>
          <button class="sp-btn sp-btn-primary" onClick=${submitReplInput} disabled=${status !== "open"}>Eval</button>
        </div>
      </div>
    </div>
  `;
}
