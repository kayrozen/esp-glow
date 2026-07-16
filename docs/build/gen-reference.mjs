#!/usr/bin/env node
//
// gen-reference.mjs — generates the `glow.*` API reference, the text-format
// grammar reference, and the enumerations page straight from the source
// files that define them (glow_lua_api.cpp, provision.cpp, live_control.h),
// instead of hand-copying facts that then drift (see docs/README.md).
//
// Parses pragmatically: registerFn(...) calls and cmd == "..." keyword
// checks are regular enough to regex; this is deliberately not a C++
// parser. Where a signature can't be extracted cleanly, the entry is
// emitted with a "see source" note instead of a guessed signature --
// guessing is exactly how the docs drifted before (see the module header
// of the old fennel-check.js stub this project already learned that lesson
// from).
//
// Usage:
//   node docs/build/gen-reference.mjs [--check] [--test-status=<path/to/json>]
//
//   --check          don't write files; exit 1 if generated output would
//                     differ from what's already on disk (the CI drift
//                     guard uses `git diff --exit-code` instead, but this
//                     flag is handy for a quick local check without git).
//   --test-status    path to a small {"suites": N, "passed": N, "ok": bool}
//                     JSON file (produced by running `make test`); folded
//                     into the test-status badge page. Omitted -> the badge
//                     page reports the static suite count only.
//
// Exports the extraction functions so gen-reference.test.mjs can unit-test
// the regexes directly against fixture strings.

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
// glow.* API extraction (glow_lua_api.cpp)
// ---------------------------------------------------------------------------

// A cheap arg-name heuristic: known stack-argument checker helpers, mapped
// to a descriptive (not authoritative) label. If a given 1-based Lua stack
// index is only ever touched by ONE recognized helper in a function body,
// that helper's label is used; if it's touched by more than one DIFFERENT
// helper (a polymorphic argument, e.g. glow.set's capability arg accepts
// both a string and a glow.CAP.* integer), the ambiguous generic "value" is
// used instead of guessing which branch is "the" signature.
const ARG_HELPERS = [
  [/\bcheckFixtureId\(L,\s*(\d+)\)/g, "fixture"],
  [/\bcheckFixtureIdList\(L,\s*(\d+)\)/g, "fixtures[]"],
  [/\bcheckVec3\(L,\s*(\d+)\)/g, "point"],
  [/\bluaL_checknumber\(L,\s*(\d+)\)/g, "number"],
  [/\bluaL_checkinteger\(L,\s*(\d+)\)/g, "integer"],
  [/\bluaL_checkstring\(L,\s*(\d+)\)/g, "string"],
  [/\bluaL_checktype\(L,\s*(\d+),\s*LUA_TTABLE\)/g, "table"],
];

// Best-effort argument list for one glow_lua_api.cpp `l_*` function body.
// Returns null (not a guess) if no recognized argument was found at all --
// the caller falls back to a "see source" note.
export function inferArgs(body) {
  const byIndex = new Map(); // 1-based stack index -> Set<label>
  for (const [re, label] of ARG_HELPERS) {
    re.lastIndex = 0;
    let m;
    while ((m = re.exec(body))) {
      const idx = parseInt(m[1], 10);
      if (!byIndex.has(idx)) byIndex.set(idx, new Set());
      byIndex.get(idx).add(label);
    }
  }
  if (byIndex.size === 0) return null;

  // An argument is optional if the body ever guards that index with
  // lua_gettop(L) or lua_isnil(L, idx) -- both are the codebase's actual
  // idioms for "this trailing argument may be omitted" (see l_matrix_pattern,
  // l_slot).
  const maxIdx = Math.max(...byIndex.keys());
  const args = [];
  for (let i = 1; i <= maxIdx; i++) {
    const labels = byIndex.get(i);
    let label = labels ? (labels.size === 1 ? [...labels][0] : "value") : "arg" + i;
    const optionalRe = new RegExp(`lua_gettop\\(L\\)\\s*[<>]=?\\s*${i}|lua_isnil\\(L,\\s*${i}\\)`);
    if (optionalRe.test(body)) label = `${label}?`;
    args.push(label);
  }
  return args;
}

