//
// project-model.js — the Project data model (§1 of the multi-show
// workspace plan). Pure data + pure functions: no DOM, no storage, no
// WASM. Every operation here takes a Project and returns a (possibly the
// same, mutated-in-place) Project, so it's trivially unit-testable under
// plain Node — see test-project-model.mjs.
//
// Shape:
//
//   Project
//   ├─ id                 stable id, assigned once at creation (storage key)
//   ├─ meta   { name, created, modified, rigDescription }
//   ├─ patch
//   │   ├─ show            { name: "show.show", text }        -- ONE per project
//   │   └─ fixtures        [{ name: "torrent.fdef", text }, …]
//   ├─ controllers         [{ name: "apc40.mdef", text }, …]
//   ├─ shows               [{ name: "boot.fnl", text }, …]     -- MANY
//   └─ bootShow            name of the show in `shows` that's flashed as
//                          startup, or null if none is marked yet
//
// Deliberately NOT in this shape: CFG1 (WiFi password etc.) -- that's
// per-device, lives in its own localStorage key (devcfg.js), and must
// never be reachable from a Project so it can never leak into an export
// (§3's "no secrets" requirement). Keeping it out of this module entirely
// is the enforcement mechanism: there's no field here a serializer could
// accidentally walk into.

// --- id / time helpers ---------------------------------------------------

export function newId() {
  if (typeof crypto !== "undefined" && crypto.randomUUID) return crypto.randomUUID();
  // Fallback for environments without crypto.randomUUID (old Node, etc.) --
  // not cryptographically strong, but this is a storage key, not a secret.
  return `id-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 10)}`;
}

function now() {
  return Date.now();
}

function touch(project) {
  project.meta.modified = now();
  return project;
}

// Exposed for callers that mutate patch/fixtures/controllers/shows arrays
// directly (e.g. app.js's drag-drop file import, which upserts by name
// rather than going through addFixture/addController/addShow's
// no-duplicate guard) and still need to bump `modified` correctly.
export const touchProject = touch;

// --- name validation / uniqueness -----------------------------------------

export class ProjectModelError extends Error {}

const EXT = {
  show: ".show",
  fdef: ".fdef",
  mdef: ".mdef",
  fnl: ".fnl",
};

function requireExt(name, kind) {
  const ext = EXT[kind];
  if (typeof name !== "string" || name.trim().length === 0) {
    throw new ProjectModelError(`${kind} name must not be empty`);
  }
  if (!name.toLowerCase().endsWith(ext)) {
    throw new ProjectModelError(`"${name}" must end in "${ext}"`);
  }
  return name;
}

// Appends "-2", "-3", … before the extension until `name` is unique against
// `existingNames` (case-sensitive, matching the filesystem these files are
// meant to round-trip through). Used by duplicate*() and the "New …"
// sidebar buttons.
export function uniqueName(name, existingNames) {
  const taken = new Set(existingNames);
  if (!taken.has(name)) return name;
  const m = /^(.*?)(\.[^.]+)$/.exec(name);
  const base = m ? m[1] : name;
  const ext = m ? m[2] : "";
  let n = 2;
  let candidate = `${base}-${n}${ext}`;
  while (taken.has(candidate)) {
    n++;
    candidate = `${base}-${n}${ext}`;
  }
  return candidate;
}

// --- project construction --------------------------------------------------

// A brand-new, empty project: no shows, no fixtures, no controllers, just
// a blank patch. Used by "New project".
export function emptyProject(name = "Untitled project") {
  const t = now();
  return {
    id: newId(),
    meta: { name, created: t, modified: t, rigDescription: "" },
    patch: {
      show: { name: "show.show", text: "SHOW 1\n\nUNIVERSE 1 DMX\n" },
      fixtures: [],
    },
    controllers: [],
    shows: [],
    bootShow: null,
  };
}

