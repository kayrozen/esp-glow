// static-server.js — tiny static file server for the provisioner-static
// editor bundle. Used for local iteration. The GitHub Action deploys to
// Pages so this is dev-only.
//
// Usage: node web/provisioner-static/static-server.js [port]

const http = require("node:http");
const fs = require("node:fs");
const path = require("node:path");

const PORT = parseInt(process.argv[2] || "8766", 10);
const ROOT = __dirname;
// /shared/** and /vendor/** (the CodeMirror/parinfer/wasmoon bundles shared
// with the device console — see web/shared/fennel-editor.js) fall back to
// the web/ parent directory for local dev, matching the CI deploy's "Build
// static site" step, which copies both into web/provisioner-static/ before
// upload (they aren't part of this directory in the repo itself).
const WEB_DIR = path.join(__dirname, "..");

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js":   "text/javascript; charset=utf-8",
  ".mjs":  "text/javascript; charset=utf-8",
  ".css":  "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".wasm": "application/wasm",
  ".svg":  "image/svg+xml",
  ".ico":  "image/x-icon",
};

// Returns the resolved path to serve, or null if it's outside both allowed
// roots (path traversal) -- the caller 403s on null.
function resolveStaticPath(urlPath) {
  const rel = path.normalize(urlPath).replace(/^([/\\])+/, "");
  const underRoot = path.join(ROOT, rel);
  if (!underRoot.startsWith(ROOT)) return null;
  if (fs.existsSync(underRoot) && fs.statSync(underRoot).isFile()) return underRoot;
  if (rel.startsWith("shared" + path.sep) || rel.startsWith("vendor" + path.sep)) {
    const underWeb = path.join(WEB_DIR, rel);
    if (!underWeb.startsWith(WEB_DIR)) return null;
    return underWeb;
  }
  return underRoot;  // 404s below if it doesn't exist
}

const server = http.createServer((req, res) => {
  let urlPath = req.url.split("?")[0];
  if (urlPath === "/") urlPath = "/index.html";

  const abs = resolveStaticPath(urlPath);
  if (abs === null) {
    res.writeHead(403);
    res.end("forbidden");
    return;
  }

  fs.readFile(abs, (err, data) => {
    if (err) {
      res.writeHead(404, { "content-type": "text/plain" });
      res.end("not found: " + urlPath);
      return;
    }
    const ext = path.extname(abs).toLowerCase();
    res.writeHead(200, {
      "content-type": MIME[ext] || "application/octet-stream",
      "cache-control": "no-cache",
    });
    res.end(data);
  });
});

server.listen(PORT, () => {
  console.log(`provisioner-static server: http://localhost:${PORT}/`);
});
