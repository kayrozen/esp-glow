// test-reconcile.mjs — P1.3: pure-logic unit tests for
// reconcileActiveCues (app.js), the web console's optimistic-UI merge of
// local state against a device-pushed `state` message. WsClient's own
// `state` message parsing (the new `master` field, id filtering) is
// covered separately in scripts/test-ws-client.mjs, the established home
// for ws-client.js tests -- this file stays focused on the one pure
// function that's new here.
//
// Plain Node, no framework -- same style as web/shared/test-devcfg.mjs.
// Run: node web/console/test-reconcile.mjs (or `make test-console`)

import { reconcileActiveCues, SCENE_MARKER_BASE } from "./app.js";

let failures = 0;
let count = 0;
function check(name, cond, detail) {
  count++;
  if (!cond) {
    failures++;
    console.error(`FAIL: ${name}${detail ? " — " + detail : ""}`);
  } else {
    console.log(`PASS: ${name}`);
  }
}

function setEq(a, b) {
  if (a.size !== b.size) return false;
  for (const x of a) if (!b.has(x)) return false;
  return true;
}

{
  // A cue the client optimistically guessed active, but the device says
  // otherwise (e.g. another console released it first) -- reconciliation
  // must trust the device, not the stale local guess.
  const prev = new Set([1, 2]);
  const next = reconcileActiveCues(prev, [2, 3]);
  check("device cue ids replace local guess wholesale",
        setEq(next, new Set([2, 3])),
        `got ${[...next]}`);
}

{
  // Scene markers (local-only, the device never reports them) must
  // survive a reconcile untouched, even though the device's cue-id list
  // says nothing about them.
  const prev = new Set([1, SCENE_MARKER_BASE + 5]);
  const next = reconcileActiveCues(prev, [1]);
  check("scene markers survive reconciliation",
        setEq(next, new Set([1, SCENE_MARKER_BASE + 5])),
        `got ${[...next]}`);
}

{
  // Every cue released, device reports an empty active set -- scene
  // markers still survive; cue ids are fully cleared.
  const prev = new Set([1, 2, SCENE_MARKER_BASE + 5]);
  const next = reconcileActiveCues(prev, []);
  check("empty device state clears cue ids but keeps scene markers",
        setEq(next, new Set([SCENE_MARKER_BASE + 5])),
        `got ${[...next]}`);
}

{
  // A brand-new client (empty local state) reconciling its very first
  // `state` push -- this is the "new client mid-show" case; it should
  // end up with exactly what the device reports, nothing more/less.
  const prev = new Set();
  const next = reconcileActiveCues(prev, [0, 3, 5]);
  check("fresh client adopts the device's full active set",
        setEq(next, new Set([0, 3, 5])),
        `got ${[...next]}`);
}

{
  // Two consoles firing different cues, each seeing the SAME reconciled
  // device state, converge to identical local sets -- the "multiple
  // consoles stay consistent" requirement, modeled here as two
  // independent reconcile calls against the same device push.
  const consoleA = reconcileActiveCues(new Set([1]), [2, 3]);
  const consoleB = reconcileActiveCues(new Set([SCENE_MARKER_BASE + 9]), [2, 3]);
  check("two consoles converge on the same cue-id set from one device push",
        setEq(new Set([...consoleA].filter((id) => id < SCENE_MARKER_BASE)),
              new Set([...consoleB].filter((id) => id < SCENE_MARKER_BASE))),
        `A=${[...consoleA]} B=${[...consoleB]}`);
}

console.log(`\n${count - failures}/${count} passed`);
if (failures > 0) process.exit(1);
