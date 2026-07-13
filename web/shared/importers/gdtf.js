// gdtf.js -- GDTF (.gdtf) importer.
//
// A GDTF file is a ZIP containing description.xml (plus 3D models and a
// thumbnail this importer never reads -- see zip-lite.js and the task
// spec's "GDTF geometry / 3D models" out-of-scope note). description.xml
// nests far deeper than QLC+'s flat <Channel>/<Capability> list:
//
//   DMXMode > DMXChannels > DMXChannel (Offset="1,2" for 16-bit; "None"
//     for a virtual channel this importer skips entirely -- it consumes
//     no physical DMX byte)
//     > LogicalChannel (Attribute="Color1", "Shutter1", "Pan", ...)
//       > ChannelFunction (Attribute, DMXFrom="value/byteCount", Default, ...)
//         > ChannelSet (DMXFrom="value/byteCount", Name="...")
//
// Two real subtleties this module exists to get right:
//  - GDTF exporters emit a trivial Min/""/Max ChannelSet triplet on
//    basically EVERY linear channel (Dimmer, ColorAdd_R, ...) as a
//    convention, not because the channel is 3 discrete slots -- see
//    isTrivialChannelSetNames.
//  - A DMXChannel can carry >1 ChannelFunction (each a sub-range of the
//    byte) and/or >1 ChannelSet within one function (a finer sub-range) --
//    see buildRangesForChannel's two-level expansion.
//
// See testdata/NOTICE.md for the real (trimmed) sample and the
// hand-authored one this was built and tested against.

import { readZip } from "./zip-lite.js";
import { parseXML } from "./xml-lite.js";
import { makeCap, clampByte } from "./model.js";

// GDTF's "value/byteCount" convention (DMXFrom, Default, ...): the number
// is the raw value using `byteCount` bytes (big-endian, i.e. the same
// value the DMX wire would carry across that many bytes). We only ever
// want the top (most-significant, "coarse") byte out of it.
function topByte(valueSlashBytes, fallback = 0) {
  const m = /^(\d+)\/(\d+)$/.exec(String(valueSlashBytes ?? ""));
  if (!m) return fallback;
  const value = parseInt(m[1], 10);
  const bytes = Math.max(1, parseInt(m[2], 10));
  return clampByte(Math.floor(value / Math.pow(256, bytes - 1)));
}

function isTrivialChannelSetNames(sets) {
  const names = new Set(sets.map((s) => s.attr("Name", "").trim()).filter((n) => n.length));
  for (const n of names) if (n !== "Min" && n !== "Max") return false;
  return true;
}

// Is `func`'s ChannelSet list "real" named slots, or just GDTF's
// documentation convention of bracketing a genuinely linear/continuous
// function with a start/end ChannelSet pair? Two signals, because neither
// alone covers every real file this was tested against:
//  - Name-based: {"Min","Max"} (ignoring the empty-name midpoint entry
//    some exporters add) is the generic convention -- but exporters also
//    use domain labels for the same convention (the real led-par-64-rgbw
//    sample brackets its *linear* Dimmer function "Closed"/""/"Open",
//    which reads exactly like a meaningful 2-slot channel by name alone).
//  - Physical-range-based: GDTF gives every ChannelFunction a
//    PhysicalFrom/PhysicalTo. When they differ, the function has a real
//    physical quantity (0%-100% dimmer, degrees, Hz, ...) -- so its
//    ChannelSets are bracketing documentation, not discrete choices, no
//    matter what they're named. When they're equal (typically both 0),
//    the function has no physical meaning -- a selector -- and its
//    ChannelSets are the real thing (this is how the hand-authored
//    colour/gobo/prism selectors here are written, matching common real
//    GDTF practice for wheels).
// PhysicalFrom==PhysicalTo is the stronger signal where available, so it
// wins; the name check is the fallback for functions that omit Physical
// attributes entirely (both default to 0, i.e. equal -- fall through to
// names so an actually-trivial function without Physical attrs still
// collapses correctly).
function isTrivialFunctionSets(func, sets) {
  if (sets.length < 2) return true;
  const from = parseFloat(func.attr("PhysicalFrom", "0"));
  const to = parseFloat(func.attr("PhysicalTo", "0"));
  if (from !== to) return true;
  return isTrivialChannelSetNames(sets);
}

const CONTINUOUS_ATTR_RE = /rotat|spin|strobe|speed|shake|fade|ramp/i;