// Builds a project from plain data (already in Project shape) -- used by
// import (project-zip.js) and by callers restoring from storage. Fills in
// any missing pieces defensively so a partially-written/older export still
// loads instead of throwing deep in the UI.
export function projectFromData(data) {
  const t = now();
  const p = {
    id: typeof data.id === "string" && data.id ? data.id : newId(),
    meta: {
      name: data?.meta?.name ?? "Untitled project",
      created: Number.isFinite(data?.meta?.created) ? data.meta.created : t,
      modified: Number.isFinite(data?.meta?.modified) ? data.meta.modified : t,
      rigDescription: typeof data?.meta?.rigDescription === "string" ? data.meta.rigDescription : "",
    },
    patch: {
      show: {
        name: data?.patch?.show?.name || "show.show",
        text: typeof data?.patch?.show?.text === "string" ? data.patch.show.text : "",
      },
      fixtures: Array.isArray(data?.patch?.fixtures)
        ? data.patch.fixtures.map((f) => ({ name: f.name, text: f.text ?? "" }))
        : [],
    },
    controllers: Array.isArray(data?.controllers)
      ? data.controllers.map((c) => ({ name: c.name, text: c.text ?? "" }))
      : [],
    shows: Array.isArray(data?.shows)
      ? data.shows.map((s) => ({ name: s.name, text: s.text ?? "" }))
      : [],
    bootShow: typeof data?.bootShow === "string" ? data.bootShow : null,
  };
  // A bootShow pointing at a file that doesn't exist (corrupt/hand-edited
  // export) is worse than none -- fall back to null rather than the UI
  // silently treating a stale name as "no boot show marked".
  if (p.bootShow && !p.shows.some((s) => s.name === p.bootShow)) p.bootShow = null;
  return p;
}

// Deep clone via structuredClone where available (Node 17+/modern
// browsers), falling back to JSON round-trip -- this data is always plain
// JSON-serializable (no Dates/Maps/etc.), so either works.
function clone(v) {
  if (typeof structuredClone === "function") return structuredClone(v);
  return JSON.parse(JSON.stringify(v));
}

export function duplicateProject(project, newName) {
  const t = now();
  const copy = clone(project);
  copy.id = newId();
  copy.meta.name = newName || `${project.meta.name} copy`;
  copy.meta.created = t;
  copy.meta.modified = t;
  return copy;
}

export function renameProject(project, name) {
  if (typeof name !== "string" || name.trim().length === 0) {
    throw new ProjectModelError("project name must not be empty");
  }
  project.meta.name = name.trim();
  return touch(project);
}

// --- legacy migration (§2: "don't lose anyone's existing work") -----------

// Converts the OLD single-workspace shape (DEFAULT_WORKSPACE in app.js:
// { show: {name,text}, boot: {name,text}, fdefs: [...], mdefs: [...] }) --
// one show, one Fennel script, into a Project. The one boot.fnl becomes the
// first (and boot-marked) entry in the new multi-show `shows` array.
export function migrateLegacyWorkspace(workspace, projectName = "My Project") {
  const t = now();
  const shows = [];
  if (workspace && workspace.boot && typeof workspace.boot.text === "string") {
    shows.push({ name: workspace.boot.name || "boot.fnl", text: workspace.boot.text });
  }
  return {
    id: newId(),
    meta: { name: projectName, created: t, modified: t, rigDescription: "" },
    patch: {
      show: {
        name: workspace?.show?.name || "show.show",
        text: typeof workspace?.show?.text === "string" ? workspace.show.text : "",
      },
      fixtures: Array.isArray(workspace?.fdefs)
        ? workspace.fdefs.map((f) => ({ name: f.name, text: f.text }))
        : [],
    },
    controllers: Array.isArray(workspace?.mdefs)
      ? workspace.mdefs.map((m) => ({ name: m.name, text: m.text }))
      : [],
    shows,
    bootShow: shows.length > 0 ? shows[0].name : null,
  };
}

// --- patch (one .show + fixture library) -----------------------------------

export function setPatchShowText(project, text) {
  project.patch.show.text = String(text);
  return touch(project);
}

export function addFixture(project, name, text = "FIXTURE New Fixture\nFOOTPRINT 1\nCAP Dimmer 0\n") {
  requireExt(name, "fdef");
  if (project.patch.fixtures.some((f) => f.name === name)) {
    throw new ProjectModelError(`a fixture named "${name}" already exists`);
  }
  project.patch.fixtures.push({ name, text });
  return touch(project);
}

export function renameFixture(project, oldName, newName) {
  requireExt(newName, "fdef");
  const f = project.patch.fixtures.find((x) => x.name === oldName);
  if (!f) throw new ProjectModelError(`no fixture named "${oldName}"`);
  if (newName !== oldName && project.patch.fixtures.some((x) => x.name === newName)) {
    throw new ProjectModelError(`a fixture named "${newName}" already exists`);
  }
  f.name = newName;
  return touch(project);
}

