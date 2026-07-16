//
// app.js — esp-glow provisioner (static editor / project workspace).
//
// Plain ESM, no framework. Loads the WASM module from ./wasm/, manages a
// *Project* (§1 of the multi-show workspace plan: one .show patch + a
// .fdef/.mdef library + MANY .fnl shows, one marked boot), persisted to
// IndexedDB (§2) with export/import to a portable .zip (§3) and an
// optional live push/pull to a running rig's scripts partition (§5).
// Renders a four-pane editor: project tree / tabs+editor / preview+
// diagnostics, plus the project switcher, device-sync, flash, and fixture-
// importer modals.
//
// Mirrors the compile/flash/import behavior of the original single-
// workspace editor; see README_PROVISIONER.md for the full picture.
//

import createModule from "./wasm/provision-wasm.js";
import { serialSupported, secureContextOk, flash as flashDevice } from "./flash.js";
import { createFennelEditor, setEditorDoc, getEditorDoc, lintFootguns } from "./shared/fennel-editor.js";
import { checkFennelSyntax } from "./fennel-check.js";
import { buildScriptsImage } from "./boot-image.js";
import { detectAndParse, listImportModes, buildImportModel, normalizeFixtureUrl, FORMAT_LABEL } from "./import.js";
import { emitFdef, fitRangeBudget, CAP_NAMES as IMPORT_CAP_NAMES } from "./shared/importers/model.js";
import { defaultDeviceConfig, encodeDeviceConfig, parseIPv4, formatIPv4 } from "./shared/devcfg.js";

import * as PM from "./shared/project-model.js";
import { createProjectStore } from "./shared/project-store.js";
import { createMemoryAdapter } from "./shared/kv-memory-adapter.js";
import { createIndexedDbAdapter, indexedDbAvailable } from "./shared/kv-indexeddb-adapter.js";
import { exportProjectZip, importProjectZip, projectTextFiles, projectManifestJson, MANIFEST_NAME, BUNDLE_NAME, projectFromFileEntries } from "./shared/project-zip.js";
import { WsClient, Status as WsStatus } from "./shared/ws-client.js";
import { createDeviceSync, planReconciliation } from "./shared/device-sync.js";

// --- CFG1 device config persistence (localStorage) -----------------------
//
// So reflashing a board isn't retyping the WiFi password every time (§5).
// Stores the plain form fields, not the encoded CFG1 bytes -- the format
// can gain fields across a browser session; re-encoding from the current
// devcfg.js on every flash is simpler than migrating a stored blob.
//
// Deliberately its own localStorage key, untouched by the project store
// below: CFG1 is per-device, not per-project, and must never be reachable
// from a Project (project-model.js has no field for it at all), so an
// exported project.zip can never leak a WiFi password (§3).
const DEVCFG_STORAGE_KEY = "esp-glow-devcfg-v1";

// A pre-existing single-workspace blob from before the multi-project
// rewrite -- this build of app.js never wrote this key (the old workspace
// lived in memory only, reset every reload), but a future or hand-crafted
// one might have; migrateLegacyIfNeeded consumes it exactly once (§2/§6).
const LEGACY_WORKSPACE_STORAGE_KEY = "esp-glow-workspace-v1";

function defaultFormDeviceConfig() {
  const d = defaultDeviceConfig();
  return { ...d, artnetFallbackIp: formatIPv4(d.artnetFallbackIp) };
}

function loadStoredDeviceConfig() {
  try {
    const raw = localStorage.getItem(DEVCFG_STORAGE_KEY);
    if (!raw) return defaultFormDeviceConfig();
    return { ...defaultFormDeviceConfig(), ...JSON.parse(raw) };
  } catch {
    return defaultFormDeviceConfig();
  }
}

function saveStoredDeviceConfig(formCfg) {
  try {
    localStorage.setItem(DEVCFG_STORAGE_KEY, JSON.stringify(formCfg));
  } catch {
    // Private browsing / storage disabled -- not fatal, just means the
    // next flash starts from defaults again.
  }
}

function deviceConfigFromForm(formCfg) {
  return { ...formCfg, artnetFallbackIp: parseIPv4(formCfg.artnetFallbackIp) };
}

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

const CAP_NAMES = [
  "Dimmer", "Red", "Green", "Blue", "White", "Amber", "Uv",
  "Cyan", "Magenta", "Yellow", "Pan", "Tilt",
  "ShutterStrobe", "Gobo", "Focus", "Zoom", "Fog", "Fan",
  "ColorWheel", "GoboRotation", "Prism", "PrismRotation", "Frost", "Iris", "CTO",
  "AnimationWheel", "Macro",
];
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

// --- default project (first-run seed content) ---------------------------
//
// The demo rig every fresh install used to hardcode as the one-and-only
// in-memory workspace. Now it's just what ensureSeeded() writes into the
// project store the very first time someone opens the page (§2) -- after
// that it's a normal project the user can rename, duplicate, or delete
// like any other.
function makeDefaultProject() {
  const p = PM.emptyProject("Demo rig");
  PM.setPatchShowText(p, `# esp-glow show definition
# Lines starting with # are comments. Tokens are whitespace-separated.
#
# SHOW 2: universes and DMX addresses are 1-indexed -- write the number
# printed on the fixture/console, not a memory offset.
SHOW 2

UNIVERSE 1 DMX
UNIVERSE 2 ARTNET

# Moving head on universe 1, address 2
FIXTURE torrent.fdef 1 2
POS 1.0 2.0 3.0
ROT 0 0 0

# Par fixture on universe 1, address 21
FIXTURE par.fdef 1 21

# 16x16 LED matrix on universe 2, starting at address 1
MATRIX 2 1 16 16 SERP H GRB

# MIDI controller: embeds the APC40 mkII .mdef below into the bundle so the
# device can drive its pad/scene LEDs. Bindings (which pad triggers which cue)
# live in boot.fnl via glow.bind.* -- see README_LIVE_CONTROL.md.
CONTROLLER apc40.mdef
`);
  PM.addFixture(p, "torrent.fdef", `# 16-channel moving head
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
`);
  PM.addFixture(p, "par.fdef", `# 5-channel RGB par
FIXTURE Par 5
FOOTPRINT 5
CAP Dimmer 0
CAP Red 1
CAP Green 2
CAP Blue 3
CAP White 4
`);
  PM.addController(p, "apc40.mdef", `# Akai APC40 mkII (USB MIDI clip launcher). Transcribed from Akai's
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
LED NOTE 0 39 velocity
  COLOR off       0
  COLOR red       5
  COLOR green     21
  COLOR blue      41
LED NOTE 82 86 velocity
  COLOR off   0
  COLOR white 3
  COLOR red   5
  COLOR green 21
  COLOR blue  41
`);
  PM.addShow(p, "boot.fnl", `;; boot.fnl -- evaluated once at startup; authored here, baked into
;; the flash image's "scripts" partition by the Flash step below.
(fn breathe [t]
  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2))))))

(glow.cue.define :breathe {:effects [breathe] :priority 0})
(glow.cue.go :breathe)
`);
  PM.setBootShow(p, "boot.fnl");
  return p;
}

// --- app state ---------------------------------------------------------

const state = {
  // Project workspace (§1/§2) -- filled in by initProjectWorkspace() before
  // the first render.
  projectStore: null,
  storageKind: null,     // "indexeddb" | "memory"
  storageDurable: false,
  project: null,
  projects: [],           // [{id, name, created, modified}], newest first
  dirtyTabs: new Set(),   // tab keys edited since the last autosave landed
  saveTimer: null,
  savePending: false,

  selection: { kind: "show" },   // {kind:"show"} | {kind:"fdef"|"mdef"|"fnl", name}
  openTabs: [{ kind: "show" }],

  diagnostics: {
    show: { ok: false, err: "Not compiled yet", bundleBytes: null, loaded: null },
    fdefs: {},
    mdefs: {},
    // Per-show Fennel syntax-check state (fennel-check.js's real compiler,
    // not the WASM show compiler) -- keyed by show name so the tree can
    // show ok/err per file, not just for whichever tab is open.
    shows: {},
  },

  wasmReady: false,
  compiling: false,
  module: null,
  compiledBundle: null,   // Uint8Array of the last successful compile, for flashing/export

  fnlEditorView: null,    // shared CodeMirror view; its doc is swapped per active .fnl tab
  fnlEditorForTab: null,  // which show name the shared view's doc currently reflects
  fnlEditorSwapping: false,

  flash: {
    open: false, busy: false, done: false, error: null, chip: null, log: "",
    fileIndex: 0, fileCount: 0, progress: 0,
    eraseFirst: false, includeShow: true, includeBoot: false, baud: 460800,
  },
  devcfg: loadStoredDeviceConfig(),

  import: {
    open: false, stage: "input", loading: false, error: null, urlText: "",
    format: null, fixtureLabel: "", parsed: null, modes: [], modeName: null,
    model: null, warnings: [], budgetDropped: [], fdefName: "", fdefText: "", fdefDirty: false,
  },

  // Project switcher / manager modal (§4).
  projectPanel: { open: false, renamingId: null, renameText: "", newName: "" },

  // File System Access (§2/§3): "Open folder"/"Save to folder" where
  // available; export/import .zip is the fallback everywhere else.
  fsSupported: typeof window !== "undefined" && typeof window.showDirectoryPicker === "function",
  fsDirHandle: null,
  fsBusy: false,

  // Device sync (§5): connects to a live rig's existing script CRUD over
  // WebSocket. A dedicated WsClient, not shared with anything else.
  deviceSync: {
    open: false, url: "", client: null, sync: null, status: WsStatus.Closed,
    fileStatus: {},   // name -> {status, localText, deviceText}
    remoteOnly: [],   // device script names with no local counterpart
    conflict: null,   // {name, localText, deviceText} while the "keep which?" prompt is up
    busy: false, error: null,
  },
};