// LogicalChannel/ChannelFunction `Attribute` -> our Capability. GDTF's
// standard attribute names are systematic (a base word, optionally a
// wheel/slot index, optionally a function suffix like "WheelSpin" or
// "PosRotate") -- this is closer to a real naming *convention* than
// QLC+'s free-text Preset, so a small set of prefix/substring rules
// covers the standard attribute set without needing an exhaustive table.
function attributeToCapability(attr) {
  const a = attr || "";
  if (a === "Dimmer") return "Dimmer";
  if (a === "Pan") return "Pan";
  if (a === "Tilt") return "Tilt";
  if (/^Color(Add|Sub)_/.test(a)) {
    const suffix = a.replace(/^Color(Add|Sub)_/, "");
    return { R: "Red", G: "Green", B: "Blue", C: "Cyan", M: "Magenta", Y: "Yellow",
      W: "White", WW: "White", CW: "White", A: "Amber", UV: "Uv" }[suffix] || "Generic";
  }
  if (/^Color\d/.test(a)) return "ColorWheel"; // Color1, Color1WheelSpin, Color1Macro, ... -- no dedicated rotation enum
  if (/^Gobo\d/.test(a)) return /rotat|spin/i.test(a) ? "GoboRotation" : "Gobo";
  if (/^Shutter\d*/.test(a)) return "ShutterStrobe";
  if (/^Prism\d/.test(a)) return /rotat|spin/i.test(a) ? "PrismRotation" : "Prism";
  if (/^Frost\d*$/.test(a)) return "Frost";
  if (/^Iris\d*$/.test(a)) return "Iris";
  if (/^Focus\d*$/.test(a)) return "Focus";
  if (/^Zoom\d*$/.test(a)) return "Zoom";
  if (/^(Fog|Haze)\d*$/.test(a)) return "Fog";
  if (/^Fan\d*$/.test(a)) return "Fan";
  if (/cto/i.test(a)) return "CTO";
  if (/^Animation/.test(a)) return "AnimationWheel";
  if (/^Macro/.test(a)) return "Macro";
  return "Generic";
}

// Builds the named-range list (possibly empty) for one DMXChannel's
// LogicalChannel, given its ChannelFunction children in document order
// (GDTF orders them by ascending DMXFrom). Two-level rule:
//  - 1 function, trivial/absent ChannelSets -> [] (plain linear)
//  - 1 function, 2+ meaningfully-named ChannelSets -> one range per set
//  - 2+ functions -> one range per function, each further expanded into
//    per-ChannelSet ranges if THAT function's sets are meaningfully named
function buildRangesForChannel(functions) {
  if (functions.length === 0) return [];
  if (functions.length === 1) {
    const sets = functions[0].children("ChannelSet");
    if (isTrivialFunctionSets(functions[0], sets)) return [];
    return expandChannelSets(sets, 0, 255);
  }
  const ranges = [];
  for (let i = 0; i < functions.length; i++) {
    const f = functions[i];
    const from = topByte(f.attr("DMXFrom"));
    const to = i + 1 < functions.length ? topByte(functions[i + 1].attr("DMXFrom")) - 1 : 255;
    const sets = f.children("ChannelSet");
    if (!isTrivialFunctionSets(f, sets)) {
      ranges.push(...expandChannelSets(sets, from, to));
    } else {
      const attr = f.attr("Attribute", "");
      ranges.push({
        from, to,
        name: f.attr("Name", "") || attr,
        continuous: CONTINUOUS_ATTR_RE.test(attr) || CONTINUOUS_ATTR_RE.test(f.attr("Name", "")),
      });
    }
  }
  return ranges;
}

function expandChannelSets(sets, spanFrom, spanTo) {
  const out = [];
  for (let j = 0; j < sets.length; j++) {
    const from = j === 0 ? spanFrom : Math.max(spanFrom, topByte(sets[j].attr("DMXFrom")));
    const to = j + 1 < sets.length ? topByte(sets[j + 1].attr("DMXFrom")) - 1 : spanTo;
    out.push({ from, to, name: sets[j].attr("Name", ""), continuous: false });
  }
  return out;
}

// --- ZIP+XML -> parsed fixture -------------------------------------------

