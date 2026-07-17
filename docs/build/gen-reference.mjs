#!/usr/bin/env node
//
// gen-reference.mjs — the anti-drift guard for esp-glow's hand-written docs.
//
// This used to generate the `glow.*` API reference and the text-format
// grammar reference as prose, straight from source. That prose was
// unreadable: `(glow.set fixture integer number)` doesn't tell anyone that
// arg 2 is a capability like `:dimmer` or that arg 3 is normalized 0..1 --
// that meaning lives in a human's head, not in a `luaL_check*` call, and no
// amount of clever regex extraction fixes that. See docs/authoring.md and
// docs/reference.md, hand-written, for the replacement.
//
// What's kept is the actual anti-drift value, in a form that survives
// hand-written prose: this script still extracts the REAL `glow.*` names
// (from glow_lua_api.cpp's `registerFn` calls) and the REAL text-format
// grammar keywords (from provision.cpp's `cmd == "..."` checks), then
// checks that docs/reference.md and docs/grammar.md document EXACTLY that
// set -- nothing missing, nothing stale. Adding `glow.foo` to the engine
// without documenting it fails the build; documenting a `glow.bar` that
// doesn't exist also fails the build. The words are human; the coverage is
// enforced.
//
// Usage:
//   node docs/build/gen-reference.mjs [--check]
//
//   (no flags)  writes docs/generated/glow-api-names.json from a fresh
//               extraction, then runs the completeness check.
//   --check     doesn't write; fails if docs/generated/glow-api-names.json
//               would differ from a fresh extraction (the CI drift guard
//               uses this so a stale committed copy can't merge), and always
//               runs the completeness check either way.
//
// Either way, a completeness failure (in either direction, for either the
// glow.* API or the grammar keywords) exits 1 with one line per problem.
//
// Exports the extraction/diff functions so gen-reference.test.mjs can unit-
// test them directly against fixture strings -- including proving the
// completeness check actually has teeth (see that file's own header).

import { readFileSync, writeFileSync, existsSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

export const __dirname = dirname(fileURLToPath(import.meta.url));
export const REPO_ROOT = join(__dirname, "..", "..");
export const OUT_DIR = join(REPO_ROOT, "docs", "generated");

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

// Finds the source text of the first top-level function/block whose
// signature matches `signatureRe` (which must NOT include the opening
// brace), then brace-matches from that function's `{` to its closing `}`.
// Returns null if the signature isn't found. Cheap and correct for this
// codebase's style (no braces inside string/char literals within these
// specific functions), not a general C++ tokenizer.
export function extractFunctionBody(src, signatureRe) {
  // signatureRe may be a module-level constant reused across many calls
  // (see GRAMMAR_SOURCES) -- reset lastIndex so a global-flag regex always
  // searches from the start, not from wherever the previous call left off.
  signatureRe.lastIndex = 0;
  const m = signatureRe.exec(src);
  if (!m) return null;
  const openBrace = src.indexOf("{", m.index + m[0].length);
  if (openBrace === -1) return null;
  let depth = 0;
  for (let i = openBrace; i < src.length; i++) {
    if (src[i] === "{") depth++;
    else if (src[i] === "}") {
      depth--;
      if (depth === 0) {
        return { body: src.slice(openBrace + 1, i), start: m.index, end: i + 1 };
      }
    }
  }
  return null; // unbalanced -- shouldn't happen on real source
}

// ---------------------------------------------------------------------------
// glow.* API name extraction (glow_lua_api.cpp)
// ---------------------------------------------------------------------------

// Parses the `install()` table-construction block into subtable groups.
// Returns { groups: [{ name: string|null, fns: [{ luaName, cFn }] }] }
// where name === null is the top-level glow.* namespace (glow.set,
// glow.beat, ...). CAP is skipped -- it's a constant table (capability
// name -> enum value), not a callable, so it has no doc entry to check.
export function extractGlowApiStructure(src) {
  const installBody = extractFunctionBody(src, /void GlowLuaApi::install\(\)\s*/g);
  const text = installBody ? installBody.body : src; // fall back to whole file if install() isn't found verbatim

  const lines = text.split("\n");
  const groups = [{ name: null, fns: [] }];
  let current = groups[0];

  const newTableRe = /lua_newtable\(L\);\s*\/\/\s*glow\.([\w-]+)/;
  const registerRe = /registerFn\(L,\s*(?:-1|glowIdx),\s*"([^"]+)",\s*&GlowLuaApi::(l_\w+)/;
  const setFieldRe = /lua_setfield\(L,\s*glowIdx,\s*"([\w-]+)"\)/;

  for (const line of lines) {
    const nt = newTableRe.exec(line);
    if (nt && nt[1] !== "CAP") {
      current = { name: nt[1], fns: [] };
      groups.push(current);
      continue;
    }
    const reg = registerRe.exec(line);
    if (reg) {
      current.fns.push({ luaName: reg[1], cFn: reg[2] });
      continue;
    }
    const sf = setFieldRe.exec(line);
    if (sf) {
      current = groups[0]; // back to top-level until the next lua_newtable
    }
  }
  return { groups: groups.filter((g) => g.fns.length > 0) };
}

