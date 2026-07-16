#!/usr/bin/env node
//
// check-relative-paths.mjs — fails if any relative import/fetch/src/href in
// a static site's JS or HTML resolves ABOVE the site root.
//
// GitHub Pages serves this whole tree under a subpath
// (kayrozen.github.io/esp-glow/), so a file's "site root" is a real
// directory on disk, and "../" from a file can walk right off the top of
// it -- a "../shared/..." import from a file at the site's own root
// resolves ABOVE the site (404s on Pages) even though it can happen to
// still work in local dev, where static-server.js's ROOT is one level
// higher (see provisioner.yml's original top-level-only version of this
// check, which only covered that one case). This is a depth-aware
// generalization covering files nested arbitrarily deep too (e.g.
// docs/interactive/demo-walkthrough.js), where the CORRECT relative path
// legitimately contains "../../..." and a blanket "no .. allowed" rule
// would reject valid code.
//
// Usage: node docs/build/check-relative-paths.mjs <site-root-directory>

import { readFileSync, readdirSync, statSync } from "node:fs";
import { join, dirname, posix, extname, relative } from "node:path";

// Patterns that pull a bare path/URL string out of JS or HTML. Deliberately
// broad (any quoted string after these tokens) rather than a full parser --
// a false positive here just means a non-path string gets checked and
// (since it won't look like a relative escape) ignored; a false negative
// would mean a real escape slips through, which is the actual thing this
// guard exists to prevent.
const JS_REF_RE = /\b(?:from|import)\s*\(?\s*["']([^"']+)["']|\bfetch\(\s*["']([^"']+)["']|new URL\(\s*["']([^"']+)["']/g;
const HTML_REF_RE = /\b(?:src|href)\s*=\s*["']([^"']+)["']/g;

function isRelative(ref) {
  if (/^[a-z][a-z0-9+.-]*:/i.test(ref)) return false; // scheme (http:, data:, mailto:, ...)
  if (ref.startsWith("/")) return false; // already root-relative -- a different (also valid) convention, not this check's concern
  if (ref.startsWith("#")) return false;
  return true;
}

function* walk(dir) {
  for (const entry of readdirSync(dir)) {
    const full = join(dir, entry);
    const st = statSync(full);
    if (st.isDirectory()) yield* walk(full);
    else yield full;
  }
}

// Returns a list of { file, ref } violations: relative references that
// resolve above `siteRoot`.
export function findEscapingReferences(siteRoot) {
  const violations = [];
  for (const file of walk(siteRoot)) {
    const ext = extname(file);
    if (![".js", ".mjs", ".html"].includes(ext)) continue;
    const src = readFileSync(file, "utf8");
    const re = ext === ".html" ? HTML_REF_RE : JS_REF_RE;
    re.lastIndex = 0;
    let m;
    while ((m = re.exec(src))) {
      const ref = (m[1] || m[2] || m[3] || "").split(/[?#]/)[0];
      if (!ref || !isRelative(ref)) continue;
      const fileDirRelToRoot = relative(siteRoot, dirname(file)).split(/[\\/]/).filter(Boolean);
      const resolved = posix.normalize(posix.join(...fileDirRelToRoot, ref));
      if (resolved.startsWith("..")) {
        violations.push({ file: relative(siteRoot, file), ref });
      }
    }
  }
  return violations;
}

function main() {
  const siteRoot = process.argv[2];
  if (!siteRoot) {
    console.error("usage: node docs/build/check-relative-paths.mjs <site-root-directory>");
    process.exit(1);
  }
  const violations = findEscapingReferences(siteRoot);
  if (violations.length > 0) {
    for (const v of violations) {
      console.error(`::error file=${v.file}::relative reference "${v.ref}" resolves above the site root -- use a path that stays under it`);
    }
    process.exit(1);
  }
  console.log(`No parent-relative escapes found under ${siteRoot}.`);
}

if (process.argv[1] && import.meta.url === `file://${process.argv[1]}`) {
  main();
}