export function deleteFixture(project, name) {
  project.patch.fixtures = project.patch.fixtures.filter((x) => x.name !== name);
  return touch(project);
}

export function duplicateFixture(project, name) {
  const f = project.patch.fixtures.find((x) => x.name === name);
  if (!f) throw new ProjectModelError(`no fixture named "${name}"`);
  const newName = uniqueName(name, project.patch.fixtures.map((x) => x.name));
  project.patch.fixtures.push({ name: newName, text: f.text });
  return touch(project);
}

// --- controllers (.mdef library) -------------------------------------------

export function addController(project, name, text = "CONTROLLER New Controller\nMIDI_CHANNEL 0\nPAD 0 7\n") {
  requireExt(name, "mdef");
  if (project.controllers.some((c) => c.name === name)) {
    throw new ProjectModelError(`a controller named "${name}" already exists`);
  }
  project.controllers.push({ name, text });
  return touch(project);
}

export function renameController(project, oldName, newName) {
  requireExt(newName, "mdef");
  const c = project.controllers.find((x) => x.name === oldName);
  if (!c) throw new ProjectModelError(`no controller named "${oldName}"`);
  if (newName !== oldName && project.controllers.some((x) => x.name === newName)) {
    throw new ProjectModelError(`a controller named "${newName}" already exists`);
  }
  c.name = newName;
  return touch(project);
}

export function deleteController(project, name) {
  project.controllers = project.controllers.filter((x) => x.name !== name);
  return touch(project);
}

export function duplicateController(project, name) {
  const c = project.controllers.find((x) => x.name === name);
  if (!c) throw new ProjectModelError(`no controller named "${name}"`);
  const newName = uniqueName(name, project.controllers.map((x) => x.name));
  project.controllers.push({ name: newName, text: c.text });
  return touch(project);
}

// --- shows (.fnl library -- the key multi-show change) ---------------------

export function addShow(project, name, text = ";; new show\n") {
  requireExt(name, "fnl");
  if (project.shows.some((s) => s.name === name)) {
    throw new ProjectModelError(`a show named "${name}" already exists`);
  }
  project.shows.push({ name, text });
  // The very first show a project gets is the obvious boot candidate --
  // explicit per §4 ("make it changeable"), but defaulting an otherwise-
  // unmarked single show avoids a project that compiles/flashes fine yet
  // silently boots nothing.
  if (project.bootShow === null) project.bootShow = name;
  return touch(project);
}

export function renameShow(project, oldName, newName) {
  requireExt(newName, "fnl");
  const s = project.shows.find((x) => x.name === oldName);
  if (!s) throw new ProjectModelError(`no show named "${oldName}"`);
  if (newName !== oldName && project.shows.some((x) => x.name === newName)) {
    throw new ProjectModelError(`a show named "${newName}" already exists`);
  }
  s.name = newName;
  if (project.bootShow === oldName) project.bootShow = newName;
  return touch(project);
}

export function deleteShow(project, name) {
  project.shows = project.shows.filter((x) => x.name !== name);
  if (project.bootShow === name) {
    project.bootShow = project.shows.length > 0 ? project.shows[0].name : null;
  }
  return touch(project);
}

export function duplicateShow(project, name) {
  const s = project.shows.find((x) => x.name === name);
  if (!s) throw new ProjectModelError(`no show named "${name}"`);
  const newName = uniqueName(name, project.shows.map((x) => x.name));
  project.shows.push({ name: newName, text: s.text });
  return touch(project);
}

// The boot-show marker (§4: "which show is boot?" must be explicit and
// changeable -- it's what actually gets baked into the flash image).
export function setBootShow(project, name) {
  if (name !== null && !project.shows.some((s) => s.name === name)) {
    throw new ProjectModelError(`no show named "${name}"`);
  }
  project.bootShow = name;
  return touch(project);
}

export function getBootShow(project) {
  if (!project.bootShow) return null;
  return project.shows.find((s) => s.name === project.bootShow) ?? null;
}

export function setShowText(project, name, text) {
  const s = project.shows.find((x) => x.name === name);
  if (!s) throw new ProjectModelError(`no show named "${name}"`);
  s.text = String(text);
  return touch(project);
}
