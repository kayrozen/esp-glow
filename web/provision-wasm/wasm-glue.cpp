// wasm-glue.cpp — embind bindings for the provision compiler.
//
// Exposes four operations to JavaScript:
//
//   parseFixtureDef(text)            → FdefParseResult
//   encodeProfile(def)               → array of bytes (the PFX1 blob)
//   compileShow(showText, readFile)  → CompileResultGlue
//   loadShow(bundleBytes)            → ShowLoadResult
//
// The TS wrapper in src/lib/wasm/provision-wasm.ts is the editor's real
// API; this file is just the C++↔JS bridge. Types are kept simple (PODs
// and std::string/std::vector<uint8_t>) so embind can auto-marshal them.
//
// `readFile` is a JS callback (path → text). The .show compiler uses it
// to resolve `FIXTURE <deffile>` and `CONTROLLER <deffile>` lines. The
// editor supplies a callback that looks up .fdef/.mdef files from its
// in-memory workspace -- compileShow doesn't care which, so no glue code
// change was needed to pick up CONTROLLER (see provision.h/mdef.h).

#include "provision.h"
#include "show_bundle.h"

#include <emscripten/bind.h>
#include <emscripten/val.h>
#include <string>
#include <vector>
#include <cstdint>

using namespace emscripten;

// --- parseFixtureDef ----------------------------------------------------

struct FdefParseResult {
  bool ok;
  std::string err;
  // The FixtureDef itself, exposed as a plain object via embind value
  // object. We flatten the vector<ChannelMap> into parallel arrays so
  // embind can marshal them.
  std::string name;
  int footprint;          // 0..255
  bool isHead;
  double panRangeDeg;
  double tiltRangeDeg;
  // Parallel arrays for caps (one entry per cap).
  std::vector<int> capTypes;       // Capability enum values (0..255)
  std::vector<int> capCoarse;
  std::vector<int> capFine;        // 0xFF = no fine
  std::vector<int> capDefault;
  std::vector<int> capFlags;       // bit0 = inverted
};

FdefParseResult parseFixtureDefGlue(const std::string& text) {
  FdefParseResult r{};
  FixtureDef def;
  std::string err;
  if (!parseFixtureDef(text, def, err)) {
    r.ok = false;
    r.err = err;
    return r;
  }
  r.ok = true;
  r.err.clear();
  r.name = def.name;
  r.footprint = def.footprint;
  r.isHead = def.isHead;
  r.panRangeDeg = def.panRangeDeg;
  r.tiltRangeDeg = def.tiltRangeDeg;
  for (const auto& c : def.caps) {
    r.capTypes.push_back((int)c.cap);
    r.capCoarse.push_back((int)c.coarse);
    r.capFine.push_back((int)c.fine);
    r.capDefault.push_back((int)c.defaultValue);
    r.capFlags.push_back((int)c.flags);
  }
  return r;
}

// --- encodeProfile ------------------------------------------------------
//
// Take the same flattened shape parseFixtureDefGlue returns, build a
// FixtureDef, call encodeProfile. Used by the editor's "download PFX1"
// path and as a building block for compileShow.
//
// The cap arrays arrive as emscripten::val (JS arrays). We convert them
// to std::vector<int> manually because embind's register_vector<int>
// doesn't auto-marshal plain JS arrays.

static std::vector<int> valToIntVec(val v) {
  std::vector<int> out;
  if (v.isUndefined() || v.isNull()) return out;
  int n = v["length"].as<int>();
  out.reserve(n);
  for (int i = 0; i < n; ++i) {
    out.push_back(v[i].as<int>());
  }
  return out;
}

std::vector<uint8_t> encodeProfileGlue(
    const std::string& name, int footprint, bool isHead,
    double panRangeDeg, double tiltRangeDeg,
    val capTypesVal, val capCoarseVal, val capFineVal,
    val capDefaultVal, val capFlagsVal) {
  FixtureDef def;
  def.name = name;
  def.footprint = (uint8_t)footprint;
  def.isHead = isHead;
  def.panRangeDeg = (float)panRangeDeg;
  def.tiltRangeDeg = (float)tiltRangeDeg;

  std::vector<int> capTypes   = valToIntVec(capTypesVal);
  std::vector<int> capCoarse  = valToIntVec(capCoarseVal);
  std::vector<int> capFine    = valToIntVec(capFineVal);
  std::vector<int> capDefault = valToIntVec(capDefaultVal);
  std::vector<int> capFlags   = valToIntVec(capFlagsVal);

  size_t n = capTypes.size();
  for (size_t i = 0; i < n; ++i) {
    ChannelMap c{};
    c.cap = (Capability)capTypes[i];
    c.coarse = (uint8_t)capCoarse[i];
    c.fine = (uint8_t)capFine[i];
    c.defaultValue = (uint8_t)capDefault[i];
    c.flags = (uint8_t)capFlags[i];
    def.caps.push_back(c);
  }
  return encodeProfile(def);
}

// --- compileShow --------------------------------------------------------
//
// readFile is a JS function (path → string). We wrap it in std::function
// so the compiler's existing signature works unchanged.

struct CompileResultGlue {
  bool ok;
  std::string err;
  std::vector<uint8_t> bundle;
};

