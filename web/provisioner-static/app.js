//
// app.js — esp-glow provisioner (static editor).
//
// Plain ESM, no framework. Loads the WASM module from ./wasm/, manages
// a workspace (one .show + a library of .fdef files), and renders a
// three-pane editor: sidebar / textarea / preview+diagnostics.
//
// Mirrors the React version's behavior: debounced .fdef parse on every
// edit, explicit Compile button for the .show, download/copy/drag-drop.
//

import createModule from "./wasm/provision-wasm.js";
import { serialSupported, secureContextOk, flash as flashDevice } from "./flash.js";
import { createFennelEditor, getEditorDoc, lintFootguns } from "./shared/fennel-editor.js";
import { checkFennelSyntax } from "./fennel-check.js";
import { buildScriptsImage } from "./boot-image.js";
import { detectAndParse, listImportModes, buildImportModel, normalizeFixtureUrl, FORMAT_LABEL } from "./import.js";
import { emitFdef, fitRangeBudget, CAP_NAMES as IMPORT_CAP_NAMES } from "./shared/importers/model.js";

// --- tiny DOM helper ----------------------------------------------------

const $ = (sel, root = document) => root.querySelector(sel);
const el = (tag, attrs = {}, ...children) => {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === "class") e.className = v;
    else if (k === "html") e.innerHTML = v;
    else if (k.startsWith("on") && typeof v === "function") {
      e.addEventListener(k.slice(2).toLowerCase(), v);
    } else if (k === "disabled" || k === "checked" || k === "selected") {
      // Boolean attributes: only set when truthy, otherwise remove.
      if (v) e.setAttribute(k, "");
      else e.removeAttribute(k);
    } else if (v !== null && v !== undefined && v !== false) {
      e.setAttribute(k, v);
    }
  }
  for (const c of children) {
    if (c == null || c === false) continue;
    e.appendChild(typeof c === "string" ? document.createTextNode(c) : c);
  }
  return e;
};

// --- capability / transport name maps (mirror fixture_profile.h) -------

// Wire order matches the Capability enum in fixture_profile.h exactly
// (0..26, then Generic=255) -- this predates PFX2 and used to stop at Fan,
// silently mislabeling every v2 capability (ColorWheel..Macro) as
// "Unknown" in the .fdef preview/diagnostics pane.
const CAP_NAMES = [
  "Dimmer", "Red", "Green", "Blue", "White", "Amber", "Uv",
  "Cyan", "Magenta", "Yellow", "Pan", "Tilt",
  "ShutterStrobe", "Gobo", "Focus", "Zoom", "Fog", "Fan",
  "ColorWheel", "GoboRotation", "Prism", "PrismRotation", "Frost", "Iris", "CTO",
  "AnimationWheel", "Macro",
];
const CAP_VALUES = (() => {
  const m = { Generic: 255 };
  CAP_NAMES.forEach((n, i) => (m[n] = i));
  return m;
})();
function capNameFromValue(v) {
  if (v === 255) return "Generic";
  return CAP_NAMES[v] ?? "Unknown";
}
function transportName(v) {
  return ["Dmx", "ArtNet", "Sacn", "Unused"][v] ?? "Unused";
}

// --- vector conversion (emscripten typed-vector → JS array) -----------

function vecToArray(v) {
  if (v == null) return [];
  if (Array.isArray(v)) return v;
  if (typeof v.size === "function" && typeof v.get === "function") {
    const n = v.size();
    const out = new Array(n);
    for (let i = 0; i < n; i++) out[i] = v.get(i);
    return out;
  }
  return Array.from(v);
}
function vecToUint8(v) {
  if (v == null) return new Uint8Array(0);
  if (v instanceof Uint8Array) return v;
  if (typeof v.size === "function" && typeof v.get === "function") {
    const n = v.size();
    const out = new Uint8Array(n);
    for (let i = 0; i < n; i++) out[i] = v.get(i);
    return out;
  }
  return new Uint8Array(v);
}

// --- default workspace -------------------------------------------------

const DEFAULT_WORKSPACE = {
  show: {
    name: "my-show.show",
    text: `# esp-glow show definition
# Lines starting with # are comments. Tokens are whitespace-separated.

UNIVERSE 0 DMX
UNIVERSE 1 ARTNET

# Moving head on universe 0, base channel 1
FIXTURE torrent.fdef 0 1
POS 1.0 2.0 3.0
ROT 0 0 0

# Par fixture on universe 0, base channel 20
FIXTURE par.fdef 0 20

# 16x16 LED matrix on universe 1, starting at channel 0
MATRIX 1 0 16 16 SERP H GRB

# MIDI controller: embeds the APC40 mkII .mdef below into the bundle so the
# device can drive its pad/scene LEDs. Bindings (which pad triggers which cue)
# live in boot.fnl via glow.bind.* -- see README_LIVE_CONTROL.md.
CONTROLLER apc40.mdef
`,
  },
  boot: {
    name: "boot.fnl",
    text: `;; boot.fnl -- evaluated once at startup; authored here, baked into
;; the flash image's "scripts" partition by the Flash step below.
(fn breathe [t]
  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2))))))

(glow.cue.define :breathe {:effects [breathe] :priority 0})
(glow.cue.go :breathe)
`,
  },
  fdefs: [
    {
      name: "torrent.fdef",
      text: `# 16-channel moving head
FIXTURE Torrent F1
FOOTPRINT 16
HEAD
PANRANGE 540
TILTRANGE 270
CAP Dimmer 0
CAP Red 1
CAP Green 2
CAP Blue 3
CAP Pan 5 6
CAP Tilt 7 8
CAP ShutterStrobe 10 - 8 inv
`,
    },
    {
      name: "par.fdef",
      text: `# 5-channel RGB par
FIXTURE Par 5
FOOTPRINT 5
CAP Dimmer 0
CAP Red 1
CAP Green 2
CAP Blue 3
CAP White 4
`,
    },
  ],
  mdefs: [
    {
      name: "apc40.mdef",
      text: `# Akai APC40 mkII (USB MIDI clip launcher). Transcribed from Akai's
# official "APC40 Mk2 Communications Protocol" v1.2, Generic Mode. MDF1 is
# single-channel: the mkII multiplexes the track number onto the MIDI channel
# (parseMidi ignores it), so per-track controls collapse to one entry each.

CONTROLLER Akai APC40 mkII
MIDI_CHANNEL 0

# --- Pads (Note-on buttons) --------------------------------------------
PAD 0 39      # 8x5 RGB clip-launch grid (notes 0x00..0x27)
PAD 48 52     # per-track: record-arm, solo, activator, select, stop (0x30..0x34)
PAD 58 66     # device / bank / view + crossfader A/B (0x3A..0x42)
PAD 80 81     # master-select + stop-all-clips (0x50..0x51)
PAD 82 86     # scene launch 1..5, RGB (0x52..0x56)
PAD 87 103    # pan/sends/user/metronome/transport/nav/shift/tap/nudge (0x57..0x67)

# --- Faders / absolute knobs (CC) --------------------------------------
FADER CC 7   track      # 9 track faders (0x07, track = MIDI channel)
FADER CC 14  master     # master fader (0x0E)
FADER CC 15  crossfader # crossfader (0x0F)
FADER CC 16 23          # 8 device-control knobs (0x10..0x17)
FADER CC 48 55          # 8 track-control knobs (0x30..0x37)

# --- Relative encoders (CC) --------------------------------------------
ENCODER CC 13 relative-2c   # tempo knob (0x0D)
ENCODER CC 47 relative-2c   # cue level (0x2F)

# --- LED feedback ------------------------------------------------------
# RGB clip grid: velocity picks a colour from the mkII's 128-entry palette
# (representative subset). Blink/pulse variants use the MIDI channel, not
# velocity, so aren't encoded here.
LED NOTE 0 39 velocity
  COLOR off       0
  COLOR grey-dim  1
  COLOR grey      2
  COLOR white     3
  COLOR red       5
  COLOR orange    9
  COLOR yellow    13
  COLOR lime      17
  COLOR green     21
  COLOR spring    29
  COLOR aqua      33
  COLOR sky-blue  37
  COLOR blue      41
  COLOR indigo    45
  COLOR violet    49
  COLOR magenta   53
  COLOR pink      57
LED NOTE 82 86 velocity
  COLOR off   0
  COLOR white 3
  COLOR red   5
  COLOR green 21
  COLOR blue  41
LED NOTE 52 52 velocity
  COLOR off   0
  COLOR on    1
  COLOR blink 2
LED NOTE 66 66 velocity
  COLOR off    0
  COLOR yellow 1
  COLOR orange 2
`,
    },
    {
      name: "apc40-original.mdef",
      text: `# Akai APC40 (original 2009 model, not the mkII). Transcribed from Akai's
# "Generic Communication Protocol for Akai APC40" Rev 1 (2009), Generic Mode.
# The original's clip grid is five CLIP LAUNCH note rows (0x35..0x39) with the
# track chosen by MIDI channel, and a fixed green/red/yellow LED palette (with
# blink variants) rather than the mkII's RGB scheme. MDF1 is single-channel and
# parseMidi ignores the channel, so per-track duplicates collapse to one entry.

CONTROLLER Akai APC40
MIDI_CHANNEL 0

# --- Pads (Note-on buttons) --------------------------------------------
PAD 48 52     # per-track: record-arm, solo, activator, select, clip-stop (0x30..0x34)
PAD 53 57     # clip-launch rows 1..5 (0x35..0x39, track = MIDI channel)
PAD 58 65     # device / view / record-mode buttons (0x3A..0x41)
PAD 80 81     # master-select + stop-all-clips (0x50..0x51)
PAD 82 86     # scene launch 1..5 (0x52..0x56)
PAD 87 90     # pan + send A/B/C selectors (0x57..0x5A)
PAD 91 101    # transport + navigation (0x5B..0x65)

# --- Faders / absolute knobs (CC) --------------------------------------
FADER CC 7   track      # 8 track level faders (0x07, track = MIDI channel)
FADER CC 14  master     # master level (0x0E)
FADER CC 15  crossfader # crossfader (0x0F)
FADER CC 16 23          # 8 device-control knobs (0x10..0x17), absolute
FADER CC 48 55          # 8 track-control knobs (0x30..0x37), absolute

# --- Relative encoders (CC) --------------------------------------------
ENCODER CC 47 relative-2c   # cue level (0x2F, two's-complement deltas)

# --- LED feedback ------------------------------------------------------
LED NOTE 53 57 velocity
  COLOR off          0
  COLOR green        1
  COLOR green-blink  2
  COLOR red          3
  COLOR red-blink    4
  COLOR yellow       5
  COLOR yellow-blink 6
LED NOTE 52 52 velocity
  COLOR off   0
  COLOR on    1
  COLOR blink 2
LED NOTE 82 86 velocity
  COLOR off   0
  COLOR on    1
  COLOR blink 2
`,
    },
  ],
};

