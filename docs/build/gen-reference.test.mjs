// gen-reference.test.mjs — unit tests for gen-reference.mjs, the docs
// completeness checker, run against small fixture strings (not the real
// source/doc files) so a source- or doc-format change that breaks
// extraction is caught here directly, rather than only showing up as a
// checker that silently stops checking anything.
//
// The property that matters most: the checker has teeth. A `glow.*` name
// (or grammar keyword) present in "source" but missing from "docs" must be
// reported as undocumented; one present in "docs" but absent from "source"
// must be reported as documenting something nonexistent; and a fixture pair
// that matches exactly must report zero problems. If any of those three
// didn't hold, `node docs/build/gen-reference.mjs --check` in CI would never
// fire on a real drift either.
//
// Run: node docs/build/gen-reference.test.mjs

import {
  extractFunctionBody,
  extractGlowApiStructure,
  namesFromGlowApiStructure,
  extractGlowApiNames,
  extractKeywords,
  buildGrammar,
  grammarKeywordsByFormat,
  extractDocumentedGlowNames,
  extractDocumentedGrammarKeywords,
  diffNames,
  checkCompleteness,
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

// --- extractGlowApiStructure / namesFromGlowApiStructure ------------------

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

int GlowLuaApi::l_set(lua_State* L) { return 0; }
int GlowLuaApi::l_cue_define(lua_State* L) { return 0; }
int GlowLuaApi::l_cue_go(lua_State* L) { return 0; }
int GlowLuaApi::l_beat_phase(lua_State* L) { return 0; }
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
  const names = extractGlowApiNames(FIXTURE_GLOW_API_CPP);
  check(
    "extractGlowApiNames produces sorted dotted names",
    JSON.stringify(names) === JSON.stringify(["glow.beat", "glow.cue.define", "glow.cue.go", "glow.set"]),
    JSON.stringify(names),
  );
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

const FIXTURE_PROVISION_CPP = `
bool parseFixtureDef(const std::string& text, FixtureDef& out, std::string& err) {
  if (cmd == "FIXTURE") {}
  else if (cmd == "CAP") {}
  return true;
}`;

{
  const grammar = buildGrammar(FIXTURE_PROVISION_CPP);
  const fdef = grammar.find((g) => g.format === ".fdef");
  check("buildGrammar extracts .fdef keywords from parseFixtureDef", JSON.stringify(fdef.keywords) === JSON.stringify(["FIXTURE", "CAP"]), JSON.stringify(fdef));
  const mdef = grammar.find((g) => g.format === ".mdef");
  check("buildGrammar reports non-extraction when a signature is absent", mdef.extracted === false && mdef.keywords.length === 0);
}

{
  const byFormat = grammarKeywordsByFormat(FIXTURE_PROVISION_CPP);
  check("grammarKeywordsByFormat keys by format", JSON.stringify(byFormat[".fdef"]) === JSON.stringify(["FIXTURE", "CAP"]), JSON.stringify(byFormat));
}

// --- extractDocumentedGlowNames -------------------------------------------

{
  const md = `
# Reference

## Fixtures

### glow.set(fixtureId, capability, value)
Sets one capability.

### \`glow.aim(fixtureId, point)\`
Aims a head.

Some prose mentioning glow.set again inline, not a heading.
`;
  const names = extractDocumentedGlowNames(md);
  check(
    "extractDocumentedGlowNames finds plain and backtick-wrapped headings, dedupes, ignores inline mentions",
    JSON.stringify(names) === JSON.stringify(["glow.aim", "glow.set"]),
    JSON.stringify(names),
  );
}

// --- extractDocumentedGrammarKeywords -------------------------------------

{
  const md = `
# Grammar

## \`.fdef\` — fixture definition

### FIXTURE
text

### \`CAP\`
text

## \`.mdef\` — controller definition

### CONTROLLER
text
`;
  const byFormat = extractDocumentedGrammarKeywords(md);
  check(
    "extractDocumentedGrammarKeywords scopes keywords to their section, handles backticks",
    JSON.stringify(byFormat[".fdef"]) === JSON.stringify(["CAP", "FIXTURE"]) && JSON.stringify(byFormat[".mdef"]) === JSON.stringify(["CONTROLLER"]),
    JSON.stringify(byFormat),
  );
  check("extractDocumentedGrammarKeywords reports an empty array for a format with no section", JSON.stringify(byFormat[".show"]) === JSON.stringify([]));
}

// --- diffNames --------------------------------------------------------------

{
  const d = diffNames(["a", "b", "c"], ["b", "c", "d"]);
  check("diffNames reports names in real but not documented as missing", JSON.stringify(d.missing) === JSON.stringify(["a"]), JSON.stringify(d));
  check("diffNames reports names in documented but not real as stale", JSON.stringify(d.stale) === JSON.stringify(["d"]), JSON.stringify(d));
}

check("diffNames reports nothing for identical sets", JSON.stringify(diffNames(["a"], ["a"])) === JSON.stringify({ missing: [], stale: [] }));

// --- checkCompleteness: the guard has teeth --------------------------------
//
// Three fixtures sharing the same "source": exactly matching docs (must
// pass clean), docs missing one real name (must fail as undocumented), and
// docs with one extra nonexistent name (must fail as documenting something
// that doesn't exist). This is the actual property CI's
// `node docs/build/gen-reference.mjs --check` depends on.

const MATCHING_REFERENCE_MD = `
### glow.set(fixtureId, capability, value)
### glow.cue.define(name, opts)
### glow.cue.go(name)
### glow.beat()
`;

const MATCHING_GRAMMAR_MD = `
## \`.fdef\` (fixture definition)

### FIXTURE
### CAP

## \`.mdef\` (controller definition)

## \`.show\` (the patch)
`;

{
  const errors = checkCompleteness({
    glowLuaApiCppSrc: FIXTURE_GLOW_API_CPP,
    referenceMd: MATCHING_REFERENCE_MD,
    provisionCppSrc: FIXTURE_PROVISION_CPP,
    grammarMd: MATCHING_GRAMMAR_MD,
  });
  check("checkCompleteness reports nothing when docs match source exactly", errors.length === 0, JSON.stringify(errors));
}

{
  // glow.beat exists in source but drop it from the docs fixture -- the
  // checker must catch the omission, not silently pass.
  const referenceMissingBeat = MATCHING_REFERENCE_MD.replace("### glow.beat()\n", "");
  const errors = checkCompleteness({
    glowLuaApiCppSrc: FIXTURE_GLOW_API_CPP,
    referenceMd: referenceMissingBeat,
    provisionCppSrc: FIXTURE_PROVISION_CPP,
    grammarMd: MATCHING_GRAMMAR_MD,
  });
  check(
    "checkCompleteness catches a glow.* name present in code but missing from docs/reference.md",
    errors.some((e) => e.includes("undocumented API: glow.beat")),
    JSON.stringify(errors),
  );
}

{
  // A name documented that was never registered in source -- the stale
  // direction, e.g. a renamed/removed function whose doc entry lingers.
  const referenceWithStaleName = MATCHING_REFERENCE_MD + "\n### glow.nonexistent()\n";
  const errors = checkCompleteness({
    glowLuaApiCppSrc: FIXTURE_GLOW_API_CPP,
    referenceMd: referenceWithStaleName,
    provisionCppSrc: FIXTURE_PROVISION_CPP,
    grammarMd: MATCHING_GRAMMAR_MD,
  });
  check(
    "checkCompleteness catches a glow.* name documented but absent from source",
    errors.some((e) => e.includes("documents nonexistent glow.nonexistent")),
    JSON.stringify(errors),
  );
}

{
  // Same two directions, for grammar keywords: FOOTPRINT is never added to
  // GRAMMAR_SOURCES' .fdef body in this fixture pairing test, so use CAP's
  // removal/an invented keyword instead.
  const grammarMissingCap = MATCHING_GRAMMAR_MD.replace("### CAP\n", "");
  const errorsMissing = checkCompleteness({
    glowLuaApiCppSrc: FIXTURE_GLOW_API_CPP,
    referenceMd: MATCHING_REFERENCE_MD,
    provisionCppSrc: FIXTURE_PROVISION_CPP,
    grammarMd: grammarMissingCap,
  });
  check(
    "checkCompleteness catches a grammar keyword present in code but missing from docs/grammar.md",
    errorsMissing.some((e) => e.includes("undocumented grammar keyword: CAP (.fdef")),
    JSON.stringify(errorsMissing),
  );

  const grammarWithStaleKeyword = MATCHING_GRAMMAR_MD.replace("### FIXTURE\n", "### FIXTURE\n### GHOST\n");
  const errorsStale = checkCompleteness({
    glowLuaApiCppSrc: FIXTURE_GLOW_API_CPP,
    referenceMd: MATCHING_REFERENCE_MD,
    provisionCppSrc: FIXTURE_PROVISION_CPP,
    grammarMd: grammarWithStaleKeyword,
  });
  check(
    "checkCompleteness catches a grammar keyword documented but absent from source",
    errorsStale.some((e) => e.includes("documents nonexistent grammar keyword: GHOST (.fdef")),
    JSON.stringify(errorsStale),
  );
}

// --- checkCompleteness against the REAL repo files ------------------------
//
// The fixture-based tests above prove the mechanism has teeth; this proves
// the real docs/reference.md and docs/grammar.md in this repo actually
// satisfy it right now -- the same check CI runs via --check, run here as
// an ordinary unit test so `node docs/build/gen-reference.test.mjs` alone
// catches a real drift, not just the fixture-based teeth tests above.

{
  const { readFileSync } = await import("node:fs");
  const { join, dirname } = await import("node:path");
  const { fileURLToPath } = await import("node:url");
  const REPO_ROOT = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

  const errors = checkCompleteness({
    glowLuaApiCppSrc: readFileSync(join(REPO_ROOT, "glow_lua_api.cpp"), "utf8"),
    referenceMd: readFileSync(join(REPO_ROOT, "docs", "reference.md"), "utf8"),
    provisionCppSrc: readFileSync(join(REPO_ROOT, "provision.cpp"), "utf8"),
    grammarMd: readFileSync(join(REPO_ROOT, "docs", "grammar.md"), "utf8"),
  });
  check("the real docs/reference.md and docs/grammar.md are complete against the real source", errors.length === 0, JSON.stringify(errors));
}

console.log(`\n${count - failures}/${count} checks passed.`);
if (failures > 0) {
  console.log(`${failures} FAILURE(S)`);
  process.exit(1);
}
