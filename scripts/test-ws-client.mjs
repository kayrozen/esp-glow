#!/usr/bin/env node
//
// test-ws-client.mjs — node tests for web/console/ws-client.js. Mocks the
// global WebSocket so the client's parse/send/reconnect paths (including
// the eval/script_* additions for the Fennel scripting UI) run without a
// real device or browser.
//
import assert from "node:assert/strict";
import { WsClient, Status } from "../web/console/ws-client.js";

let failCount = 0;
function test(name, fn) {
  try {
    fn();
    console.log(`ok - ${name}`);
  } catch (err) {
    failCount++;
    console.log(`FAIL - ${name}`);
    console.log(`  ${err.message}`);
  }
}

// --- mock WebSocket --------------------------------------------------------

class MockWebSocket {
  static OPEN = 1;
  static CONNECTING = 0;
  static CLOSED = 3;

  constructor(url) {
    this.url = url;
    this.readyState = MockWebSocket.CONNECTING;
    this.sent = [];
    MockWebSocket.lastInstance = this;
  }
  send(data) { this.sent.push(data); }
  close() { this.readyState = MockWebSocket.CLOSED; if (this.onclose) this.onclose(); }
  open() { this.readyState = MockWebSocket.OPEN; if (this.onopen) this.onopen(); }
  message(data) { if (this.onmessage) this.onmessage({ data }); }
}
globalThis.WebSocket = MockWebSocket;

function makeClient(opts = {}) {
  const client = new WsClient("ws://test/ws", { backoffMinMs: 1, backoffMaxMs: 2, ...opts });
  client.connect();
  const ws = MockWebSocket.lastInstance;
  ws.open();
  ws.sent = []; // drop the auto-sent "hello"
  return { client, ws };
}

function lastSent(ws) {
  return JSON.parse(ws.sent[ws.sent.length - 1]);
}

// --- existing surface (regression coverage) --------------------------------

test("connect() sends hello and fires onStatus(Open)", () => {
  let status = null;
  const client = new WsClient("ws://test/ws");
  client.onStatus((s) => { status = s; });
  client.connect();
  const ws = MockWebSocket.lastInstance;
  ws.open();
  assert.equal(status, Status.Open);
  assert.deepEqual(JSON.parse(ws.sent[0]), { type: "hello" });
});

test("onConfig normalizes cues/scenes/hasMaster", () => {
  let cfg = null;
  const { ws } = (() => {
    const client = new WsClient("ws://test/ws");
    client.onConfig((c) => { cfg = c; });
    client.connect();
    const ws = MockWebSocket.lastInstance;
    ws.open();
    return { client, ws };
  })();
  ws.message(JSON.stringify({ type: "config", cues: [{ id: 0, label: "A", color: "#fff", mode: "flash" }], hasMaster: true }));
  assert.equal(cfg.cues.length, 1);
  assert.equal(cfg.scenes.length, 0);
  assert.equal(cfg.hasMaster, true);
});

test("sendCue/sendScene validate id range and pressed type", () => {
  const { client, ws } = makeClient();
  assert.equal(client.sendCue(0, true), true);
  assert.deepEqual(lastSent(ws), { type: "cue", id: 0, pressed: true });
  assert.equal(client.sendCue(-1, true), false);
  assert.equal(client.sendCue(70000, true), false);
  assert.equal(client.sendScene(3, "not-a-bool"), false);
});

test("sendMaster clamps to [0,1]", () => {
  const { client, ws } = makeClient();
  client.sendMaster(1.5);
  assert.deepEqual(lastSent(ws), { type: "master", value: 1 });
  client.sendMaster(-0.5);
  assert.deepEqual(lastSent(ws), { type: "master", value: 0 });
});

// --- state (P1.3: device-pushed active cues + master level) ---------------

test("onState delivers active ids and master level", () => {
  let received = null;
  const { client, ws } = makeClient();
  client.onState((ids, master) => { received = { ids, master }; });
  ws.message(JSON.stringify({ type: "state", active: [0, 3, 5], master: 0.75 }));
  assert.deepEqual(received, { ids: [0, 3, 5], master: 0.75 });
});

test("onState filters out-of-range/non-integer active ids", () => {
  let received = null;
  const { client, ws } = makeClient();
  client.onState((ids) => { received = ids; });
  ws.message(JSON.stringify({ type: "state", active: [0, -1, 1.5, 70000, "x", 3], master: 1 }));
  assert.deepEqual(received, [0, 3]);
});

test("onState clamps an out-of-range master level", () => {
  const received = [];
  const { client, ws } = makeClient();
  client.onState((_ids, master) => received.push(master));
  ws.message(JSON.stringify({ type: "state", active: [], master: 1.5 }));
  ws.message(JSON.stringify({ type: "state", active: [], master: -0.5 }));
  assert.deepEqual(received, [1, 0]);
});