// --- app state ---------------------------------------------------------

const state = {
  workspace: structuredClone(DEFAULT_WORKSPACE),
  selection: { kind: "show" },  // {kind:"show"} | {kind:"fdef", name} | {kind:"mdef", name} | {kind:"boot"}
  diagnostics: {
    show: { ok: false, err: "Not compiled yet", bundleBytes: null, loaded: null },
    fdefs: {},
    mdefs: {},
    // boot.fnl: syntax-checked via the real Fennel compiler in-browser
    // (fennel-check.js), not the WASM show compiler above.
    boot: { checking: false, checked: false, ok: null, err: null },
  },
  wasmReady: false,
  compiling: false,
  module: null,
  compiledBundle: null,  // Uint8Array of the last successful compile, for flashing
  bootEditorView: null,  // lazily-created CodeMirror view; survives re-renders (see getBootEditorView)
  flash: {
    open: false,
    busy: false,
    done: false,
    error: null,
    chip: null,
    log: "",
    fileIndex: 0,
    fileCount: 0,
    progress: 0,
    eraseFirst: false,
    includeShow: true,
    includeBoot: false,
    baud: 460800,
  },
  // Fixture importer (QLC+/OFL/GDTF) -- see import.js for format dispatch,
  // web/shared/importers/ for the actual parsing + Capability/range
  // mapping (shared with the Node test suite).
  import: {
    open: false,
    stage: "input",       // "input" | "mode" | "table"
    loading: false,
    error: null,
    urlText: "",
    format: null,         // "qlcplus" | "ofl" | "gdtf"
    fixtureLabel: "",      // manufacturer/model or name, for display
    parsed: null,          // format-specific parsed fixture
    modes: [],             // [{name, footprint}]
    modeName: null,
    model: null,           // current (possibly hand-edited) intermediate model -- FULL mapping, never trimmed (the channel table always shows everything the source defines)
    warnings: [],
    // fitRangeBudget() result for what's ACTUALLY about to be saved (see
    // regenerateImportFdefText). Empty on every real fixture this importer
    // has been tested against at PFX2's current MAX_RANGES=192/
    // MAX_RANGE_NAME_BLOB=2048 budget -- this is a last-resort safety net
    // for a pathological one, and it must never trim silently: rendered as
    // its own unmissable banner (renderImportModal), separate from the
    // softer informational `warnings` above, plus a per-range strike-
    // through in the channel table so the user sees exactly which named
    // slots wouldn't make it into the saved .fdef.
    budgetDropped: [],
    fdefName: "",
    fdefText: "",
    fdefDirty: false,      // true once the user hand-edits the preview textarea
  },
};

let parseTimer = null;

// --- main entry --------------------------------------------------------

async function main() {
  try {
    state.module = await createModule({ locateFile: (p) => "./wasm/" + p });
    state.wasmReady = true;
    // Initial parse of all .fdef and .mdef files.
    await reparseFdefs();
    await reparseMdefs();
    render();
  } catch (e) {
    console.error("main() failed:", e);
    document.getElementById("app").innerHTML =
      `<div style="padding:24px;color:#f48771;font-family:monospace">Failed to start: ${e.message ?? e}</div>`;
  }
}

// --- operations --------------------------------------------------------

async function parseFdef(text) {
  const r = state.module.parseFixtureDef(text);
  if (!r.ok) return { ok: false, err: r.err };
  const capTypes = vecToArray(r.capTypes);
  const capCoarse = vecToArray(r.capCoarse);
  const capFine = vecToArray(r.capFine);
  const capDefault = vecToArray(r.capDefault);
  const capFlags = vecToArray(r.capFlags);
  return {
    ok: true,
    def: {
      name: r.name,
      footprint: r.footprint,
      isHead: r.isHead,
      panRangeDeg: r.panRangeDeg,
      tiltRangeDeg: r.tiltRangeDeg,
      caps: capTypes.map((_, i) => ({
        capType: capTypes[i],
        coarse: capCoarse[i],
        fine: capFine[i],
        defaultValue: capDefault[i],
        inverted: (capFlags[i] & 1) !== 0,
      })),
    },
  };
}

