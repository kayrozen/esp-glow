// gen-reference.test.mjs — unit tests for gen-reference.mjs's extraction
// regexes, run against small fixture strings (not the real source files) so
// a source-format change that breaks extraction is caught here directly,
// rather than only showing up as an empty/wrong generated page later.
//
// Also proves the anti-drift guard has teeth: mutates a fixture "source
// file" (a name a script would extract) and asserts the generator's output
// actually changes -- if it didn't, `git diff --exit-code docs/generated/`
// in CI would never fire on a real drift either.
//
// Run: node docs/build/gen-reference.test.mjs

import {
  extractFunctionBody,
  inferArgs,
  extractGlowApiStructure,
  extractCapabilities,
  extractMatrixPatterns,
  buildGlowApi,
  extractKeywords,
  buildGrammar,
  extractActionKinds,
  extractTestSuiteCount,
  generateAll,
} from "./gen-reference.mjs";

let failures = 0;
let count = 0;
function check(name, cond, detail) {
  count++;
  if (!cond) {
    failures++;
    console.error(`FAIL: ${name}${detail !== undefined ? " -- " + detail : ""}`);
  } else {
    console.log(`PASS: ${name}`);
  }
}

// --- extractFunctionBody -----------------------------------------------

{
  const src = `int foo(lua_State* L) {\n  if (x) { return 1; }\n  return 0;\n}\nint bar() {}`;
  const result = extractFunctionBody(src, /int foo\(lua_State\* L\)\s*/g);
  check("extractFunctionBody finds the matching close brace", result !== null && result.body.includes("return 0;"));
  check("extractFunctionBody doesn't overrun into the next function", !result.body.includes("int bar"));
}

check("extractFunctionBody returns null for an absent signature", extractFunctionBody("int foo() {}", /int missing\(\)\s*/g) === null);

// --- inferArgs -----------------------------------------------------------

{
  const body = `
    uint16_t fid = checkFixtureId(L, 1);
    double v = luaL_checknumber(L, 2);
  `;
  const args = inferArgs(body);
  check("inferArgs reads recognized helpers in stack-index order", JSON.stringify(args) === JSON.stringify(["fixture", "number"]), JSON.stringify(args));
}

{
  // Ambiguous: index 2 touched by two different recognized helpers across
  // branches -- must fall back to the generic "value", not silently pick one.
  const body = `
    uint16_t fid = checkFixtureId(L, 1);
    if (x) { double v = luaL_checknumber(L, 2); } else { luaL_checkstring(L, 2); }
  `;
  const args = inferArgs(body);
  check("inferArgs falls back to generic 'value' on ambiguous branches", args[1] === "value", JSON.stringify(args));
}

{
  const body = `
    uint16_t fid = checkFixtureId(L, 1);
    double b = 0.5;
    if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) { b = luaL_checknumber(L, 2); }
  `;
  const args = inferArgs(body);
  check("inferArgs marks a guarded trailing argument optional", args[1] === "number?", JSON.stringify(args));
}

check("inferArgs returns null when nothing recognizable is present", inferArgs("return 0;") === null);

// --- extractGlowApiStructure / extractCapabilities / extractMatrixPatterns

const FIXTURE_GLOW_API_CPP = `
constexpr CapName kCapNames[] = {
    {"dimmer", Capability::Dimmer},
    {"red", Capability::Red},
};

void GlowLuaApi::install() {
  lua_newtable(L);  // glow
  int glowIdx = lua_gettop(L);

  registerFn(L, glowIdx, "set", &GlowLuaApi::l_set, this);

  lua_newtable(L);  // glow.cue
  registerFn(L, -1, "define", &GlowLuaApi::l_cue_define, this);
  registerFn(L, -1, "go", &GlowLuaApi::l_cue_go, this);
  lua_setfield(L, glowIdx, "cue");

  registerFn(L, glowIdx, "beat", &GlowLuaApi::l_beat_phase, this);

  lua_setglobal(L, "glow");
}

int GlowLuaApi::l_set(lua_State* L) {
  uint16_t fid = checkFixtureId(L, 1);
  return 0;
}
int GlowLuaApi::l_cue_define(lua_State* L) { return 0; }
int GlowLuaApi::l_cue_go(lua_State* L) { return 0; }
int GlowLuaApi::l_beat_phase(lua_State* L) { return 0; }

  if (std::strcmp(patName, "plasma") == 0) {}
  else if (std::strcmp(patName, "rainbow") == 0) {}
`;

{
  const { groups } = extractGlowApiStructure(FIXTURE_GLOW_API_CPP);
  const top = groups.find((g) => g.name === null);
  const cue = groups.find((g) => g.name === "cue");
  check("extractGlowApiStructure finds the top-level group", top && top.fns.some((f) => f.luaName === "set"));
  check("extractGlowApiStructure finds top-level fns registered after a subtable closes", top && top.fns.some((f) => f.luaName === "beat"));
  check("extractGlowApiStructure groups cue.define/cue.go under 'cue'", cue && cue.fns.length === 2, JSON.stringify(cue));
  check("extractGlowApiStructure does not create a group for CAP", !groups.some((g) => g.name === "CAP"));
}

