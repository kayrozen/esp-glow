#!/usr/bin/env node
//
// render-site.mjs — renders docs/*.md (all hand-written) into a small
// static HTML site. No SSG framework: markdown-lite.mjs is a few dozen
// lines covering exactly what these pages use (see its header).
//
// Usage: node docs/build/render-site.mjs --out <directory>
//
// The output directory becomes DOCS_ROOT inside the eventual site root --
// i.e. pass `web/provisioner-static/docs` in the real Pages deploy (where
// `shared/` and `vendor/` are copied in as siblings of `docs/` by the
// existing CI step), or any scratch directory for local preview/testing.

import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { renderMarkdown, extractTitle } from "./markdown-lite.mjs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(__dirname, "..", "..");
const DOCS_SRC = join(REPO_ROOT, "docs");

// Every page rendered through the shared template, in nav order. `src` is
// relative to docs/; `out` is relative to the output docs/ directory (kept
// equal to `src` with a .html extension so links are predictable).
const PAGES = [
  { src: "index.md", nav: "Home" },
  { src: "architecture.md", nav: "Architecture" },
  { src: "authoring.md", nav: "Writing a show" },
  { src: "reference.md", nav: "API reference" },
  { src: "grammar.md", nav: "Grammar reference" },
  { src: "bring-up.md", nav: "Bring-up" },
];

function htmlOutPath(src) {
  return src.replace(/\.md$/, ".html");
}

// Depth of a docs-relative path below docs/ itself (index.md -> 0,
// a hypothetical subdir/page.md -> 1) -- how many "../" to prepend to reach
// docs/ (add one more to reach the site root above docs/). Every page today
// is top-level (depth 0); kept general in case a page ever nests.
function depthOf(relPath) {
  return relPath.split("/").length - 1;
}

function template({ title, bodyHtml, depth, currentSrc }) {
  const toDocsRoot = depth === 0 ? "." : Array(depth).fill("..").join("/");
  const nav = PAGES.map((p) => {
    const href = depth === 0 ? htmlOutPath(p.src) : `${toDocsRoot}/${htmlOutPath(p.src)}`;
    const active = p.src === currentSrc ? ' class="active"' : "";
    return `<a href="${href}"${active}>${p.nav}</a>`;
  });

  return `<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${title} — esp-glow docs</title>
<link rel="stylesheet" href="${depth === 0 ? "style.css" : toDocsRoot + "/style.css"}">
</head>
<body>
<div class="layout">
<nav class="sidebar">
<div class="brand">esp-glow docs</div>
${nav.join("\n")}
</nav>
<main>
${bodyHtml}
</main>
</div>
</body>
</html>
`;
}

function renderPages(outDir) {
  for (const page of PAGES) {
    const srcPath = join(DOCS_SRC, page.src);
    const md = readFileSync(srcPath, "utf8");
    const title = extractTitle(md, page.nav);
    const bodyHtml = renderMarkdown(md);
    const depth = depthOf(page.src);
    const html = template({ title, bodyHtml, depth, currentSrc: page.src });

    const outPath = join(outDir, htmlOutPath(page.src));
    mkdirSync(dirname(outPath), { recursive: true });
    writeFileSync(outPath, html);
  }
}

const STYLE_CSS = `:root {
  --bg: #ffffff; --fg: #1a1e2a; --muted: #5a6478; --accent: #2f6a3f;
  --code-bg: #f2f4f8; --border: #e2e6ee; --sidebar-bg: #f8f9fb;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #12141c; --fg: #e8ecf4; --muted: #9aa4b8; --accent: #7fd0ff;
    --code-bg: #1a1e2a; --border: #2a3040; --sidebar-bg: #0e1017;
  }
}
* { box-sizing: border-box; }
body {
  margin: 0; background: var(--bg); color: var(--fg);
  font: 16px/1.6 -apple-system, "Segoe UI", Helvetica, Arial, sans-serif;
}
.layout { display: flex; min-height: 100vh; }
.sidebar {
  flex: 0 0 220px; background: var(--sidebar-bg); border-right: 1px solid var(--border);
  padding: 1.5rem 1rem; position: sticky; top: 0; height: 100vh; overflow-y: auto;
}
.sidebar .brand { font-weight: 700; margin-bottom: 1rem; }
.sidebar a {
  display: block; padding: 0.35rem 0.5rem; border-radius: 6px; color: var(--fg);
  text-decoration: none; font-size: 0.92rem;
}
.sidebar a:hover { background: var(--code-bg); }
.sidebar a.active { color: var(--accent); font-weight: 600; }
main { flex: 1; max-width: 860px; padding: 2rem 3rem; overflow-x: auto; }
h1, h2, h3 { line-height: 1.25; }
h1 { border-bottom: 1px solid var(--border); padding-bottom: 0.5rem; }
code {
  background: var(--code-bg); padding: 0.1em 0.35em; border-radius: 4px;
  font-family: "SF Mono", Menlo, Consolas, monospace; font-size: 0.9em;
}
pre {
  background: var(--code-bg); padding: 1rem; border-radius: 8px; overflow-x: auto;
  border: 1px solid var(--border);
}
pre code { background: none; padding: 0; }
a { color: var(--accent); }
table { border-collapse: collapse; width: 100%; margin: 1rem 0; }
th, td { border: 1px solid var(--border); padding: 0.4rem 0.6rem; text-align: left; }
`;

function main() {
  const outArgIdx = process.argv.indexOf("--out");
  if (outArgIdx === -1 || !process.argv[outArgIdx + 1]) {
    console.error("usage: node docs/build/render-site.mjs --out <directory>");
    process.exit(1);
  }
  const outDir = resolve(process.cwd(), process.argv[outArgIdx + 1]);

  mkdirSync(outDir, { recursive: true });
  renderPages(outDir);
  writeFileSync(join(outDir, "style.css"), STYLE_CSS);

  console.log(`docs site rendered to ${outDir}`);
}

if (process.argv[1] && import.meta.url === `file://${process.argv[1]}`) {
  main();
}
