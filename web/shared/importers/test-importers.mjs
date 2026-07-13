// test-importers.mjs -- pure-JS tests for the QLC+/OFL/GDTF importers,
// run under plain Node (no npm, no test framework -- matches the rest of
// this repo's JS, e.g. web/provision-wasm/smoke-test-wasm.mjs).
//
// Run: node web/shared/importers/test-importers.mjs
// (or `make test-importers`, which builds the fdef_check round-trip
// oracle first)
//
// Real fixture files under testdata/ are the primary test material --
// see testdata/NOTICE.md for where each one came from. The goal per the
// task spec: footprint preserved exactly, 16-bit pairs correctly formed,
// named slots with the right DMX spans, continuous ranges only where the
// source says so, unmapped channels become Generic without shifting
// offsets, mode selection changes the output, defaults carried, and a
// round-trip through the actual PFX2 compiler (fdef_check, the host build
// of the same C++ the WASM editor uses) confirms the emitted .fdef
// compiles to the channel map and ranges expected.

import { readFileSync, writeFileSync, unlinkSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { execFileSync } from "node:child_process";
import zlib from "node:zlib";

import { parseXML } from "./xml-lite.js";
import { readZip } from "./zip-lite.js";
import { emitFdef, fitRangeBudget, MAX_RANGES, MAX_RANGE_NAME_BLOB, makeCap } from "./model.js";
import * as qlcplus from "./qlcplus.js";
import * as ofl from "./ofl.js";
import * as gdtf from "./gdtf.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const DATA = join(__dirname, "testdata");
const REPO_ROOT = join(__dirname, "..", "..", "..");
const FDEF_CHECK = join(REPO_ROOT, "fdef_check");

let failures = 0;
let count = 0;
function check(name, cond, detail) {
  count++;
  if (!cond) {
    failures++;
    console.log(`  FAIL: ${name}${detail !== undefined ? " -- " + JSON.stringify(detail) : ""}`);
  } else {
    console.log(`  ok:   ${name}`);
  }
}
function section(title) {
  console.log(`\n== ${title} ==`);
}
function readTestFile(...parts) {
  return readFileSync(join(DATA, ...parts), "utf8");
}
function readTestBytes(...parts) {
  return new Uint8Array(readFileSync(join(DATA, ...parts)));
}

// Every DMX offset in [0, footprint) must be covered by exactly one cap's
// coarse or fine field -- "never drop a channel" and "no offset shifting"
// both reduce to this one invariant.
function assertNoGapsOrOverlaps(model) {
  const covered = new Array(model.footprint).fill(0);
  for (const c of model.caps) {
    covered[c.coarse]++;
    if (c.fine != null) covered[c.fine]++;
  }
  const gaps = [];
  const overlaps = [];
  for (let i = 0; i < model.footprint; i++) {
    if (covered[i] === 0) gaps.push(i);
    if (covered[i] > 1) overlaps.push(i);
  }
  return { gaps, overlaps };
}

function capAt(model, offset) {
  return model.caps.find((c) => c.coarse === offset);
}

// Round-trips a model through emitFdef -> fdef_check (parseFixtureDef ->
// encodeProfile -> parseProfile, the real host build of the same C++ the
// WASM editor compiles). Applies fitRangeBudget first, exactly like a
// real importer's buildModel does, so this never feeds fdef_check text
// that couldn't possibly fit PFX2's budget.
function roundTrip(model) {
  const { model: fitted } = fitRangeBudget(model);
  const text = emitFdef(fitted);
  const tmp = join(__dirname, `.tmp-roundtrip-${process.pid}.fdef`);
  writeFileSync(tmp, text);
  try {
    const out = execFileSync(FDEF_CHECK, [tmp], { encoding: "utf8" });
    return { text, fitted, result: JSON.parse(out) };
  } finally {
    try { unlinkSync(tmp); } catch { /* already gone */ }
  }
}

// ===========================================================================
section("xml-lite");

{
  const root = parseXML(`<?xml version="1.0"?><!-- c --><a x="1" y="2&amp;3"><b>hi &lt;there&gt;</b><c/></a>`);
  check("root tag", root.tag === "a");
  check("attr decoding", root.attr("y") === "2&3");
  check("child text + entity decoding", root.child("b").text === "hi <there>");
  check("self-closing child parsed", root.child("c") != null);
  let threw = false;
  try { parseXML("<a><b></a>"); } catch { threw = true; }
  check("mismatched tags throw", threw);
}

// ===========================================================================
section("zip-lite");

{
  const inflateRaw = (b) => new Uint8Array(zlib.inflateRawSync(Buffer.from(b)));
  const storeBytes = readTestBytes("gdtf", "led-par-64-rgbw.gdtf");
  const deflateBytes = readTestBytes("gdtf", "ESP-Glow@Test-Beam.gdtf");
  const storeEntries = await readZip(storeBytes, { inflateRaw });
  const deflateEntries = await readZip(deflateBytes, { inflateRaw });
  check("STORE entry readable", storeEntries.has("description.xml"));
  check("DEFLATE entry readable", deflateEntries.has("description.xml"));
  check("DEFLATE decompresses to well-formed XML", (() => {
    try { parseXML(new TextDecoder().decode(deflateEntries.get("description.xml"))); return true; }
    catch { return false; }
  })());
}

// ===========================================================================
section("model.js: emitFdef golden text + validation");

{
  const model = {
    name: "Test Par", footprint: 4, isHead: false, panRangeDeg: 540, tiltRangeDeg: 270,
    caps: [
      makeCap({ cap: "Dimmer", coarse: 0 }),
      makeCap({ cap: "Red", coarse: 1 }),
      makeCap({ cap: "ShutterStrobe", coarse: 2, default: 200, inverted: true, ranges: [
        { from: 0, to: 31, name: "Closed", continuous: false },
        { from: 32, to: 255, name: "Strobe", continuous: true },
      ] }),
      makeCap({ cap: "Pan", coarse: 3, fine: null }),
    ],
  };
  const text = emitFdef(model);
  const expected =
`FIXTURE Test Par
FOOTPRINT 4
CAP Dimmer 0 - 0
CAP Red 1 - 0
CAP ShutterStrobe 2 - 200 inv
  SLOT 0 31 Closed
  RANGE 32 255 Strobe
CAP Pan 3 - 0
`;
  check("emitFdef exact grammar", text === expected, text);

  let threw = false;
  try { emitFdef({ ...model, footprint: 300 }); } catch { threw = true; }
  check("emitFdef rejects out-of-range footprint", threw);

  // A '#' or newline in a source name must not corrupt the grammar (see
  // provision.cpp's stripComments, which runs before tokenizing).
  const dirty = emitFdef({
    name: "Weird # Name\nwith newline", footprint: 1, isHead: false,
    caps: [makeCap({ cap: "Generic", coarse: 0, ranges: [{ from: 0, to: 5, name: "a # b", continuous: false }] })],
  });
  check("sanitizeName strips '#'/newlines", !dirty.includes("#") && dirty.split("\n").filter((l) => l.trim()).length === 4, dirty);
}

// ===========================================================================
section("model.js: fitRangeBudget never exceeds PFX2's device-side limits");

{
  const manyRanges = Array.from({ length: 40 }, (_, i) => ({ from: i * 6, to: i * 6 + 5, name: `slot ${i} with a fairly long descriptive name`, continuous: false }));
  const model = {
    name: "Overloaded", footprint: 3, isHead: false,
    caps: [
      makeCap({ cap: "Generic", coarse: 0, ranges: manyRanges }),
      makeCap({ cap: "ColorWheel", coarse: 1, ranges: manyRanges.slice(0, 40) }),
      makeCap({ cap: "Dimmer", coarse: 2 }),
    ],
  };
  const { model: fitted, dropped } = fitRangeBudget(model);
  const total = fitted.caps.reduce((n, c) => n + c.ranges.length, 0);
  check("total ranges within MAX_RANGES", total <= MAX_RANGES, total);
  const nameBytes = fitted.caps.reduce((n, c) => n + c.ranges.reduce((m, r) => m + (r.name ? Buffer.byteLength(r.name, "utf8") + 1 : 0), 0), 0);
  check("name blob within MAX_RANGE_NAME_BLOB", nameBytes <= MAX_RANGE_NAME_BLOB, nameBytes);
  check("Generic dropped before ColorWheel", dropped.some((d) => d.cap === "Generic"));
  check("footprint/offsets untouched by trimming", fitted.footprint === 3 && capAt(fitted, 2).cap === "Dimmer");
  const { result } = roundTrip(model);
  check("still compiles end-to-end after trimming", result.ok === true, result);
}

// ===========================================================================
section("QLC+: Generic RGB par (simple, multi-mode)");

{
  const parsed = qlcplus.parseQlcPlusQxf(readTestFile("qlcplus", "Generic-RGB.qxf"));
  const modes = qlcplus.listModes(parsed);
  check("5 modes listed", modes.length === 5, modes);

  const { model: rgbDimmer } = qlcplus.buildModel(parsed, "RGB Dimmer");
  check("RGB Dimmer footprint", rgbDimmer.footprint === 4);
  check("RGB Dimmer channel order", ["Red", "Green", "Blue", "Dimmer"].every((cap, i) => rgbDimmer.caps[i].cap === cap), rgbDimmer.caps.map((c) => c.cap));
  check("no ranges on plain colour channels", rgbDimmer.caps.every((c) => c.ranges.length === 0));
  const gaps1 = assertNoGapsOrOverlaps(rgbDimmer);
  check("RGB Dimmer: no gaps/overlaps", gaps1.gaps.length === 0 && gaps1.overlaps.length === 0, gaps1);

  const { model: dimmerRgb } = qlcplus.buildModel(parsed, "Dimmer RGB");
  check("mode selection changes channel order", dimmerRgb.caps[0].cap === "Dimmer" && rgbDimmer.caps[0].cap === "Red");
  check("mode selection changes footprint correctly (same count here)", dimmerRgb.footprint === 4);

  const { model: rgbOnly } = qlcplus.buildModel(parsed, "RGB");
  check("3-channel mode footprint", rgbOnly.footprint === 3);

  const { result } = roundTrip(rgbDimmer);
  check("Generic-RGB round-trips through fdef_check", result.ok === true, result);
  check("round-trip footprint matches", result.footprint === 4);
}

// ===========================================================================
section("QLC+: Clay Paky Sharpy (moving head, 16-bit pan/tilt, wheels, multi-mode)");

let sharpyStandardModel; // reused by the cross-format comparison below
{
  const parsed = qlcplus.parseQlcPlusQxf(readTestFile("qlcplus", "Clay-Paky-Sharpy.qxf"));
  const modes = qlcplus.listModes(parsed);
  check("2 modes (Standard 16ch, Vector 20ch)", modes.length === 2 && modes[0].footprint === 16 && modes[1].footprint === 20, modes);

  const { model, warnings } = qlcplus.buildModel(parsed, "Standard");
  sharpyStandardModel = model;
  check("footprint preserved exactly (16)", model.footprint === 16);
  check("isHead detected", model.isHead === true);
  check("panRangeDeg/tiltRangeDeg from Physical/Focus", model.panRangeDeg === 540 && model.tiltRangeDeg === 250, [model.panRangeDeg, model.tiltRangeDeg]);

  const pan = capAt(model, 9), tilt = capAt(model, 11);
  check("Pan is 16-bit: coarse=9 fine=10", pan?.cap === "Pan" && pan.fine === 10, pan);
  check("Tilt is 16-bit: coarse=11 fine=12", tilt?.cap === "Tilt" && tilt.fine === 12, tilt);
  check("Pan/Tilt fine offsets don't get their own CAP entries", !capAt(model, 10) && !capAt(model, 12));

  const colorWheel = capAt(model, 0);
  check("ColorWheel mapped at offset 0", colorWheel?.cap === "ColorWheel");
  const redSlot = colorWheel.ranges.find((r) => r.name === "Red");
  check("ColorWheel has a discrete 'Red' slot with the source's DMX span", redSlot && redSlot.from === 9 && redSlot.to === 12 && redSlot.continuous === false, redSlot);
  const spinRange = colorWheel.ranges.find((r) => r.continuous);
  check("ColorWheel's wheel-spin entry is continuous (RANGE, not SLOT)", spinRange && spinRange.from === 128 && spinRange.to === 255, spinRange);
  check("ColorWheel's discrete colour slots are NOT marked continuous", colorWheel.ranges.filter((r) => !r.continuous).length === 30);

  const gobo = capAt(model, 3);
  check("Gobo mapped at offset 3", gobo?.cap === "Gobo");
  const goboStop = gobo.ranges.find((r) => r.name === "Stop");
  check("Gobo's 'Stop' between the two rotation sweeps is discrete", goboStop && goboStop.continuous === false, goboStop);
  check("Gobo has exactly 2 continuous rotation ranges (CW and CCW)", gobo.ranges.filter((r) => r.continuous).length === 2);

  const prism = capAt(model, 4), prismRot = capAt(model, 5);
  check("Prism Insertion -> Prism (own channel)", prism?.cap === "Prism");
  check("Prism Rotation -> PrismRotation (separate channel, separate capability)", prismRot?.cap === "PrismRotation");

  const shutter = capAt(model, 1);
  check("ShutterStrobe default lands inside a slot actually named \"Open\"",
    shutter.ranges.some((r) => r.name === "Open" && shutter.default >= r.from && shutter.default <= r.to),
    { default: shutter.default, ranges: shutter.ranges.filter((r) => r.name === "Open") });
  check("ShutterStrobe default is not 0 (never boots dark)", shutter.default !== 0);

  const speedGeneric = [13, 14, 15].map((o) => capAt(model, o));
  check("housekeeping channels (Function/Reset/LampControl) -> Generic, flagged unmapped", speedGeneric.every((c) => c.cap === "Generic" && c.unmapped));
  check("...but still carry their named states (Generic isn't a dead end)", speedGeneric.every((c) => c.ranges.length > 0));

  const gaps = assertNoGapsOrOverlaps(model);
  check("no gaps or overlaps across all 16 offsets", gaps.gaps.length === 0 && gaps.overlaps.length === 0, gaps);
  check("warnings are informational only, parse still succeeded", Array.isArray(warnings));

  const { model: vectorModel } = qlcplus.buildModel(parsed, "Vector");
  check("Vector mode: footprint grows to 20 without disturbing 0..15", vectorModel.footprint === 20 &&
    capAt(vectorModel, 9).fine === 10 && capAt(vectorModel, 0).cap === "ColorWheel");
  const extraChannels = [16, 17, 18, 19].map((o) => capAt(vectorModel, o));
  check("Vector's 4 extra Speed/Time channels -> plain linear Generic (no bogus ranges)",
    extraChannels.every((c) => c.cap === "Generic" && c.ranges.length === 0), extraChannels);

  const { result } = roundTrip(model);
  check("Sharpy Standard round-trips through fdef_check (even after budget trimming)", result.ok === true, result);
  check("round-trip footprint == 16", result.footprint === 16);
  const rtPan = result.caps.find((c) => c.coarse === 9);
  check("round-trip preserves the Pan 16-bit pair exactly", rtPan?.cap === "Pan" && rtPan.fine === 10, rtPan);
}

// ===========================================================================
section("OFL: RGB fader (fine-channel aliases across 8/16/24-bit modes)");

{
  const parsed = ofl.parseOflJson(readTestFile("ofl", "rgb-fader.json"));
  const { model: m16 } = ofl.buildModel(parsed, "16 bit");
  check("16-bit mode: footprint 6", m16.footprint === 6);
  const red = capAt(m16, 0), green = capAt(m16, 2), blue = capAt(m16, 4);
  check("Red 16-bit pair via fineChannelAliases", red?.cap === "Red" && red.fine === 1, red);
  check("Green 16-bit pair via fineChannelAliases", green?.cap === "Green" && green.fine === 3, green);
  check("Blue 16-bit pair via fineChannelAliases", blue?.cap === "Blue" && blue.fine === 5, blue);

  const { model: m8 } = ofl.buildModel(parsed, "8 bit");
  check("8-bit mode: same channels, no fine pairing", m8.footprint === 3 && capAt(m8, 0).fine == null);

  const { model: m24, warnings } = ofl.buildModel(parsed, "24 bit");
  check("24-bit mode: footprint 9 (3rd byte kept, not dropped)", m24.footprint === 9);
  check("fine^2 byte becomes its own Generic channel (never silently dropped)",
    capAt(m24, 2).cap === "Generic" && capAt(m24, 5).cap === "Generic" && capAt(m24, 8).cap === "Generic");
  check("warns about the unsupported 3rd-byte precision", warnings.some((w) => w.includes("fine^2")));
  const gaps = assertNoGapsOrOverlaps(m24);
  check("24-bit mode: no gaps/overlaps", gaps.gaps.length === 0 && gaps.overlaps.length === 0, gaps);
}

// ===========================================================================
section("OFL: Pan/Tilt (double-fine 24-bit pairing)");

{
  const parsed = ofl.parseOflJson(readTestFile("ofl", "pan-tilt.json"));
  const { model } = ofl.buildModel(parsed, "16 bit");
  const pan = capAt(model, 0), tilt = capAt(model, 2);
  check("Pan 16-bit pair", pan?.cap === "Pan" && pan.fine === 1, pan);
  check("Tilt 16-bit pair", tilt?.cap === "Tilt" && tilt.fine === 3, tilt);
  check("defaultValue \"50%\" -> centre byte 128", pan.default === 128 && tilt.default === 128, [pan.default, tilt.default]);
  check("panRangeDeg from angleStart/angleEnd", model.panRangeDeg === 540);
  check("tiltRangeDeg from angleStart/angleEnd", model.tiltRangeDeg === 180);

  const { model: altModel } = ofl.buildModel(parsed, "16 bit (alternate)");
  check("mode selection changes channel ORDER even with identical footprint",
    altModel.footprint === model.footprint &&
    JSON.stringify(altModel.caps.map((c) => [c.coarse, c.fine])) !== JSON.stringify(model.caps.map((c) => [c.coarse, c.fine])));
}

// ===========================================================================
section("OFL: Sharpy -- cross-format agreement with the QLC+ import of the same fixture");

{
  const parsed = ofl.parseOflJson(readTestFile("ofl", "sharpy.json"));
  const { model } = ofl.buildModel(parsed, "Standard");
  check("footprint matches the QLC+ import of the same physical fixture", model.footprint === sharpyStandardModel.footprint);
  check("isHead matches", model.isHead === sharpyStandardModel.isHead);
  const sameShape = model.caps.every((c, i) => {
    const other = sharpyStandardModel.caps[i];
    return other && c.cap === other.cap && c.coarse === other.coarse && c.fine === other.fine;
  });
  check("every channel's capability/coarse/fine matches the QLC+ import exactly", sameShape,
    { ofl: model.caps.map((c) => [c.cap, c.coarse, c.fine]), qlcplus: sharpyStandardModel.caps.map((c) => [c.cap, c.coarse, c.fine]) });
  const shutter = capAt(model, 1);
  check("ShutterStrobe default also lands on 106 in the OFL encoding (same fixture, same convention)",
    shutter.default === capAt(sharpyStandardModel, 1).default, shutter.default);
}

// ===========================================================================
section("OFL: SlimPAR Pro H USB (hard case -- NoFunction padding, switchChannels, multi-mode)");

{
  const parsed = ofl.parseOflJson(readTestFile("ofl", "slimpar-pro-h-usb.json"));
  const { model, warnings } = ofl.buildModel(parsed, "12-channel");
  check("footprint preserved (12) despite the complexity", model.footprint === 12);
  check("Strobe channel resolves past its NoFunction padding to ShutterStrobe", capAt(model, 7)?.cap === "ShutterStrobe", capAt(model, 7));
  check("Color Macros channel resolves past its NoFunction padding to ColorWheel", capAt(model, 8)?.cap === "ColorWheel", capAt(model, 8));
  check("genuinely ambiguous 'Mode' channel (mixes ShutterStrobe + generic Effect) -> Generic, not a guess",
    capAt(model, 9)?.cap === "Generic" && capAt(model, 9).ranges.length > 0);
  check("mode-dependent switchChannels virtual channel -> Generic, doesn't crash or shift offsets",
    capAt(model, 10)?.cap === "Generic" && capAt(model, 10).unmapped === true);
  check("warns about both the ambiguous channel and the switch channel", warnings.length >= 2, warnings);
  const gaps = assertNoGapsOrOverlaps(model);
  check("no gaps/overlaps despite all the edge cases", gaps.gaps.length === 0 && gaps.overlaps.length === 0, gaps);

  const { result } = roundTrip(model);
  check("hard case still round-trips through fdef_check", result.ok === true, result);
}

// ===========================================================================
section("GDTF: real LED PAR 64 RGBW (trivial ChannelSet convention must NOT become fake slots)");

{
  const bytes = readTestBytes("gdtf", "led-par-64-rgbw.gdtf");
  const parsed = await gdtf.parseGdtf(bytes); // STORE compression, no inflateRaw needed
  const modes = gdtf.listModes(parsed);
  check("1 DMX mode, footprint 5", modes.length === 1 && modes[0].footprint === 5, modes);

  const { model, warnings } = gdtf.buildModel(parsed, modes[0].name);
  check("footprint preserved (5)", model.footprint === 5);
  const expectCaps = ["Dimmer", "Red", "Green", "Blue", "White"];
  check("channel order/capabilities", model.caps.every((c, i) => c.cap === expectCaps[i]), model.caps.map((c) => c.cap));
  check("every channel is plain linear -- the Min/\"\"/Max ChannelSet triplet is NOT read as 3 discrete slots",
    model.caps.every((c) => c.ranges.length === 0), model.caps.map((c) => c.ranges.length));
  check("defaults carried from GDTF's Default=\"value/1\" attribute (R/G/B default to full)",
    capAt(model, 1).default === 255 && capAt(model, 2).default === 255 && capAt(model, 3).default === 255);
  check("no warnings for a clean, simple fixture", warnings.length === 0, warnings);

  const { result } = roundTrip(model);
  check("round-trips through fdef_check", result.ok === true, result);
}

// ===========================================================================
section("GDTF: hand-authored moving head (16-bit pan/tilt, colour+gobo wheel w/ rotation, standalone PrismRotation, 2 modes)");

{
  const inflateRaw = (b) => new Uint8Array(zlib.inflateRawSync(Buffer.from(b)));
  const bytes = readTestBytes("gdtf", "ESP-Glow@Test-Beam.gdtf"); // DEFLATE-compressed
  const parsed = await gdtf.parseGdtf(bytes, { inflateRaw });
  const modes = gdtf.listModes(parsed);
  check("2 modes: Basic (9ch), Extended (12ch)",
    modes.length === 2 && modes.find((m) => m.name === "Basic").footprint === 9 && modes.find((m) => m.name === "Extended").footprint === 12,
    modes);

  const { model: basic, warnings } = gdtf.buildModel(parsed, "Basic");
  check("footprint 9", basic.footprint === 9);
  check("isHead + pan/tilt range from PhysicalFrom/PhysicalTo", basic.isHead && basic.panRangeDeg === 540 && basic.tiltRangeDeg === 270);

  const pan = capAt(basic, 0), tilt = capAt(basic, 2);
  check("Pan 16-bit via comma Offset=\"1,2\"", pan?.cap === "Pan" && pan.fine === 1, pan);
  check("Tilt 16-bit via comma Offset=\"3,4\"", tilt?.cap === "Tilt" && tilt.fine === 3, tilt);
  check("Pan default from Default=\"128/1\"", pan.default === 128);

  const colorWheel = capAt(basic, 4);
  check("ColorWheel: 4 discrete slots + 1 continuous rotation, sharing one DMX byte", colorWheel?.cap === "ColorWheel" &&
    colorWheel.ranges.filter((r) => !r.continuous).length === 4 && colorWheel.ranges.filter((r) => r.continuous).length === 1);
  const redRange = colorWheel.ranges.find((r) => r.name === "Red");
  check("colour slot DMX span exactly matches the ChannelSet boundaries", redRange && redRange.from === 32 && redRange.to === 63, redRange);

  const gobo = capAt(basic, 5);
  check("Gobo: 4 discrete slots + 1 continuous rotation", gobo?.cap === "Gobo" &&
    gobo.ranges.filter((r) => !r.continuous).length === 4 && gobo.ranges.filter((r) => r.continuous).length === 1);

  check("Dimmer stays plain linear (trivial Min/\"\"/Max ChannelSet, not fake slots)", capAt(basic, 6).cap === "Dimmer" && capAt(basic, 6).ranges.length === 0);

  const shutter = capAt(basic, 7);
  check("ShutterStrobe: Closed/Open discrete, Strobe continuous (multi-ChannelFunction channel)",
    shutter?.cap === "ShutterStrobe" &&
    shutter.ranges.find((r) => r.name === "Closed")?.continuous === false &&
    shutter.ranges.find((r) => r.name === "Open")?.continuous === false &&
    shutter.ranges.find((r) => r.name === "Strobe")?.continuous === true);

  const prism = capAt(basic, 8);
  check("Prism: 2 meaningfully-named discrete slots (Out/In), not folded away as trivial", prism?.cap === "Prism" &&
    prism.ranges.length === 2 && prism.ranges.every((r) => !r.continuous));

  check("no warnings for this fully-mapped fixture", warnings.length === 0, warnings);
  const gaps = assertNoGapsOrOverlaps(basic);
  check("no gaps/overlaps", gaps.gaps.length === 0 && gaps.overlaps.length === 0, gaps);

  const { model: extended } = gdtf.buildModel(parsed, "Extended");
  check("Extended mode adds Frost/PrismRotation/Iris after the Basic 9, unchanged below", extended.footprint === 12 &&
    capAt(extended, 9).cap === "Frost" && capAt(extended, 10).cap === "PrismRotation" && capAt(extended, 11).cap === "Iris" &&
    capAt(extended, 0).cap === "Pan" && capAt(extended, 0).fine === 1);
  check("standalone PrismRotation channel (own DMX byte, not sharing Prism's) is plain linear here", capAt(extended, 10).ranges.length === 0);
  check("Iris/Frost defaults carried (255 fully open, 0 no frost)", capAt(extended, 11).default === 255 && capAt(extended, 9).default === 0);

  const { result: rBasic } = roundTrip(basic);
  check("Basic mode round-trips through fdef_check with EVERY range intact (well under budget)",
    rBasic.ok === true && rBasic.caps.reduce((n, c) => n + c.ranges.length, 0) === basic.caps.reduce((n, c) => n + c.ranges.length, 0),
    rBasic);
  const rtColorWheel = rBasic.caps.find((c) => c.coarse === 4);
  check("round-trip ColorWheel ranges match name/span/continuous exactly", JSON.stringify(rtColorWheel.ranges.map((r) => [r.from, r.to, r.continuous, r.name])) ===
    JSON.stringify(colorWheel.ranges.map((r) => [r.from, r.to, r.continuous, r.name])), rtColorWheel.ranges);

  const { result: rExt } = roundTrip(extended);
  check("Extended mode round-trips too", rExt.ok === true && rExt.footprint === 12, rExt);
}

// ===========================================================================
console.log(`\n${count - failures}/${count} checks passed.`);
if (failures > 0) {
  console.log(`${failures} FAILURES`);
  process.exit(1);
}
