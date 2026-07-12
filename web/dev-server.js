#!/usr/bin/env node
//
// dev-server.js — Phase 1/2 iteration server.
//
// Standalone Node script. No dependencies. Does two jobs:
//   1. Serves the static console bundle (index.html, app.js, styles.css,
//      vendor/) from web/console/ so you can open the UI in a browser.
//   2. Hosts a mock WebSocket endpoint at /ws that:
//        - On connect, sends the `config` message the real device would.
//        - Logs every UI->device message to stdout (so you can watch
//          cue/scene/master events arrive).
//        - Optionally echoes a `state` message back whenever a cue toggle
//          fires, to exercise the Phase-4 highlight path.
//
// Usage:
//   node web/dev-server.js                 # http://localhost:8080/
//   node web/dev-server.js --port 3000     # custom port
//   node web/dev-server.js --no-echo       # disable state echo-back
//
// Open the page, then connect a real device's UI by visiting:
//   http://localhost:8080/?url=ws://localhost:8080/ws
// (WsClient already defaults to ws://<page-host>/ws, so the query string
//  is only needed when pointing at a different host/port.)
//

const http = require("http");
const crypto = require("crypto");
const fs = require("fs");
const path = require("path");

const args = process.argv.slice(2);
let port = 8080;
let echoState = true;
for (let i = 0; i < args.length; i++) {
  if (args[i] === "--port" && args[i + 1]) { port = parseInt(args[i + 1], 10); i++; }
  else if (args[i] === "--no-echo") { echoState = false; }
  else if (args[i] === "--help" || args[i] === "-h") {
    console.log("Usage: node web/dev-server.js [--port N] [--no-echo]");
    process.exit(0);
  }
}

const CONSOLE_DIR = path.join(__dirname, "console");
const WEB_DIR = __dirname;  // fallback root for /shared/** and /vendor/**, shared with the provisioner

// --- sample config (mirrors README_LIVE_CONTROL.md example) ------------

const sampleConfig = {
  type: "config",
  cues: [
    { id: 0, label: "Blue wash",   color: "#3060ff", mode: "toggle" },
    { id: 1, label: "Verse spk",   color: "#30c0ff", mode: "toggle" },
    { id: 2, label: "Chorus",      color: "#ff6030", mode: "toggle" },
    { id: 3, label: "Bridge dim",  color: "#90ff60", mode: "toggle" },
    { id: 4, label: "Solo spot",   color: "#ffdf30", mode: "flash"  },
    { id: 5, label: "Strobe",      color: "#ff3060", mode: "flash"  },
    { id: 6, label: "Bows",        color: "#ff90c0", mode: "toggle" },
    { id: 7, label: "Blackout",    color: "#404050", mode: "flash"  },
  ],
  scenes: [
    { id: 0, label: "Verse" },
    { id: 1, label: "Chorus" },
    { id: 2, label: "Bows" },
  ],
  hasMaster: true,
};

// --- static file server -------------------------------------------------

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js":   "text/javascript; charset=utf-8",
  ".mjs":  "text/javascript; charset=utf-8",
  ".css":  "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".map":  "application/json; charset=utf-8",
  ".wasm": "application/wasm",
};

// The console bundle (web/console/**) is the served root; /shared/** and
// /vendor/** (the CodeMirror/parinfer/wasmoon bundles shared with the
// provisioner — see web/shared/fennel-editor.js) fall back to the web/
// parent directory, since neither lives under web/console/. This mirrors
// how the eventual device httpd and the provisioner's GitHub Pages deploy
// both need to expose those two directories at the served root alongside
// each front's own files (see .github/workflows/provisioner.yml's "Build
// static site" step, which copies them into web/provisioner-static/ before
// upload for exactly this reason).
// Returns the resolved path to serve, or null if it's outside both allowed
// roots (path traversal) -- the caller 403s on null.
function resolveStaticPath(urlPath) {
  const rel = path.normalize(urlPath).replace(/^([/\\])+/, "");
  const underConsole = path.join(CONSOLE_DIR, rel);
  if (!underConsole.startsWith(CONSOLE_DIR)) return null;
  if (fs.existsSync(underConsole) && fs.statSync(underConsole).isFile()) return underConsole;
  if (rel.startsWith("shared" + path.sep) || rel.startsWith("vendor" + path.sep)) {
    const underWeb = path.join(WEB_DIR, rel);
    if (!underWeb.startsWith(WEB_DIR)) return null;
    return underWeb;
  }
  return underConsole;  // 404s below if it doesn't exist
}

