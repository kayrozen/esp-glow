// test-project-model.mjs — data-model unit tests (§6) for project-model.js:
// create/rename/delete/duplicate for projects and every file kind, the
// boot-show marker, and legacy-workspace migration.
//
// Plain Node, no framework -- same style as web/shared/test-devcfg.mjs.
// Run: node web/shared/test-project-model.mjs

import * as M from "./project-model.js";

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
function throws(fn) {
  try {
    fn();
    return false;
  } catch {
    return true;
  }
}
function section(title) {
  console.log(`\n== ${title} ==`);
}

section("empty project shape");
{
  const p = M.emptyProject("My Rig");
  check("has an id", typeof p.id === "string" && p.id.length > 0);
  check("meta.name set", p.meta.name === "My Rig");
  check("created === modified on creation", p.meta.created === p.meta.modified);
  check("starts with no shows", p.shows.length === 0);
  check("starts with no bootShow", p.bootShow === null);
  check("starts with a patch show", p.patch.show.name === "show.show");
  check("starts with no fixtures", p.patch.fixtures.length === 0);
  check("starts with no controllers", p.controllers.length === 0);
}

section("project rename / duplicate");
{
  const p = M.emptyProject("Original");
  const beforeModified = p.meta.modified;
  M.renameProject(p, "Renamed");
  check("rename updates meta.name", p.meta.name === "Renamed");
  check("rename touches modified", p.meta.modified >= beforeModified);
  check("rename rejects empty name", throws(() => M.renameProject(p, "  ")));

  M.addShow(p, "boot.fnl", ";; boot\n");
  const copy = M.duplicateProject(p, "Copy of Renamed");
  check("duplicate gets a new id", copy.id !== p.id);
  check("duplicate gets the requested name", copy.meta.name === "Copy of Renamed");
  check("duplicate carries over shows", copy.shows.length === 1 && copy.shows[0].name === "boot.fnl");
  check("duplicate is independent (deep clone)", copy.shows !== p.shows);
  copy.shows[0].text = "changed";
  check("mutating the duplicate doesn't affect the original", p.shows[0].text === ";; boot\n");
}

section("shows: add/rename/delete/duplicate + boot marker");
{
  const p = M.emptyProject("Rig");
  M.addShow(p, "boot.fnl", ";; boot\n");
  check("first show ever added becomes boot automatically", p.bootShow === "boot.fnl");

  M.addShow(p, "set-friday.fnl", ";; friday\n");
  check("second show does not steal the boot marker", p.bootShow === "boot.fnl");
  check("rejects a .fnl-less name", throws(() => M.addShow(p, "no-extension")));
  check("rejects a duplicate name", throws(() => M.addShow(p, "boot.fnl")));

  M.setBootShow(p, "set-friday.fnl");
  check("boot marker is explicitly changeable", p.bootShow === "set-friday.fnl");
  check("setBootShow rejects an unknown name", throws(() => M.setBootShow(p, "nope.fnl")));

  M.renameShow(p, "set-friday.fnl", "set-saturday.fnl");
  check("renaming the boot show carries the marker along", p.bootShow === "set-saturday.fnl");
  check("renamed show is findable under the new name", p.shows.some((s) => s.name === "set-saturday.fnl"));
  check("renamed show is gone under the old name", !p.shows.some((s) => s.name === "set-friday.fnl"));

  const dup = M.duplicateShow(p, "boot.fnl");
  const dupName = p.shows.find((s) => s.text === ";; boot\n" && s.name !== "boot.fnl");
  check("duplicateShow creates a uniquely-named copy", !!dupName && dupName.name === "boot-2.fnl");
  check("duplicateShow returns the mutated project", dup === p);

  M.setShowText(p, "boot.fnl", ";; edited\n");
  check("setShowText updates the right file", p.shows.find((s) => s.name === "boot.fnl").text === ";; edited\n");

  M.deleteShow(p, "set-saturday.fnl");
  check("deleting the boot show falls back to another remaining show", p.bootShow !== null && p.bootShow !== "set-saturday.fnl");

  M.deleteShow(p, "boot.fnl");
  M.deleteShow(p, "boot-2.fnl");
  check("deleting every show clears the boot marker", p.shows.length === 0 && p.bootShow === null);
}

