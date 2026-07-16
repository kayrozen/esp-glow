//
// project-store.js — persistence orchestration (§2) over a pluggable KV
// adapter (kv-memory-adapter.js for tests/fallback, kv-indexeddb-adapter.js
// in the browser). Owns key naming, the project index, the active-project
// pointer, first-run seeding, and legacy-workspace migration. All the
// actual data-shape logic (CRUD, boot marker, migration mapping) lives in
// project-model.js -- this file is the thin "where do the bytes go" layer,
// kept separate so it can be unit-tested with an in-memory adapter without
// touching a browser at all.
//
// Key naming: "proj:<id>" per project, plus a couple of singleton keys.
// Listing scans adapter.keys() for the "proj:" prefix -- fine at the
// project counts a rig workspace will ever have (this is a lighting show
// library, not a database).

import { projectFromData, migrateLegacyWorkspace, emptyProject, duplicateProject as dupProject, renameProject as renameProjectModel } from "./project-model.js";

const PROJECT_PREFIX = "proj:";
const ACTIVE_KEY = "active-project-id";
const MIGRATED_KEY = "migrated-legacy-workspace-v1";
const SEEDED_KEY = "seeded-default-project-v1";

export function createProjectStore(adapter) {
  function keyFor(id) {
    return PROJECT_PREFIX + id;
  }

  async function list() {
    const keys = await adapter.keys();
    const ids = keys.filter((k) => typeof k === "string" && k.startsWith(PROJECT_PREFIX));
    const projects = await Promise.all(ids.map((k) => adapter.get(k)));
    return projects
      .filter(Boolean)
      .map((data) => {
        const p = projectFromData(data);
        return { id: p.id, name: p.meta.name, created: p.meta.created, modified: p.meta.modified };
      })
      .sort((a, b) => b.modified - a.modified);
  }

  async function load(id) {
    const data = await adapter.get(keyFor(id));
    return data ? projectFromData(data) : null;
  }

  async function save(project) {
    await adapter.set(keyFor(project.id), project);
    return project;
  }

  async function remove(id) {
    await adapter.delete(keyFor(id));
    const activeId = await getActiveId();
    if (activeId === id) await setActiveId(null);
  }

  async function create(name) {
    const project = emptyProject(name);
    await save(project);
    return project;
  }

  async function duplicate(id, newName) {
    const project = await load(id);
    if (!project) throw new Error(`no project with id "${id}"`);
    const copy = dupProject(project, newName);
    await save(copy);
    return copy;
  }

  async function rename(id, name) {
    const project = await load(id);
    if (!project) throw new Error(`no project with id "${id}"`);
    renameProjectModel(project, name);
    await save(project);
    return project;
  }

  async function getActiveId() {
    const id = await adapter.get(ACTIVE_KEY);
    return typeof id === "string" ? id : null;
  }

  async function setActiveId(id) {
    if (id === null) await adapter.delete(ACTIVE_KEY);
    else await adapter.set(ACTIVE_KEY, id);
  }

  // First-ever load with no projects at all: seed one from the caller's
  // default-content factory (the demo show/fixtures the static editor used
  // to hardcode) so the workspace isn't blank the very first time someone
  // opens the page. Runs at most once, ever -- tracked by SEEDED_KEY so
  // deleting the seeded project doesn't bring it back on the next reload.
  async function ensureSeeded(makeDefaultProject) {
    const alreadySeeded = await adapter.get(SEEDED_KEY);
    if (alreadySeeded) return null;
    const existing = await list();
    await adapter.set(SEEDED_KEY, true);
    if (existing.length > 0) return null; // something's already here (e.g. migration ran first)
    const project = makeDefaultProject();
    await save(project);
    await setActiveId(project.id);
    return project;
  }

  // §2/§6: migrate a pre-existing single-workspace localStorage blob into a
  // real Project, exactly once. `legacyWorkspace` is the already-
  // JSON-parsed old shape (or null/undefined if there was none) -- reading
  // localStorage and deciding whether to delete the old key afterward is
  // the caller's job (keeps this module storage-adapter-agnostic and
  // trivially testable). Returns the new Project, or null if there was
  // nothing to migrate or migration already ran.
  async function migrateLegacyIfNeeded(legacyWorkspace, projectName) {
    const alreadyMigrated = await adapter.get(MIGRATED_KEY);
    if (alreadyMigrated) return null;
    await adapter.set(MIGRATED_KEY, true);
    if (!legacyWorkspace) return null;
    const project = migrateLegacyWorkspace(legacyWorkspace, projectName);
    await save(project);
    await setActiveId(project.id);
    // A migration means real prior work existed -- don't also seed the
    // canned demo project on top of it.
    await adapter.set(SEEDED_KEY, true);
    return project;
  }

  return {
    list,
    load,
    save,
    remove,
    create,
    duplicate,
    rename,
    getActiveId,
    setActiveId,
    ensureSeeded,
    migrateLegacyIfNeeded,
  };
}
