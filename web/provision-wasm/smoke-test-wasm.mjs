// smoke-test-wasm.mjs — verify the editor WASM module exports the right
// functions and can round-trip a small .fdef + .show.
//
// Run: node web/provision-wasm/smoke-test-wasm.mjs

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

// The editor build is ESM (EXPORT_ES6=1, MODULARIZE=1). Import it.
const modPath = join(__dirname, "provision-wasm.js");
const createModule = (await import("file://" + modPath)).default;

// Under Node, the module's web-target fetch() can't load the .wasm via
// relative path. Provide locateFile returning a file:// URL so fetch()
// in Node 24 can resolve it.
const wasmPath = join(__dirname, "provision-wasm.wasm");
const Module = await createModule({
  locateFile: (p) => "file://" + join(__dirname, p),
});

// Emscripten returns std::vector<T> as typed-vector wrapper objects
// (IntVector, Uint8Vector, BoolVector) with size()/get() methods, NOT
// as JS arrays. Convert them once at the boundary so the rest of the
// test (and the editor) can use normal indexing / .length / spread.
function vecToArray(v) {
  if (v == null) return [];
  if (Array.isArray(v)) return v;
  if (typeof v.size === "function" && typeof v.get === "function") {
    const n = v.size();
    const out = new Array(n);
    for (let i = 0; i < n; i++) out[i] = v.get(i);
    return out;
  }
  // Already array-like (Uint8Array etc.).
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

let failures = 0;
function check(name, cond, actual) {
  if (!cond) { console.log("  FAIL:", name, actual !== undefined ? "actual=" + JSON.stringify(actual) : ""); failures++; }
  else        console.log("  ok:  ", name);
}

console.log("== exports ==");
check("parseFixtureDef exported", typeof Module.parseFixtureDef === "function");
check("encodeProfile exported",   typeof Module.encodeProfile   === "function");
check("compileShow exported",     typeof Module.compileShow     === "function");
check("loadShow exported",        typeof Module.loadShow        === "function");
check("parseController exported", typeof Module.parseController === "function");

console.log("== parseFixtureDef (valid) ==");
const fdefText = `FIXTURE Torrent F1
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
`;
const parsed = Module.parseFixtureDef(fdefText);
parsed.capTypes    = vecToArray(parsed.capTypes);
parsed.capCoarse   = vecToArray(parsed.capCoarse);
parsed.capFine     = vecToArray(parsed.capFine);
parsed.capDefault  = vecToArray(parsed.capDefault);
parsed.capFlags    = vecToArray(parsed.capFlags);
check("ok=true", parsed.ok === true);
check("err empty", parsed.err === "");
check("name", parsed.name === "Torrent F1");
check("footprint", parsed.footprint === 16);
check("isHead", parsed.isHead === true);
check("panRangeDeg", parsed.panRangeDeg === 540);
check("tiltRangeDeg", parsed.tiltRangeDeg === 270);
check("7 caps", parsed.capTypes.length === 7);
check("cap[0] is Dimmer (0)", parsed.capTypes[0] === 0);
check("cap[6] is ShutterStrobe (12)", parsed.capTypes[6] === 12);
check("cap[6] default 8", parsed.capDefault[6] === 8);
check("cap[6] flags bit0 set", (parsed.capFlags[6] & 1) === 1);
check("cap[4] Pan coarse=5 fine=6", parsed.capCoarse[4] === 5 && parsed.capFine[4] === 6);
check("cap[0] Dimmer fine=0xFF", parsed.capFine[0] === 0xFF);

console.log("== parseFixtureDef (invalid) ==");
const bad = Module.parseFixtureDef("FIXTURE X\nFOOTPRINT 0\n");
check("ok=false on footprint 0", bad.ok === false);
check("err non-empty", bad.err.length > 0);

console.log("== encodeProfile ==");
const blob = vecToUint8(Module.encodeProfile(
  "Torrent F1", 16, true, 540, 270,
  parsed.capTypes, parsed.capCoarse, parsed.capFine,
  parsed.capDefault, parsed.capFlags
));
check("blob is Uint8Array-like", blob && typeof blob.length === "number");
check("blob size = 54 bytes (9 + 10-char name + 7 caps × 5)", blob.length === 54, blob.length);
check("magic PFX1", String.fromCharCode(blob[0], blob[1], blob[2], blob[3]) === "PFX1");
check("version 1", blob[4] === 1);
check("footprint 16", blob[6] === 16);
check("capCount 7", blob[7] === 7);
check("nameLen 10 ('Torrent F1' is 10 chars)", blob[8] === 10, blob[8]);

console.log("== compileShow ==");
const showText = `UNIVERSE 0 DMX
UNIVERSE 1 ARTNET
FIXTURE torrent.fdef 0 1
POS 1 2 3
ROT 0 0 0
FIXTURE par.fdef 0 20
MATRIX 1 0 16 16 SERP H GRB
`;
const fdefTorrent = `FIXTURE Torrent F1
FOOTPRINT 16
HEAD
PANRANGE 540
TILTRANGE 270
CAP Dimmer 0
CAP Pan 5 6
CAP Tilt 7 8
`;
const fdefPar = `FIXTURE Par 5
FOOTPRINT 5
CAP Dimmer 0
CAP Red 1
CAP Green 2
CAP Blue 3
`;
const readFile = (path) => {
  if (path === "torrent.fdef") return fdefTorrent;
  if (path === "par.fdef")     return fdefPar;
  return "";
};
const compiled = Module.compileShow(showText, readFile);
compiled.bundle = vecToUint8(compiled.bundle);
check("ok=true", compiled.ok === true);
check("err empty", compiled.err === "");
check("bundle non-empty", compiled.bundle.length > 0);
check("magic SHW1", String.fromCharCode(compiled.bundle[0], compiled.bundle[1], compiled.bundle[2], compiled.bundle[3]) === "SHW1");
check("version 1", compiled.bundle[4] === 1);

console.log("== loadShow (round-trip) ==");
const loaded = Module.loadShow(compiled.bundle);
loaded.transports       = vecToArray(loaded.transports);
loaded.fixProfileIndex  = vecToArray(loaded.fixProfileIndex);
loaded.fixUniverse      = vecToArray(loaded.fixUniverse);
loaded.fixBase          = vecToArray(loaded.fixBase);
loaded.fixIsHead        = vecToArray(loaded.fixIsHead);
loaded.matWidth         = vecToArray(loaded.matWidth);
loaded.matHeight        = vecToArray(loaded.matHeight);
loaded.matStartUniverse = vecToArray(loaded.matStartUniverse);
loaded.matStartChannel  = vecToArray(loaded.matStartChannel);
check("ok=true", loaded.ok === true);
check("universeCount=2", loaded.universeCount === 2);
check("transport[0]=Dmx (0)", loaded.transports[0] === 0);
check("transport[1]=ArtNet (1)", loaded.transports[1] === 1, loaded.transports);
check("fixtureCount=2", loaded.fixtureCount === 2);
check("matrixCount=1", loaded.matrixCount === 1);
check("matrix 16x16", loaded.matWidth[0] === 16 && loaded.matHeight[0] === 16);

console.log("== parseController (valid .mdef) ==");
const mdefText = `CONTROLLER Test Pad
MIDI_CHANNEL 0
PAD 0 7
FADER CC 7 level
ENCODER CC 13 relative-2c
LED NOTE 0 7 velocity
  COLOR off 0
  COLOR on  1
`;
const ctrl = Module.parseController(mdefText);
check("ctrl ok=true", ctrl.ok === true, ctrl.err);
check("ctrl err empty", ctrl.err === "");
check("ctrl name", ctrl.name === "Test Pad");
check("ctrl midiChannel 0", ctrl.midiChannel === 0);
check("ctrl padCount 1", ctrl.padCount === 1);
check("ctrl faderCount 1", ctrl.faderCount === 1);
check("ctrl encoderCount 1", ctrl.encoderCount === 1);
check("ctrl ledCount 1", ctrl.ledCount === 1);
check("ctrl colorCount 2", ctrl.colorCount === 2);
check("ctrl blobBytes > 0", ctrl.blobBytes > 0, ctrl.blobBytes);

console.log("== parseController (invalid) ==");
const badCtrl = Module.parseController("MIDI_CHANNEL 0\nPAD 0 7\n");
check("ctrl ok=false without CONTROLLER", badCtrl.ok === false);
check("ctrl err non-empty", badCtrl.err.length > 0);

console.log("== compileShow with CONTROLLER ==");
const showWithCtrl = `UNIVERSE 0 DMX
FIXTURE par.fdef 0 1
CONTROLLER pad.mdef
`;
const readFile2 = (path) => {
  if (path === "par.fdef") return fdefPar;
  if (path === "pad.mdef") return mdefText;
  return "";
};
const compiledCtrl = Module.compileShow(showWithCtrl, readFile2);
check("ctrl-show ok=true", compiledCtrl.ok === true, compiledCtrl.err);
check("ctrl-show bundle non-empty", vecToUint8(compiledCtrl.bundle).length > 0);

console.log("== bundle size meter ==");
console.log("    bundle size:", compiled.bundle.length, "bytes");
console.log("    (header 11 + universe 1 + profile table + fixture table + matrix table)");

console.log("");
if (failures === 0) console.log("All WASM smoke tests passed.");
else { console.log(failures + " test(s) failed."); process.exit(1); }