function serveStatic(req, res) {
  let urlPath = req.url.split("?")[0];
  if (urlPath === "/") urlPath = "/index.html";

  const abs = resolveStaticPath(urlPath);
  if (abs === null) {
    res.writeHead(403); res.end("forbidden"); return;
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
}

// --- WebSocket upgrade handler -----------------------------------------

const GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

function acceptKey(req) {
  const key = req.headers["sec-websocket-key"] || "";
  return crypto.createHash("sha1").update(key + GUID).digest("base64");
}

function writeFrame(sock, payload, opcode = 0x01) {
  // Text frame, masked=false. Payload length up to 64k (our config is < 1k).
  const len = Buffer.byteLength(payload);
  const header = Buffer.alloc(len < 126 ? 2 : 4);
  header[0] = 0x80 | opcode;
  if (len < 126) {
    header[1] = len;
  } else {
    header[1] = 126;
    header.writeUInt16BE(len, 2);
  }
  sock.write(Buffer.concat([header, Buffer.from(payload, "utf8")]));
}

function readFrame(buf) {
  if (buf.length < 2) return { consumed: 0 };
  const b0 = buf[0], b1 = buf[1];
  const opcode = b0 & 0x0f;
  const masked = (b1 & 0x80) !== 0;
  let len = b1 & 0x7f;
  let offset = 2;
  if (len === 126) {
    if (buf.length < 4) return { consumed: 0 };
    len = buf.readUInt16BE(2);
    offset = 4;
  } else if (len === 127) {
    if (buf.length < 10) return { consumed: 0 };
    // We don't expect payloads > 64k; reject.
    return { consumed: buf.length, error: "payload too large" };
  }
  let mask = null;
  if (masked) {
    if (buf.length < offset + 4) return { consumed: 0 };
    mask = buf.slice(offset, offset + 4);
    offset += 4;
  }
  if (buf.length < offset + len) return { consumed: 0 };
  let payload = buf.slice(offset, offset + len);
  if (mask) {
    payload = Buffer.from(payload);  // copy
    for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
  }
  return { consumed: offset + len, opcode, payload };
}

// --- mock script store (mirrors scripts_storage.h's flat-root LittleFS
// semantics: a name -> source map, "boot" is not special except by name) ---

const mockScripts = new Map([
  ["boot", "; runs at startup\n(fn breathe [t]\n  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2))))))\n"],
  ["verse", "(fn verse-wash [t]\n  (glow.set 1 :dimmer 0.6))\n"],
]);

function scriptsMessage() {
  return { type: "scripts", names: Array.from(mockScripts.keys()) };
}

function handleWs(req, sock) {
  // Complete the handshake.
  sock.write(
    "HTTP/1.1 101 Switching Protocols\r\n" +
    "Upgrade: websocket\r\n" +
    "Connection: Upgrade\r\n" +
    "Sec-WebSocket-Accept: " + acceptKey(req) + "\r\n\r\n"
  );

  let activeToggles = new Set();

  // Send config immediately on connect.
  writeFrame(sock, JSON.stringify(sampleConfig));
  console.log("[ws] client connected — sent config (%d cues, %d scenes)",
              sampleConfig.cues.length, sampleConfig.scenes.length);

  let buf = Buffer.alloc(0);
  sock.on("data", (chunk) => {
    buf = Buffer.concat([buf, chunk]);
    while (buf.length > 0) {
      const frame = readFrame(buf);
      if (frame.consumed === 0) break;
      buf = buf.slice(frame.consumed);
      if (frame.error) { console.log("[ws] frame error:", frame.error); sock.end(); return; }
      if (frame.opcode === 0x08) { sock.end(); return; }  // close
      if (frame.opcode !== 0x01) continue;                // ignore non-text

      const text = frame.payload.toString("utf8");
      let msg;
      try { msg = JSON.parse(text); } catch { console.log("[ws] non-JSON:", text); continue; }

      console.log("[ws <-]", JSON.stringify(msg));

      // Optional Phase-4 echo: when a toggle cue fires, push a `state`
      // message back so the UI's onState highlight path is exercised.
      if (echoState && msg.type === "cue" && msg.pressed === true) {
        // We don't know the cue's mode from the message alone; assume
        // toggle for cues 0..3 (matches our sample config).
        if (msg.id <= 3) {
          if (activeToggles.has(msg.id)) activeToggles.delete(msg.id);
          else activeToggles.add(msg.id);
          const stateMsg = { type: "state", active: Array.from(activeToggles).sort((a,b)=>a-b) };
          writeFrame(sock, JSON.stringify(stateMsg));
          console.log("[ws ->]", JSON.stringify(stateMsg));
        }
      }

      // --- Fennel scripting UI: eval + script CRUD (mirrors web_input.cpp) --

      if (msg.type === "eval") {
        // Mock semantics: a source containing "error" fails with a fake
        // compile error, a source mentioning "boom" simulates an fx_error
        // on a named effect (so the console's fx_error path is exercisable
        // without hardware), everything else "succeeds".
        const seq = Number.isInteger(msg.seq) ? msg.seq : 0;
        const src = typeof msg.src === "string" ? msg.src : "";
        let reply;
        if (/\berror\b/.test(src)) {
          reply = { type: "eval_result", seq, ok: false, err: "1:1: unexpected symbol near 'error'" };
        } else {
          reply = { type: "eval_result", seq, ok: true };
        }
        writeFrame(sock, JSON.stringify(reply));
        console.log("[ws ->]", JSON.stringify(reply));
        if (/\bboom\b/.test(src)) {
          const fxErr = { type: "fx_error", effect: "verse#0", err: "attempt to call a nil value" };
          writeFrame(sock, JSON.stringify(fxErr));
          console.log("[ws ->]", JSON.stringify(fxErr));
        }
        continue;
      }

      if (msg.type === "script_list") {
        writeFrame(sock, JSON.stringify(scriptsMessage()));
        continue;
      }

      if (msg.type === "script_load" && typeof msg.name === "string") {
        const src = mockScripts.get(msg.name) || "";
        const reply = { type: "script", name: msg.name, src };
        writeFrame(sock, JSON.stringify(reply));
        console.log("[ws ->]", JSON.stringify(reply));
        continue;
      }

      if (msg.type === "script_save" && typeof msg.name === "string") {
        mockScripts.set(msg.name, typeof msg.src === "string" ? msg.src : "");
        writeFrame(sock, JSON.stringify(scriptsMessage()));
        continue;
      }

      if (msg.type === "script_delete" && typeof msg.name === "string") {
        mockScripts.delete(msg.name);
        writeFrame(sock, JSON.stringify(scriptsMessage()));
        continue;
      }
    }
  });

  sock.on("end",  () => console.log("[ws] client disconnected"));
  sock.on("error", (e) => { /* swallow ECONNRESET etc. */ });
}

// --- http server --------------------------------------------------------

const server = http.createServer((req, res) => {
  const isWs = (req.headers.upgrade || "").toLowerCase() === "websocket";
  if (isWs) {
    // Handshake is finished in the upgrade event.
    res.writeHead(426); res.end("upgrade required");
    return;
  }
  serveStatic(req, res);
});

server.on("upgrade", (req, sock) => {
  if ((req.url || "").split("?")[0] !== "/ws") {
    sock.destroy();
    return;
  }
  handleWs(req, sock);
});

server.listen(port, () => {
  console.log("esp-glow dev server");
  console.log("  console:  http://localhost:%d/", port);
  console.log("  ws:       ws://localhost:%d/ws", port);
  console.log("  echo state: %s", echoState ? "on" : "off");
});
