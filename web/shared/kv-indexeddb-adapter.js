//
// kv-indexeddb-adapter.js — browser IndexedDB-backed key/value adapter,
// same get/set/delete/keys interface as kv-memory-adapter.js (see that
// file's header for why the interface is this small). This is the "durable
// but browser-local" tier from §2: bigger than localStorage's ~5MB cap and
// survives more, but still gone on a cache clear or a different browser --
// project-zip.js's export/import is the actual portability guarantee.
//
// Not exercised by the Node test suite (no `indexedDB` global there);
// project-store.js's logic is proven against kv-memory-adapter.js instead,
// which implements the identical interface, so the coverage that matters
// (create/rename/delete/duplicate, migration, active-project tracking) is
// still real. This file is intentionally a thin, boring wrapper.

function reqToPromise(req) {
  return new Promise((resolve, reject) => {
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}

export function indexedDbAvailable() {
  return typeof indexedDB !== "undefined";
}

export function createIndexedDbAdapter(opts = {}) {
  const dbName = opts.dbName || "esp-glow-projects";
  const storeName = opts.storeName || "kv";
  let dbPromise = null;

  function openDb() {
    if (!dbPromise) {
      dbPromise = new Promise((resolve, reject) => {
        const req = indexedDB.open(dbName, 1);
        req.onupgradeneeded = () => {
          if (!req.result.objectStoreNames.contains(storeName)) {
            req.result.createObjectStore(storeName);
          }
        };
        req.onsuccess = () => resolve(req.result);
        req.onerror = () => reject(req.error);
        req.onblocked = () => reject(new Error("kv-indexeddb-adapter: open blocked by another tab"));
      });
    }
    return dbPromise;
  }

  return {
    kind: "indexeddb",
    durable: true,
    async get(key) {
      const db = await openDb();
      const tx = db.transaction(storeName, "readonly");
      return reqToPromise(tx.objectStore(storeName).get(key));
    },
    async set(key, value) {
      const db = await openDb();
      const tx = db.transaction(storeName, "readwrite");
      await reqToPromise(tx.objectStore(storeName).put(value, key));
    },
    async delete(key) {
      const db = await openDb();
      const tx = db.transaction(storeName, "readwrite");
      await reqToPromise(tx.objectStore(storeName).delete(key));
    },
    async keys() {
      const db = await openDb();
      const tx = db.transaction(storeName, "readonly");
      return reqToPromise(tx.objectStore(storeName).getAllKeys());
    },
  };
}
