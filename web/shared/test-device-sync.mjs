// test-device-sync.mjs — §5/§6: push/pull against a mock WebSocket client,
// the pure computeSyncStatus() classification, and proof the reconcile
// path never silently clobbers a newer side.
//
// Run: node web/shared/test-device-sync.mjs

import { computeSyncStatus, planReconciliation, createDeviceSync } from "./device-sync.js";

let failures = 0;
let count = 0;
function check(name, cond, detail) {
  count++;
  if (!cond) {
    failures++;
    console.error(`FAIL: ${name}${detail !== undefined ? " -- " + JSON.stringify(detail) : ""}`);
  } else {
    console.log(`ok:   ${name}`);
  }
}
function section(title) {
  console.log(`\n== ${title} ==`);
}

// A minimal WsClient-shaped double: same onScripts/onScript/sendScriptList/
// sendScriptLoad/sendScriptSave surface as web/console/ws-client.js, but
// driven synchronously/deterministically by the test instead of a real
// socket.
function makeFakeClient({ connected = true, scripts = {} } = {}) {
  const cb = { scripts: null, script: null };
  const sent = [];
  return {
    sent,
    scripts, // name -> src, the "device"'s current state; tests mutate this directly
    connected,
    onScripts(fn) { cb.scripts = fn; },
    onScript(fn) { cb.script = fn; },
    sendScriptList() {
      sent.push({ type: "script_list" });
      if (!this.connected) return false;
      queueMicrotask(() => cb.scripts && cb.scripts(Object.keys(this.scripts)));
      return true;
    },
    sendScriptLoad(name) {
      sent.push({ type: "script_load", name });
      if (!this.connected) return false;
      queueMicrotask(() => {
        if (!cb.script) return;
        if (Object.prototype.hasOwnProperty.call(this.scripts, name)) {
          cb.script({ name, src: this.scripts[name] });
        }
        // A name absent on the device never gets a `script` reply in the
        // real protocol -- checkFile()'s timeout/absent-handling covers
        // that below.
      });
      return true;
    },
    sendScriptSave(name, src) {
      sent.push({ type: "script_save", name, src });
      if (!this.connected) return false;
      this.scripts[name] = src;
      return true;
    },
  };
}

