// test-project-store.mjs — project-store.js against the in-memory KV
// adapter: create/list/load/duplicate/rename/delete projects, the active-
// project pointer, first-run seeding, and legacy-workspace migration
// (§6: "no data loss").
//
// Run: node web/shared/test-project-store.mjs

import { createMemoryAdapter } from "./kv-memory-adapter.js";
import { createProjectStore } from "./project-store.js";
import { emptyProject, addShow } from "./project-model.js";

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

async function run() {
  section("create / list / load");
  {
    const store = createProjectStore(createMemoryAdapter());
    check("starts empty", (await store.list()).length === 0);

    const a = await store.create("Show A");
    const b = await store.create("Show B");
    const listed = await store.list();
    check("both projects listed", listed.length === 2);
    check("listing includes names", listed.some((p) => p.name === "Show A") && listed.some((p) => p.name === "Show B"));

    const loaded = await store.load(a.id);
    check("load round-trips the project", loaded.meta.name === "Show A" && loaded.id === a.id);
    check("loading an unknown id returns null", (await store.load("nonexistent")) === null);
    void b;
  }

  section("save persists in-place edits");
  {
    const store = createProjectStore(createMemoryAdapter());
    const p = await store.create("Rig");
    addShow(p, "boot.fnl", ";; hi\n");
    await store.save(p);
    const reloaded = await store.load(p.id);
    check("edited shows survive a save/load round trip", reloaded.shows.length === 1 && reloaded.shows[0].name === "boot.fnl");
  }

  section("rename / duplicate / delete");
  {
    const store = createProjectStore(createMemoryAdapter());
    const p = await store.create("Original");
    addShow(p, "boot.fnl", ";; hi\n");
    await store.save(p);

    const renamed = await store.rename(p.id, "Renamed");
    check("rename persists", renamed.meta.name === "Renamed");
    check("rename is visible on reload", (await store.load(p.id)).meta.name === "Renamed");

    const copy = await store.duplicate(p.id, "Copy");
    check("duplicate gets a distinct id", copy.id !== p.id);
    check("duplicate is independently persisted", (await store.load(copy.id)).shows[0].name === "boot.fnl");
    check("original still lists alongside the duplicate", (await store.list()).length === 2);

    await store.remove(copy.id);
    check("delete removes it from the list", (await store.list()).length === 1);
    check("delete makes load return null", (await store.load(copy.id)) === null);
  }

  section("active-project pointer");
  {
    const store = createProjectStore(createMemoryAdapter());
    check("no active project initially", (await store.getActiveId()) === null);
    const p = await store.create("Rig");
    await store.setActiveId(p.id);
    check("active id round-trips", (await store.getActiveId()) === p.id);

    await store.remove(p.id);
    check("deleting the active project clears the pointer", (await store.getActiveId()) === null);
  }

  section("first-run seeding");
  {
    const adapter = createMemoryAdapter();
    const store = createProjectStore(adapter);
    const seeded = await store.ensureSeeded(() => emptyProject("Demo"));
    check("seeds a default project on a truly empty store", seeded !== null && seeded.meta.name === "Demo");
    check("seeded project becomes active", (await store.getActiveId()) === seeded.id);

    // User deletes the seeded demo -- a second ensureSeeded() call (e.g. on
    // a later reload) must NOT bring it back; SEEDED_KEY makes this a
    // one-shot.
    await store.remove(seeded.id);
    const secondCall = await store.ensureSeeded(() => emptyProject("Demo 2"));
    check("ensureSeeded never re-seeds after the first run, even post-delete", secondCall === null);
    check("store stays empty (deleted demo doesn't come back)", (await store.list()).length === 0);
  }

  section("ensureSeeded is a no-op when projects already exist");
  {
    const adapter = createMemoryAdapter();
    const store = createProjectStore(adapter);
    await store.create("Real work");
    const seeded = await store.ensureSeeded(() => emptyProject("Demo"));
    check("does not seed on top of existing projects", seeded === null);
    check("only the real project is present", (await store.list()).length === 1);
  }

  section("legacy-workspace migration (§6: no data loss)");
  {
    const adapter = createMemoryAdapter();
    const store = createProjectStore(adapter);
    const legacyWorkspace = {
      show: { name: "my-show.show", text: "SHOW 1\nUNIVERSE 1 DMX\n" },
      boot: { name: "boot.fnl", text: "(glow.cue.go :breathe)\n" },
      fdefs: [{ name: "torrent.fdef", text: "FIXTURE Torrent F1\n" }],
      mdefs: [],
    };
    const migrated = await store.migrateLegacyIfNeeded(legacyWorkspace, "My Project");
    check("migration produces a project", migrated !== null && migrated.meta.name === "My Project");
    check("migrated project is persisted", (await store.load(migrated.id)).patch.show.text === legacyWorkspace.show.text);
    check("migrated project becomes active", (await store.getActiveId()) === migrated.id);

    // A second call (e.g. the app reloading) must not migrate again, even
    // if the caller still has (or re-reads) the same legacy blob -- that
    // would duplicate the project every reload.
    const second = await store.migrateLegacyIfNeeded(legacyWorkspace, "My Project");
    check("migration runs at most once", second === null);
    check("no duplicate project was created", (await store.list()).length === 1);

    // ensureSeeded must not layer the canned demo on top of real migrated
    // work.
    const seeded = await store.ensureSeeded(() => emptyProject("Demo"));
    check("seeding is suppressed after a real migration", seeded === null);
    check("still exactly one (the migrated) project", (await store.list()).length === 1);
  }

  section("migrating when there was nothing to migrate");
  {
    const store = createProjectStore(createMemoryAdapter());
    const result = await store.migrateLegacyIfNeeded(null, "My Project");
    check("null legacy workspace migrates to nothing", result === null);
    check("store stays empty", (await store.list()).length === 0);
    // A later ensureSeeded should still be free to seed the demo, since
    // there really was no prior work.
    const seeded = await store.ensureSeeded(() => emptyProject("Demo"));
    check("seeding still works when migration found nothing", seeded !== null);
  }

  console.log(`\n${count - failures}/${count} passed`);
  if (failures > 0) process.exit(1);
}

run();