// `bytes`: Uint8Array of the .gdtf file. `opts.inflateRaw`: see zip-lite.js
// (only needed if the archive uses DEFLATE rather than STORE).
export async function parseGdtf(bytes, opts = {}) {
  const entries = await readZip(bytes, opts);
  const xmlBytes = entries.get("description.xml");
  if (!xmlBytes) throw new Error("gdtf: no description.xml in archive");
  const root = parseXML(new TextDecoder("utf-8").decode(xmlBytes));
  const fixtureType = root.find("FixtureType");
  if (!fixtureType) throw new Error("gdtf: no <FixtureType> in description.xml");

  const manufacturer = fixtureType.attr("Manufacturer", "");
  const name = fixtureType.attr("Name", "");

  const dmxModesEl = fixtureType.child("DMXModes");
  const modes = (dmxModesEl ? dmxModesEl.children("DMXMode") : []).map((modeEl) => {
    const channelsEl = modeEl.child("DMXChannels");
    const channels = (channelsEl ? channelsEl.children("DMXChannel") : [])
      .map((chEl) => parseDmxChannel(chEl))
      .filter((c) => c !== null); // Offset="None": virtual, no physical byte
    return { name: modeEl.attr("Name", ""), channels };
  });

  return { manufacturer, name, modes };
}

function parseDmxChannel(chEl) {
  const offsetAttr = chEl.attr("Offset", "None");
  if (offsetAttr === "None") return null;
  const offsets = offsetAttr.split(",").map((s) => parseInt(s.trim(), 10) - 1); // 1-based -> 0-based
  const logicalChannel = chEl.child("LogicalChannel");
  const attribute = logicalChannel ? logicalChannel.attr("Attribute", "") : "";
  const functions = logicalChannel ? logicalChannel.children("ChannelFunction") : [];
  return {
    offsets, // [coarse, fine?, ...extraPrecision]
    attribute,
    functions,
    defaultByte: functions.length ? topByte(functions[0].attr("Default"), 0) : 0,
  };
}

export function listModes(parsed) {
  return parsed.modes.map((m) => ({ name: m.name, footprint: computeFootprint(m) }));
}

function computeFootprint(mode) {
  let max = -1;
  for (const ch of mode.channels) for (const o of ch.offsets) if (o > max) max = o;
  return max + 1;
}

// --- mode -> intermediate model --------------------------------------------

export function buildModel(parsed, modeName) {
  const mode = parsed.modes.find((m) => m.name === modeName);
  if (!mode) throw new Error(`gdtf: no such mode: ${modeName}`);
  const warnings = [];

  const footprint = computeFootprint(mode);
  const filled = new Array(footprint).fill(false);
  const caps = [];

  for (const ch of mode.channels) {
    const [coarse, fine, ...extra] = ch.offsets;
    const capName = attributeToCapability(ch.attribute);
    const ranges = buildRangesForChannel(ch.functions);
    caps.push(makeCap({
      cap: capName, coarse, fine: fine ?? null, default: ch.defaultByte,
      sourceName: ch.attribute, unmapped: capName === "Generic", ranges,
    }));
    filled[coarse] = true;
    if (fine != null) filled[fine] = true;
    for (const o of extra) {
      // A 3rd+ byte (24-bit+ precision): our capability model only has
      // room for coarse+fine. Preserve the footprint slot as its own
      // linear Generic byte rather than silently dropping it.
      caps.push(makeCap({ cap: "Generic", coarse: o, sourceName: `${ch.attribute} (extra precision)`, unmapped: true }));
      filled[o] = true;
      warnings.push(`"${ch.attribute}": Offset lists a 3rd+ byte (${o + 1}); kept as its own 8-bit Generic channel (16-bit is this importer's ceiling)`);
    }
  }

  for (let offset = 0; offset < footprint; offset++) {
    if (!filled[offset]) {
      caps.push(makeCap({ cap: "Generic", coarse: offset, sourceName: "(unused)", unmapped: true }));
      warnings.push(`DMX offset ${offset + 1} isn't covered by any DMXChannel in mode "${modeName}"; kept as an unnamed Generic channel`);
    }
  }
  caps.sort((a, b) => a.coarse - b.coarse);

  const isHead = caps.some((c) => c.cap === "Pan") && caps.some((c) => c.cap === "Tilt");
  const model = {
    name: [parsed.manufacturer, parsed.name].filter(Boolean).join(" ") || parsed.name || "Imported Fixture",
    footprint,
    isHead,
    panRangeDeg: findAngleRange(mode, "Pan") ?? 540,
    tiltRangeDeg: findAngleRange(mode, "Tilt") ?? 270,
    caps,
  };
  return { model, warnings };
}

function findAngleRange(mode, attribute) {
  for (const ch of mode.channels) {
    if (ch.attribute !== attribute || !ch.functions.length) continue;
    const f = ch.functions[0];
    const from = parseFloat(f.attr("PhysicalFrom", "NaN"));
    const to = parseFloat(f.attr("PhysicalTo", "NaN"));
    if (Number.isFinite(from) && Number.isFinite(to)) return Math.abs(to - from);
  }
  return null;
}
