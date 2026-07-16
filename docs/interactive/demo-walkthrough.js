// demo-walkthrough.js — the interactive demo show tutorial. Fetches the
// REAL samples/demo-boot.fnl (copied alongside this page at docs build
// time -- see docs/build/render-site.mjs), splits it into narrated
// sections using the file's OWN comments as the "why" prose (so the prose
// can't drift from the code the way a hand-duplicated copy could), renders
// a live-editable CodeMirror editor wired to the same checkFennelSyntax
// compile-check the provisioner uses, and mocks the APC40's 8x5 clip-launch
// grid from whatever glow.bind.pad-xy/glow.led.auto-xy calls are currently
// in the editor buffer.
//
// This page doubles as a test (see docs/build/test-demo-walkthrough.mjs):
// because it compiles the actual demo-boot.fnl through the actual Fennel
// compiler, an API regression that breaks the real demo show breaks this
// page's "compiles clean" status too.

import { createFennelEditor, getEditorDoc } from "../../shared/fennel-editor.js";
import { checkFennelSyntax } from "../../fennel-check.js";

const GRID_COLS = 8;
const GRID_ROWS = 5;

const NAMED_COLORS = {
  red: "#e0483e", green: "#4caf50", blue: "#3f8ee0", yellow: "#e0c93f",
  amber: "#e0913f", white: "#e8ecf4", cyan: "#3fd0e0", magenta: "#c93fe0",
  off: "#1a1e2a",
};
function colorFor(name) {
  return NAMED_COLORS[name] || "#4a5468";
}

// Splits the raw Fennel source into narrated blocks on blank lines. The
// first block (the file's own header comment) is rendered as an intro
// callout; every later block is rendered as { prose, code }, where `prose`
// is that block's own leading ";;" comment lines (the file's real
// commentary -- nothing here is duplicated by hand) and `code` is
// whatever's left.
function splitIntoSections(src) {
  const blocks = src.split(/\n{2,}/).map((b) => b.replace(/^\n+|\n+$/g, "")).filter(Boolean);
  if (blocks.length === 0) return { intro: "", sections: [] };
  const [intro, ...rest] = blocks;
  const sections = rest.map((block) => {
    const lines = block.split("\n");
    const proseLines = [];
    let i = 0;
    while (i < lines.length && /^\s*;;/.test(lines[i])) {
      proseLines.push(lines[i].replace(/^\s*;;\s?/, ""));
      i++;
    }
    const code = lines.slice(i).join("\n").trim();
    const prose = proseLines.join(" ").trim() ||
      (code.includes("cue.define") ? "Defines the cues every binding below refers to." : "");
    return { prose, code };
  }).filter((s) => s.code.length > 0);
  return { intro: intro.replace(/^\s*;;\s?/gm, "").trim(), sections };
}

function renderIntro(introText) {
  const el = document.getElementById("intro");
  el.textContent = introText;
}

function renderSections(sections) {
  const container = document.getElementById("sections");
  container.innerHTML = "";
  sections.forEach((s, i) => {
    const section = document.createElement("section");
    section.className = "walkthrough-step";
    if (s.prose) {
      const p = document.createElement("p");
      p.textContent = s.prose;
      section.appendChild(p);
    }
    const pre = document.createElement("pre");
    const code = document.createElement("code");
    code.textContent = s.code;
    pre.appendChild(code);
    section.appendChild(pre);
    container.appendChild(section);
  });
}

// Parses whatever pad-xy bindings + LED feedback the CURRENT editor buffer
// declares (not a fixed snapshot) -- editing the live buffer updates the
// grid mock, so "the pad turns green when its cue is active" stays tied to
// the actual source being compile-checked, not a separate static picture.
function parseBindings(src) {
  const cells = new Map(); // "col,row" -> { mode, cue, onColor, offColor }
  const bindRe = /\(glow\.bind\.pad-xy\s+(\d+)\s+(\d+)\s+:(\S+)\s+:(\S+)\)/g;
  const ledRe = /\(glow\.led\.auto-xy\s+(\d+)\s+(\d+)\s+:(\S+)\s+:(\S+)\s+:(\S+)\)/g;
  let m;
  while ((m = bindRe.exec(src))) {
    const key = `${m[1]},${m[2]}`;
    cells.set(key, { ...(cells.get(key) || {}), mode: m[3], cue: m[4] });
  }
  while ((m = ledRe.exec(src))) {
    const key = `${m[1]},${m[2]}`;
    cells.set(key, { ...(cells.get(key) || {}), cue: m[3], onColor: m[4], offColor: m[5] });
  }
  return cells;
}

function renderGrid(src) {
  const cells = parseBindings(src);
  const svgNs = "http://www.w3.org/2000/svg";
  const cellSize = 40, gap = 6;
  const width = GRID_COLS * (cellSize + gap) + gap;
  const height = GRID_ROWS * (cellSize + gap) + gap;

  const svg = document.createElementNS(svgNs, "svg");
  svg.setAttribute("viewBox", `0 0 ${width} ${height}`);
  svg.setAttribute("width", "100%");
  svg.setAttribute("role", "img");
  svg.setAttribute("aria-label", "APC40 8x5 clip-launch grid, coloured by the current glow.led.auto-xy bindings");

  for (let row = 0; row < GRID_ROWS; row++) {
    for (let col = 0; col < GRID_COLS; col++) {
      const cell = cells.get(`${col},${row}`);
      const rect = document.createElementNS(svgNs, "rect");
      rect.setAttribute("x", gap + col * (cellSize + gap));
      rect.setAttribute("y", gap + row * (cellSize + gap));
      rect.setAttribute("width", cellSize);
      rect.setAttribute("height", cellSize);
      rect.setAttribute("rx", 6);
      rect.setAttribute("fill", cell ? colorFor(cell.onColor || "off") : "#20242f");
      rect.setAttribute("stroke", cell ? "#0e1017" : "#2a3040");
      rect.setAttribute("stroke-width", "2");
      if (cell) {
        const title = document.createElementNS(svgNs, "title");
        title.textContent = `(${col},${row}) ${cell.mode ? cell.mode + " -> " : ""}:${cell.cue}`;
        rect.appendChild(title);
      }
      svg.appendChild(rect);
    }
  }
  const gridEl = document.getElementById("grid");
  gridEl.innerHTML = "";
  gridEl.appendChild(svg);
}

function setStatus(state, detail) {
  const el = document.getElementById("compile-status");
  el.dataset.state = state;
  el.textContent = detail;
}

let checkTimer = null;
function scheduleCheck(view) {
  clearTimeout(checkTimer);
  setStatus("checking", "checking…");
  checkTimer = setTimeout(async () => {
    const text = getEditorDoc(view);
    renderGrid(text);
    const result = await checkFennelSyntax(text);
    if (result.ok) setStatus("ok", "compiles clean");
    else setStatus("error", result.err);
  }, 250);
}

async function main() {
  const res = await fetch("../samples/demo-boot.fnl");
  const src = await res.text();

  const { intro, sections } = splitIntoSections(src);
  renderIntro(intro);
  renderSections(sections);
  renderGrid(src);

  const view = createFennelEditor({
    parent: document.getElementById("editor"),
    doc: src,
    onChange: () => scheduleCheck(view),
  });
  window.__demoWalkthroughView = view; // exposed for the Playwright test
  scheduleCheck(view);
}

main().catch((e) => {
  setStatus("error", `failed to load: ${e && e.message ? e.message : e}`);
});