let parseTimer = null;

// --- project workspace bootstrap (§2/§6) --------------------------------

async function initProjectWorkspace() {
  const adapter = indexedDbAvailable() ? createIndexedDbAdapter() : createMemoryAdapter();
  state.storageKind = adapter.kind;
  state.storageDurable = adapter.durable;
  const store = createProjectStore(adapter);
  state.projectStore = store;

  let legacyWorkspace = null;
  try {
    const raw = localStorage.getItem(LEGACY_WORKSPACE_STORAGE_KEY);
    if (raw) legacyWorkspace = JSON.parse(raw);
  } catch {
    legacyWorkspace = null;
  }
  const migrated = await store.migrateLegacyIfNeeded(legacyWorkspace, "My Project");
  if (migrated) {
    try { localStorage.removeItem(LEGACY_WORKSPACE_STORAGE_KEY); } catch { /* ignore */ }
  }
  await store.ensureSeeded(makeDefaultProject);

  state.projects = await store.list();
  let activeId = await store.getActiveId();
  if (!activeId || !state.projects.some((p) => p.id === activeId)) {
    activeId = state.projects[0]?.id ?? null;
  }
  state.project = activeId ? await store.load(activeId) : makeDefaultProject();
  if (activeId) await store.setActiveId(activeId);
}

function scheduleAutosave() {
  state.savePending = true;
  clearTimeout(state.saveTimer);
  state.saveTimer = setTimeout(async () => {
    if (!state.projectStore || !state.project) return;
    await state.projectStore.save(state.project);
    state.projects = await state.projectStore.list();
    state.dirtyTabs.clear();
    state.savePending = false;
    render();
  }, 500);
}

function markDirty(tabKeyStr) {
  state.dirtyTabs.add(tabKeyStr);
  scheduleAutosave();
}

async function switchProject(id) {
  // Refresh the cached list regardless of whether we actually swap the
  // active project below -- callers that just created/duplicated/imported
  // a project rely on this to pick up the new entry (project-store.js's
  // create/duplicate/save don't touch this app-level cache themselves).
  state.projects = await state.projectStore.list();
  if (!state.project || id === state.project.id) return;
  const p = await state.projectStore.load(id);
  if (!p) return;
  state.project = p;
  await state.projectStore.setActiveId(id);
  state.selection = { kind: "show" };
  state.openTabs = [{ kind: "show" }];
  state.diagnostics = { show: { ok: false, err: "Not compiled yet", bundleBytes: null, loaded: null }, fdefs: {}, mdefs: {}, shows: {} };
  state.compiledBundle = null;
  state.dirtyTabs.clear();
  state.fnlEditorForTab = null;
  await reparseFdefs();
  await reparseMdefs();
  render();
}

// --- main entry --------------------------------------------------------

async function main() {
  try {
    await initProjectWorkspace();
    state.module = await createModule({ locateFile: (p) => "./wasm/" + p });
    state.wasmReady = true;
    await reparseFdefs();
    await reparseMdefs();
    render();
  } catch (e) {
    console.error("main() failed:", e);
    document.getElementById("app").innerHTML =
      `<div style="padding:24px;color:#f48771;font-family:monospace">Failed to start: ${e.message ?? e}</div>`;
  }
}

// --- compile / parse operations -----------------------------------------

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

function readPatchFile(path) {
  const norm = path.replace(/^\.\//, "").replace(/^\//, "");
  const f = state.project.patch.fixtures.find((x) => x.name === norm || x.name === path);
  if (f) return f.text;
  const m = state.project.controllers.find((x) => x.name === norm || x.name === path);
  return m ? m.text : "";
}

async function compileShowNow() {
  if (state.compiling) return;
  state.compiling = true;
  render();
  try {
    const r = state.module.compileShow(state.project.patch.show.text, readPatchFile);
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
    state.diagnostics.show = { ok: true, err: "", bundleBytes: bundle.byteLength, loaded: summary };
    state.compiledBundle = bundle;
    toast("Compiled", `${bundle.byteLength}-byte SHW1 bundle (${summary.fixtureCount} fixtures, ${summary.matrixCount} matrices)`, "ok");
  } catch (e) {
    toast("Compile error", String(e), "err");
  } finally {
    state.compiling = false;
    render();
  }
}

async function reparseFdefs() {
  const entries = await Promise.all(
    state.project.patch.fixtures.map(async (f) => [f.name, await parseFdef(f.text)]),
  );
  state.diagnostics.fdefs = {};
  for (const [name, r] of entries) {
    state.diagnostics.fdefs[name] = r.ok ? { ok: true, err: "", def: r.def } : { ok: false, err: r.err };
  }
}

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
    state.project.controllers.map(async (m) => [m.name, await parseMdef(m.text)]),
  );
  state.diagnostics.mdefs = {};
  for (const [name, r] of entries) {
    state.diagnostics.mdefs[name] = r.ok ? { ok: true, err: "", def: r.def } : { ok: false, err: r.err };
  }
}

// --- selection / tabs (§4: tabbed multi-file editor) ---------------------

function tabKey(sel) {
  return `${sel.kind}:${sel.name ?? ""}`;
}
function sameSel(a, b) {
  return a.kind === b.kind && (a.name ?? "") === (b.name ?? "");
}

function openTab(sel) {
  if (!state.openTabs.some((t) => sameSel(t, sel))) state.openTabs.push(sel);
  state.selection = sel;
  render();
}

function closeTab(sel) {
  const wasActive = sameSel(state.selection, sel);
  state.openTabs = state.openTabs.filter((t) => !sameSel(t, sel));
  if (state.openTabs.length === 0) state.openTabs.push({ kind: "show" });
  if (wasActive) state.selection = state.openTabs[state.openTabs.length - 1];
  render();
}

// --- file operations ---------------------------------------------------

function currentText() {
  const s = state.selection;
  if (s.kind === "show") return state.project.patch.show.text;
  if (s.kind === "fnl") return state.project.shows.find((x) => x.name === s.name)?.text ?? "";
  if (s.kind === "mdef") return state.project.controllers.find((x) => x.name === s.name)?.text ?? "";
  return state.project.patch.fixtures.find((x) => x.name === s.name)?.text ?? "";
}
function currentName() {
  const s = state.selection;
  if (s.kind === "show") return state.project.patch.show.name;
  return s.name;
}

function setText(text) {
  const s = state.selection;
  if (s.kind === "show") {
    PM.setPatchShowText(state.project, text);
  } else if (s.kind === "mdef") {
    const m = state.project.controllers.find((x) => x.name === s.name);
    if (m) { m.text = text; PM.touchProject(state.project); }
  } else if (s.kind === "fdef") {
    const f = state.project.patch.fixtures.find((x) => x.name === s.name);
    if (f) { f.text = text; PM.touchProject(state.project); }
  }
  markDirty(tabKey(s));

  if (s.kind === "fdef") {
    clearTimeout(parseTimer);
    parseTimer = setTimeout(async () => {
      const r = await parseFdef(currentText());
      state.diagnostics.fdefs[s.name] = r.ok ? { ok: true, err: "", def: r.def } : { ok: false, err: r.err };
      render();
    }, 250);
  }
  if (s.kind === "mdef") {
    clearTimeout(parseTimer);
    parseTimer = setTimeout(async () => {
      const r = await parseMdef(currentText());
      state.diagnostics.mdefs[s.name] = r.ok ? { ok: true, err: "", def: r.def } : { ok: false, err: r.err };
      render();
    }, 250);
  }
}

// --- .fnl shows: shared CodeMirror editor (persists across render()'s DOM churn) --

function activeFnlName() {
  return state.selection.kind === "fnl" ? state.selection.name : null;
}

function getFnlEditorView() {
  const name = activeFnlName();
  if (!state.fnlEditorView) {
    state.fnlEditorView = createFennelEditor({
      parent: document.createElement("div"),
      doc: name ? (state.project.shows.find((s) => s.name === name)?.text ?? "") : "",
      onChange: (text) => {
        if (state.fnlEditorSwapping) return;
        const target = state.fnlEditorForTab;
        if (!target) return;
        PM.setShowText(state.project, target, text);
        state.diagnostics.shows[target] = { ...(state.diagnostics.shows[target] || {}), checked: false };
        markDirty(tabKey({ kind: "fnl", name: target }));
      },
    });
    state.fnlEditorForTab = name;
  } else if (state.fnlEditorForTab !== name) {
    state.fnlEditorSwapping = true;
    setEditorDoc(state.fnlEditorView, name ? (state.project.shows.find((s) => s.name === name)?.text ?? "") : "");
    state.fnlEditorForTab = name;
    state.fnlEditorSwapping = false;
  }
  return state.fnlEditorView;
}