async function run() {
  section("computeSyncStatus: pure classification");
  {
    check("both absent", computeSyncStatus({ localText: null, deviceText: null }) === "missing-both");
    check("local only -> safe to push", computeSyncStatus({ localText: "a", deviceText: null }) === "local-only");
    check("device only -> safe to pull", computeSyncStatus({ localText: null, deviceText: "a" }) === "device-only");
    check("identical content -> in sync", computeSyncStatus({ localText: "a", deviceText: "a" }) === "in-sync");
    check(
      "device unchanged since sync, local edited -> local-changed (push)",
      computeSyncStatus({ localText: "a2", deviceText: "a", syncedText: "a" }) === "local-changed",
    );
    check(
      "local unchanged since sync, device edited (e.g. live-coded at a gig) -> device-changed (pull)",
      computeSyncStatus({ localText: "a", deviceText: "a3", syncedText: "a" }) === "device-changed",
    );
    check(
      "both sides changed since the last sync -> conflict, must ask",
      computeSyncStatus({ localText: "a2", deviceText: "a3", syncedText: "a" }) === "conflict",
    );
    check(
      "never synced and they already differ -> conflict, must ask (not a silent guess)",
      computeSyncStatus({ localText: "a2", deviceText: "a3", syncedText: null }) === "conflict",
    );
  }

  section("planReconciliation: bucketing local vs. device script names");
  {
    const plan = planReconciliation(["boot.fnl", "set-friday.fnl", "shared.fnl"], ["boot.fnl", "shared.fnl", "jam.fnl"]);
    check("workspace-only shows identified", plan.localOnly.length === 1 && plan.localOnly[0] === "set-friday.fnl");
    check("device-only scripts identified", plan.deviceOnly.length === 1 && plan.deviceOnly[0] === "jam.fnl");
    check("names present on both sides identified for a status check", plan.both.sort().join(",") === "boot.fnl,shared.fnl");
  }

  section("createDeviceSync: pull a device-only script (device-only status)");
  {
    const client = makeFakeClient({ scripts: { "jam.fnl": "(glow.cue.go :jam)\n" } });
    const sync = createDeviceSync(client, { timeoutMs: 200 });
    const result = await sync.checkFile("jam.fnl", null);
    check("device-only script is classified correctly", result.status === "device-only");
    check("device text is surfaced for the pull", result.deviceText === "(glow.cue.go :jam)\n");
  }

  section("createDeviceSync: push a local-only script (local-only status)");
  {
    const client = makeFakeClient({ scripts: {} });
    const sync = createDeviceSync(client, { timeoutMs: 200 });
    const result = await sync.checkFile("new.fnl", ";; brand new\n");
    check("local-only script is classified correctly", result.status === "local-only");
    sync.pushFile("new.fnl", ";; brand new\n");
    check("push actually sends script_save", client.sent.some((m) => m.type === "script_save" && m.name === "new.fnl"));
    check("device now has the pushed script", client.scripts["new.fnl"] === ";; brand new\n");
  }

  section("reconcile: device has a newer version -- does NOT silently clobber");
  {
    const client = makeFakeClient({ scripts: { "boot.fnl": "(glow.cue.go :breathe)\n" } });
    const sync = createDeviceSync(client, { timeoutMs: 200 });

    // First sync: workspace pushes its version, establishing a synced
    // baseline (as if the user had pushed once already).
    const original = "(glow.cue.go :breathe)\n";
    sync.pushFile("boot.fnl", original);
    check("baseline push landed on the device", client.scripts["boot.fnl"] === original);

    // The user live-codes on the device at a gig -- the device now has a
    // newer version than the workspace's still-original local copy.
    client.scripts["boot.fnl"] = "(glow.cue.go :breathe) ;; live tweak\n";

    const result = await sync.checkFile("boot.fnl", original);
    check("device-changed is reported, not silently applied", result.status === "device-changed");
    check("the UI still has the local text available to compare, unclobbered", result.localText === original);
    check("the UI has the device's newer text to show in the prompt", result.deviceText === "(glow.cue.go :breathe) ;; live tweak\n");
  }

  section("reconcile: both sides changed -- conflict, never auto-resolved");
  {
    const client = makeFakeClient({ scripts: { "boot.fnl": "(base)\n" } });
    const sync = createDeviceSync(client, { timeoutMs: 200 });
    sync.pushFile("boot.fnl", "(base)\n");

    client.scripts["boot.fnl"] = "(base) ;; device edit\n"; // device moved on
    const localEdited = "(base) ;; local edit\n"; // workspace also moved on, independently

    const result = await sync.checkFile("boot.fnl", localEdited);
    check("genuinely divergent edits are reported as a conflict", result.status === "conflict");
    check("neither side was overwritten by checkFile alone", client.scripts["boot.fnl"] === "(base) ;; device edit\n");
  }

  section("recordPull updates the sync baseline so a later check reports in-sync");
  {
    const client = makeFakeClient({ scripts: { "boot.fnl": "(device version)\n" } });
    const sync = createDeviceSync(client, { timeoutMs: 200 });
    const first = await sync.checkFile("boot.fnl", "(old local)\n");
    check("initially a conflict/first-sight (no baseline yet)", first.status === "conflict" || first.status === "device-only");

    // The user resolves the prompt in favor of the device -- the UI pulls
    // deviceText into the workspace and records the new baseline.
    sync.recordPull("boot.fnl", first.deviceText);
    const after = await sync.checkFile("boot.fnl", first.deviceText);
    check("after pulling, the same file reports in-sync", after.status === "in-sync");
  }

  section("checkFile against a disconnected client doesn't throw or fabricate device state");
  {
    const client = makeFakeClient({ connected: false, scripts: {} });
    const sync = createDeviceSync(client, { timeoutMs: 50 });
    const result = await sync.checkFile("boot.fnl", "local text");
    check("a disconnected device is treated as absent, not errored", result.status === "local-only");
  }

  console.log(`\n${count - failures}/${count} passed`);
  if (failures > 0) process.exit(1);
}

run();