section("fixtures (patch.fixtures)");
{
  const p = M.emptyProject("Rig");
  M.addFixture(p, "torrent.fdef", "FIXTURE Torrent F1\nFOOTPRINT 16\n");
  check("fixture added", p.patch.fixtures.length === 1);
  check("rejects a .fdef-less name", throws(() => M.addFixture(p, "torrent")));
  check("rejects a duplicate name", throws(() => M.addFixture(p, "torrent.fdef")));

  M.renameFixture(p, "torrent.fdef", "beam.fdef");
  check("fixture renamed", p.patch.fixtures[0].name === "beam.fdef");

  M.duplicateFixture(p, "beam.fdef");
  check("duplicateFixture creates a unique second copy", p.patch.fixtures.map((f) => f.name).includes("beam-2.fdef"));

  M.deleteFixture(p, "beam.fdef");
  check("fixture deleted", !p.patch.fixtures.some((f) => f.name === "beam.fdef"));
  check("sibling copy survives the delete", p.patch.fixtures.some((f) => f.name === "beam-2.fdef"));
}

section("controllers (.mdef library)");
{
  const p = M.emptyProject("Rig");
  M.addController(p, "apc40.mdef", "CONTROLLER Akai APC40 mkII\nMIDI_CHANNEL 0\n");
  check("controller added", p.controllers.length === 1);
  check("rejects a .mdef-less name", throws(() => M.addController(p, "apc40")));

  M.renameController(p, "apc40.mdef", "akai-apc40.mdef");
  check("controller renamed", p.controllers[0].name === "akai-apc40.mdef");

  M.duplicateController(p, "akai-apc40.mdef");
  check("duplicateController creates a unique copy", p.controllers.map((c) => c.name).includes("akai-apc40-2.mdef"));

  M.deleteController(p, "akai-apc40.mdef");
  check("controller deleted", !p.controllers.some((c) => c.name === "akai-apc40.mdef"));
}

section("patch (one .show per project)");
{
  const p = M.emptyProject("Rig");
  M.setPatchShowText(p, "SHOW 2\nUNIVERSE 1 DMX\n");
  check("patch show text updated", p.patch.show.text === "SHOW 2\nUNIVERSE 1 DMX\n");
}

section("uniqueName helper");
{
  check("no collision returns the name unchanged", M.uniqueName("a.fnl", []) === "a.fnl");
  check("first collision becomes -2", M.uniqueName("a.fnl", ["a.fnl"]) === "a-2.fnl");
  check("second collision becomes -3", M.uniqueName("a.fnl", ["a.fnl", "a-2.fnl"]) === "a-3.fnl");
}

section("legacy single-workspace migration (§2/§6: no data loss)");
{
  const legacyWorkspace = {
    show: { name: "my-show.show", text: "SHOW 1\nUNIVERSE 1 DMX\n" },
    boot: { name: "boot.fnl", text: "(glow.cue.go :breathe)\n" },
    fdefs: [{ name: "torrent.fdef", text: "FIXTURE Torrent F1\n" }],
    mdefs: [{ name: "apc40.mdef", text: "CONTROLLER Akai APC40 mkII\n" }],
  };
  const p = M.migrateLegacyWorkspace(legacyWorkspace, "My Project");
  check("migrated project keeps the patch show text", p.patch.show.text === legacyWorkspace.show.text);
  check("migrated project keeps the patch show name", p.patch.show.name === "my-show.show");
  check("migrated project keeps every fixture", p.patch.fixtures.length === 1 && p.patch.fixtures[0].text === legacyWorkspace.fdefs[0].text);
  check("migrated project keeps every controller", p.controllers.length === 1 && p.controllers[0].text === legacyWorkspace.mdefs[0].text);
  check("migrated project turns the old single boot.fnl into a show", p.shows.length === 1 && p.shows[0].name === "boot.fnl");
  check("migrated project marks that show as boot", p.bootShow === "boot.fnl");
  check("migrated project keeps the boot script's text exactly", p.shows[0].text === legacyWorkspace.boot.text);

  const empty = M.migrateLegacyWorkspace(undefined);
  check("migrating nothing still returns a valid, empty-ish project", empty.shows.length === 0 && empty.bootShow === null);
}

section("projectFromData is defensive against partial/corrupt input");
{
  const p = M.projectFromData({});
  check("missing fields fall back to sane defaults", p.patch.show.name === "show.show" && p.shows.length === 0);

  const stale = M.projectFromData({ shows: [{ name: "a.fnl", text: "x" }], bootShow: "nonexistent.fnl" });
  check("a bootShow pointing at a missing file is dropped, not trusted", stale.bootShow === null);

  const ok = M.projectFromData({ shows: [{ name: "a.fnl", text: "x" }], bootShow: "a.fnl" });
  check("a bootShow pointing at a real file survives", ok.bootShow === "a.fnl");
}

console.log(`\n${count - failures}/${count} passed`);
if (failures > 0) process.exit(1);
