//
// device-sync.js — §5: connects the browser workspace to a live rig's
// existing script CRUD over WebSocket (script_list/script_load/
// script_save/script_delete -- see web/console/ws-client.js, the protocol
// this pairs with). Two layers:
//
//   - computeSyncStatus(): a pure function classifying one file's local vs.
//     device text against the last text both sides are known to have
//     agreed on ("synced text"). No git, no history -- just enough to
//     answer "who's ahead?" for the one prompt §5 asks for.
//   - createDeviceSync(client): a thin request/response wrapper over a
//     WsClient-shaped object (anything with onScripts/onScript/
//     sendScriptList/sendScriptLoad/sendScriptSave -- a real WsClient or a
//     test double), plus a session-only "last synced text" cache so
//     repeated syncs in the same session know which side moved. That cache
//     is deliberately NOT part of the persisted Project (project-model.js)
//     or its export -- it's a live-session convenience, not project data,
//     and resets on reconnect. That keeps the portability guarantee (§3)
//     simple: nothing here can end up in a project.zip.
//
// The patch (.show -> SHW1) never goes through this module -- it's a raw
// flashed partition (README_PROVISIONER.md), not something LittleFS
// script CRUD touches. Only .fnl shows sync live.

// --- pure reconciliation ---------------------------------------------------

// One of:
//   "missing-both"   neither side has this file (nothing to do)
//   "local-only"     only the workspace has it -- safe to push (creates it)
//   "device-only"    only the device has it -- safe to pull (brings it in)
//   "in-sync"        identical content, nothing to do
//   "local-changed"  workspace edited since the last known sync, device
//                    unchanged -- safe to push
//   "device-changed" device changed (e.g. live-coded at a gig) since the
//                    last known sync, workspace unchanged -- safe to pull
//   "conflict"       both sides changed (or never synced and they differ)
//                    -- must ask, per §5's "which side is newer?" prompt
export function computeSyncStatus({ localText, deviceText, syncedText = null }) {
  const hasLocal = localText != null;
  const hasDevice = deviceText != null;
  if (!hasLocal && !hasDevice) return "missing-both";
  if (hasLocal && !hasDevice) return "local-only";
  if (!hasLocal && hasDevice) return "device-only";
  if (localText === deviceText) return "in-sync";
  if (syncedText != null && deviceText === syncedText) return "local-changed";
  if (syncedText != null && localText === syncedText) return "device-changed";
  return "conflict";
}

// Buckets a project's show names against the device's script list, so the
// UI can render "3 only in workspace, 1 only on device, 2 in both (check
// status)" without per-file round trips for the ones that are obviously
// one-sided.
export function planReconciliation(localNames, remoteNames) {
  const local = new Set(localNames);
  const remote = new Set(remoteNames);
  return {
    localOnly: [...local].filter((n) => !remote.has(n)),
    deviceOnly: [...remote].filter((n) => !local.has(n)),
    both: [...local].filter((n) => remote.has(n)),
  };
}

// --- live orchestration over a WsClient-shaped object -----------------------

const DEFAULT_TIMEOUT_MS = 5000;

// Takes exclusive ownership of `client`'s onScripts/onScript callbacks --
// intended for a WsClient instance dedicated to the workspace's device-sync
// panel, not one shared with e.g. a live REPL that wants its own script
// list handling. Requests are serialized one in flight at a time per kind,
// matching this protocol's lack of request ids.
export function createDeviceSync(client, { timeoutMs = DEFAULT_TIMEOUT_MS } = {}) {
  let pendingList = null;
  const pendingLoads = new Map(); // name -> {resolve, reject}
  const synced = new Map(); // name -> { text, at }

  client.onScripts((names) => {
    if (pendingList) {
      pendingList.resolve(names);
      pendingList = null;
    }
  });
  client.onScript(({ name, src }) => {
    const p = pendingLoads.get(name);
    if (p) {
      pendingLoads.delete(name);
      p.resolve(src);
    }
  });

  function listRemote() {
    if (pendingList) return pendingList.promise;
    // The entry (and its `.promise` back-reference, used only for dedup by
    // a second concurrent caller) is filled in AFTER construction, since
    // the executor below can synchronously reject and clear `pendingList`
    // before `new Promise` even returns -- writing `.promise` onto it
    // unconditionally after that point would dereference null/undefined
    // and throw, orphaning the already-settled promise as an unhandled
    // rejection instead of the caller's awaited one.
    const entry = { resolve: null, reject: null, promise: null };
    const promise = new Promise((resolve, reject) => {
      entry.resolve = resolve;
      entry.reject = reject;
      pendingList = entry;
      if (!client.sendScriptList()) {
        pendingList = null;
        reject(new Error("device-sync: not connected"));
        return;
      }
      setTimeout(() => {
        if (pendingList === entry) {
          pendingList = null;
          reject(new Error("device-sync: timed out waiting for the script list"));
        }
      }, timeoutMs);
    });
    entry.promise = promise;
    return promise;
  }

  function loadRemote(name) {
    const existing = pendingLoads.get(name);
    if (existing) return existing.promise;
    // Same ordering hazard as listRemote() above -- see its comment.
    const entry = { resolve: null, reject: null, promise: null };
    const promise = new Promise((resolve, reject) => {
      entry.resolve = resolve;
      entry.reject = reject;
      pendingLoads.set(name, entry);
      if (!client.sendScriptLoad(name)) {
        pendingLoads.delete(name);
        reject(new Error("device-sync: not connected"));
        return;
      }
      setTimeout(() => {
        if (pendingLoads.get(name) === entry) {
          pendingLoads.delete(name);
          reject(new Error(`device-sync: timed out waiting for "${name}" from the device`));
        }
      }, timeoutMs);
    });
    entry.promise = promise;
    return promise;
  }

  function markSynced(name, text) {
    synced.set(name, { text, at: Date.now() });
  }

  function syncedTextFor(name) {
    return synced.has(name) ? synced.get(name).text : null;
  }

  // Fetches the device's current copy of `name` and classifies it against
  // `localText` and this session's synced-text cache. Does not mutate
  // anything -- the caller decides what to do with the status (see
  // pushFile/pullFile below).
  async function checkFile(name, localText) {
    let deviceText = null;
    try {
      deviceText = await loadRemote(name);
    } catch {
      deviceText = null; // not on the device (or unreachable) -- treated as absent
    }
    const status = computeSyncStatus({ localText, deviceText, syncedText: syncedTextFor(name) });
    return { name, status, localText, deviceText };
  }

  // Pushes local text to the device and records the sync point. Callers
  // should only call this after checkFile() confirms it's safe (or the
  // user explicitly resolved a conflict in favor of the workspace).
  function pushFile(name, text) {
    if (!client.sendScriptSave(name, text)) throw new Error("device-sync: not connected");
    markSynced(name, text);
  }

  // Records a pull -- the caller is responsible for writing `text` into
  // the project's show (project-model.js's setShowText/addShow); this just
  // updates the sync point so the next checkFile() call sees "in-sync".
  function recordPull(name, text) {
    markSynced(name, text);
  }

  return { listRemote, loadRemote, checkFile, pushFile, recordPull, syncedTextFor };
}
