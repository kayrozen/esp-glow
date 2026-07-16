// test-demo-walkthrough.mjs — proves the interactive demo walkthrough
// (docs/interactive/demo-walkthrough.html) actually does what it claims:
// it loads the real samples/demo-boot.fnl, compiles it clean through the
// real fennel-check.js, and a broken edit shows a real compile error. This
// is the "doc and test are the same artifact" property from the task --
// an API regression that breaks the demo show breaks this test too, not
// just a stale doc page.
//
// Builds the docs site into a scratch temp directory laid out exactly like
// the real Pages deploy (docs/ alongside shared/, vendor/, fennel-check.js
// at the site root -- see .github/workflows/provisioner.yml's "Copy shared
// assets" step), then drives real headless Chromium against it via
// Playwright, same pattern as test-fennel-check.mjs (the vendored wasmoon
// bundle needs a real browser JS environment, not plain Node).
//
// Run: node docs/build/test-demo-walkthrough.mjs
// (playwright + a downloaded Chromium must be available, and docs/generated/
// must already exist -- run gen-reference.mjs first if starting fresh.)

import { chromium } from "playwright";
import { mkdtempSync, cpSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { spawnSync } from "node:child_process";

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(__dirname, "..", "..");
const PORT = 8798;

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

// Assembles a scratch site root: docs/ (rendered fresh) plus shared/,
// vendor/, fennel-check.js copied in as siblings -- exactly the layout
// deploy-pages produces (see provisioner.yml), so relative imports/fetches
// inside the rendered pages resolve exactly as they will in production.
const siteRoot = mkdtempSync(join(tmpdir(), "esp-glow-docs-site-"));
try {
  const renderResult = spawnSync(process.execPath, [
    join(REPO_ROOT, "docs", "build", "render-site.mjs"),
    "--out", join(siteRoot, "docs"),
  ], { stdio: "inherit" });
  if (renderResult.status !== 0) {
    console.error("render-site.mjs failed");
    process.exit(1);
  }
  cpSync(join(REPO_ROOT, "web", "shared"), join(siteRoot, "shared"), { recursive: true });
  cpSync(join(REPO_ROOT, "web", "vendor"), join(siteRoot, "vendor"), { recursive: true });
  cpSync(join(REPO_ROOT, "web", "provisioner-static", "fennel-check.js"), join(siteRoot, "fennel-check.js"));

  let browser;
  let httpServer;
  try {
    // static-server.js (web/provisioner-static/) always serves its own
    // __dirname and has no CLI flag to point elsewhere, so spin up a
    // minimal server directly over the scratch site root instead -- no
    // path-traversal cleverness needed since this tree is generated
    // locally by this script, not user input.
    const { createServer } = await import("node:http");
    const { readFileSync: rf, existsSync: ex, statSync } = await import("node:fs");
    httpServer = createServer((req, res) => {
      const url = new URL(req.url, "http://localhost");
      let relPath = decodeURIComponent(url.pathname);
      if (relPath === "/") relPath = "/index.html";
      const filePath = join(siteRoot, relPath);
      if (!filePath.startsWith(siteRoot) || !ex(filePath) || statSync(filePath).isDirectory()) {
        res.writeHead(404);
        res.end("not found");
        return;
      }
      const ext = filePath.split(".").pop();
      const mime = { html: "text/html", js: "text/javascript", mjs: "text/javascript",
        css: "text/css", json: "application/json", wasm: "application/wasm", fnl: "text/plain",
        lua: "text/plain", txt: "text/plain" }[ext] || "application/octet-stream";
      res.writeHead(200, { "Content-Type": mime });
      res.end(rf(filePath));
    });
    await new Promise((resolve) => httpServer.listen(PORT, resolve));

    await waitForServer(`http://localhost:${PORT}/docs/interactive/demo-walkthrough.html`);

    browser = await chromium.launch();
    const page = await browser.newPage();
    page.on("pageerror", (e) => console.error("PAGE ERROR:", e));

    await page.goto(`http://localhost:${PORT}/docs/interactive/demo-walkthrough.html`);

    // Wait for the initial compile-check (debounced 250ms after the editor
    // mounts) to settle to something other than "checking…".
    await page.waitForFunction(
      () => document.getElementById("compile-status")?.dataset.state !== "checking",
      undefined, { timeout: 15000 },
    );

    const initialState = await page.getAttribute("#compile-status", "data-state");
    const initialText = await page.textContent("#compile-status");
    check("the real samples/demo-boot.fnl compiles clean on load", initialState === "ok", initialText);

    const introText = await page.textContent("#intro");
    check("the intro is populated from the fetched file's header comment", introText.length > 0 && !introText.includes("Loading"), introText);

    const sectionCount = await page.evaluate(() => document.querySelectorAll("#sections .walkthrough-step").length);
    check("the walkthrough renders at least one narrated section", sectionCount > 0, sectionCount);

    const gridCellCount = await page.evaluate(() => document.querySelectorAll("#grid svg rect").length);
    check("the APC40 grid mock renders 8x5 = 40 cells", gridCellCount === 40, gridCellCount);

    const litCellCount = await page.evaluate(() =>
      [...document.querySelectorAll("#grid svg rect")].filter((r) => r.getAttribute("fill") !== "#20242f").length);
    check("at least one grid cell is coloured by a real binding from the file", litCellCount > 0, litCellCount);

    // Break the buffer via CodeMirror's own dispatch (typing isn't
    // necessary -- we're proving the compile-check wiring reacts to a real
    // edit, same as the provisioner does). An unbalanced PAREN is not a
    // good test edit here: fennel-editor.js's Parinfer integration runs on
    // every doc change and actively re-balances parens (that's its whole
    // job), so a dropped close-paren gets silently repaired before
    // checkFennelSyntax ever sees it. An unterminated string literal is a
    // lex error Parinfer doesn't (and per its own contract, safely can't)
    // "fix" by rebalancing brackets.
    await page.evaluate(() => {
      const view = window.__demoWalkthroughView;
      view.dispatch({ changes: { from: 0, to: view.state.doc.length, insert: '(glow.cue.define :broken "unterminated)' } });
    });
    try {
      await page.waitForFunction(
        () => document.getElementById("compile-status")?.dataset.state === "error",
        undefined, { timeout: 15000 },
      );
    } catch (e) {
      const debugState = await page.evaluate(() => ({
        state: document.getElementById("compile-status")?.dataset.state,
        text: document.getElementById("compile-status")?.textContent,
        hasView: !!window.__demoWalkthroughView,
      }));
      console.error("DEBUG:", JSON.stringify(debugState));
      throw e;
    }
    const brokenText = await page.textContent("#compile-status");
    check("a deliberately-broken edit shows a compile error", brokenText.length > 0, brokenText);

    console.log(`\n${count - failures}/${count} checks passed.`);
  } finally {
    if (browser) await browser.close();
    if (httpServer) await new Promise((resolve) => httpServer.close(resolve));
  }
} finally {
  rmSync(siteRoot, { recursive: true, force: true });
}

if (failures > 0) console.log(`${failures} FAILURE(S)`);
process.exit(failures > 0 ? 1 : 0);