// Flat, sorted list of dotted `glow.*` names actually registered in source
// (`glow.set`, `glow.cue.define`, ...) -- the ground truth the completeness
// check and glow-api-names.json are both built from.
export function namesFromGlowApiStructure({ groups }) {
  const names = [];
  for (const g of groups) {
    for (const fn of g.fns) {
      names.push(g.name === null ? `glow.${fn.luaName}` : `glow.${g.name}.${fn.luaName}`);
    }
  }
  return names.sort();
}

export function extractGlowApiNames(glowLuaApiCppSrc) {
  return namesFromGlowApiStructure(extractGlowApiStructure(glowLuaApiCppSrc));
}

// ---------------------------------------------------------------------------
// Text-format grammar keyword extraction (provision.cpp)
// ---------------------------------------------------------------------------

// One entry per (format, parsing function) pair we know about. `format` is
// the file extension a reader recognizes; `fnSignature` locates the
// function whose body we brace-match and scan for `cmd == "KEYWORD"`.
const GRAMMAR_SOURCES = [
  { format: ".fdef", fnName: "parseFixtureDef", fnSignature: /bool parseFixtureDef\(const std::string& text, FixtureDef& out, std::string& err\)\s*/g },
  { format: ".mdef", fnName: "parseControllerDef", fnSignature: /bool parseControllerDef\(const std::string& text, ControllerBuilder& out, std::string& err\)\s*/g },
  { format: ".show", fnName: "compileShow", fnSignature: /CompileResult compileShow\(const std::string& showText,[\s\S]*?\)\s*/g },
];

// Extracts `cmd == "KEYWORD"` keywords from one function body, in the order
// they first appear (a keyword may be tested more than once, e.g. `cmd ==
// "RANGE"` also appears in a later boolean expression -- dedupe by name,
// keep first-seen order).
export function extractKeywords(body) {
  const re = /cmd\s*==\s*"(\w+)"/g;
  const seen = new Set();
  const out = [];
  let m;
  while ((m = re.exec(body))) {
    if (!seen.has(m[1])) {
      seen.add(m[1]);
      out.push(m[1]);
    }
  }
  return out;
}

export function buildGrammar(provisionCppSrc) {
  return GRAMMAR_SOURCES.map(({ format, fnName, fnSignature }) => {
    const fnBody = extractFunctionBody(provisionCppSrc, fnSignature);
    return {
      format,
      fnName,
      keywords: fnBody ? extractKeywords(fnBody.body) : [],
      extracted: fnBody !== null,
    };
  });
}

// { ".fdef": ["FIXTURE", ...], ".mdef": [...], ".show": [...] }
export function grammarKeywordsByFormat(provisionCppSrc) {
  const out = {};
  for (const f of buildGrammar(provisionCppSrc)) out[f.format] = f.keywords;
  return out;
}

// ---------------------------------------------------------------------------
// Documented-name extraction (docs/reference.md, docs/grammar.md)
// ---------------------------------------------------------------------------

// docs/reference.md documents each function as a heading of the form
// `### glow.foo(args...)` (optionally backtick-wrapped). Returns the set of
// dotted names actually documented, deduplicated.
export function extractDocumentedGlowNames(referenceMd) {
  const re = /^###\s+`?(glow\.[A-Za-z0-9_.?-]+)\(/gm;
  const out = new Set();
  let m;
  while ((m = re.exec(referenceMd))) out.add(m[1]);
  return [...out].sort();
}