// Parses the `install()` table-construction block into subtable groups.
// Returns { groups: [{ name: string|null, fns: [{ luaName, cFn }] }] }
// where name === null is the top-level glow.* namespace (glow.set,
// glow.beat, ...). CAP is handled separately (extractCapabilities) since
// it's a constant table built from a C++ array, not registerFn calls.
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

// Capability names (glow.CAP.* / glow.set|glow.slot's string form) from the
// kCapNames array literal.
export function extractCapabilities(src) {
  const m = /constexpr CapName kCapNames\[\]\s*=\s*\{([\s\S]*?)\n\};/.exec(src);
  if (!m) return [];
  const body = m[1];
  const entryRe = /\{\s*"([a-z0-9-]+)",\s*Capability::(\w+)\s*\}/g;
  const out = [];
  let em;
  while ((em = entryRe.exec(body))) out.push({ name: em[1], enumMember: em[2] });
  return out;
}

// glow.matrix.pattern's pattern-name strcmp chain.
export function extractMatrixPatterns(src) {
  const re = /std::strcmp\(patName,\s*"(\w+)"\)\s*==\s*0/g;
  const out = [];
  let m;
  while ((m = re.exec(src))) out.push(m[1]);
  return out;
}

// Builds the full glow.* API model: subtable groups with their functions'
// best-effort argument lists.
export function buildGlowApi(glowLuaApiCppSrc) {
  const { groups } = extractGlowApiStructure(glowLuaApiCppSrc);
  for (const g of groups) {
    for (const fn of g.fns) {
      const fnBody = extractFunctionBody(
        glowLuaApiCppSrc,
        new RegExp(`int GlowLuaApi::${fn.cFn}\\(lua_State\\* L\\)\\s*`, "g"),
      );
      fn.args = fnBody ? inferArgs(fnBody.body) : null;
    }
  }
  return { groups, capabilities: extractCapabilities(glowLuaApiCppSrc), matrixPatterns: extractMatrixPatterns(glowLuaApiCppSrc) };
}

// ---------------------------------------------------------------------------
// Text-format grammar extraction (provision.cpp)
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

// ---------------------------------------------------------------------------
// live_control.h: ActionKind enum
// ---------------------------------------------------------------------------

export function extractActionKinds(liveControlHeaderSrc) {
  const m = /enum class ActionKind\s*:\s*\w+\s*\{([^}]*)\}/.exec(liveControlHeaderSrc);
  if (!m) return [];
  return m[1].split(",").map((s) => s.trim()).filter(Boolean);
}

// ---------------------------------------------------------------------------
// Markdown rendering
// ---------------------------------------------------------------------------

const GENERATED_BANNER = (source) => `<!-- GENERATED FILE -- do not hand-edit.
Produced by docs/build/gen-reference.mjs from ${source}.
Run \`node docs/build/gen-reference.mjs\` to regenerate; CI fails the build
if this file doesn't match what the generator produces (drift guard). -->

`;

