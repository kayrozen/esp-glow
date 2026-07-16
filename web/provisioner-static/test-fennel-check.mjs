// test-fennel-check.mjs — proves fennel-check.js's browser compile-check
// (checkFennelSyntax) accepts any syntactically valid glow.* usage and
// still rejects real syntax errors, now that GLOW_STUB_SRC is an
// accept-everything proxy instead of a hand-maintained list of subtable
// names (see fennel-check.js's header comment for why the hand-list was
// removed -- it drifted out of sync with glow_lua_api.cpp and rejected
// valid scripts, e.g. any use of glow.bind, worse than missing a typo).
//
// fennel-check.js runs the REAL vendored fennel.lua against the REAL WASM
// Lua VM (wasmoon), and that vendored bundle is built for a browser
// JS environment only -- it does not run under plain Node (no
// document/window, and Node's own require()/module built-ins are stubbed
// out by the bundler for the browser target). So this test drives a real
// headless Chromium via Playwright, loading fennel-check.js as an actual
// ES module the same way the provisioner page does, and calls
// checkFennelSyntax from inside that page.
//
// Run: node web/provisioner-static/test-fennel-check.mjs
// (playwright + a downloaded Chromium must be available -- see the
// "Install Playwright" CI step in .github/workflows/provisioner.yml)

import { chromium } from "playwright";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { spawn } from "node:child_process";

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(__dirname, "..", "..");
const PORT = 8799;

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

function waitForServer(url, tries = 50) {
  return new Promise((resolve, reject) => {
    const attempt = (n) => {
      fetch(url).then(() => resolve()).catch((e) => {
        if (n <= 0) return reject(e);
        setTimeout(() => attempt(n - 1), 100);
      });
    };
    attempt(tries);
  });
}

const server = spawn(process.execPath, ["web/provisioner-static/static-server.js", String(PORT)], {
  cwd: REPO_ROOT,
  stdio: "ignore",
});

let browser;
try {
  await waitForServer(`http://localhost:${PORT}/index.html`);

  browser = await chromium.launch();
  const page = await browser.newPage();

  // Loading fennel-check.js as a module builds a fresh WASM Lua VM on the
  // page's first checkFennelSyntax call and reuses it after -- exercise
  // all fixtures against the one page/module instance, same as the
  // real provisioner UI does across repeated "Check syntax" clicks.
  async function checkSyntax(src) {
    return page.evaluate(async (s) => {
      const mod = await import("./fennel-check.js");
      return mod.checkFennelSyntax(s);
    }, src);
  }

  await page.goto(`http://localhost:${PORT}/index.html`);

  // The regression: demo-boot.fnl uses glow.bind, glow.led.auto-xy,
  // glow.cue.define -- none of which the old hand-listed stub knew about
  // (only "set"/"aim"/"cue"/"scene"/"fx"/"matrix" were stubbed), so it
  // used to fail with "attempt to index a nil value (field 'bind')" at
  // the very first glow.bind call, even though the script is valid.
  const demoBoot = readFileSync(join(REPO_ROOT, "samples", "demo-boot.fnl"), "utf8");
  const demoResult = await checkSyntax(demoBoot);
  check("demo-boot.fnl (glow.bind/glow.led.auto-xy/glow.cue.define) compiles",
        demoResult.ok === true, JSON.stringify(demoResult));

  // Subtables the old stub never listed at all (glow.wled, glow.param,
  // top-level glow.slot/glow.beat) must also pass -- the whole point of
  // the proxy is that no future glow.* addition needs this file touched.
  const unlistedResult = await checkSyntax(
    "(glow.wled.set-pixel 0 :red) (glow.param.get :intensity) (glow.slot 1) (print (glow.beat))"
  );
  check("glow.wled/glow.param/glow.slot/glow.beat (never in the old hand-list) compile",
        unlistedResult.ok === true, JSON.stringify(unlistedResult));

  // A genuine syntax error must still be caught, with a line number --
  // this checker's actual job (syntax/compilation) must still work.
  const syntaxErrResult = await checkSyntax("(glow.cue.define :warm {:effects []}");
  check("unbalanced parens still rejected", syntaxErrResult.ok === false, JSON.stringify(syntaxErrResult));
  check("rejection reports a line number",
        typeof syntaxErrResult.err === "string" && /:\d+:/.test(syntaxErrResult.err),
        syntaxErrResult.err);

  // Arithmetic on a glow accessor -- proves the __add/__sub/__mul/etc.
  // metamethods work, not just __index/__call. Real effects do this
  // routinely (e.g. fading against the beat clock).
  const arithResult = await checkSyntax(
    "(print (- 1 (glow.beat))) (print (* 0.5 (+ 1 (glow.bar))))"
  );
  check("arithmetic on glow.beat/glow.bar compiles", arithResult.ok === true, JSON.stringify(arithResult));

  // Deep, arbitrarily-named access -- proves recursion (not a fixed
  // depth) and documents that name-checking is intentionally not done
  // here: glow.a.b.c.d isn't a real API path, and this checker doesn't
  // care.
  const deepResult = await checkSyntax("(glow.a.b.c.d)");
  check("deep/arbitrary glow.a.b.c.d access compiles", deepResult.ok === true, JSON.stringify(deepResult));

  console.log(`\n${count - failures}/${count} checks passed.`);
} finally {
  if (browser) await browser.close();
  server.kill();
}

if (failures > 0) {
  console.log(`${failures} FAILURE(S)`);
  process.exit(1);
}