test("onState reports null master when the device omits the field", () => {
  let received = "unset";
  const { client, ws } = makeClient();
  client.onState((_ids, master) => { received = master; });
  ws.message(JSON.stringify({ type: "state", active: [1] }));
  assert.equal(received, null);
});

// --- eval channel ------------------------------------------------------

test("sendEval sends type/src/seq", () => {
  const { client, ws } = makeClient();
  assert.equal(client.sendEval("(+ 1 2)", 7), true);
  assert.deepEqual(lastSent(ws), { type: "eval", src: "(+ 1 2)", seq: 7 });
});

test("sendEval rejects a non-string src", () => {
  const { client } = makeClient();
  assert.equal(client.sendEval(42, 1), false);
});

test("onEvalResult delivers seq/ok/err, normalizing a missing err to null", () => {
  const results = [];
  const { ws } = (() => {
    const client = new WsClient("ws://test/ws");
    client.onEvalResult((r) => results.push(r));
    client.connect();
    const ws = MockWebSocket.lastInstance;
    ws.open();
    return { client, ws };
  })();
  ws.message(JSON.stringify({ type: "eval_result", seq: 7, ok: true }));
  ws.message(JSON.stringify({ type: "eval_result", seq: 8, ok: false, err: "1:1: unexpected symbol" }));
  assert.deepEqual(results[0], { seq: 7, ok: true, err: null });
  assert.deepEqual(results[1], { seq: 8, ok: false, err: "1:1: unexpected symbol" });
});

// --- script CRUD ---------------------------------------------------------

test("sendScriptList/Load/Save/Delete send the right shapes", () => {
  const { client, ws } = makeClient();
  client.sendScriptList();
  assert.deepEqual(lastSent(ws), { type: "script_list" });
  client.sendScriptLoad("verse");
  assert.deepEqual(lastSent(ws), { type: "script_load", name: "verse" });
  client.sendScriptSave("verse", "(fn f [t] t)");
  assert.deepEqual(lastSent(ws), { type: "script_save", name: "verse", src: "(fn f [t] t)" });
  client.sendScriptDelete("verse");
  assert.deepEqual(lastSent(ws), { type: "script_delete", name: "verse" });
});

test("sendScriptLoad/Save/Delete reject an empty/non-string name", () => {
  const { client } = makeClient();
  assert.equal(client.sendScriptLoad(""), false);
  assert.equal(client.sendScriptSave(null, "x"), false);
  assert.equal(client.sendScriptDelete(42), false);
});

test("onScripts normalizes names to a string array", () => {
  const names = [];
  const { ws } = (() => {
    const client = new WsClient("ws://test/ws");
    client.onScripts((n) => names.push(n));
    client.connect();
    const ws = MockWebSocket.lastInstance;
    ws.open();
    return { client, ws };
  })();
  ws.message(JSON.stringify({ type: "scripts", names: ["boot", "verse", 42, null] }));
  assert.deepEqual(names[0], ["boot", "verse"]);
});

test("onScript delivers name/src", () => {
  let received = null;
  const { ws } = (() => {
    const client = new WsClient("ws://test/ws");
    client.onScript((s) => { received = s; });
    client.connect();
    const ws = MockWebSocket.lastInstance;
    ws.open();
    return { client, ws };
  })();
  ws.message(JSON.stringify({ type: "script", name: "verse", src: "(fn f [t] t)" }));
  assert.deepEqual(received, { name: "verse", src: "(fn f [t] t)" });
});

// --- fx_error (unsolicited) ----------------------------------------------

test("onFxError delivers effect/err", () => {
  let received = null;
  const { ws } = (() => {
    const client = new WsClient("ws://test/ws");
    client.onFxError((e) => { received = e; });
    client.connect();
    const ws = MockWebSocket.lastInstance;
    ws.open();
    return { client, ws };
  })();
  ws.message(JSON.stringify({ type: "fx_error", effect: "verse#1", err: "attempt to index nil value" }));
  assert.deepEqual(received, { effect: "verse#1", err: "attempt to index nil value" });
});

test("malformed fx_error/script messages are ignored, not thrown", () => {
  const { ws } = (() => {
    const client = new WsClient("ws://test/ws");
    client.connect();
    const ws = MockWebSocket.lastInstance;
    ws.open();
    return { client, ws };
  })();
  assert.doesNotThrow(() => {
    ws.message(JSON.stringify({ type: "fx_error", err: "no effect field" }));
    ws.message(JSON.stringify({ type: "script", src: "no name field" }));
    ws.message("not json");
    ws.message(JSON.stringify({ type: "unknown_future_type", whatever: 1 }));
  });
});

// ---------------------------------------------------------------------------

if (failCount === 0) {
  console.log("\nAll tests passed.");
  process.exit(0);
} else {
  console.log(`\n${failCount} test(s) failed.`);
  process.exit(1);
}