async function checkFnlSyntax(name) {
  state.diagnostics.shows[name] = { ...(state.diagnostics.shows[name] || {}), checking: true };
  render();
  const text = state.project.shows.find((s) => s.name === name)?.text ?? "";
  const result = await checkFennelSyntax(text);
  state.diagnostics.shows[name] = { checking: false, checked: true, ok: result.ok, err: result.ok ? null : result.err };
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

function slugify(name) {
  return (name || "project").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "project";
}

const IMPORT_FORMAT_EXTENSIONS = [".qxf", ".gdtf", ".json"];

function handleFiles(files) {
  const arr = Array.from(files);
  const importable = arr.find((f) => IMPORT_FORMAT_EXTENSIONS.some((ext) => f.name.toLowerCase().endsWith(ext)));
  if (importable) {
    openImportModal();
    importFromFile(importable);
    return;
  }
  const projectZip = arr.find((f) => f.name.toLowerCase().endsWith(".zip"));
  if (projectZip) {
    importProjectFromFile(projectZip);
    return;
  }
  let i = 0;
  const reader = new FileReader();
  const next = () => {
    if (i >= arr.length) {
      toast("Imported", `${arr.length} file${arr.length === 1 ? "" : "s"}`, "ok");
      scheduleAutosave();
      render();
      return;
    }
    const file = arr[i++];
    reader.onload = () => {
      const text = typeof reader.result === "string" ? reader.result : "";
      const lower = file.name.toLowerCase();
      if (lower.endsWith(".show")) {
        state.project.patch.show = { name: file.name, text };
        PM.touchProject(state.project);
      } else if (lower.endsWith(".fdef")) {
        const others = state.project.patch.fixtures.filter((f) => f.name !== file.name);
        state.project.patch.fixtures = [...others, { name: file.name, text }];
        PM.touchProject(state.project);
        reparseFdefs();
      } else if (lower.endsWith(".mdef")) {
        const others = state.project.controllers.filter((f) => f.name !== file.name);
        state.project.controllers = [...others, { name: file.name, text }];
        PM.touchProject(state.project);
        reparseMdefs();
      } else if (lower.endsWith(".fnl")) {
        const others = state.project.shows.filter((s) => s.name !== file.name);
        state.project.shows = [...others, { name: file.name, text }];
        if (!state.project.bootShow) state.project.bootShow = file.name;
        PM.touchProject(state.project);
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

// --- project export / import (§3: the portability guarantee) ------------

function exportActiveProjectZip() {
  const bytes = exportProjectZip(state.project, { compiledBundle: state.compiledBundle ?? undefined });
  downloadBlob(`${slugify(state.project.meta.name)}.zip`, "application/zip", bytes);
  toast("Exported", `${state.project.meta.name}.zip`, "ok");
}

async function importProjectFromBytes(bytes, fallbackName) {
  try {
    const { project, compiledBundle } = await importProjectZip(bytes);
    project.id = PM.newId(); // never silently overwrite an existing stored project
    if (!project.meta.name || project.meta.name === "Imported project") project.meta.name = fallbackName;
    await state.projectStore.save(project);
    await state.projectStore.setActiveId(project.id);
    state.projects = await state.projectStore.list();
    state.project = project;
    state.compiledBundle = compiledBundle ?? null;
    state.selection = { kind: "show" };
    state.openTabs = [{ kind: "show" }];
    state.diagnostics = { show: { ok: false, err: "Not compiled yet", bundleBytes: null, loaded: null }, fdefs: {}, mdefs: {}, shows: {} };
    await reparseFdefs();
    await reparseMdefs();
    toast("Project imported", project.meta.name, "ok");
    render();
  } catch (e) {
    toast("Import failed", e && e.message ? e.message : String(e), "err");
  }
}

function importProjectFromFile(file) {
  const reader = new FileReader();
  reader.onload = () => importProjectFromBytes(new Uint8Array(reader.result), file.name.replace(/\.zip$/i, ""));
  reader.onerror = () => toast("Import failed", `Couldn't read ${file.name}`, "err");
  reader.readAsArrayBuffer(file);
}

// --- File System Access (§2/§3): "Open folder" / "Save to folder" -------

async function writeFileInDirectory(dirHandle, path, bytesOrText) {
  const parts = path.split("/");
  let dir = dirHandle;
  for (let i = 0; i < parts.length - 1; i++) {
    dir = await dir.getDirectoryHandle(parts[i], { create: true });
  }
  const fileHandle = await dir.getFileHandle(parts[parts.length - 1], { create: true });
  const writable = await fileHandle.createWritable();
  await writable.write(typeof bytesOrText === "string" ? new TextEncoder().encode(bytesOrText) : bytesOrText);
  await writable.close();
}

async function saveActiveProjectToFolder() {
  if (!state.fsSupported) return;
  state.fsBusy = true;
  render();
  try {
    const dirHandle = state.fsDirHandle || await window.showDirectoryPicker({ mode: "readwrite" });
    state.fsDirHandle = dirHandle;
    await writeFileInDirectory(dirHandle, MANIFEST_NAME, projectManifestJson(state.project));
    for (const f of projectTextFiles(state.project)) await writeFileInDirectory(dirHandle, f.path, f.text);
    if (state.compiledBundle) await writeFileInDirectory(dirHandle, BUNDLE_NAME, state.compiledBundle);
    toast("Saved to folder", dirHandle.name, "ok");
  } catch (e) {
    if (e && e.name !== "AbortError") toast("Save to folder failed", e && e.message ? e.message : String(e), "err");
  } finally {
    state.fsBusy = false;
    render();
  }
}

async function collectDirectoryFiles(dirHandle, prefix = "") {
  const out = new Map();
  for await (const [name, handle] of dirHandle.entries()) {
    const path = prefix ? `${prefix}/${name}` : name;
    if (handle.kind === "file") {
      const file = await handle.getFile();
      out.set(path, new Uint8Array(await file.arrayBuffer()));
    } else {
      const nested = await collectDirectoryFiles(handle, path);
      for (const [k, v] of nested) out.set(k, v);
    }
  }
  return out;
}

async function openProjectFromFolder() {
  if (!state.fsSupported) return;
  state.fsBusy = true;
  render();
  try {
    const dirHandle = await window.showDirectoryPicker({ mode: "readwrite" });
    const files = await collectDirectoryFiles(dirHandle);
    const { project, compiledBundle } = projectFromFileEntries(files, dirHandle.name);
    project.id = PM.newId();
    await state.projectStore.save(project);
    await state.projectStore.setActiveId(project.id);
    state.fsDirHandle = dirHandle;
    state.projects = await state.projectStore.list();
    state.project = project;
    state.compiledBundle = compiledBundle ?? null;
    state.selection = { kind: "show" };
    state.openTabs = [{ kind: "show" }];
    state.diagnostics = { show: { ok: false, err: "Not compiled yet", bundleBytes: null, loaded: null }, fdefs: {}, mdefs: {}, shows: {} };
    await reparseFdefs();
    await reparseMdefs();
    toast("Opened from folder", dirHandle.name, "ok");
    render();
  } catch (e) {
    if (e && e.name !== "AbortError") toast("Open folder failed", e && e.message ? e.message : String(e), "err");
  } finally {
    state.fsBusy = false;
    render();
  }
}

// --- device sync (§5) ----------------------------------------------------

function closeDeviceSync() {
  const ds = state.deviceSync;
  if (ds.client) ds.client.disconnect();
  ds.client = null;
  ds.sync = null;
  ds.open = false;
  ds.status = WsStatus.Closed;
  ds.fileStatus = {};
  ds.remoteOnly = [];
  ds.conflict = null;
  ds.error = null;
  render();
}

function openDeviceSync() {
  state.deviceSync.open = true;
  state.deviceSync.url = state.deviceSync.url || (typeof location !== "undefined" ? `ws://${location.hostname}/ws` : "");
  render();
}

function connectDeviceSync() {
  const ds = state.deviceSync;
  if (ds.client) ds.client.disconnect();
  ds.error = null;
  const client = new WsClient(ds.url || undefined);
  client.onStatus((status) => {
    ds.status = status;
    if (status === WsStatus.Open) reconcileAll();
    render();
  });
  ds.client = client;
  ds.sync = createDeviceSync(client);
  client.connect();
  render();
}

async function reconcileAll() {
  const ds = state.deviceSync;
  if (!ds.sync) return;
  ds.busy = true;
  render();
  try {
    const remoteNames = await ds.sync.listRemote();
    const localNames = state.project.shows.map((s) => s.name);
    const plan = planReconciliation(localNames, remoteNames);
    ds.remoteOnly = plan.deviceOnly;
    const namesToCheck = [...plan.localOnly, ...plan.both];
    const results = await Promise.all(namesToCheck.map((name) => {
      const localText = state.project.shows.find((s) => s.name === name)?.text ?? null;
      return ds.sync.checkFile(name, localText);
    }));
    ds.fileStatus = {};
    for (const r of results) ds.fileStatus[r.name] = r;
    for (const name of plan.deviceOnly) {
      ds.fileStatus[name] = { name, status: "device-only", localText: null, deviceText: null };
    }
  } catch (e) {
    ds.error = e && e.message ? e.message : String(e);
  } finally {
    ds.busy = false;
    render();
  }
}

async function pushShowToDevice(name) {
  const ds = state.deviceSync;
  const text = state.project.shows.find((s) => s.name === name)?.text ?? "";
  ds.sync.pushFile(name, text);
  await reconcileAll();
  toast("Pushed", name, "ok");
}

async function pullShowFromDevice(name) {
  const ds = state.deviceSync;
  const deviceText = await ds.sync.loadRemote(name);
  if (state.project.shows.some((s) => s.name === name)) {
    PM.setShowText(state.project, name, deviceText);
  } else {
    PM.addShow(state.project, name, deviceText);
  }
  ds.sync.recordPull(name, deviceText);
  if (state.fnlEditorForTab === name) setEditorDoc(getFnlEditorView(), deviceText);
  scheduleAutosave();
  await reconcileAll();
  toast("Pulled", name, "ok");
}

function openConflictPrompt(name) {
  const ds = state.deviceSync;
  const s = ds.fileStatus[name];
  if (!s) return;
  ds.conflict = { name, localText: s.localText, deviceText: s.deviceText };
  render();
}

async function resolveConflict(choice) {
  const ds = state.deviceSync;
  const c = ds.conflict;
  if (!c) return;
  ds.conflict = null;
  if (choice === "local") await pushShowToDevice(c.name);
  else if (choice === "device") await pullShowFromDevice(c.name);
  else render();
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
  folder: `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z"/></svg>`,
  star: `<svg width="13" height="13" viewBox="0 0 24 24" fill="currentColor" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"><polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2"/></svg>`,
  starOutline: `<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linejoin="round"><polygon points="12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2"/></svg>`,
  plug: `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M22 9.5 14.5 17a3.54 3.54 0 0 1-5 0l-2.5-2.5a3.54 3.54 0 0 1 0-5L9.5 2"/><path d="m3 21 3.5-3.5"/><path d="m18 3.5-2 2"/><path d="m20.5 6-2 2"/></svg>`,
  switch: `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><polyline points="17 1 21 5 17 9"/><path d="M3 11V9a4 4 0 0 1 4-4h14"/><polyline points="7 23 3 19 7 15"/><path d="M21 13v2a4 4 0 0 1-4 4H3"/></svg>`,
};
function icon(name, extraClass = "") {
  return el("span", { class: `icon ${extraClass}`, html: ICON[name] });
}

// --- render ------------------------------------------------------------

function render() {
  const app = $("#app");
  app.innerHTML = "";
  if (!state.project) {
    app.appendChild(el("div", { style: "padding:24px;color:#888;font-family:monospace" }, "Loading workspace…"));
    return;
  }
  app.appendChild(renderConsole());
  if (state.flash.open) app.appendChild(renderFlashModal());
  if (state.import.open) app.appendChild(renderImportModal());
  if (state.projectPanel.open) app.appendChild(renderProjectPanel());
  if (state.deviceSync.open) app.appendChild(renderDeviceSyncModal());
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
      el("div", { class: "editor-col" }, renderTabsBar(), renderEditorPane()),
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
    el(
      "button",
      { class: "btn project-switch-btn", title: "Switch / manage projects", onclick: () => { state.projectPanel.open = true; render(); } },
      icon("switch"),
      el("span", {}, state.project.meta.name),
      state.savePending && el("span", { class: "saving-dot", title: "Saving…" }, "•"),
    ),
    el("div", { class: "spacer" }),
    el(
      "button",
      { class: "btn btn-primary", onclick: compileShowNow, disabled: state.compiling || !state.wasmReady },
      state.compiling ? icon("spin") : icon("package"),
      el("span", {}, "Compile"),
    ),
    el("button", { class: "btn", onclick: () => $("#file-input").click() }, icon("upload"), el("span", {}, "Import")),
    el("button", { class: "btn", onclick: openDeviceSync }, icon("plug"), el("span", {}, "Device sync")),
    el("button", { class: "btn", onclick: openFlashModal }, icon("zap"), el("span", {}, "Flash device")),
    el("input", {
      id: "file-input",
      type: "file",
      accept: ".show,.fdef,.mdef,.fnl,.zip,.qxf,.gdtf,.json",
      multiple: "",
      style: "display:none",
      onchange: (e) => {
        if (e.target.files.length > 0) handleFiles(e.target.files);
        e.target.value = "";
      },
    }),
  );
}

// --- sidebar: project file tree (§1/§4) ---------------------------------

function renderSidebar() {
  const p = state.project;
  const list = el("div", { class: "sidebar-list" });

  list.appendChild(el("div", { class: "sidebar-section" }, "Patch (one per project)"));
  list.appendChild(
    fileRow(p.patch.show.name, sameSel(state.selection, { kind: "show" }), "show", state.diagnostics.show.ok, {
      onClick: () => openTab({ kind: "show" }),
      onDownload: () => downloadBlob(p.patch.show.name, "text/plain", p.patch.show.text),
      onCopy: () => copyText(p.patch.show.text),
    }),
  );

  list.appendChild(el("div", { class: "sidebar-section sidebar-subsection" }, "Fixture library"));
  for (const f of p.patch.fixtures) {
    const d = state.diagnostics.fdefs[f.name];
    list.appendChild(
      fileRow(f.name, sameSel(state.selection, { kind: "fdef", name: f.name }), "fdef", d?.ok, {
        onClick: () => openTab({ kind: "fdef", name: f.name }),
        onDownload: () => downloadBlob(f.name, "text/plain", f.text),
        onCopy: () => copyText(f.text),
        onRename: () => renameFileFlow("fdef", f.name, PM.renameFixture, reparseFdefs),
        onDuplicate: () => { PM.duplicateFixture(p, f.name); reparseFdefs().then(() => { scheduleAutosave(); render(); }); },
        onDelete: () => {
          PM.deleteFixture(p, f.name);
          closeTab({ kind: "fdef", name: f.name });
          reparseFdefs().then(() => { scheduleAutosave(); render(); });
        },
      }),
    );
  }
  if (p.patch.fixtures.length === 0) {
    list.appendChild(el("div", { class: "tree-empty" }, "No fixtures. Click + to add one."));
  }

  list.appendChild(el("div", { class: "sidebar-section" }, "Controllers"));
  for (const m of p.controllers) {
    const d = state.diagnostics.mdefs[m.name];
    list.appendChild(
      fileRow(m.name, sameSel(state.selection, { kind: "mdef", name: m.name }), "mdef", d?.ok, {
        onClick: () => openTab({ kind: "mdef", name: m.name }),
        onDownload: () => downloadBlob(m.name, "text/plain", m.text),
        onCopy: () => copyText(m.text),
        onRename: () => renameFileFlow("mdef", m.name, PM.renameController, reparseMdefs),
        onDuplicate: () => { PM.duplicateController(p, m.name); reparseMdefs().then(() => { scheduleAutosave(); render(); }); },
        onDelete: () => {
          PM.deleteController(p, m.name);
          closeTab({ kind: "mdef", name: m.name });
          reparseMdefs().then(() => { scheduleAutosave(); render(); });
        },
      }),
    );
  }
  if (p.controllers.length === 0) {
    list.appendChild(el("div", { class: "tree-empty" }, "No controllers. Click ♪ to add one."));
  }

  list.appendChild(el("div", { class: "sidebar-section" }, "Shows (Fennel — many per project)"));
  for (const s of p.shows) {
    const d = state.diagnostics.shows[s.name];
    const isBoot = p.bootShow === s.name;
    const row = fileRow(s.name, sameSel(state.selection, { kind: "fnl", name: s.name }), "fnl", d?.checked ? d.ok : null, {
      onClick: () => openTab({ kind: "fnl", name: s.name }),
      onDownload: () => downloadBlob(s.name, "text/plain", s.text),
      onCopy: () => copyText(s.text),
      onRename: () => renameFileFlow("fnl", s.name, PM.renameShow),
      onDuplicate: () => { PM.duplicateShow(p, s.name); scheduleAutosave(); render(); },
      onDelete: () => {
        PM.deleteShow(p, s.name);
        closeTab({ kind: "fnl", name: s.name });
        scheduleAutosave();
        render();
      },
    });
    const bootBtn = el(
      "button",
      {
        class: `boot-star ${isBoot ? "boot-star-active" : ""}`,
        title: isBoot ? "Boots on this rig — click to unmark" : "Mark as the show that boots on this rig",
        onclick: (e) => {
          e.stopPropagation();
          PM.setBootShow(p, isBoot ? null : s.name);
          scheduleAutosave();
          render();
        },
      },
      icon(isBoot ? "star" : "starOutline"),
    );
    row.insertBefore(bootBtn, row.firstChild);
    list.appendChild(row);
  }
  if (p.shows.length === 0) {
    list.appendChild(el("div", { class: "tree-empty" }, "No shows yet. Click + to add one."));
  }

  const footer = el("div", { class: "sidebar-footer" });
  if (state.diagnostics.show.ok && state.diagnostics.show.bundleBytes != null) {
    footer.appendChild(el("span", { class: "ok" }, `Last bundle: ${state.diagnostics.show.bundleBytes} bytes`));
  } else {
    footer.appendChild(el("span", {}, "Click Compile to build the bundle."));
  }
  footer.appendChild(el(
    "div",
    { class: "storage-note" },
    state.storageDurable
      ? "Autosaved to this browser (IndexedDB). Browser-local only — export a .zip or save to a folder for a real backup."
      : "⚠ This browser has no IndexedDB — work is in-memory only and lost on reload. Export a .zip often.",
  ));

  return el(
    "div",
    { class: "sidebar" },
    el(
      "div",
      { class: "sidebar-header" },
      el("span", { class: "label" }, "Project"),
      el("button", { class: "btn btn-icon", title: "Import fixture (QLC+/OFL/GDTF)", onclick: openImportModal }, icon("upload")),
      el("button", { class: "btn btn-icon", title: "New .fdef", onclick: () => {
        const name = PM.uniqueName("fixture.fdef", p.patch.fixtures.map((f) => f.name));
        PM.addFixture(p, name);
        openTab({ kind: "fdef", name });
        reparseFdefs().then(() => { scheduleAutosave(); render(); });
      } }, icon("plus")),
      el("button", { class: "btn btn-icon", title: "New .mdef", onclick: () => {
        const name = PM.uniqueName("controller.mdef", p.controllers.map((c) => c.name));
        PM.addController(p, name);
        openTab({ kind: "mdef", name });
        reparseMdefs().then(() => { scheduleAutosave(); render(); });
      } }, "♪"),
      el("button", { class: "btn btn-icon", title: "New show (.fnl)", onclick: () => {
        const name = PM.uniqueName("show.fnl", p.shows.map((s) => s.name));
        PM.addShow(p, name);
        openTab({ kind: "fnl", name });
        scheduleAutosave();
        render();
      } }, "+fnl"),
    ),
    list,
    footer,
  );
}

// opts: { onClick, onDownload, onCopy, onDelete, onRename, onDuplicate }.
// onDelete/onRename/onDuplicate are optional -- the patch's one .show file
// can't be deleted/renamed/duplicated (there's exactly one per project),
// so those rows omit them.
function fileRow(name, active, badge, ok, opts) {
  const { onClick, onDownload, onCopy, onDelete, onRename, onDuplicate } = opts;
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
      onRename && el("button", { title: "Rename", onclick: (e) => { e.stopPropagation(); onRename(); } }, "✎"),
      onDuplicate && el("button", { title: "Duplicate", onclick: (e) => { e.stopPropagation(); onDuplicate(); } }, "⧉"),
      el("button", { title: "Copy", onclick: (e) => { e.stopPropagation(); onCopy(); } }, icon("copy")),
      el("button", { title: "Download", onclick: (e) => { e.stopPropagation(); onDownload(); } }, icon("download")),
      onDelete && el("button", { class: "danger", title: "Delete", onclick: (e) => { e.stopPropagation(); onDelete(); } }, "×"),
    ),
    el("span", { class: "badge" }, badge),
  );
}

// Shared rename flow for fixtures/controllers/shows: prompts for a new
// name, applies it via the given project-model function, and keeps any
// open tab / active selection pointed at the renamed file rather than
// leaving it referencing a name that no longer exists.
function renameFileFlow(kind, oldName, renameFn, afterReparse) {
  const newName = window.prompt(`Rename "${oldName}" to:`, oldName);
  if (!newName || newName === oldName) return;
  try {
    renameFn(state.project, oldName, newName);
  } catch (e) {
    toast("Rename failed", e && e.message ? e.message : String(e), "err");
    return;
  }
  state.openTabs = state.openTabs.map((t) => (t.kind === kind && t.name === oldName ? { ...t, name: newName } : t));
  if (state.selection.kind === kind && state.selection.name === oldName) state.selection = { kind, name: newName };
  if (state.diagnostics.fdefs[oldName]) { state.diagnostics.fdefs[newName] = state.diagnostics.fdefs[oldName]; delete state.diagnostics.fdefs[oldName]; }
  if (state.diagnostics.mdefs[oldName]) { state.diagnostics.mdefs[newName] = state.diagnostics.mdefs[oldName]; delete state.diagnostics.mdefs[oldName]; }
  if (state.diagnostics.shows[oldName]) { state.diagnostics.shows[newName] = state.diagnostics.shows[oldName]; delete state.diagnostics.shows[oldName]; }
  scheduleAutosave();
  (afterReparse ? afterReparse() : Promise.resolve()).then(render);
}

// --- tabs bar (§4) --------------------------------------------------------

function renderTabsBar() {
  const bar = el("div", { class: "tabs-bar" });
  for (const t of state.openTabs) {
    const label = t.kind === "show" ? state.project.patch.show.name : t.name;
    const dirty = state.dirtyTabs.has(tabKey(t));
    bar.appendChild(el(
      "div",
      { class: `tab-chip ${sameSel(state.selection, t) ? "active" : ""}`, onclick: () => { state.selection = t; render(); } },
      el("span", { class: "tab-chip-label" }, label),
      dirty && el("span", { class: "tab-chip-dot", title: "Unsaved changes (autosaving…)" }, "•"),
      el("button", { class: "tab-chip-close", title: "Close tab", onclick: (e) => { e.stopPropagation(); closeTab(t); } }, "×"),
    ));
  }
  return bar;
}

// --- editor pane ---------------------------------------------------------

function renderEditorPane() {
  if (state.selection.kind === "fnl") return renderFnlEditorPane();

  const kindBadge = state.selection.kind === "show" ? ".show" : state.selection.kind === "mdef" ? ".mdef" : ".fdef";
  const header = el(
    "div",
    { class: "editor-header" },
    el("span", { class: "file-icon", html: ICON.file }),
    el("span", {}, currentName()),
    el("span", { class: "badge" }, kindBadge),
    el("div", { class: "spacer" }),
    el(
      "div",
      { class: "actions" },
      el("button", { title: "Copy contents", onclick: () => copyText(currentText()) }, icon("copy")),
      el("button", { title: "Download file", onclick: () => downloadBlob(currentName(), "text/plain", currentText()) }, icon("download")),
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

// .fnl shows get the shared CodeMirror editor (bracket matching, auto-
// close, Parinfer, Fennel-ish highlighting -- same component as the device
// console's REPL/editor), swapped between the currently active show's text
// as tabs change (getFnlEditorView), plus a "Check syntax" action running
// the real vendored Fennel compiler in-browser (fennel-check.js). Syntax
// check only -- glow.* is stubbed, not the real render loop.
function renderFnlEditorPane() {
  const name = activeFnlName();
  const view = getFnlEditorView();
  const d = state.diagnostics.shows[name] || {};
  const isBoot = state.project.bootShow === name;

  const header = el(
    "div",
    { class: "editor-header" },
    el("span", { class: "file-icon", html: ICON.file }),
    el("span", {}, name),
    el("span", { class: "badge" }, ".fnl"),
    isBoot && el("span", { class: "badge badge-ok" }, "BOOT"),
    el("div", { class: "spacer" }),
    el(
      "div",
      { class: "actions" },
      el(
        "button",
        { class: "btn", disabled: d.checking, onclick: () => checkFnlSyntax(name) },
        d.checking ? icon("spin") : icon("package"),
        el("span", {}, d.checking ? "Checking…" : "Check syntax"),
      ),
      el("button", { title: "Copy contents", onclick: () => copyText(getEditorDoc(view)) }, icon("copy")),
      el("button", { title: "Download", onclick: () => downloadBlob(name, "text/plain", getEditorDoc(view)) }, icon("download")),
    ),
  );

  const host = el("div", { class: "script-editor-host fnl-editor-host" });
  host.appendChild(view.dom);

  const panels = [];
  panels.push(el(
    "div",
    { class: "boot-scope-note" },
    "Syntax check only: this runs the real Fennel compiler and calls stubbed glow.* functions " +
      "once, so a compile error or a top-level typo (e.g. glow.st) is caught -- but argument " +
      "types, fixture ids, and anything inside a function that's never called at the top level " +
      "are not. It is not a dry run of the show.",
  ));
  if (d.checked) {
    panels.push(
      d.ok
        ? el("div", { class: "boot-check-ok" }, icon("ok"), " No syntax errors found.")
        : el("div", { class: "boot-check-err" }, icon("err"), " ", d.err),
    );
  }
  const lints = lintFootguns(getEditorDoc(view));
  if (lints.length > 0) {
    panels.push(el("div", { class: "lint-hints" }, ...lints.map((w) => el("div", { class: "lint-hint" }, `line ${w.line}: ${w.message}`))));
  }

  return el("div", { class: "editor-pane" }, header, host, ...panels);
}

// --- preview / diagnostics pane -------------------------------------------

function renderPreviewPane() {
  const activeTab = state._tab ?? "preview";
  const tabBtn = (name, label) =>
    el("button", { class: `tab ${activeTab === name ? "active" : ""}`, onclick: () => { state._tab = name; render(); } }, label);

  const panel = el("div", { class: "tab-panel" });
  panel.appendChild(activeTab === "preview" ? renderPreview() : renderDiagnostics());

  const footer = el("div", { class: "preview-footer" });
  footer.appendChild(
    el(
      "button",
      {
        class: "btn btn-primary",
        disabled: !state.wasmReady,
        onclick: async () => {
          const r = state.module.compileShow(state.project.patch.show.text, readPatchFile);
          if (!r.ok) { toast("Compile failed", r.err, "err"); return; }
          const bundle = vecToUint8(r.bundle);
          downloadBlob(`${slugify(state.project.meta.name)}.shw1`, "application/octet-stream", bundle);
        },
      },
      icon("download"),
      el("span", {}, "Download compiled .shw1"),
    ),
  );
  footer.appendChild(
    el("button", { class: "btn", onclick: exportActiveProjectZip }, icon("download"), el("span", {}, "Export project (.zip)")),
  );
  if (state.diagnostics.show.ok && state.diagnostics.show.bundleBytes != null) {
    footer.appendChild(el("div", { class: "bundle-info" }, `${state.diagnostics.show.bundleBytes}-byte SHW1 bundle ready`));
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
    if (!d.ok) return errorBlock("Show not compiled", d.err, "Click Compile in the top bar.");
    const s = d.loaded;
    const root = el("div", {});
    root.appendChild(section("Bundle", [el("div", {}, `${d.bundleBytes} bytes SHW1`)]));
    root.appendChild(section(`Universes (${s.universeCount})`, s.transports.map((t, i) => row(`U${i + 1}`, transportName(t)))));
    root.appendChild(section(`Fixtures (${s.fixtureCount})`, s.fixtures.length === 0
      ? [el("div", { class: "preview-empty" }, "None")]
      : s.fixtures.map((f, i) => row(`[${i}]`, `u${f.universe + 1} ch${f.base + 1}${f.isHead ? " (head)" : ""}`))));
    root.appendChild(section(`Matrices (${s.matrixCount})`, s.matrices.length === 0
      ? [el("div", { class: "preview-empty" }, "None")]
      : s.matrices.map((m, i) => row(`[${i}]`, `${m.width}×${m.height} @ u${m.startUniverse + 1} ch${m.startChannel + 1}`))));
    return root;
  }
  if (state.selection.kind === "fnl") {
    const name = state.selection.name;
    const isBoot = state.project.bootShow === name;
    const root = el("div", {});
    root.appendChild(section("Show script", [
      el("div", {}, name),
      el("div", { style: "color: var(--text-dim);" }, isBoot ? "Boots on this rig (baked into flash by the Flash step)." : "Not the boot show -- won't be baked in until marked with the star."),
    ]));
    return root;
  }
  if (state.selection.kind === "mdef") {
    const dm = state.diagnostics.mdefs[state.selection.name];
    if (!dm || !dm.ok || !dm.def) return errorBlock("Parse error", dm?.err ?? "Not parsed yet", "");
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
  const d = state.diagnostics.fdefs[state.selection.name];
  if (!d || !d.ok || !d.def) return errorBlock("Parse error", d?.err ?? "Not parsed yet", "");
  const def = d.def;
  const root = el("div", {});
  root.appendChild(section("Fixture", [
    el("div", {}, def.name),
    el("div", { style: "color: var(--text-dim);" }, `${def.footprint} channels${def.isHead ? " · moving head" : ""}`),
    def.isHead && el("div", { style: "color: var(--text-dim);" }, `pan ${def.panRangeDeg}° · tilt ${def.tiltRangeDeg}°`),
  ]));
  root.appendChild(section(`Capabilities (${def.caps.length})`, def.caps.length === 0
    ? [el("div", { class: "preview-empty" }, "No caps")]
    : def.caps.map((c) => row(
        capNameFromValue(c.capType),
        `ch${c.coarse}${c.fine !== 0xff ? `+${c.fine}` : ""}${c.defaultValue !== 0 ? ` def=${c.defaultValue}` : ""}${c.inverted ? " inv" : ""}`,
      ))));
  return root;
}

function renderDiagnostics() {
  const root = el("div", {});
  const p = state.project;
  root.appendChild(diagRow(
    p.patch.show.name, state.diagnostics.show.ok, state.diagnostics.show.err,
    state.diagnostics.show.ok && state.diagnostics.show.bundleBytes != null ? `${state.diagnostics.show.bundleBytes}-byte bundle` : null,
  ));
  for (const f of p.patch.fixtures) {
    const d = state.diagnostics.fdefs[f.name];
    root.appendChild(diagRow(f.name, d?.ok, d?.err, d?.ok ? `${d.def.caps.length} caps, footprint ${d.def.footprint}` : null));
  }
  for (const m of p.controllers) {
    const d = state.diagnostics.mdefs[m.name];
    root.appendChild(diagRow(m.name, d?.ok, d?.err, d?.ok ? `${d.def.padCount} pads, ${d.def.faderCount} faders, ${d.def.blobBytes}-byte blob` : null));
  }
  for (const s of p.shows) {
    const d = state.diagnostics.shows[s.name];
    root.appendChild(diagRow(`${s.name}${p.bootShow === s.name ? " (boot)" : ""}`, d?.checked ? d.ok : undefined, d?.err, d?.checked && d.ok ? "syntax ok" : null));
  }
  return root;
}

function section(title, children) {
  return el("div", { class: "preview-section" }, el("div", { class: "section-title" }, title), el("div", { class: "section-content" }, ...children));
}
function row(key, val) {
  return el("div", { class: "preview-row" }, el("span", { class: "key" }, key), el("span", { class: "val" }, val));
}
function errorBlock(title, detail, hint) {
  return el(
    "div",
    { class: "preview-error" },
    el("span", { class: "icon err", html: ICON.err }),
    el("div", {}, el("div", { class: "title" }, title), el("div", { class: "detail" }, detail), hint && el("div", { class: "hint" }, hint)),
  );
}
function diagRow(label, ok, err, extra) {
  return el(
    "div",
    { class: "diag-row" },
    ok === true && el("span", { class: "icon ok", html: ICON.ok }),
    ok === false && el("span", { class: "icon err", html: ICON.err }),
    ok === undefined && el("span", { class: "icon" }),
    el("div", {}, el("div", { class: "label" }, label), ok === false && err && el("div", { class: "detail" }, err), ok === true && extra && el("div", { class: "extra" }, extra)),
  );
}

// --- project switcher / manager modal (§4) --------------------------------

function renderProjectPanel() {
  const pp = state.projectPanel;
  const rows = state.projects.map((meta) => {
    const isActive = meta.id === state.project.id;
    const isRenaming = pp.renamingId === meta.id;
    return el(
      "div",
      { class: `project-row ${isActive ? "active" : ""}` },
      isRenaming
        ? el("input", {
            type: "text", value: pp.renameText, autofocus: "",
            oninput: (e) => { pp.renameText = e.target.value; },
            onkeydown: (e) => { if (e.key === "Enter") commitRename(meta.id); if (e.key === "Escape") { pp.renamingId = null; render(); } },
          })
        : el("button", { class: "project-row-name", onclick: () => { state.projectPanel.open = false; switchProject(meta.id); } }, meta.name),
      el("span", { class: "project-row-meta" }, new Date(meta.modified).toLocaleString()),
      el(
        "span",
        { class: "actions" },
        isRenaming
          ? el("button", { title: "Save", onclick: () => commitRename(meta.id) }, icon("ok"))
          : el("button", { title: "Rename", onclick: () => { pp.renamingId = meta.id; pp.renameText = meta.name; render(); } }, "✎"),
        el("button", { title: "Duplicate", onclick: () => duplicateProjectFlow(meta.id) }, icon("copy")),
        state.projects.length > 1 && el("button", { class: "danger", title: "Delete", onclick: () => deleteProjectFlow(meta.id) }, "×"),
      ),
    );
  });

  const body = [
    el("div", { class: "project-list" }, ...rows),
    el(
      "div",
      { class: "project-new-row" },
      el("input", {
        type: "text", placeholder: "New project name…", value: pp.newName,
        oninput: (e) => { pp.newName = e.target.value; },
        onkeydown: (e) => { if (e.key === "Enter") createProjectFlow(); },
      }),
      el("button", { class: "btn btn-primary", onclick: createProjectFlow }, icon("plus"), el("span", {}, "New project")),
    ),
    el("div", { class: "project-panel-divider" }),
    el(
      "div",
      { class: "project-io-row" },
      el("button", { class: "btn", onclick: exportActiveProjectZip }, icon("download"), el("span", {}, "Export active project (.zip)")),
      el("button", { class: "btn", onclick: () => $("#project-zip-input").click() }, icon("upload"), el("span", {}, "Import project (.zip)")),
      el("input", {
        id: "project-zip-input", type: "file", accept: ".zip", style: "display:none",
        onchange: (e) => { if (e.target.files.length > 0) importProjectFromFile(e.target.files[0]); e.target.value = ""; },
      }),
    ),
    el(
      "div",
      { class: "project-io-row" },
      el("button", { class: "btn", disabled: !state.fsSupported || state.fsBusy, onclick: saveActiveProjectToFolder }, icon("folder"), el("span", {}, "Save to folder")),
      el("button", { class: "btn", disabled: !state.fsSupported || state.fsBusy, onclick: openProjectFromFolder }, icon("folder"), el("span", {}, "Open folder")),
    ),
    !state.fsSupported && el("div", { class: "storage-note" }, "Open/Save to folder needs Chrome or Edge (File System Access API). Use Export/Import .zip here instead — same portability guarantee, works everywhere."),
    el("div", { class: "storage-note" }, "A project's home is this browser's storage (IndexedDB) — convenient, but browser-local and gone on a cache clear. A saved folder or an exported .zip is the real backup; that's what to hand off or keep long-term."),
  ];

  return el(
    "div",
    { class: "modal-backdrop", onclick: (e) => { if (e.target === e.currentTarget) { state.projectPanel.open = false; render(); } } },
    el(
      "div",
      { class: "modal modal-wide" },
      el(
        "div",
        { class: "modal-header" },
        icon("switch"),
        el("span", { class: "title" }, "Projects"),
        el("div", { class: "spacer" }),
        el("button", { class: "btn btn-icon", onclick: () => { state.projectPanel.open = false; render(); } }, icon("close")),
      ),
      el("div", { class: "modal-body" }, ...body),
    ),
  );
}

async function commitRename(id) {
  const pp = state.projectPanel;
  const name = pp.renameText.trim();
  if (name) {
    await state.projectStore.rename(id, name);
    if (state.project.id === id) state.project.meta.name = name;
    state.projects = await state.projectStore.list();
  }
  pp.renamingId = null;
  render();
}

async function createProjectFlow() {
  const pp = state.projectPanel;
  const name = pp.newName.trim() || "Untitled project";
  const project = await state.projectStore.create(name);
  pp.newName = "";
  state.projectPanel.open = false;
  await switchProject(project.id);
}

async function duplicateProjectFlow(id) {
  const copy = await state.projectStore.duplicate(id, `${state.projects.find((p) => p.id === id)?.name ?? "Project"} copy`);
  state.projects = await state.projectStore.list();
  render();
  await switchProject(copy.id);
}

async function deleteProjectFlow(id) {
  if (!window.confirm("Delete this project? This cannot be undone (export a .zip first if you want a backup).")) return;
  const wasActive = id === state.project.id;
  await state.projectStore.remove(id);
  state.projects = await state.projectStore.list();
  if (wasActive) {
    const next = state.projects[0];
    if (next) await switchProject(next.id);
  } else {
    render();
  }
}

// --- device sync modal (§5) ------------------------------------------------

function statusLabel(status) {
  return {
    "in-sync": "in sync", "local-only": "only in workspace", "device-only": "only on device",
    "local-changed": "workspace ahead — push", "device-changed": "device ahead — pull", "conflict": "conflict — pick one",
  }[status] || status;
}

function renderDeviceSyncModal() {
  const ds = state.deviceSync;
  const body = [];
  body.push(el(
    "div",
    { class: "sync-scope-note" },
    "Only Fennel shows sync live, over the same script_list/script_load/script_save channel the device console uses. " +
      "The patch (.show → SHW1) is a raw flashed partition — it never syncs here; use Flash device for that.",
  ));
  body.push(el(
    "div",
    { class: "import-url-row" },
    el("input", { type: "text", placeholder: "ws://192.168.1.50/ws", value: ds.url, oninput: (e) => { ds.url = e.target.value; } }),
    ds.status === WsStatus.Open
      ? el("button", { class: "btn", onclick: closeDeviceSync }, "Disconnect")
      : el("button", { class: "btn btn-primary", onclick: connectDeviceSync }, "Connect"),
  ));
  body.push(el("div", { class: `sync-status sync-status-${ds.status}` }, `status: ${ds.status}`));
  if (ds.error) body.push(errorBlock("Sync error", ds.error, ""));

  if (ds.status === WsStatus.Open) {
    body.push(el("button", { class: "btn", disabled: ds.busy, onclick: reconcileAll }, ds.busy ? icon("spin") : icon("plug"), el("span", {}, "Reconcile all")));
    const names = Object.keys(ds.fileStatus).sort();
    const rows = names.map((name) => {
      const s = ds.fileStatus[name];
      const actions = [];
      if (s.status === "local-only") actions.push(el("button", { class: "btn btn-small", onclick: () => pushShowToDevice(name) }, "Push"));
      else if (s.status === "device-only") actions.push(el("button", { class: "btn btn-small", onclick: () => pullShowFromDevice(name) }, "Pull"));
      else if (s.status === "local-changed") actions.push(el("button", { class: "btn btn-small btn-primary", onclick: () => pushShowToDevice(name) }, "Push"));
      else if (s.status === "device-changed") actions.push(el("button", { class: "btn btn-small btn-primary", onclick: () => pullShowFromDevice(name) }, "Pull"));
      else if (s.status === "conflict") actions.push(el("button", { class: "btn btn-small", onclick: () => openConflictPrompt(name) }, "Resolve…"));
      return el(
        "div",
        { class: `sync-row sync-row-${s.status}` },
        el("span", { class: "file-name" }, name),
        el("span", { class: "sync-row-status" }, statusLabel(s.status)),
        el("span", { class: "actions" }, ...actions),
      );
    });
    body.push(el("div", { class: "sync-row-list" }, ...(rows.length ? rows : [el("div", { class: "tree-empty" }, "Nothing to reconcile.")])));
  }

  if (ds.conflict) {
    body.push(el(
      "div",
      { class: "sync-conflict" },
      el("div", { class: "sync-conflict-title" }, `"${ds.conflict.name}" was edited on both sides — keep which?`),
      el("div", { class: "sync-conflict-cols" },
        el("div", { class: "sync-conflict-col" }, el("div", { class: "sync-conflict-label" }, "Workspace"), el("pre", {}, ds.conflict.localText ?? "(missing)")),
        el("div", { class: "sync-conflict-col" }, el("div", { class: "sync-conflict-label" }, "Device"), el("pre", {}, ds.conflict.deviceText ?? "(missing)"))),
      el("div", { class: "flash-footer" },
        el("button", { class: "btn", onclick: () => resolveConflict("cancel") }, "Cancel"),
        el("button", { class: "btn", onclick: () => resolveConflict("device") }, "Keep device"),
        el("button", { class: "btn btn-primary", onclick: () => resolveConflict("local") }, "Keep workspace")),
    ));
  }

  return el(
    "div",
    { class: "modal-backdrop", onclick: (e) => { if (e.target === e.currentTarget) closeDeviceSync(); } },
    el(
      "div",
      { class: "modal modal-wide" },
      el("div", { class: "modal-header" }, icon("plug"), el("span", { class: "title" }, "Device sync"), el("div", { class: "spacer" }), el("button", { class: "btn btn-icon", onclick: closeDeviceSync }, icon("close"))),
      el("div", { class: "modal-body" }, ...body),
    ),
  );
}

// --- fixture importer modal (QLC+ / OFL / GDTF) -------------------------

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
    if (modes.length === 1) im.modeName = modes[0].name;
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
  reader.onerror = () => { state.import.error = `Couldn't read ${file.name}`; render(); };
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
  const slug = (fixtureName || "fixture").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "fixture";
  return `${slug}.fdef`;
}

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
  c.unmapped = false;
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
  const finalName = PM.uniqueName(name, state.project.patch.fixtures.map((f) => f.name));
  state.project.patch.fixtures.push({ name: finalName, text: im.fdefText });
  PM.touchProject(state.project);
  openTab({ kind: "fdef", name: finalName });
  im.open = false;
  reparseFdefs().then(() => { scheduleAutosave(); render(); });
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
        el("button", { class: "btn", onclick: () => $("#import-file-input").click() }, "Choose file…"),
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
          ...im.modes.map((m) => el("option", { value: m.name, selected: im.modeName === m.name }, `${m.name} (${m.footprint}ch)`)),
        ),
      ),
    );
    if (im.error) body.push(errorBlock("Import failed", im.error, ""));
  } else if (im.stage === "table") {
    if (im.budgetDropped.length) {
      const totalDropped = im.budgetDropped.reduce((n, d) => n + d.count, 0);
      body.push(
        el(
          "div",
          { class: "import-budget-alert" },
          icon("err"),
          el(
            "div",
            {},
            el("div", { class: "import-budget-alert-title" }, `${totalDropped} named state${totalDropped === 1 ? "" : "s"} won't be saved -- the fixture profile's slot budget is full`),
            ...im.budgetDropped.map((d) => el("div", { class: "import-budget-alert-detail" }, `"${d.cap}" (offset ${d.coarse}): ${d.count} of its trailing ranges dropped, marked with strikethrough below`)),
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
        el("textarea", { class: "import-fdef-textarea", spellcheck: "false", oninput: (e) => importEditFdefText(e.target.value) }, im.fdefText),
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

  const footerButtons = [el("button", { class: "btn", disabled: im.loading, onclick: closeImportModal }, "Cancel")];
  if (im.stage === "mode") footerButtons.unshift(el("button", { class: "btn", onclick: () => { im.stage = "input"; render(); } }, "Back"));
  if (im.stage === "table") {
    footerButtons.unshift(el("button", { class: "btn", onclick: () => { im.stage = "mode"; render(); } }, "Back"));
    footerButtons.push(el("button", { class: "btn btn-primary", disabled: !im.fdefName.trim(), onclick: importSaveToLibrary }, "Add to fixture library"));
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
          { onchange: (e) => importSetRangeField(c.coarse, i, "continuous", e.target.value === "continuous") },
          el("option", { value: "discrete", selected: !r.continuous }, "SLOT (discrete)"),
          el("option", { value: "continuous", selected: r.continuous }, "RANGE (continuous)"),
        ),
        el("input", { type: "text", class: "import-range-name", value: r.name || "", oninput: (e) => importSetRangeField(c.coarse, i, "name", e.target.value) }),
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
        el("select", { onchange: (e) => importSetCap(c.coarse, e.target.value) }, ...IMPORT_CAP_NAMES.map((name) => el("option", { value: name, selected: c.cap === name }, name))),
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
  if (state.flash.busy) return;
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
      const bootShow = PM.getBootShow(state.project);
      if (!bootShow) throw new Error("No show is marked as boot -- click the star next to a show in the tree first.");
      appendFlashLog(`Building scripts-partition image from ${bootShow.name}...\n`);
      scriptsImageBytes = await buildScriptsImage(bootShow.text);
    }

    saveStoredDeviceConfig(state.devcfg);
    let devcfg;
    try {
      devcfg = deviceConfigFromForm(state.devcfg);
    } catch (e) {
      throw new Error(`Device config: ${e.message}`);
    }
    if (!devcfg.skipWifi && !devcfg.wifiSsid) {
      throw new Error("Device config: WiFi SSID is empty. Tick \"No WiFi\" if that's intentional, or fill in an SSID.");
    }
    const devcfgBytes = encodeDeviceConfig(devcfg);

    await flashDevice({
      bundleBytes,
      scriptsImageBytes,
      devcfgBytes,
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

// --- CFG1 device config form (§5) ---------------------------------------

function devcfgField(labelText, inputAttrs) {
  return el("div", { class: "devcfg-field" }, el("label", {}, labelText), el("input", inputAttrs));
}

function onDevcfgChange(key, transform) {
  return (e) => {
    state.devcfg[key] = transform ? transform(e.target.value, e.target) : e.target.value;
    render();
  };
}

function renderDevcfgForm() {
  const d = state.devcfg;
  const f = state.flash;

  return el(
    "div",
    { class: "devcfg-form" },
    el("p", { class: "devcfg-section-title" }, "Device config (written to the board's devcfg partition)"),

    el(
      "label",
      { class: "flash-check" },
      el("input", { type: "checkbox", checked: d.skipWifi, disabled: f.busy, onchange: onDevcfgChange("skipWifi", (_, target) => target.checked) }),
      el("span", {}, "No WiFi (DMX/Fennel only, no network at all)"),
    ),

    !d.skipWifi &&
      el(
        "div",
        { class: "devcfg-grid" },
        devcfgField("WiFi SSID", { type: "text", value: d.wifiSsid, maxlength: 32, disabled: f.busy, oninput: onDevcfgChange("wifiSsid") }),
        devcfgField("WiFi password", { type: "password", value: d.wifiPass, maxlength: 64, disabled: f.busy, oninput: onDevcfgChange("wifiPass") }),
      ),

    el(
      "div",
      { class: "devcfg-grid" },
      devcfgField("Art-Net fallback destination (blank = broadcast)", { type: "text", value: d.artnetFallbackIp, placeholder: "e.g. 192.168.1.50", disabled: f.busy, oninput: onDevcfgChange("artnetFallbackIp") }),
      devcfgField("Art-Net port", { type: "number", value: d.artnetPort, min: 1, max: 65535, disabled: f.busy, oninput: onDevcfgChange("artnetPort", (v) => Number(v) || 6454) }),
    ),

    el(
      "div",
      { class: "devcfg-grid" },
      devcfgField("DMX TX GPIO", { type: "number", value: d.dmxTxGpio, min: 0, max: 255, disabled: f.busy, oninput: onDevcfgChange("dmxTxGpio", (v) => Number(v) & 0xff) }),
      devcfgField("DMX RX GPIO", { type: "number", value: d.dmxRxGpio, min: 0, max: 255, disabled: f.busy, oninput: onDevcfgChange("dmxRxGpio", (v) => Number(v) & 0xff) }),
      devcfgField("DMX RTS GPIO", { type: "number", value: d.dmxRtsGpio, min: 0, max: 255, disabled: f.busy, oninput: onDevcfgChange("dmxRtsGpio", (v) => Number(v) & 0xff) }),
      devcfgField("Status LED GPIO", { type: "number", value: d.ledGpio, min: 0, max: 255, disabled: f.busy, oninput: onDevcfgChange("ledGpio", (v) => Number(v) & 0xff) }),
    ),

    el(
      "label",
      { class: "flash-check" },
      el("input", { type: "checkbox", checked: d.usbMidiHost, disabled: f.busy, onchange: onDevcfgChange("usbMidiHost", (_, target) => target.checked) }),
      el("span", {}, "Enable USB-MIDI host"),
    ),
    d.usbMidiHost && el("p", { class: "devcfg-warn" }, "Requires a board with a powered USB-A port (VBUS). Leave off unless your hardware provides it."),
  );
}

function renderFlashModal() {
  const f = state.flash;
  const supported = serialSupported() && secureContextOk();
  const bootShow = PM.getBootShow(state.project);

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
          el("input", { type: "checkbox", checked: f.eraseFirst, disabled: f.busy, onchange: (e) => { f.eraseFirst = e.target.checked; render(); } }),
          el("span", {}, "Erase flash first (recommended for a fresh/wedged board)"),
        ),
        el(
          "label",
          { class: "flash-check" },
          el("input", { type: "checkbox", checked: f.includeShow, disabled: f.busy || !state.compiledBundle, onchange: (e) => { f.includeShow = e.target.checked; render(); } }),
          el("span", {}, state.compiledBundle ? `Include my compiled show (${state.compiledBundle.byteLength} bytes)` : "Include my compiled show (Compile it first — using the CI demo show for now)"),
        ),
        el(
          "label",
          { class: "flash-check" },
          el("input", { type: "checkbox", checked: f.includeBoot, disabled: f.busy || !bootShow, onchange: (e) => { f.includeBoot = e.target.checked; render(); } }),
          el("span", {}, bootShow ? `Bake ${bootShow.name} into the scripts partition (board comes up already running this show)` : "Bake the boot show into the scripts partition (mark one with the ★ in the Shows tree first)"),
        ),
        el(
          "label",
          { class: "flash-baud" },
          el("span", {}, "Baud rate"),
          el(
            "select",
            { disabled: f.busy, onchange: (e) => { f.baud = parseInt(e.target.value, 10); render(); } },
            el("option", { value: "460800", selected: f.baud === 460800 }, "460800 (fast)"),
            el("option", { value: "115200", selected: f.baud === 115200 }, "115200 (safe)"),
          ),
        ),
      ),
    );

    body.push(renderDevcfgForm());

    if (f.chip) body.push(el("div", { class: "flash-chip" }, `Detected: ${f.chip}`));

    if (f.busy || f.fileCount > 0) {
      const pct = Math.round((f.progress || 0) * 100);
      body.push(
        el(
          "div",
          { class: "flash-progress" },
          el("div", { class: "flash-progress-label" }, f.fileCount > 0 ? `Writing part ${f.fileIndex + 1} of ${f.fileCount} — ${pct}%` : "Connecting…"),
          el("div", { class: "flash-progress-bar" }, el("div", { class: "flash-progress-fill", style: `width:${pct}%` })),
        ),
      );
    }

    if (f.log) body.push(el("pre", { class: "flash-log" }, f.log));
    if (f.error) body.push(errorBlock("Flash failed", f.error, ""));
    if (f.done) body.push(el("div", { class: "flash-done" }, icon("ok"), el("span", {}, "Done — device is rebooting.")));
  }

  const footer = el(
    "div",
    { class: "flash-footer" },
    el("button", { class: "btn", disabled: f.busy, onclick: closeFlashModal }, "Close"),
    supported &&
      el(
        "button",
        { class: "btn btn-primary", disabled: f.busy, onclick: doFlash },
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
