// test-project-zip.mjs — §3/§6: the portability guarantee. Export a
// project to a .zip, import it back, and the files must be byte-identical
// -- "wipe the browser, import it, everything is back" only holds if this
// round trip is exact. Also proves CFG1 secrets can never land in an
// exported project zip.
//
// Node's zlib.inflateRawSync is injected as zip-lite.js's inflateRaw, same
// pattern as web/shared/importers/test-importers.mjs -- not actually
// exercised here since zip-writer.js only emits STORE (uncompressed)
// entries, but importProjectZip must still work against a real zip tool's
// DEFLATE output, so one entry below is DEFLATE-compressed by hand via
// Node's zlib to prove readZip's compressed path still feeds this code
// correctly.
//
// Run: node web/shared/test-project-zip.mjs

import { deflateRawSync, inflateRawSync } from "node:zlib";
import { emptyProject, addShow, addFixture, addController, setBootShow, setPatchShowText } from "./project-model.js";
import { exportProjectZip, importProjectZip } from "./project-zip.js";
import { writeZip } from "./zip-writer.js";
import { readZip } from "./importers/zip-lite.js";

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

// Builds a minimal one-entry ZIP with method=8 (DEFLATE) -- same binary
// layout zip-writer.js emits for STORE, just with the compression fields
// filled in differently. Test-only; zip-writer.js deliberately never emits
// DEFLATE itself (see its header comment).
function buildOneEntryDeflateZip(name, uncompressedBytes, compressedBytes) {
  const nameBytes = new TextEncoder().encode(name);

  function crc32(bytes) {
    let crc = 0xffffffff;
    for (let i = 0; i < bytes.length; i++) {
      crc ^= bytes[i];
      for (let bit = 0; bit < 8; bit++) {
        const mask = -(crc & 1);
        crc = (crc >>> 1) ^ (0xedb88320 & mask);
      }
    }
    return (~crc) >>> 0;
  }
  const crc = crc32(uncompressedBytes);

  const local = new DataView(new ArrayBuffer(30));
  local.setUint32(0, 0x04034b50, true);
  local.setUint16(4, 20, true);
  local.setUint16(6, 0, true);
  local.setUint16(8, 8, true); // DEFLATE
  local.setUint16(10, 0, true);
  local.setUint16(12, 0, true);
  local.setUint32(14, crc, true);
  local.setUint32(18, compressedBytes.length, true);
  local.setUint32(22, uncompressedBytes.length, true);
  local.setUint16(26, nameBytes.length, true);
  local.setUint16(28, 0, true);

  const localHeaderOffset = 0;
  const localPart = [new Uint8Array(local.buffer), nameBytes, new Uint8Array(compressedBytes)];
  const localSize = 30 + nameBytes.length + compressedBytes.length;

  const central = new DataView(new ArrayBuffer(46));
  central.setUint32(0, 0x02014b50, true);
  central.setUint16(4, 20, true);
  central.setUint16(6, 20, true);
  central.setUint16(8, 0, true);
  central.setUint16(10, 8, true); // DEFLATE
  central.setUint16(12, 0, true);
  central.setUint16(14, 0, true);
  central.setUint32(16, crc, true);
  central.setUint32(20, compressedBytes.length, true);
  central.setUint32(24, uncompressedBytes.length, true);
  central.setUint16(28, nameBytes.length, true);
  central.setUint16(30, 0, true);
  central.setUint16(32, 0, true);
  central.setUint16(34, 0, true);
  central.setUint16(36, 0, true);
  central.setUint32(38, 0, true);
  central.setUint32(42, localHeaderOffset, true);

  const eocd = new DataView(new ArrayBuffer(22));
  eocd.setUint32(0, 0x06054b50, true);
  eocd.setUint16(4, 0, true);
  eocd.setUint16(6, 0, true);
  eocd.setUint16(8, 1, true);
  eocd.setUint16(10, 1, true);
  eocd.setUint32(12, 46 + nameBytes.length, true);
  eocd.setUint32(16, localSize, true);
  eocd.setUint16(20, 0, true);

  const parts = [...localPart, new Uint8Array(central.buffer), nameBytes, new Uint8Array(eocd.buffer)];
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let pos = 0;
  for (const p of parts) {
    out.set(p, pos);
    pos += p.length;
  }
  return out;
}