function renderApiMarkdown(api) {
  let out = GENERATED_BANNER("glow_lua_api.cpp");
  out += `# \`glow.*\` API reference\n\n`;
  out += `Every function \`glow_lua_api.cpp\`'s \`GlowLuaApi::install()\` registers into the Fennel/Lua \`glow\` global, grouped by subtable. Argument lists are a best-effort static read of each function's own \`luaL_check*\` calls -- when an argument's type can't be inferred cleanly (or the check calls disagree across branches), it's shown as \`value\` rather than guessed specifically.\n\n`;

  let totalFns = 0;
  for (const g of api.groups) totalFns += g.fns.length;
  out += `${totalFns} functions across ${api.groups.length} groups.\n\n`;
  out += `The dotted names alone are also emitted as \`glow-api-names.json\` (a flat, generated allow-list) for any tool that wants one -- e.g. \`web/provisioner-static/fennel-check.js\` deliberately does NOT hand-list \`glow.*\` names (see that file's own header for why a hand-list drifted and rejected valid scripts); this JSON is here if a future consumer wants a generated one instead.\n\n`;

  for (const g of api.groups) {
    const label = g.name === null ? "Top-level (`glow.*`)" : `\`glow.${g.name}.*\``;
    out += `## ${label}\n\n`;
    for (const fn of g.fns) {
      const dotted = g.name === null ? `glow.${fn.luaName}` : `glow.${g.name}.${fn.luaName}`;
      if (fn.args) {
        out += `- \`(${dotted} ${fn.args.join(" ")})\`\n`;
      } else {
        out += `- \`${dotted}\` -- signature not cheaply extractable; see \`glow_lua_api.cpp\`'s \`GlowLuaApi::${fn.cFn}\`.\n`;
      }
    }
    out += "\n";
  }

  out += `## \`glow.CAP.*\` capability constants\n\n`;
  out += `Also accepted as lowercase/kebab-case strings anywhere a capability is expected (\`glow.set\`, \`glow.slot\`, \`glow.ranges\`).\n\n`;
  out += api.capabilities.map((c) => `\`${c.name}\``).join(", ") + "\n\n";

  out += `## \`glow.matrix.pattern\` pattern names\n\n`;
  out += api.matrixPatterns.map((p) => `\`${p}\``).join(", ") + "\n";

  return out;
}

function renderGrammarMarkdown(grammar) {
  let out = GENERATED_BANNER("provision.cpp");
  out += `# Text-format grammar reference\n\n`;
  out += `Keywords each text format's compiler (\`provision.cpp\`) recognizes, in \`cmd == "..."\` source order. Grammar/argument shape for each keyword is not auto-extracted (freeform token parsing per keyword) -- see FORMAT.md and \`provision.cpp\`'s own parsing code for exact syntax.\n\n`;
  for (const f of grammar) {
    out += `## \`${f.format}\` (\`${f.fnName}\`)\n\n`;
    if (!f.extracted) {
      out += `_Not extracted -- \`${f.fnName}\` wasn't found at its expected signature in \`provision.cpp\`; see source._\n\n`;
      continue;
    }
    out += f.keywords.map((k) => `\`${k}\``).join(", ") + "\n\n";
  }
  return out;
}

function renderEnumMarkdown(api, actionKinds) {
  let out = GENERATED_BANNER("glow_lua_api.cpp, live_control.h");
  out += `# Enumerations\n\n`;
  out += `## Capabilities (\`glow.CAP.*\`, \`fixture_profile.h\`'s \`Capability\`)\n\n`;
  out += `| String name | Enum member |\n|---|---|\n`;
  for (const c of api.capabilities) out += `| \`${c.name}\` | \`Capability::${c.enumMember}\` |\n`;
  out += `\n## Matrix patterns (\`glow.matrix.pattern\`)\n\n`;
  out += api.matrixPatterns.map((p) => `\`${p}\``).join(", ") + "\n\n";
  out += `## Binding action kinds (\`live_control.h\`'s \`ActionKind\`)\n\n`;
  out += actionKinds.length
    ? actionKinds.map((a) => `\`${a}\``).join(", ") + "\n"
    : "_Not extracted -- see \`live_control.h\`._\n";
  return out;
}