{
  const caps = extractCapabilities(FIXTURE_GLOW_API_CPP);
  check("extractCapabilities parses the kCapNames array", caps.length === 2 && caps[0].name === "dimmer" && caps[0].enumMember === "Dimmer", JSON.stringify(caps));
}

{
  const patterns = extractMatrixPatterns(FIXTURE_GLOW_API_CPP);
  check("extractMatrixPatterns parses the strcmp chain", JSON.stringify(patterns) === JSON.stringify(["plasma", "rainbow"]), JSON.stringify(patterns));
}

{
  const api = buildGlowApi(FIXTURE_GLOW_API_CPP);
  const setFn = api.groups.find((g) => g.name === null).fns.find((f) => f.luaName === "set");
  check("buildGlowApi attaches inferred args to each function", JSON.stringify(setFn.args) === JSON.stringify(["fixture"]), JSON.stringify(setFn));
}

// --- grammar keyword extraction ------------------------------------------

{
  const body = `
    if (cmd == "FIXTURE") {
    } else if (cmd == "FOOTPRINT") {
    } else if (cmd == "FIXTURE") { // seen again later, must not duplicate
    }
  `;
  const keywords = extractKeywords(body);
  check("extractKeywords dedupes repeated keywords, keeps first-seen order", JSON.stringify(keywords) === JSON.stringify(["FIXTURE", "FOOTPRINT"]), JSON.stringify(keywords));
}

{
  const src = `
bool parseFixtureDef(const std::string& text, FixtureDef& out, std::string& err) {
  if (cmd == "FIXTURE") {}
  else if (cmd == "CAP") {}
  return true;
}`;
  const grammar = buildGrammar(src);
  const fdef = grammar.find((g) => g.format === ".fdef");
  check("buildGrammar extracts .fdef keywords from parseFixtureDef", JSON.stringify(fdef.keywords) === JSON.stringify(["FIXTURE", "CAP"]), JSON.stringify(fdef));
  const mdef = grammar.find((g) => g.format === ".mdef");
  check("buildGrammar reports non-extraction when a signature is absent", mdef.extracted === false && mdef.keywords.length === 0);
}

// --- ActionKind -----------------------------------------------------------

{
  const src = `enum class ActionKind : uint8_t { CueFlash, CueToggle, SceneGo, SceneToggle, Master, CueLevel, ParamSet };`;
  const kinds = extractActionKinds(src);
  check("extractActionKinds parses the enum member list", kinds.length === 7 && kinds[0] === "CueFlash" && kinds[6] === "ParamSet", JSON.stringify(kinds));
}

check("extractActionKinds returns [] when the enum isn't found", JSON.stringify(extractActionKinds("// nothing here")) === "[]");

// --- test suite count -----------------------------------------------------

{
  const makefile = `
.PHONY: test

test: $(AIM_TARGET) $(FP_TARGET)
\t./$(AIM_TARGET)
\t./$(FP_TARGET)

test-importers: $(FDEF_CHECK_TARGET)
\tnode web/shared/importers/test-importers.mjs
`;
  check("extractTestSuiteCount counts only the test: recipe's invocation lines", extractTestSuiteCount(makefile) === 2, extractTestSuiteCount(makefile));
}

// --- the drift guard has teeth: renaming a registered function in the
// "source" changes the generated API reference output. This is the actual
// property the CI guard (`git diff --exit-code docs/generated/`) depends
// on -- if mutating source didn't change the generator's output, the guard
// would be a no-op that could never catch real drift.

{
  const before = renderApiOutputFor(FIXTURE_GLOW_API_CPP);
  const mutated = FIXTURE_GLOW_API_CPP.replace(/"set"/, '"set-renamed"');
  const after = renderApiOutputFor(mutated);
  check("renaming a registered glow.* function changes the generated API reference", before !== after);
  const afterNames = JSON.parse(after).groups.find((g) => g.name === null).fns.map((f) => f.luaName);
  check("the renamed function's new name appears in the regenerated output", afterNames.includes("set-renamed") && !afterNames.includes("set"), afterNames);
}

function renderApiOutputFor(glowLuaApiCppSrc) {
  const api = buildGlowApi(glowLuaApiCppSrc);
  return JSON.stringify(api);
}

// --- generateAll produces every expected file, deterministically ---------

{
  const files = generateAll({});
  const expected = ["api-reference.md", "grammar-reference.md", "enumerations.md", "test-status.md", "glow-api-names.json"];
  check("generateAll produces exactly the expected file set", expected.every((f) => f in files) && Object.keys(files).length === expected.length, Object.keys(files));
  const files2 = generateAll({});
  check("generateAll is deterministic given the same source tree", JSON.stringify(files) === JSON.stringify(files2));
}

console.log(`\n${count - failures}/${count} checks passed.`);
if (failures > 0) {
  console.log(`${failures} FAILURE(S)`);
  process.exit(1);
}