CompileResultGlue compileShowGlue(const std::string& showText, val readFileFn) {
  // Bridge: emscripten::val → std::function<std::string(const std::string&)>.
  // Calling JS from C++ via val::as<> wraps the JS function once.
  std::function<std::string(const std::string&)> readFile =
    [readFileFn](const std::string& path) -> std::string {
      val v = readFileFn(path);
      if (v.isUndefined() || v.isNull()) return "";
      return v.as<std::string>();
    };

  CompileResult result = compileShow(showText, readFile);
  CompileResultGlue g;
  g.ok = result.ok;
  g.err = result.err;
  g.bundle = std::move(result.bundle);
  return g;
}

// --- loadShow -----------------------------------------------------------
//
// Round-trip verification: take a bundle blob, parse it back, return a
// summary the editor can show next to the compiled size.

struct ShowLoadResult {
  bool ok;
  std::string err;
  int universeCount;
  std::vector<int> transports;      // per-universe transport enum
  int fixtureCount;
  int matrixCount;
  int controllerCount;  // v2 only: embedded .mdef controller definitions
  // Per-fixture summary (parallel arrays).
  std::vector<int> fixProfileIndex;
  std::vector<int> fixUniverse;
  std::vector<int> fixBase;
  std::vector<bool> fixIsHead;
  // Per-matrix summary (parallel arrays).
  std::vector<int> matWidth;
  std::vector<int> matHeight;
  std::vector<int> matStartUniverse;
  std::vector<int> matStartChannel;
};

ShowLoadResult loadShowGlue(val bundleVal) {
  // Accept Uint8Array / Int8Array / array-like from JS.
  std::vector<uint8_t> bytes;
  if (!bundleVal.isUndefined() && !bundleVal.isNull()) {
    int n = bundleVal["length"].as<int>();
    bytes.reserve(n);
    for (int i = 0; i < n; ++i) {
      bytes.push_back((uint8_t)bundleVal[i].as<int>());
    }
  }

  ShowLoadResult r{};
  LoadedShow show;
  if (!loadShow(bytes.data(), bytes.size(), show)) {
    r.ok = false;
    r.err = "loadShow: rejected bundle (bad magic/version/truncation/OOB)";
    return r;
  }
  r.ok = true;
  r.universeCount = (int)show.universeCount;
  for (int i = 0; i < r.universeCount; ++i) {
    r.transports.push_back((int)show.transport[i]);
  }
  r.fixtureCount = (int)show.fixtures.size();
  r.matrixCount = (int)show.matrices.size();
  r.controllerCount = (int)show.controllers.size();
  for (const auto& f : show.fixtures) {
    r.fixProfileIndex.push_back(0);  // profileIndex isn't on PatchEntry; we
                                      // don't expose it (not needed for the
                                      // editor's preview).
    r.fixUniverse.push_back((int)f.universe);
    r.fixBase.push_back((int)f.base);
    r.fixIsHead.push_back(f.isHead);
  }
  for (const auto& m : show.matrices) {
    r.matWidth.push_back((int)m.width);
    r.matHeight.push_back((int)m.height);
    r.matStartUniverse.push_back((int)m.startUniverse);
    r.matStartChannel.push_back((int)m.startChannel);
  }
  return r;
}

// --- embind registration ------------------------------------------------

EMSCRIPTEN_BINDINGS(provision_wasm) {
  value_object<FdefParseResult>("FdefParseResult")
    .field("ok", &FdefParseResult::ok)
    .field("err", &FdefParseResult::err)
    .field("name", &FdefParseResult::name)
    .field("footprint", &FdefParseResult::footprint)
    .field("isHead", &FdefParseResult::isHead)
    .field("panRangeDeg", &FdefParseResult::panRangeDeg)
    .field("tiltRangeDeg", &FdefParseResult::tiltRangeDeg)
    .field("capTypes", &FdefParseResult::capTypes)
    .field("capCoarse", &FdefParseResult::capCoarse)
    .field("capFine", &FdefParseResult::capFine)
    .field("capDefault", &FdefParseResult::capDefault)
    .field("capFlags", &FdefParseResult::capFlags)
    ;

  function("parseFixtureDef", &parseFixtureDefGlue);
  function("encodeProfile", &encodeProfileGlue);

  value_object<CompileResultGlue>("CompileResult")
    .field("ok", &CompileResultGlue::ok)
    .field("err", &CompileResultGlue::err)
    .field("bundle", &CompileResultGlue::bundle)
    ;
  function("compileShow", &compileShowGlue);

  value_object<ShowLoadResult>("ShowLoadResult")
    .field("ok", &ShowLoadResult::ok)
    .field("err", &ShowLoadResult::err)
    .field("universeCount", &ShowLoadResult::universeCount)
    .field("transports", &ShowLoadResult::transports)
    .field("fixtureCount", &ShowLoadResult::fixtureCount)
    .field("matrixCount", &ShowLoadResult::matrixCount)
    .field("controllerCount", &ShowLoadResult::controllerCount)
    .field("fixProfileIndex", &ShowLoadResult::fixProfileIndex)
    .field("fixUniverse", &ShowLoadResult::fixUniverse)
    .field("fixBase", &ShowLoadResult::fixBase)
    .field("fixIsHead", &ShowLoadResult::fixIsHead)
    .field("matWidth", &ShowLoadResult::matWidth)
    .field("matHeight", &ShowLoadResult::matHeight)
    .field("matStartUniverse", &ShowLoadResult::matStartUniverse)
    .field("matStartChannel", &ShowLoadResult::matStartChannel)
    ;
  function("loadShow", &loadShowGlue);

  register_vector<uint8_t>("Uint8Vector");
  register_vector<int>("IntVector");
  register_vector<bool>("BoolVector");
}