async function compileShowNow() {
  if (state.compiling) return;
  state.compiling = true;
  render();
  try {
    const readFile = (path) => {
      const norm = path.replace(/^\.\//, "").replace(/^\//, "");
      const f = state.workspace.fdefs.find((x) => x.name === norm || x.name === path);
      if (f) return f.text;
      const m = state.workspace.mdefs.find((x) => x.name === norm || x.name === path);
      return m ? m.text : "";
    };
    const r = state.module.compileShow(state.workspace.show.text, readFile);
    if (!r.ok) {
      state.diagnostics.show = { ok: false, err: r.err, bundleBytes: null, loaded: null };
      toast("Compile failed", r.err, "err");
      render();
      return;
    }
    const bundle = vecToUint8(r.bundle);
    const loaded = state.module.loadShow(bundle);
    if (!loaded.ok) {
      state.diagnostics.show = {
        ok: false,
        err: `Compiled OK but loader rejected: ${loaded.err}`,
        bundleBytes: null,
        loaded: null,
      };
      render();
      return;
    }
    const summary = {
      universeCount: loaded.universeCount,
      transports: vecToArray(loaded.transports),
      fixtureCount: loaded.fixtureCount,
      matrixCount: loaded.matrixCount,
      fixtures: vecToArray(loaded.fixUniverse).map((universe, i) => ({
        universe,
        base: vecToArray(loaded.fixBase)[i],
        isHead: vecToArray(loaded.fixIsHead)[i],
      })),
      matrices: vecToArray(loaded.matWidth).map((width, i) => ({
        width,
        height: vecToArray(loaded.matHeight)[i],
        startUniverse: vecToArray(loaded.matStartUniverse)[i],
        startChannel: vecToArray(loaded.matStartChannel)[i],
      })),
    };
    state.diagnostics.show = {
      ok: true,
      err: "",
      bundleBytes: bundle.byteLength,
      loaded: summary,
    };
    state.compiledBundle = bundle;
    toast(
      "Compiled",
      `${bundle.byteLength}-byte SHW1 bundle (${summary.fixtureCount} fixtures, ${summary.matrixCount} matrices)`,
      "ok",
    );
  } catch (e) {
    toast("Compile error", String(e), "err");
  } finally {
    state.compiling = false;
    render();
  }
}

async function reparseFdefs() {
  const entries = await Promise.all(
    state.workspace.fdefs.map(async (f) => [f.name, await parseFdef(f.text)]),
  );
  for (const [name, r] of entries) {
    state.diagnostics.fdefs[name] = r.ok
      ? { ok: true, err: "", def: r.def }
      : { ok: false, err: r.err };
  }
}

// .mdef (MIDI controller) parse -- parity with parseFdef. The WASM
// parseController binding parses + encodes so we get the same live ok/err
// feedback and encoded MDF1 size the .show compiler would produce.
async function parseMdef(text) {
  const r = state.module.parseController(text);
  if (!r.ok) return { ok: false, err: r.err };
  return {
    ok: true,
    def: {
      name: r.name,
      midiChannel: r.midiChannel,
      padCount: r.padCount,
      faderCount: r.faderCount,
      encoderCount: r.encoderCount,
      ledCount: r.ledCount,
      colorCount: r.colorCount,
      blobBytes: r.blobBytes,
    },
  };
}

async function reparseMdefs() {
  const entries = await Promise.all(
    state.workspace.mdefs.map(async (f) => [f.name, await parseMdef(f.text)]),
  );
  for (const [name, r] of entries) {
    state.diagnostics.mdefs[name] = r.ok
      ? { ok: true, err: "", def: r.def }
      : { ok: false, err: r.err };
  }
}

// --- file operations ---------------------------------------------------

function currentText() {
  if (state.selection.kind === "show") return state.workspace.show.text;
  if (state.selection.kind === "boot") return state.workspace.boot.text;
  if (state.selection.kind === "mdef") {
    const m = state.workspace.mdefs.find((x) => x.name === state.selection.name);
    return m ? m.text : "";
  }
  const f = state.workspace.fdefs.find((x) => x.name === state.selection.name);
  return f ? f.text : "";
}
function currentName() {
  if (state.selection.kind === "show") return state.workspace.show.name;
  if (state.selection.kind === "boot") return state.workspace.boot.name;
  return state.selection.name;
}

function setText(text) {
  if (state.selection.kind === "show") {
    state.workspace.show.text = text;
  } else if (state.selection.kind === "boot") {
    state.workspace.boot.text = text;
  } else if (state.selection.kind === "mdef") {
    const m = state.workspace.mdefs.find((x) => x.name === state.selection.name);
    if (m) m.text = text;
  } else {
    const f = state.workspace.fdefs.find((x) => x.name === state.selection.name);
    if (f) f.text = text;
  }
  // Debounced re-parse of the edited .fdef.
  if (state.selection.kind === "fdef") {
    clearTimeout(parseTimer);
    parseTimer = setTimeout(async () => {
      const r = await parseFdef(currentText());
      state.diagnostics.fdefs[state.selection.name] = r.ok
        ? { ok: true, err: "", def: r.def }
        : { ok: false, err: r.err };
      render();
    }, 250);
  }
  // Debounced re-parse of the edited .mdef.
  if (state.selection.kind === "mdef") {
    clearTimeout(parseTimer);
    parseTimer = setTimeout(async () => {
      const r = await parseMdef(currentText());
      state.diagnostics.mdefs[state.selection.name] = r.ok
        ? { ok: true, err: "", def: r.def }
        : { ok: false, err: r.err };
      render();
    }, 250);
  }
}

// --- boot.fnl: CodeMirror editor (persists across render()'s DOM churn) --

function getBootEditorView() {
  if (!state.bootEditorView) {
    state.bootEditorView = createFennelEditor({
      parent: document.createElement("div"),  // detached; renderBootEditorPane moves .dom into place
      doc: state.workspace.boot.text,
      onChange: (text) => {
        state.workspace.boot.text = text;
        state.diagnostics.boot.checked = false;  // stale after an edit
      },
    });
  }
  return state.bootEditorView;
}

async function checkBootSyntax() {
  state.diagnostics.boot.checking = true;
  render();
  const result = await checkFennelSyntax(getEditorDoc(getBootEditorView()));
  state.diagnostics.boot.checking = false;
  state.diagnostics.boot.checked = true;
  state.diagnostics.boot.ok = result.ok;
  state.diagnostics.boot.err = result.ok ? null : result.err;
  render();
}

function downloadBlob(filename, mime, data) {
  const blob = new Blob(
    [data instanceof Uint8Array ? data : new TextEncoder().encode(data)],
    { type: mime },
  );
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}

const IMPORT_FORMAT_EXTENSIONS = [".qxf", ".gdtf", ".json"];

function handleFiles(files) {
  const arr = Array.from(files);
  // A dropped/selected QLC+/GDTF/OFL file goes through the importer (it
  // needs a mode picked and a mapping review, not a straight text load)
  // rather than being silently ignored by the .show/.fdef branches below.
  // One at a time, matching every other importer entry point.
  const importable = arr.find((f) => IMPORT_FORMAT_EXTENSIONS.some((ext) => f.name.toLowerCase().endsWith(ext)));
  if (importable) {
    openImportModal();
    importFromFile(importable);
    return;
  }
  let i = 0;
  const reader = new FileReader();
  const next = () => {
    if (i >= arr.length) {
      toast("Imported", `${arr.length} file${arr.length === 1 ? "" : "s"}`, "ok");
      render();
      return;
    }
    const file = arr[i++];
    reader.onload = () => {
      const text = typeof reader.result === "string" ? reader.result : "";
      const lower = file.name.toLowerCase();
      if (lower.endsWith(".show")) {
        state.workspace.show = { name: file.name, text };
      } else if (lower.endsWith(".fdef")) {
        const others = state.workspace.fdefs.filter((f) => f.name !== file.name);
        state.workspace.fdefs = [...others, { name: file.name, text }];
      } else if (lower.endsWith(".mdef")) {
        const others = state.workspace.mdefs.filter((f) => f.name !== file.name);
        state.workspace.mdefs = [...others, { name: file.name, text }];
        reparseMdefs();
      }
      next();
    };
    reader.readAsText(file);
  };
  next();
}

async function copyText(text) {
  try {
    await navigator.clipboard.writeText(text);
    toast("Copied", "", "ok");
  } catch {
    toast("Copy failed", "", "err");
  }
}

// --- toast -------------------------------------------------------------

let toastTimer = null;
function toast(title, desc, kind = "") {
  clearTimeout(toastTimer);
  const t = $("#toast");
  if (t) t.remove();
  const node = el(
    "div",
    { id: "toast", class: `toast ${kind}` },
    el("div", { class: "title" }, title),
    desc && el("div", { class: "desc" }, desc),
  );
  document.body.appendChild(node);
  toastTimer = setTimeout(() => node.remove(), 3000);
}

// --- icons (inline SVG) ------------------------------------------------

const ICON = {
  file: `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><polyline points="14 2 14 8 20 8"/></svg>`,
  ok: `<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>`,
  err: `<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/></svg>`,
  copy: `<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="9" y="9" width="13" height="13" rx="2" ry="2"/><path d="M5 15H4a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2h9a2 2 0 0 1 2 2v1"/></svg>`,
  download: `<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg>`,
  package: `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="16.5" y1="9.4" x2="7.5" y2="4.21"/><path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><polyline points="3.27 6.96 12 12.01 20.73 6.96"/><line x1="12" y1="22.08" x2="12" y2="12"/></svg>`,
  plus: `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="12" y1="5" x2="12" y2="19"/><line x1="5" y1="12" x2="19" y2="12"/></svg>`,
  upload: `<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><polyline points="17 8 12 3 7 8"/><line x1="12" y1="3" x2="12" y2="15"/></svg>`,
  spin: `<svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" class="spin"><path d="M21 12a9 9 0 1 1-6.219-8.56"/></svg>`,
  zap: `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2"/></svg>`,
  close: `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>`,
};
function icon(name, extraClass = "") {
  return el("span", { class: `icon ${extraClass}`, html: ICON[name] });
}

// --- render ------------------------------------------------------------

function render() {
  const app = $("#app");
  app.innerHTML = "";
  app.appendChild(renderConsole());
  if (state.flash.open) app.appendChild(renderFlashModal());
  if (state.import.open) app.appendChild(renderImportModal());
}

function renderConsole() {
  return el(
    "div",
    { class: "console" },
    renderTopbar(),
    el(
      "div",
      { class: "body" },
      renderSidebar(),
      renderEditorPane(),
      renderPreviewPane(),
    ),
  );
}

function renderTopbar() {
  return el(
    "div",
    { class: "topbar" },
    icon("package"),
    el("span", { class: "title" }, "esp-glow provisioner"),
    el(
      "span",
      { class: `badge ${state.wasmReady ? "badge-ok" : ""}` },
      state.wasmReady ? "wasm ready" : "loading wasm…",
    ),
    el("div", { class: "spacer" }),
    el(
      "button",
      {
        class: "btn btn-primary",
        onclick: compileShowNow,
        disabled: state.compiling || !state.wasmReady,
      },
      state.compiling ? icon("spin") : icon("package"),
      el("span", {}, "Compile"),
    ),
    el(
      "button",
      { class: "btn", onclick: () => $("#file-input").click() },
      icon("upload"),
      el("span", {}, "Import"),
    ),
    el(
      "button",
      { class: "btn", onclick: openFlashModal },
      icon("zap"),
      el("span", {}, "Flash device"),
    ),
    el("input", {
      id: "file-input",
      type: "file",
      accept: ".show,.fdef,.mdef,.qxf,.gdtf,.json",
      multiple: "",
      style: "display:none",
      onchange: (e) => {
        if (e.target.files.length > 0) handleFiles(e.target.files);
        e.target.value = "";
      },
    }),
  );
}

function renderSidebar() {
  const list = el("div", { class: "sidebar-list" });
  // Show file row
  list.appendChild(
    fileRow(
      state.workspace.show.name,
      state.selection.kind === "show",
      "show",
      state.diagnostics.show.ok,
      () => (state.selection = { kind: "show" }, render()),
      () => downloadBlob(state.workspace.show.name, "text/plain", state.workspace.show.text),
      () => copyText(state.workspace.show.text),
      null,
    ),
  );
  // Show script (boot.fnl) row -- the Fennel show, baked into the flash
  // image's scripts partition by the Flash step (B3).
  list.appendChild(el("div", { class: "sidebar-section" }, "Show script"));
  list.appendChild(
    fileRow(
      state.workspace.boot.name,
      state.selection.kind === "boot",
      "fnl",
      state.diagnostics.boot.checked ? state.diagnostics.boot.ok : null,
      () => (state.selection = { kind: "boot" }, render()),
      () => downloadBlob(state.workspace.boot.name, "text/plain", state.workspace.boot.text),
      () => copyText(state.workspace.boot.text),
      null,
    ),
  );

  // Separator + section header
  list.appendChild(el("div", { class: "sidebar-section" }, "Fixture library"));
  // Fdef rows
  for (const f of state.workspace.fdefs) {
    const d = state.diagnostics.fdefs[f.name];
    list.appendChild(
      fileRow(
        f.name,
        state.selection.kind === "fdef" && state.selection.name === f.name,
        "fdef",
        d?.ok,
        () => (state.selection = { kind: "fdef", name: f.name }, render()),
        () => downloadBlob(f.name, "text/plain", f.text),
        () => copyText(f.text),
        () => {
          state.workspace.fdefs = state.workspace.fdefs.filter((x) => x.name !== f.name);
          if (state.selection.kind === "fdef" && state.selection.name === f.name) {
            state.selection = { kind: "show" };
          }
          render();
        },
      ),
    );
  }
  if (state.workspace.fdefs.length === 0) {
    list.appendChild(
      el("div", { style: "padding: 8px 12px; color: var(--text-faint); font-style: italic;" }, "No fixtures. Click + to add one."),
    );
  }

  // Separator + section header for MIDI controllers (.mdef)
  list.appendChild(el("div", { class: "sidebar-section" }, "Controllers"));
  for (const m of state.workspace.mdefs) {
    const d = state.diagnostics.mdefs[m.name];
    list.appendChild(
      fileRow(
        m.name,
        state.selection.kind === "mdef" && state.selection.name === m.name,
        "mdef",
        d?.ok,
        () => (state.selection = { kind: "mdef", name: m.name }, render()),
        () => downloadBlob(m.name, "text/plain", m.text),
        () => copyText(m.text),
        () => {
          state.workspace.mdefs = state.workspace.mdefs.filter((x) => x.name !== m.name);
          if (state.selection.kind === "mdef" && state.selection.name === m.name) {
            state.selection = { kind: "show" };
          }
          render();
        },
      ),
    );
  }
  if (state.workspace.mdefs.length === 0) {
    list.appendChild(
      el("div", { style: "padding: 8px 12px; color: var(--text-faint); font-style: italic;" }, "No controllers. Click ♪ to add one."),
    );
  }

  const footer = el("div", { class: "sidebar-footer" });
  if (state.diagnostics.show.ok && state.diagnostics.show.bundleBytes != null) {
    footer.appendChild(el("span", { class: "ok" }, `Last bundle: ${state.diagnostics.show.bundleBytes} bytes`));
  } else {
    footer.appendChild(el("span", {}, "Click Compile to build the bundle."));
  }

  return el(
    "div",
    { class: "sidebar" },
    el(
      "div",
      { class: "sidebar-header" },
      el("span", { class: "label" }, "Files"),
      el(
        "button",
        {
          class: "btn btn-icon",
          title: "Import fixture (QLC+/OFL/GDTF)",
          onclick: openImportModal,
        },
        icon("upload"),
      ),
      el(
        "button",
        {
          class: "btn btn-icon",
          title: "New .fdef",
          onclick: () => {
            const name = `fixture-${state.workspace.fdefs.length + 1}.fdef`;
            state.workspace.fdefs.push({
              name,
              text: "FIXTURE New Fixture\nFOOTPRINT 1\nCAP Dimmer 0\n",
            });
            state.selection = { kind: "fdef", name };
            render();
          },
        },
        icon("plus"),
      ),
      el(
        "button",
        {
          class: "btn btn-icon",
          title: "New .mdef",
          onclick: () => {
            const name = `controller-${state.workspace.mdefs.length + 1}.mdef`;
            state.workspace.mdefs.push({
              name,
              text: "CONTROLLER New Controller\nMIDI_CHANNEL 0\nPAD 0 7\n",
            });
            state.selection = { kind: "mdef", name };
            reparseMdefs().then(render);
          },
        },
        "♪",
      ),
    ),
    list,
    footer,
  );
}

function fileRow(name, active, badge, ok, onClick, onDownload, onCopy, onDelete) {
  return el(
    "div",
    { class: `file-row ${active ? "active" : ""}`, onclick: onClick },
    el("span", { class: "file-icon", html: ICON.file }),
    el("span", { class: "file-name", title: name }, name),
    ok === true && el("span", { class: "status-icon ok", html: ICON.ok }),
    ok === false && el("span", { class: "status-icon err", html: ICON.err }),
    el(
      "span",
      { class: "actions" },
      el("button", { title: "Copy", onclick: (e) => { e.stopPropagation(); onCopy(); } }, icon("copy")),
      el("button", { title: "Download", onclick: (e) => { e.stopPropagation(); onDownload(); } }, icon("download")),
      onDelete && el("button", { class: "danger", title: "Delete", onclick: (e) => { e.stopPropagation(); onDelete(); } }, "×"),
    ),
    el("span", { class: "badge" }, badge),
  );
}

function renderEditorPane() {
  if (state.selection.kind === "boot") return renderBootEditorPane();

  const header = el(
    "div",
    { class: "editor-header" },
    el("span", { class: "file-icon", html: ICON.file }),
    el("span", {}, currentName()),
    el("span", { class: "badge" }, state.selection.kind === "show" ? ".show" : state.selection.kind === "mdef" ? ".mdef" : ".fdef"),
    el("div", { class: "spacer" }),
    el(
      "div",
      { class: "actions" },
      el("button", { title: "Copy contents", onclick: () => copyText(currentText()) }, icon("copy")),
      el(
        "button",
        {
          title: "Download file",
          onclick: () => {
            if (state.selection.kind === "show") {
              downloadBlob(state.workspace.show.name, "text/plain", state.workspace.show.text);
            } else if (state.selection.kind === "mdef") {
              const m = state.workspace.mdefs.find((x) => x.name === state.selection.name);
              if (m) downloadBlob(m.name, "text/plain", m.text);
            } else {
              const f = state.workspace.fdefs.find((x) => x.name === state.selection.name);
              if (f) downloadBlob(f.name, "text/plain", f.text);
            }
          },
        },
        icon("download"),
      ),
    ),
  );
  const textarea = el("textarea", {
    class: "editor-textarea",
    spellcheck: "false",
    oninput: (e) => setText(e.target.value),
  });
  textarea.value = currentText();
  return el("div", { class: "editor-pane" }, header, textarea);
}

// boot.fnl gets the shared CodeMirror editor (bracket matching, auto-close,
// Parinfer, Fennel-ish highlighting -- same component as the device
// console's REPL/editor, see web/shared/fennel-editor.js) instead of the
// plain textarea above, plus a "Check syntax" action running the real
// vendored Fennel compiler in-browser (fennel-check.js). This is a syntax
// check only -- glow.* is stubbed, not the real render loop -- and the UI
// says so explicitly rather than implying a dry run.
function renderBootEditorPane() {
  const view = getBootEditorView();
  const d = state.diagnostics.boot;

  const header = el(
    "div",
    { class: "editor-header" },
    el("span", { class: "file-icon", html: ICON.file }),
    el("span", {}, state.workspace.boot.name),
    el("span", { class: "badge" }, ".fnl"),
    el("div", { class: "spacer" }),
    el(
      "div",
      { class: "actions" },
      el(
        "button",
        { class: "btn", disabled: d.checking, onclick: checkBootSyntax },
        d.checking ? icon("spin") : icon("package"),
        el("span", {}, d.checking ? "Checking…" : "Check syntax"),
      ),
      el("button", { title: "Copy contents", onclick: () => copyText(getEditorDoc(view)) }, icon("copy")),
      el(
        "button",
        { title: "Download boot.fnl", onclick: () => downloadBlob(state.workspace.boot.name, "text/plain", getEditorDoc(view)) },
        icon("download"),
      ),
    ),
  );

  const host = el("div", { class: "script-editor-host boot-editor-host" });
  host.appendChild(view.dom);  // move the live view into this render's tree, never destroy/recreate it

  const panels = [];
  panels.push(
    el(
      "div",
      { class: "boot-scope-note" },
      "Syntax check only: this runs the real Fennel compiler and calls stubbed glow.* functions " +
        "once, so a compile error or a top-level typo (e.g. glow.st) is caught -- but argument " +
        "types, fixture ids, and anything inside a function that's never called at the top level " +
        "are not. It is not a dry run of the show.",
    ),
  );
  if (d.checked) {
    panels.push(
      d.ok
        ? el("div", { class: "boot-check-ok" }, icon("ok"), " No syntax errors found.")
        : el("div", { class: "boot-check-err" }, icon("err"), " ", d.err),
    );
  }
  const lints = lintFootguns(getEditorDoc(view));
  if (lints.length > 0) {
    panels.push(
      el(
        "div",
        { class: "lint-hints" },
        ...lints.map((w) => el("div", { class: "lint-hint" }, `line ${w.line}: ${w.message}`)),
      ),
    );
  }

  return el("div", { class: "editor-pane" }, header, host, ...panels);
}

function renderPreviewPane() {
  const activeTab = state._tab ?? "preview";
  const tabBtn = (name, label) =>
    el(
      "button",
      {
        class: `tab ${activeTab === name ? "active" : ""}`,
        onclick: () => { state._tab = name; render(); },
      },
      label,
    );

  const panel = el("div", { class: "tab-panel" });
  if (activeTab === "preview") {
    panel.appendChild(renderPreview());
  } else {
    panel.appendChild(renderDiagnostics());
  }

  const footer = el("div", { class: "preview-footer" });
  footer.appendChild(
    el(
      "button",
      {
        class: "btn btn-primary",
        disabled: !state.wasmReady,
        onclick: async () => {
          const readFile = (path) => {
            const norm = path.replace(/^\.\//, "").replace(/^\//, "");
            const f = state.workspace.fdefs.find((x) => x.name === norm || x.name === path);
            if (f) return f.text;
            const m = state.workspace.mdefs.find((x) => x.name === norm || x.name === path);
            return m ? m.text : "";
          };
          const r = state.module.compileShow(state.workspace.show.text, readFile);
          if (!r.ok) {
            toast("Compile failed", r.err, "err");
            return;
          }
          const bundle = vecToUint8(r.bundle);
          const baseName = state.workspace.show.name.replace(/\.show$/i, "");
          downloadBlob(`${baseName}.shw1`, "application/octet-stream", bundle);
        },
      },
      icon("download"),
      el("span", {}, "Download compiled .shw1"),
    ),
  );
  if (state.diagnostics.show.ok && state.diagnostics.show.bundleBytes != null) {
    footer.appendChild(
      el("div", { class: "bundle-info" }, `${state.diagnostics.show.bundleBytes}-byte SHW1 bundle ready`),
    );
  }

  return el(
    "div",
    { class: "preview-pane" },
    el("div", { class: "tabs" }, tabBtn("preview", "Preview"), tabBtn("diagnostics", "Diagnostics")),
    panel,
    footer,
  );
}

function renderPreview() {
  if (state.selection.kind === "show") {
    const d = state.diagnostics.show;
    if (!d.ok) {
      return errorBlock("Show not compiled", d.err, "Click Compile in the top bar.");
    }
    const s = d.loaded;
    const root = el("div", {});
    root.appendChild(section("Bundle", [el("div", {}, `${d.bundleBytes} bytes SHW1`)]));
    root.appendChild(
      section(`Universes (${s.universeCount})`, s.transports.map((t, i) =>
        row(`[${i}]`, transportName(t)),
      )),
    );
    root.appendChild(
      section(`Fixtures (${s.fixtureCount})`, s.fixtures.length === 0
        ? [el("div", { class: "preview-empty" }, "None")]
        : s.fixtures.map((f, i) => row(`[${i}]`, `u${f.universe} ch${f.base}${f.isHead ? " (head)" : ""}`)),
      ),
    );
    root.appendChild(
      section(`Matrices (${s.matrixCount})`, s.matrices.length === 0
        ? [el("div", { class: "preview-empty" }, "None")]
        : s.matrices.map((m, i) => row(`[${i}]`, `${m.width}×${m.height} @ u${m.startUniverse} ch${m.startChannel}`)),
      ),
    );
    return root;
  }
  // .mdef preview
  if (state.selection.kind === "mdef") {
    const dm = state.diagnostics.mdefs[state.selection.name];
    if (!dm || !dm.ok || !dm.def) {
      return errorBlock("Parse error", dm?.err ?? "Not parsed yet", "");
    }
    const md = dm.def;
    const root = el("div", {});
    root.appendChild(section("Controller", [
      el("div", {}, md.name),
      el("div", { style: "color: var(--text-dim);" }, `MIDI channel ${md.midiChannel} · ${md.blobBytes}-byte MDF1 blob`),
    ]));
    root.appendChild(section("Controls", [
      row("Pads", String(md.padCount)),
      row("Faders", String(md.faderCount)),
      row("Encoders", String(md.encoderCount)),
      row("LED ranges", String(md.ledCount)),
      row("Colors", String(md.colorCount)),
    ]));
    return root;
  }
  // .fdef preview
  const d = state.diagnostics.fdefs[state.selection.name];
  if (!d || !d.ok || !d.def) {
    return errorBlock("Parse error", d?.err ?? "Not parsed yet", "");
  }
  const def = d.def;
  const root = el("div", {});
  root.appendChild(section("Fixture", [
    el("div", {}, def.name),
    el("div", { style: "color: var(--text-dim);" }, `${def.footprint} channels${def.isHead ? " · moving head" : ""}`),
    def.isHead && el("div", { style: "color: var(--text-dim);" }, `pan ${def.panRangeDeg}° · tilt ${def.tiltRangeDeg}°`),
  ]));
  root.appendChild(
    section(`Capabilities (${def.caps.length})`, def.caps.length === 0
      ? [el("div", { class: "preview-empty" }, "No caps")]
      : def.caps.map((c) => row(
          capNameFromValue(c.capType),
          `ch${c.coarse}${c.fine !== 0xff ? `+${c.fine}` : ""}${c.defaultValue !== 0 ? ` def=${c.defaultValue}` : ""}${c.inverted ? " inv" : ""}`,
        )),
    ),
  );
  return root;
}

function renderDiagnostics() {
  const root = el("div", {});
  root.appendChild(diagRow(
    state.workspace.show.name,
    state.diagnostics.show.ok,
    state.diagnostics.show.err,
    state.diagnostics.show.ok && state.diagnostics.show.bundleBytes != null
      ? `${state.diagnostics.show.bundleBytes}-byte bundle`
      : null,
  ));
  for (const f of state.workspace.fdefs) {
    const d = state.diagnostics.fdefs[f.name];
    root.appendChild(diagRow(
      f.name,
      d?.ok,
      d?.err,
      d?.ok ? `${d.def.caps.length} caps, footprint ${d.def.footprint}` : null,
    ));
  }
  for (const m of state.workspace.mdefs) {
    const d = state.diagnostics.mdefs[m.name];
    root.appendChild(diagRow(
      m.name,
      d?.ok,
      d?.err,
      d?.ok ? `${d.def.padCount} pads, ${d.def.faderCount} faders, ${d.def.blobBytes}-byte blob` : null,
    ));
  }
  return root;
}

function section(title, children) {
  return el(
    "div",
    { class: "preview-section" },
    el("div", { class: "section-title" }, title),
    el("div", { class: "section-content" }, ...children),
  );
}
function row(key, val) {
  return el("div", { class: "preview-row" }, el("span", { class: "key" }, key), el("span", { class: "val" }, val));
}
function errorBlock(title, detail, hint) {
  return el(
    "div",
    { class: "preview-error" },
    el("span", { class: "icon err", html: ICON.err }),
    el(
      "div",
      {},
      el("div", { class: "title" }, title),
      el("div", { class: "detail" }, detail),
      hint && el("div", { class: "hint" }, hint),
    ),
  );
}
function diagRow(label, ok, err, extra) {
  return el(
    "div",
    { class: "diag-row" },
    ok === true && el("span", { class: "icon ok", html: ICON.ok }),
    ok === false && el("span", { class: "icon err", html: ICON.err }),
    ok === undefined && el("span", { class: "icon" }),
    el(
      "div",
      {},
      el("div", { class: "label" }, label),
      ok === false && err && el("div", { class: "detail" }, err),
      ok === true && extra && el("div", { class: "extra" }, extra),
    ),
  );
}

// --- fixture importer modal (QLC+ / OFL / GDTF) -------------------------
//
// Three stages: pick a file/URL -> pick a mode (mandatory, no default --
// a fixture patched in the wrong mode is completely mispatched) -> review
// the channel table and .fdef preview, both editable, before saving into
// the fixture library. See import.js + web/shared/importers/ for the
// actual parsing/mapping.

function openImportModal() {
  state.import = {
    ...state.import,
    open: true, stage: "input", loading: false, error: null, urlText: "",
    format: null, fixtureLabel: "", parsed: null, modes: [], modeName: null,
    model: null, warnings: [], budgetDropped: [], fdefName: "", fdefText: "", fdefDirty: false,
  };
  render();
}

function closeImportModal() {
  if (state.import.loading) return;
  state.import.open = false;
  render();
}

async function importFromBytes(name, bytes) {
  const im = state.import;
  im.loading = true;
  im.error = null;
  render();
  try {
    const { format, parsed } = await detectAndParse(name, bytes);
    const modes = listImportModes(format, parsed);
    im.format = format;
    im.parsed = parsed;
    im.modes = modes;
    im.fixtureLabel = `${FORMAT_LABEL[format]} fixture -- ${name}`;
    im.stage = "mode";
    if (modes.length === 1) {
      // Still an explicit choice, just a trivial one: pre-fill the
      // dropdown's value but leave the user to press Continue (no
      // auto-advance -- "never auto-pick" applies even when there's only
      // one option, since a future re-import of an updated file could add
      // modes).
      im.modeName = modes[0].name;
    }
  } catch (e) {
    im.error = e && e.message ? e.message : String(e);
  } finally {
    im.loading = false;
    render();
  }
}

function importFromFile(file) {
  const reader = new FileReader();
  reader.onload = () => importFromBytes(file.name, new Uint8Array(reader.result));
  reader.onerror = () => {
    state.import.error = `Couldn't read ${file.name}`;
    render();
  };
  reader.readAsArrayBuffer(file);
}

async function importFromUrl() {
  const im = state.import;
  const url = normalizeFixtureUrl(im.urlText);
  if (!url) return;
  im.loading = true;
  im.error = null;
  render();
  try {
    const resp = await fetch(url);
    if (!resp.ok) throw new Error(`fetch failed: HTTP ${resp.status}`);
    const bytes = new Uint8Array(await resp.arrayBuffer());
    const name = url.split("/").pop() || url;
    im.loading = false;
    await importFromBytes(name, bytes);
  } catch (e) {
    im.loading = false;
    im.error = `${e && e.message ? e.message : String(e)} -- if this is a CORS/network problem, download the file and drop it in instead.`;
    render();
  }
}

function importSelectMode(modeName) {
  const im = state.import;
  im.modeName = modeName || null;
  if (!modeName) return render();
  try {
    const { model, warnings } = buildImportModel(im.format, im.parsed, modeName);
    im.model = model;
    im.warnings = warnings;
    im.fdefName = suggestFdefName(model.name);
    im.fdefDirty = false;
    regenerateImportFdefText();
    im.stage = "table";
  } catch (e) {
    im.error = e && e.message ? e.message : String(e);
  }
  render();
}

function suggestFdefName(fixtureName) {
  const slug = (fixtureName || "fixture")
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, "-")
    .replace(/^-+|-+$/g, "") || "fixture";
  return `${slug}.fdef`;
}

// Recomputes the .fdef preview text from the current (possibly
// hand-edited-in-the-table) model. A no-op once the user has typed
// directly into the preview textarea (fdefDirty) -- their raw edits win
// until they explicitly ask to regenerate.
//
// The table (im.model) always shows the FULL mapping; what's emitted here
// is run through fitRangeBudget first, because that's what actually gets
// saved and compiled. On every real fixture this is a no-op (PFX2's
// budget is sized for real fixtures now -- see fixture_profile.h), but if
// it ever does trim, im.budgetDropped must be populated so the "table"
// stage's warning banner and the per-range strikethrough can show exactly
// what got cut -- never a silent loss.
function regenerateImportFdefText() {
  const im = state.import;
  if (!im.model) return;
  try {
    const { model: fitted, dropped } = fitRangeBudget(im.model);
    im.budgetDropped = dropped;
    im.fdefText = emitFdef(fitted);
    im.error = null;
  } catch (e) {
    im.error = e && e.message ? e.message : String(e);
  }
}

function importSetCap(coarseOffset, capName) {
  const im = state.import;
  const c = im.model.caps.find((x) => x.coarse === coarseOffset);
  if (!c) return;
  c.cap = capName;
  c.unmapped = false; // an explicit user choice is never "unmapped"
  if (!im.fdefDirty) regenerateImportFdefText();
  render();
}

function importSetRangeField(coarseOffset, rangeIdx, field, value) {
  const im = state.import;
  const c = im.model.caps.find((x) => x.coarse === coarseOffset);
  if (!c || !c.ranges[rangeIdx]) return;
  c.ranges[rangeIdx][field] = value;
  if (!im.fdefDirty) regenerateImportFdefText();
  render();
}

function importEditFdefText(text) {
  state.import.fdefText = text;
  state.import.fdefDirty = true;
}

function importRegenerateFromTable() {
  state.import.fdefDirty = false;
  regenerateImportFdefText();
  render();
}

function importSaveToLibrary() {
  const im = state.import;
  let name = (im.fdefName || "").trim();
  if (!name) return;
  if (!name.toLowerCase().endsWith(".fdef")) name += ".fdef";
  const existing = new Set(state.workspace.fdefs.map((f) => f.name));
  let finalName = name;
  let n = 2;
  while (existing.has(finalName)) {
    finalName = name.replace(/\.fdef$/i, `-${n}.fdef`);
    n++;
  }
  state.workspace.fdefs.push({ name: finalName, text: im.fdefText });
  state.selection = { kind: "fdef", name: finalName };
  im.open = false;
  reparseFdefs().then(render);
  toast("Imported", `${finalName} added to the fixture library`, "ok");
  render();
}

function renderImportModal() {
  const im = state.import;
  const body = [];

  if (im.stage === "input") {
    body.push(
      el("div", { class: "import-dropzone",
        ondragover: (e) => { e.preventDefault(); e.stopPropagation(); },
        ondrop: (e) => {
          e.preventDefault();
          e.stopPropagation();
          if (e.dataTransfer.files.length > 0) importFromFile(e.dataTransfer.files[0]);
        },
      },
        icon("upload"),
        el("div", {}, "Drop a .qxf, .gdtf, or Open Fixture Library .json file here"),
        el(
          "button",
          { class: "btn", onclick: () => $("#import-file-input").click() },
          "Choose file…",
        ),
        el("input", {
          id: "import-file-input", type: "file", accept: ".qxf,.gdtf,.json",
          style: "display:none",
          onchange: (e) => { if (e.target.files.length > 0) importFromFile(e.target.files[0]); e.target.value = ""; },
        }),
      ),
    );
    body.push(
      el("div", { class: "import-url-row" },
        el("input", {
          type: "text", placeholder: "…or paste a QLC+/OFL GitHub URL",
          value: im.urlText,
          oninput: (e) => { im.urlText = e.target.value; },
          onkeydown: (e) => { if (e.key === "Enter") importFromUrl(); },
        }),
        el("button", { class: "btn", disabled: !im.urlText.trim() || im.loading, onclick: importFromUrl }, "Fetch"),
      ),
    );
    if (im.loading) body.push(el("div", { class: "import-status" }, icon("spin"), " Parsing…"));
    if (im.error) body.push(errorBlock("Import failed", im.error, ""));
  } else if (im.stage === "mode") {
    body.push(el("div", { class: "import-fixture-label" }, im.fixtureLabel));
    body.push(
      el(
        "label",
        { class: "import-mode-select" },
        el("span", {}, "Mode (personality) -- pick the one you'll patch:"),
        el(
          "select",
          { onchange: (e) => importSelectMode(e.target.value) },
          el("option", { value: "", selected: im.modeName == null }, "Select a mode…"),
          ...im.modes.map((m) =>
            el("option", { value: m.name, selected: im.modeName === m.name }, `${m.name} (${m.footprint}ch)`),
          ),
        ),
      ),
    );
    if (im.error) body.push(errorBlock("Import failed", im.error, ""));
  } else if (im.stage === "table") {
    if (im.budgetDropped.length) {
      // Unmissable, on its own, above the softer informational warnings --
      // this is data loss (named states that won't make it into the
      // saved .fdef), not a mapping judgment call. Should be vanishingly
      // rare at PFX2's current budget; if it fires, the user must not
      // find out only after flashing a fixture with half its gobos.
      const totalDropped = im.budgetDropped.reduce((n, d) => n + d.count, 0);
      body.push(
        el(
          "div",
          { class: "import-budget-alert" },
          icon("err"),
          el(
            "div",
            {},
            el(
              "div",
              { class: "import-budget-alert-title" },
              `${totalDropped} named state${totalDropped === 1 ? "" : "s"} won't be saved -- the fixture profile's slot budget is full`,
            ),
            ...im.budgetDropped.map((d) =>
              el("div", { class: "import-budget-alert-detail" },
                `"${d.cap}" (offset ${d.coarse}): ${d.count} of its trailing ranges dropped, marked with strikethrough below`),
            ),
          ),
        ),
      );
    }
    if (im.warnings.length) {
      body.push(
        el(
          "div",
          { class: "import-warnings" },
          el("div", { class: "import-warnings-title" }, `${im.warnings.length} note${im.warnings.length === 1 ? "" : "s"} from the importer`),
          ...im.warnings.map((w) => el("div", { class: "import-warning" }, w)),
        ),
      );
    }
    body.push(renderImportChannelTable(im.model, im.budgetDropped));
    body.push(
      el(
        "div",
        { class: "import-fdef-preview" },
        el(
          "div",
          { class: "import-fdef-preview-header" },
          el("span", {}, ".fdef preview (editable)"),
          im.fdefDirty && el("button", { class: "btn btn-small", onclick: importRegenerateFromTable }, "Regenerate from table"),
        ),
        el("textarea", {
          class: "import-fdef-textarea",
          spellcheck: "false",
          oninput: (e) => importEditFdefText(e.target.value),
        }, im.fdefText),
      ),
    );
    body.push(
      el(
        "label",
        { class: "import-save-row" },
        el("span", {}, "Save as:"),
        el("input", { type: "text", value: im.fdefName, oninput: (e) => { im.fdefName = e.target.value; } }),
      ),
    );
  }

  const footerButtons = [
    el("button", { class: "btn", disabled: im.loading, onclick: closeImportModal }, "Cancel"),
  ];
  if (im.stage === "mode") {
    footerButtons.unshift(el("button", { class: "btn", onclick: () => { im.stage = "input"; render(); } }, "Back"));
  }
  if (im.stage === "table") {
    footerButtons.unshift(el("button", { class: "btn", onclick: () => { im.stage = "mode"; render(); } }, "Back"));
    footerButtons.push(
      el("button", { class: "btn btn-primary", disabled: !im.fdefName.trim(), onclick: importSaveToLibrary }, "Add to fixture library"),
    );
  }

  return el(
    "div",
    { class: "modal-backdrop", onclick: (e) => { if (e.target === e.currentTarget) closeImportModal(); } },
    el(
      "div",
      { class: "modal modal-wide" },
      el(
        "div",
        { class: "modal-header" },
        icon("upload"),
        el("span", { class: "title" }, "Import fixture"),
        el("div", { class: "spacer" }),
        el("button", { class: "btn btn-icon", onclick: closeImportModal, disabled: im.loading }, icon("close")),
      ),
      el("div", { class: "modal-body" }, ...body),
      el("div", { class: "flash-footer" }, ...footerButtons),
    ),
  );
}

function renderImportChannelTable(model, budgetDropped = []) {
  // fitRangeBudget trims from the END of a channel's own range list (see
  // model.js) -- mirror that here so the marked rows are the exact ones
  // that will actually be cut, not just "some N of them".
  const droppedCountByOffset = new Map(budgetDropped.map((d) => [d.coarse, d.count]));
  const rows = model.caps.map((c) => {
    const offsetLabel = c.fine != null ? `${c.coarse}/${c.fine}` : `${c.coarse}`;
    const droppedCount = droppedCountByOffset.get(c.coarse) || 0;
    const firstDroppedIdx = droppedCount > 0 ? c.ranges.length - droppedCount : Infinity;
    const rangeRows = c.ranges.map((r, i) =>
      el(
        "div",
        { class: `import-range-row ${i >= firstDroppedIdx ? "import-range-will-drop" : ""}` },
        el("span", { class: "import-range-span" }, `${r.from}–${r.to}`),
        el(
          "select",
          {
            onchange: (e) => importSetRangeField(c.coarse, i, "continuous", e.target.value === "continuous"),
          },
          el("option", { value: "discrete", selected: !r.continuous }, "SLOT (discrete)"),
          el("option", { value: "continuous", selected: r.continuous }, "RANGE (continuous)"),
        ),
        el("input", {
          type: "text", class: "import-range-name", value: r.name || "",
          oninput: (e) => importSetRangeField(c.coarse, i, "name", e.target.value),
        }),
      ),
    );
    return el(
      "div",
      { class: `import-channel-row ${c.unmapped ? "import-unmapped" : ""}` },
      el(
        "div",
        { class: "import-channel-main" },
        el("span", { class: "import-channel-offset" }, offsetLabel),
        el("span", { class: "import-channel-source" }, c.sourceName || "(unnamed)"),
        el(
          "select",
          { onchange: (e) => importSetCap(c.coarse, e.target.value) },
          ...IMPORT_CAP_NAMES.map((name) => el("option", { value: name, selected: c.cap === name }, name)),
        ),
        c.unmapped && el("span", { class: "import-unmapped-badge" }, "unmapped"),
        droppedCount > 0 && el("span", { class: "import-unmapped-badge" }, `${droppedCount} won't be saved`),
      ),
      rangeRows.length > 0 && el("div", { class: "import-range-list" }, ...rangeRows),
    );
  });
  return el("div", { class: "import-channel-table" }, ...rows);
}

// --- USB flash modal (esptool-js over Web Serial) -----------------------

function openFlashModal() {
  state.flash.open = true;
  state.flash.busy = false;
  state.flash.done = false;
  state.flash.error = null;
  state.flash.chip = null;
  state.flash.log = "";
  state.flash.fileIndex = 0;
  state.flash.fileCount = 0;
  state.flash.progress = 0;
  render();
}

function closeFlashModal() {
  if (state.flash.busy) return;  // don't yank the modal mid-flash
  state.flash.open = false;
  render();
}

function appendFlashLog(line) {
  state.flash.log += line;
  render();
}

async function doFlash() {
  const f = state.flash;
  f.busy = true;
  f.done = false;
  f.error = null;
  f.chip = null;
  f.log = "";
  f.fileIndex = 0;
  f.fileCount = 0;
  f.progress = 0;
  render();
  try {
    const bundleBytes = f.includeShow && state.compiledBundle ? state.compiledBundle : null;
    let scriptsImageBytes = null;
    if (f.includeBoot) {
      appendFlashLog("Building scripts-partition image from boot.fnl...\n");
      scriptsImageBytes = await buildScriptsImage(state.workspace.boot.text);
    }
    await flashDevice({
      bundleBytes,
      scriptsImageBytes,
      eraseFirst: f.eraseFirst,
      baudrate: f.baud,
      onLog: (line) => appendFlashLog(line),
      onChip: (chip) => { f.chip = chip; render(); },
      onProgress: (fileIndex, fileCount, fraction) => {
        f.fileIndex = fileIndex;
        f.fileCount = fileCount;
        f.progress = fraction;
        render();
      },
    });
    f.done = true;
    toast("Flashed", "Device is rebooting.", "ok");
  } catch (e) {
    f.error = e && e.message ? e.message : String(e);
    toast("Flash failed", f.error, "err");
  } finally {
    f.busy = false;
    render();
  }
}

function renderFlashModal() {
  const f = state.flash;
  const supported = serialSupported() && secureContextOk();

  const body = [];
  if (!supported) {
    body.push(
      errorBlock(
        !secureContextOk() ? "Requires HTTPS" : "Web Serial not available",
        !secureContextOk()
          ? "This page must be served over HTTPS (or localhost) to flash over USB."
          : "Flashing over USB needs the Web Serial API, available in Chrome or Edge " +
            "89+ on desktop (or Chrome on Android). Safari and iOS don't support it — " +
            "use the \"Download compiled .shw1\" button and flash from the command line instead.",
        "",
      ),
    );
  } else {
    body.push(
      el(
        "div",
        { class: "flash-options" },
        el(
          "label",
          { class: "flash-check" },
          el("input", {
            type: "checkbox",
            checked: f.eraseFirst,
            disabled: f.busy,
            onchange: (e) => { f.eraseFirst = e.target.checked; render(); },
          }),
          el("span", {}, "Erase flash first (recommended for a fresh/wedged board)"),
        ),
        el(
          "label",
          { class: "flash-check" },
          el("input", {
            type: "checkbox",
            checked: f.includeShow,
            disabled: f.busy || !state.compiledBundle,
            onchange: (e) => { f.includeShow = e.target.checked; render(); },
          }),
          el(
            "span",
            {},
            state.compiledBundle
              ? `Include my compiled show (${state.compiledBundle.byteLength} bytes)`
              : "Include my compiled show (Compile it first — using the CI demo show for now)",
          ),
        ),
        el(
          "label",
          { class: "flash-check" },
          el("input", {
            type: "checkbox",
            checked: f.includeBoot,
            disabled: f.busy,
            onchange: (e) => { f.includeBoot = e.target.checked; render(); },
          }),
          el(
            "span",
            {},
            "Bake boot.fnl into the scripts partition (board comes up already running this show)",
          ),
        ),
        el(
          "label",
          { class: "flash-baud" },
          el("span", {}, "Baud rate"),
          el(
            "select",
            {
              disabled: f.busy,
              onchange: (e) => { f.baud = parseInt(e.target.value, 10); render(); },
            },
            el("option", { value: "460800", selected: f.baud === 460800 }, "460800 (fast)"),
            el("option", { value: "115200", selected: f.baud === 115200 }, "115200 (safe)"),
          ),
        ),
      ),
    );

    if (f.chip) {
      body.push(el("div", { class: "flash-chip" }, `Detected: ${f.chip}`));
    }

    if (f.busy || f.fileCount > 0) {
      const pct = Math.round((f.progress || 0) * 100);
      body.push(
        el(
          "div",
          { class: "flash-progress" },
          el("div", { class: "flash-progress-label" },
            f.fileCount > 0 ? `Writing part ${f.fileIndex + 1} of ${f.fileCount} — ${pct}%` : "Connecting…"),
          el("div", { class: "flash-progress-bar" },
            el("div", { class: "flash-progress-fill", style: `width:${pct}%` })),
        ),
      );
    }

    if (f.log) {
      body.push(el("pre", { class: "flash-log" }, f.log));
    }

    if (f.error) {
      body.push(errorBlock("Flash failed", f.error, ""));
    }

    if (f.done) {
      body.push(
        el("div", { class: "flash-done" }, icon("ok"), el("span", {}, "Done — device is rebooting.")),
      );
    }
  }

  const footer = el(
    "div",
    { class: "flash-footer" },
    el("button", { class: "btn", disabled: f.busy, onclick: closeFlashModal }, "Close"),
    supported &&
      el(
        "button",
        {
          class: "btn btn-primary",
          disabled: f.busy,
          onclick: doFlash,
        },
        f.busy ? icon("spin") : icon("zap"),
        el("span", {}, f.busy ? "Flashing…" : "Connect device & flash"),
      ),
  );

  return el(
    "div",
    { class: "modal-backdrop", onclick: (e) => { if (e.target === e.currentTarget) closeFlashModal(); } },
    el(
      "div",
      { class: "modal" },
      el(
        "div",
        { class: "modal-header" },
        icon("zap"),
        el("span", { class: "title" }, "Flash ESP32-S3 over USB"),
        el("div", { class: "spacer" }),
        el("button", { class: "btn btn-icon", onclick: closeFlashModal, disabled: f.busy }, icon("close")),
      ),
      el("div", { class: "modal-body" }, ...body),
      footer,
    ),
  );
}

// --- drag-drop on the whole window ------------------------------------

window.addEventListener("dragover", (e) => e.preventDefault());
window.addEventListener("drop", (e) => {
  e.preventDefault();
  if (e.dataTransfer.files.length > 0) handleFiles(e.dataTransfer.files);
});

// --- go --------------------------------------------------------------

main();