// docs/grammar.md documents each format as a `## ... .fdef|.mdef|.show ...`
// section, with one `### KEYWORD` (optionally backtick-wrapped) heading per
// keyword inside that section, up to the next `## ` heading. Returns
// { ".fdef": [...], ".mdef": [...], ".show": [...] }, deduplicated.
export function extractDocumentedGrammarKeywords(grammarMd) {
  const lines = grammarMd.split("\n");
  const out = { ".fdef": new Set(), ".mdef": new Set(), ".show": new Set() };
  let currentFormat = null;

  for (const line of lines) {
    const section = /^##\s+(.*)$/.exec(line);
    if (section) {
      const fmt = /\.(fdef|mdef|show)\b/.exec(section[1]);
      currentFormat = fmt ? `.${fmt[1]}` : null;
      continue;
    }
    if (currentFormat === null) continue;
    const kw = /^###\s+`?([A-Z][A-Z0-9_]*)`?\s*$/.exec(line);
    if (kw) out[currentFormat].add(kw[1]);
  }

  return Object.fromEntries(Object.entries(out).map(([k, v]) => [k, [...v].sort()]));
}

// ---------------------------------------------------------------------------
// Completeness diff
// ---------------------------------------------------------------------------

// { missing: names in `real` but not `documented`, stale: names in
// `documented` but not `real` } -- both sorted arrays.
export function diffNames(real, documented) {
  const realSet = new Set(real);
  const docSet = new Set(documented);
  return {
    missing: real.filter((n) => !docSet.has(n)).sort(),
    stale: documented.filter((n) => !realSet.has(n)).sort(),
  };
}

// Runs the full completeness check: glow.* API names (glow_lua_api.cpp vs
// docs/reference.md) and grammar keywords per format (provision.cpp vs
// docs/grammar.md). Returns a flat array of human-readable error strings;
// empty means clean.
export function checkCompleteness({ glowLuaApiCppSrc, referenceMd, provisionCppSrc, grammarMd }) {
  const errors = [];

  const realNames = extractGlowApiNames(glowLuaApiCppSrc);
  const docNames = extractDocumentedGlowNames(referenceMd);
  const apiDiff = diffNames(realNames, docNames);
  for (const n of apiDiff.missing) errors.push(`undocumented API: ${n} (add it to docs/reference.md)`);
  for (const n of apiDiff.stale) errors.push(`documents nonexistent ${n} (remove it from docs/reference.md)`);

  const realKeywords = grammarKeywordsByFormat(provisionCppSrc);
  const docKeywords = extractDocumentedGrammarKeywords(grammarMd);
  for (const format of Object.keys(realKeywords)) {
    const kwDiff = diffNames(realKeywords[format], docKeywords[format] || []);
    for (const k of kwDiff.missing) errors.push(`undocumented grammar keyword: ${k} (${format}, add it to docs/grammar.md)`);
    for (const k of kwDiff.stale) errors.push(`documents nonexistent grammar keyword: ${k} (${format}, remove it from docs/grammar.md)`);
  }

  return errors;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

function namesJson(glowLuaApiCppSrc) {
  return JSON.stringify({ names: extractGlowApiNames(glowLuaApiCppSrc) }, null, 2) + "\n";
}

function loadSources() {
  return {
    glowLuaApiCppSrc: readFileSync(join(REPO_ROOT, "glow_lua_api.cpp"), "utf8"),
    provisionCppSrc: readFileSync(join(REPO_ROOT, "provision.cpp"), "utf8"),
    referenceMd: readFileSync(join(REPO_ROOT, "docs", "reference.md"), "utf8"),
    grammarMd: readFileSync(join(REPO_ROOT, "docs", "grammar.md"), "utf8"),
  };
}

function main() {
  const check = process.argv.includes("--check");
  const sources = loadSources();
  let failed = false;

  const namesPath = join(OUT_DIR, "glow-api-names.json");
  const freshNames = namesJson(sources.glowLuaApiCppSrc);
  if (check) {
    const existing = existsSync(namesPath) ? readFileSync(namesPath, "utf8") : null;
    if (existing !== freshNames) {
      console.error("DRIFT: docs/generated/glow-api-names.json differs from a fresh extraction.");
      console.error("Run `node docs/build/gen-reference.mjs` and commit the result.");
      failed = true;
    }
  } else {
    mkdirSync(OUT_DIR, { recursive: true });
    writeFileSync(namesPath, freshNames);
    console.log("wrote docs/generated/glow-api-names.json");
  }

  const errors = checkCompleteness(sources);
  if (errors.length > 0) {
    console.error(`\n${errors.length} completeness problem(s):`);
    for (const e of errors) console.error(`::error::${e}`);
    failed = true;
  } else {
    console.log("docs/reference.md and docs/grammar.md cover exactly the real glow.* API and grammar.");
  }

  if (failed) process.exit(1);
}

if (process.argv[1] && import.meta.url === `file://${process.argv[1]}`) {
  main();
}
