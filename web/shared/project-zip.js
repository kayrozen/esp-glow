//
// project-zip.js — §3: the portability guarantee. Exports a Project (see
// project-model.js) to a single .zip -- the whole tree from §1, plus a
// project.json manifest -- and imports one back. Export -> import must be
// lossless; that round trip is the single most important test in this
// feature (test-project-zip.mjs).
//
// project.json is the authoritative source on import (it's the exact
// Project object, so re-hydrating it is exact by construction); the
// individual patch/…, controllers/…, shows/… files alongside it are for
// humans (unzip and read a .fnl in a text editor) and other tools, not
// re-parsed on import unless project.json is missing (a hand-assembled or
// older zip) -- see importProjectZip's fallback branch.
//
// No secrets: this module only ever accepts a Project (project-model.js's
// shape), which has no CFG1/WiFi fields at all -- there is nothing here
// that could reach into localStorage's devcfg blob even by accident. Keep
// it that way: never import devcfg.js from this file.

import { readZip } from "./importers/zip-lite.js";
import { writeZip } from "./zip-writer.js";
import { projectFromData } from "./project-model.js";

export const MANIFEST_NAME = "project.json";
export const BUNDLE_NAME = "bundle.shw1";
export const PROJECT_ZIP_FORMAT_VERSION = 1;

function textEntry(name, text) {
  return { name, data: new TextEncoder().encode(text) };
}

// The individual patch/…, controllers/…, shows/… text files (not the
// manifest or bundle) -- shared by exportProjectZip and the File System
// Access "Save to folder" writer (app.js), so both lay out the same tree.
export function projectTextFiles(project) {
  const files = [{ path: `patch/${project.patch.show.name}`, text: project.patch.show.text }];
  for (const f of project.patch.fixtures) files.push({ path: `patch/fixtures/${f.name}`, text: f.text });
  for (const c of project.controllers) files.push({ path: `controllers/${c.name}`, text: c.text });
  for (const s of project.shows) files.push({ path: `shows/${s.name}`, text: s.text });
  return files;
}

export function projectManifestJson(project) {
  return JSON.stringify({ formatVersion: PROJECT_ZIP_FORMAT_VERSION, project }, null, 2);
}

// opts.compiledBundle: optional Uint8Array of the last-compiled SHW1 bytes
// (§3: "export the compiled bundle too ... the exact bytes that produced a
// known-good rig"). Not re-derived here -- callers pass whatever the last
// successful compile produced, since project-zip.js has no WASM compiler
// access of its own.
export function exportProjectZip(project, opts = {}) {
  const entries = [textEntry(MANIFEST_NAME, projectManifestJson(project))];
  for (const f of projectTextFiles(project)) entries.push(textEntry(f.path, f.text));
  if (opts.compiledBundle instanceof Uint8Array) {
    entries.push({ name: BUNDLE_NAME, data: opts.compiledBundle });
  }
  return writeZip(entries);
}

// Rebuilds a Project from a bare file tree (no project.json) -- best
// effort, used only as a fallback for a zip that wasn't produced by
// exportProjectZip (hand-assembled, or from a future format this code
// doesn't understand). bootShow can't be recovered this way (it's not
// encoded in the path layout), so it comes back null; the UI should nudge
// the user to re-mark one.
function projectFromTree(files, fallbackName) {
  const patchShow = [...files.entries()].find(([p]) => p.startsWith("patch/") && !p.startsWith("patch/fixtures/"));
  const dec = new TextDecoder("utf-8");
  const readAll = (prefix) =>
    [...files.entries()]
      .filter(([p]) => p.startsWith(prefix) && p.length > prefix.length)
      .map(([p, bytes]) => ({ name: p.slice(prefix.length), text: dec.decode(bytes) }));

  return projectFromData({
    meta: { name: fallbackName },
    patch: {
      show: patchShow
        ? { name: patchShow[0].slice("patch/".length), text: dec.decode(patchShow[1]) }
        : { name: "show.show", text: "" },
      fixtures: readAll("patch/fixtures/"),
    },
    controllers: readAll("controllers/"),
    shows: readAll("shows/"),
    bootShow: null,
  });
}

// Rehydrates a Project from a Map<path, Uint8Array> laid out the way
// exportProjectZip (or the File System Access "Save to folder" writer)
// produces it -- shared by importProjectZip (after unzipping) and the
// provisioner's "Open folder" path (after walking a directory), so both
// get the same project.json-authoritative-with-tree-fallback behavior
// (see projectFromTree's header comment) from one place.
export function projectFromFileEntries(files, fallbackName = "Imported project") {
  const manifestBytes = files.get(MANIFEST_NAME);
  let project;
  if (manifestBytes) {
    const manifest = JSON.parse(new TextDecoder("utf-8").decode(manifestBytes));
    const raw = manifest && typeof manifest === "object" && "project" in manifest ? manifest.project : manifest;
    project = projectFromData(raw);
  } else {
    project = projectFromTree(files, fallbackName);
  }
  const bundleBytes = files.get(BUNDLE_NAME);
  return { project, compiledBundle: bundleBytes ? new Uint8Array(bundleBytes) : null };
}

// Returns { project, compiledBundle } -- compiledBundle is the Uint8Array
// from bundle.shw1 if the zip had one, else null. `inflateRaw` is passed
// through to readZip for Node tests (see zip-lite.js); the browser default
// needs nothing.
export async function importProjectZip(bytes, opts = {}) {
  const files = await readZip(bytes, opts);
  return projectFromFileEntries(files, "Imported project");
}