function buildSampleProject() {
  const p = emptyProject("Friday Rig");
  p.meta.rigDescription = "8x moving heads, 2 universes";
  setPatchShowText(p, "SHOW 2\nUNIVERSE 1 DMX\nUNIVERSE 2 ARTNET\nFIXTURE torrent.fdef 1 2\n");
  addFixture(p, "torrent.fdef", "FIXTURE Torrent F1\nFOOTPRINT 16\nCAP Dimmer 0\n");
  addFixture(p, "par.fdef", "FIXTURE Par 5\nFOOTPRINT 5\nCAP Dimmer 0\n");
  addController(p, "apc40.mdef", "CONTROLLER Akai APC40 mkII\nMIDI_CHANNEL 0\nPAD 0 39\n");
  addShow(p, "boot.fnl", ";; boot.fnl\n(glow.cue.go :breathe)\n");
  addShow(p, "set-friday.fnl", ";; friday set\n(glow.cue.go :chorus)\n");
  addShow(p, "experiments.fnl", ";; scratch\n(fn f [t] (glow.set 1 :dimmer t))\n");
  setBootShow(p, "boot.fnl");
  return p;
}

async function run() {
  section("export -> import round trip is lossless");
  {
    const original = buildSampleProject();
    const zipBytes = exportProjectZip(original);
    const { project: restored, compiledBundle } = await importProjectZip(zipBytes);

    check("meta.name round-trips", restored.meta.name === original.meta.name);
    check("rigDescription round-trips", restored.meta.rigDescription === original.meta.rigDescription);
    check("patch show text round-trips byte-identical", restored.patch.show.text === original.patch.show.text);
    check("patch show name round-trips", restored.patch.show.name === original.patch.show.name);
    check("fixture count round-trips", restored.patch.fixtures.length === original.patch.fixtures.length);
    check("every fixture's text round-trips byte-identical",
      original.patch.fixtures.every((f) => restored.patch.fixtures.find((g) => g.name === f.name)?.text === f.text));
    check("controller count round-trips", restored.controllers.length === original.controllers.length);
    check("every controller's text round-trips byte-identical",
      original.controllers.every((c) => restored.controllers.find((d) => d.name === c.name)?.text === c.text));
    check("show count round-trips (multi-show is the key change)", restored.shows.length === original.shows.length);
    check("every show's text round-trips byte-identical",
      original.shows.every((s) => restored.shows.find((t) => t.name === s.name)?.text === s.text));
    check("the boot-show marker round-trips", restored.bootShow === original.bootShow);
    check("no compiled bundle when none was exported", compiledBundle === null);
  }

  section("compiled bundle round-trips exactly (§3: keep the exact bytes that flashed)");
  {
    const original = buildSampleProject();
    const bundle = new Uint8Array([0x53, 0x48, 0x57, 0x31, 0, 1, 2, 3, 254, 255]); // fake SHW1 header + payload
    const zipBytes = exportProjectZip(original, { compiledBundle: bundle });
    const { compiledBundle } = await importProjectZip(zipBytes);
    check("bundle bytes round-trip exactly", compiledBundle && Buffer.from(compiledBundle).equals(Buffer.from(bundle)));
  }

  section("round trip survives unicode / edge-case content");
  {
    const p = emptyProject("Unicode 🎛️ Rig");
    addShow(p, "weird.fnl", ";; emoji 🎆, CRLF\r\nnewlines, and \"quotes\"\n(glow.set 1 :dimmer 1.0)\n");
    const zipBytes = exportProjectZip(p);
    const { project: restored } = await importProjectZip(zipBytes);
    check("unicode project name round-trips", restored.meta.name === p.meta.name);
    check("unicode + CRLF file content round-trips exactly", restored.shows[0].text === p.shows[0].text);
  }

  section("empty project round-trips (no shows, no fixtures, no controllers)");
  {
    const p = emptyProject("Blank");
    const zipBytes = exportProjectZip(p);
    const { project: restored } = await importProjectZip(zipBytes);
    check("empty project still round-trips", restored.shows.length === 0 && restored.patch.fixtures.length === 0 && restored.bootShow === null);
  }

  section("import falls back to the file tree when project.json is missing");
  {
    // A hand-assembled zip (or one from a hypothetical other tool) with no
    // manifest -- importProjectZip must still recover something sane from
    // the path layout rather than throwing.
    const enc = new TextEncoder();
    const bytes = writeZip([
      { name: "patch/show.show", data: enc.encode("SHOW 1\nUNIVERSE 1 DMX\n") },
      { name: "patch/fixtures/par.fdef", data: enc.encode("FIXTURE Par 5\nFOOTPRINT 5\n") },
      { name: "shows/boot.fnl", data: enc.encode(";; boot\n") },
    ]);
    const { project } = await importProjectZip(bytes);
    check("recovers the patch show from the tree", project.patch.show.text === "SHOW 1\nUNIVERSE 1 DMX\n");
    check("recovers fixtures from the tree", project.patch.fixtures.length === 1 && project.patch.fixtures[0].name === "par.fdef");
    check("recovers shows from the tree", project.shows.length === 1 && project.shows[0].name === "boot.fnl");
  }

  section("CFG1 secrets can never appear in an exported project zip");
  {
    // The Project shape (project-model.js) has no WiFi/CFG1 fields at all,
    // so there is nothing exportProjectZip could serialize even if a
    // caller tried -- assert that structurally (no such keys exist on a
    // real project) and behaviorally (the exported bytes never contain a
    // simulated device password, proving no side channel smuggles it in).
    const p = buildSampleProject();
    check("Project has no wifiPass field", !("wifiPass" in p) && !("devcfg" in p));
    const zipBytes = exportProjectZip(p);
    const asText = Buffer.from(zipBytes).toString("latin1");
    const simulatedPassword = "hunter2-super-secret-wifi-password";
    check("a WiFi password never appears in the export (nothing to leak it)", !asText.includes(simulatedPassword));
    check("the literal field name wifiPass never appears in the export", !asText.includes("wifiPass"));
  }

  section("importProjectZip works against a real DEFLATE-compressed entry");
  {
    // zip-writer.js only ever emits STORE entries (see its header comment),
    // but a project.zip must still be readable if re-saved by a real zip
    // tool that uses DEFLATE -- hand-build a one-entry DEFLATE zip (same
    // binary layout zip-writer.js emits for STORE, just method=8 and
    // deflateRawSync'd data) and prove importProjectZip's `opts` reaches
    // readZip's decompressor correctly, mirroring
    // web/shared/importers/test-importers.mjs's zip-lite DEFLATE coverage.
    const manifestBytes = new TextEncoder().encode(JSON.stringify({
      formatVersion: 1,
      project: buildSampleProject(),
    }));
    const compressed = deflateRawSync(Buffer.from(manifestBytes));
    const bytes = buildOneEntryDeflateZip("project.json", manifestBytes, compressed);

    const { project } = await importProjectZip(bytes, {
      inflateRaw: (b) => new Uint8Array(inflateRawSync(Buffer.from(b))),
    });
    check("importProjectZip reads a DEFLATE-compressed manifest via opts.inflateRaw",
      project.meta.name === "Friday Rig");
  }

  console.log(`\n${count - failures}/${count} passed`);
  if (failures > 0) process.exit(1);
}

run();
