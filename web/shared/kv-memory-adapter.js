//
// kv-memory-adapter.js — an in-memory key/value adapter implementing the
// same tiny async interface as kv-indexeddb-adapter.js: get/set/delete/
// keys. Used by project-store.js's Node tests (no real IndexedDB in Node)
// and as a same-tab fallback if IndexedDB is unavailable (very old
// browsers, some locked-down embedded webviews) -- see §2's trade-off
// note: this tier doesn't even survive a page reload, so the UI must not
// present it as durable.
//
// Values are deep-cloned in and out, mirroring IndexedDB's structured-clone
// semantics -- a caller mutating an object after set() (or after get())
// must not be able to reach back into the store's internal state.

function clone(v) {
  if (v === undefined) return undefined;
  if (typeof structuredClone === "function") return structuredClone(v);
  return JSON.parse(JSON.stringify(v));
}

export function createMemoryAdapter() {
  const map = new Map();
  return {
    kind: "memory",
    durable: false,
    async get(key) {
      return clone(map.get(key));
    },
    async set(key, value) {
      map.set(key, clone(value));
    },
    async delete(key) {
      map.delete(key);
    },
    async keys() {
      return Array.from(map.keys());
    },
  };
}