function renderTestStatusMarkdown(testStatus) {
  let out = GENERATED_BANNER("Makefile, and (if provided) a --test-status JSON file");
  out += `# Test status\n\n`;
  out += `\`make test\` runs ${testStatus.suites} host-tested suites (see the \`test:\` target in \`Makefile\`).\n\n`;
  if (testStatus.ranAt) {
    out += testStatus.ok
      ? `**All ${testStatus.passed}/${testStatus.suites} suites passed** as of the last docs build (${testStatus.ranAt}).\n`
      : `**${testStatus.passed}/${testStatus.suites} suites passed** as of the last docs build (${testStatus.ranAt}) -- see CI logs.\n`;
  } else {
    out += `_Suite count is statically read from the Makefile; this build did not also run \`make test\` to confirm pass/fail._\n`;
  }
  return out;
}

// Count of `./$(SOMETHING_TARGET)` invocation lines under the Makefile's
// `test:` recipe -- the number of host-tested suites `make test` runs.
export function extractTestSuiteCount(makefileSrc) {
  const testTargetMatch = /\ntest:[^\n]*\n((?:\t\.\/\$\([^\n]*\n)*)/.exec(makefileSrc);
  if (!testTargetMatch) return 0;
  return (testTargetMatch[1].match(/\t\.\/\$\(/g) || []).length;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

export function generateAll({ testStatusPath } = {}) {
  const glowLuaApiCppSrc = readFileSync(join(REPO_ROOT, "glow_lua_api.cpp"), "utf8");
  const provisionCppSrc = readFileSync(join(REPO_ROOT, "provision.cpp"), "utf8");
  const liveControlHeaderSrc = readFileSync(join(REPO_ROOT, "live_control.h"), "utf8");
  const makefileSrc = readFileSync(join(REPO_ROOT, "Makefile"), "utf8");

  const api = buildGlowApi(glowLuaApiCppSrc);
  const grammar = buildGrammar(provisionCppSrc);
  const actionKinds = extractActionKinds(liveControlHeaderSrc);

  let testStatus = { suites: extractTestSuiteCount(makefileSrc) };
  if (testStatusPath && existsSync(testStatusPath)) {
    testStatus = { ...testStatus, ...JSON.parse(readFileSync(testStatusPath, "utf8")) };
  }

  const names = [];
  for (const g of api.groups) {
    for (const fn of g.fns) {
      names.push(g.name === null ? `glow.${fn.luaName}` : `glow.${g.name}.${fn.luaName}`);
    }
  }

  return {
    "api-reference.md": renderApiMarkdown(api),
    "grammar-reference.md": renderGrammarMarkdown(grammar),
    "enumerations.md": renderEnumMarkdown(api, actionKinds),
    "test-status.md": renderTestStatusMarkdown(testStatus),
    "glow-api-names.json": JSON.stringify({ names: names.sort() }, null, 2) + "\n",
  };
}

function parseArgs(argv) {
  const args = { check: false, testStatusPath: null };
  for (const a of argv) {
    if (a === "--check") args.check = true;
    else if (a.startsWith("--test-status=")) args.testStatusPath = a.slice("--test-status=".length);
  }
  return args;
}

function main() {
  const { check, testStatusPath } = parseArgs(process.argv.slice(2));
  const files = generateAll({ testStatusPath });

  if (check) {
    let drifted = false;
    for (const [name, content] of Object.entries(files)) {
      const path = join(OUT_DIR, name);
      const existing = existsSync(path) ? readFileSync(path, "utf8") : null;
      if (existing !== content) {
        console.error(`DRIFT: ${name} differs from generated output`);
        drifted = true;
      }
    }
    if (drifted) {
      console.error("\ndocs/generated/ is out of date -- run `node docs/build/gen-reference.mjs` and commit the result.");
      process.exit(1);
    }
    console.log("docs/generated/ matches the generator output.");
    return;
  }

  mkdirSync(OUT_DIR, { recursive: true });
  for (const [name, content] of Object.entries(files)) {
    writeFileSync(join(OUT_DIR, name), content);
    console.log(`wrote docs/generated/${name}`);
  }
}

if (process.argv[1] && import.meta.url === `file://${process.argv[1]}`) {
  main();
}
