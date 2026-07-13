// ofl.js -- Open Fixture Library (OFL) JSON importer.
//
// OFL fixture JSON is far more structured than QLC+'s XML: every
// capability carries an explicit `type` (WheelSlot, ShutterStrobe,
// PrismRotation, ...) instead of an optional free-text preset, coarse/fine
// pairing is declared explicitly via `fineChannelAliases` instead of
// needing to be inferred, and shutter defaults carry a `shutterEffect`
// field instead of needing a name-text search. Less guessing than QLC+;
// more structure to walk.
//
// See testdata/NOTICE.md for the real fixtures this was built and tested
// against, including the Clay Paky Sharpy in *both* QLC+ and OFL form --
// two independent encodings of the same physical fixture is the best
// cross-check this importer has.

import { makeCap, clampByte } from "./model.js";

// OFL capability `type` -> our Capability enum. Types with no entry here
// (Effect, EffectSpeed, EffectDuration, SoundSensitivity, Maintenance,
// Time, PanTiltSpeed, ...) have no matching capability in fixture_profile.h
// and fall back to Generic -- see resolveCapabilityType's default case.
const SIMPLE_TYPE_TO_CAP = {
  Intensity: "Dimmer",
  ShutterStrobe: "ShutterStrobe",
  Pan: "Pan",
  Tilt: "Tilt",
  Focus: "Focus",
  Zoom: "Zoom",
  Iris: "Iris",
  Frost: "Frost",
  Fog: "Fog",
  Prism: "Prism",
  PrismRotation: "PrismRotation",
  ColorPreset: "ColorWheel",
  ColorTemperature: "CTO",
};
const COLOR_TO_CAP = {
  Red: "Red", Green: "Green", Blue: "Blue", White: "White", Amber: "Amber",
  UV: "Uv", Cyan: "Cyan", Magenta: "Magenta", Yellow: "Yellow",
};

function parseDefaultValue(v) {
  if (v == null) return null;
  if (typeof v === "number") return clampByte(v);
  const m = /^(-?[\d.]+)%$/.exec(String(v).trim());
  if (m) return clampByte((parseFloat(m[1]) / 100) * 255);
  return null;
}

// Is this one capability entry continuous (a user-tunable sweep) or
// discrete (a fixed named state)? OFL's structured fields let us decide
// without QLC+'s text-heuristic guessing -- see FORMAT.md's range model
// and the spec's "bias to SLOT when unsure" rule (applied here via the
// explicit deny-list before the generic "*Start paired with *End" check).
function isContinuousCapability(type, cap) {
  if (type === "WheelSlot" || type === "WheelShake" || type === "NoFunction" ||
      type === "Maintenance" || type === "ColorPreset") return false;
  if ("angle" in cap) return false;       // fixed rotational position
  if ("speed" in cap) return false;       // fixed named/percentage speed, not a sweep
  return Object.keys(cap).some((k) => k.endsWith("Start") && k !== "slotNumberStart");
}

function wheelSlotName(wheels, wheelName, slotNumber) {
  const slot = wheels?.[wheelName]?.slots?.[slotNumber - 1];
  if (!slot) return `Slot ${slotNumber}`;
  return slot.name || `${slot.type || "Slot"} ${slotNumber}`;
}

function capabilityRangeName(wheels, wheelName, type, cap) {
  if (cap.comment) return cap.comment;
  if (type === "WheelSlot" || type === "WheelShake") {
    if (cap.slotNumber != null) return wheelSlotName(wheels, wheelName, cap.slotNumber);
    if (cap.slotNumberStart != null) {
      return `${wheelSlotName(wheels, wheelName, cap.slotNumberStart)} + ${wheelSlotName(wheels, wheelName, cap.slotNumberEnd)}`;
    }
  }
  if (cap.shutterEffect) return cap.shutterEffect + (cap.speed || cap.randomTiming ? ` (${cap.speed || "random"})` : "");
  return type || "";
}

// Base Capability for a WheelSlot/WheelRotation/WheelShake/ColorPreset
// channel: which physical wheel is this? Same substring heuristic as
// qlcplus.js's capHintFromText, applied to the channel's own `wheel`
// override (if the JSON gives one) or its name.
function wheelChannelCapability(wheelKeyOrName) {
  const t = (wheelKeyOrName || "").toLowerCase();
  if (/colou?r/.test(t)) return "ColorWheel";
  if (/gobo/.test(t)) return "Gobo";
  return "Generic";
}

function resolveCapabilityType(type, channelNameForWheel, wheels, cap) {
  if (type === "ColorIntensity") return COLOR_TO_CAP[cap.color] || "Generic";
  if (type === "WheelSlot" || type === "WheelRotation" || type === "WheelShake") {
    return wheelChannelCapability(channelNameForWheel);
  }
  return SIMPLE_TYPE_TO_CAP[type] || "Generic";
}

// --- JSON -> parsed fixture ------------------------------------------------

export function parseOflJson(jsonText) {
  const doc = typeof jsonText === "string" ? JSON.parse(jsonText) : jsonText;
  if (!doc.availableChannels || !doc.modes) {
    throw new Error("ofl: missing availableChannels/modes (not an OFL fixture file?)");
  }
  // Every virtual channel name any capability's switchChannels can point
  // a mode-channel slot at -- see buildModel's handling of switch channels.
  const switchTargets = new Set();
  for (const ch of Object.values(doc.availableChannels)) {
    const caps = ch.capabilities || (ch.capability ? [ch.capability] : []);
    for (const cap of caps) {
      if (cap.switchChannels) for (const target of Object.values(cap.switchChannels)) switchTargets.add(target);
    }
  }
  return { doc, switchTargets };
}

export function listModes(parsed) {
  return parsed.doc.modes.map((m) => ({ name: m.name, footprint: m.channels.length }));
}

// --- mode -> intermediate model --------------------------------------------

export function buildModel(parsed, modeName) {
  const { doc, switchTargets } = parsed;
  const mode = doc.modes.find((m) => m.name === modeName);
  if (!mode) throw new Error(`ofl: no such mode: ${modeName}`);
  const warnings = [];

  const footprint = mode.channels.length;
  const slotName = new Array(footprint).fill(null); // string | null, raw mode.channels entry (validated below)
  for (let i = 0; i < footprint; i++) {
    const entry = mode.channels[i];
    if (entry !== null && typeof entry !== "string") {
      throw new Error(
        `ofl: mode "${modeName}" channel ${i} uses a pixel-matrix channel template ` +
        `(not a plain channel name) -- unsupported by this importer; matrices are ` +
        `a .show-level MATRIX construct in esp-glow, not per-pixel CAP lines`,
      );
    }
    slotName[i] = entry;
  }

  // Coarse -> its fine alias name(s), from availableChannels declarations
  // (no inference needed, unlike QLC+/GDTF -- OFL states pairing directly).
  const fineAliasOf = new Map(); // fineName -> { coarseName, level (1 = fine, 2 = fine^2 / unsupported extra precision) }
  for (const [name, ch] of Object.entries(doc.availableChannels)) {
    (ch.fineChannelAliases || []).forEach((alias, idx) => fineAliasOf.set(alias, { coarseName: name, level: idx + 1 }));
  }

  const fineForCoarseOffset = new Map(); // coarse offset -> fine offset (level 1 only; level 2+ becomes its own Generic byte)
  const consumed = new Set();
  for (let offset = 0; offset < footprint; offset++) {
    const name = slotName[offset];
    if (name == null) continue;
    const alias = fineAliasOf.get(name);
    if (!alias) continue;
    const coarseOffset = slotName.indexOf(alias.coarseName);
    if (coarseOffset === -1) {
      warnings.push(`"${name}" is a fine alias of "${alias.coarseName}", which isn't in mode "${modeName}"; kept as its own 8-bit Generic channel`);
      continue;
    }
    if (alias.level === 1) {
      fineForCoarseOffset.set(coarseOffset, offset);
      consumed.add(offset);
    }
    // level >= 2 (fine^2, 24-bit): no third byte in our 16-bit capability
    // model. Left un-consumed below so it becomes its own linear Generic
    // capability -- the footprint slot is preserved, just not folded into
    // the 16-bit pair (see the importer spec's "normalized [0,1]" scope
    // note; this is strictly a bonus byte a plain 16-bit write ignores).
  }

  const caps = [];
  for (let offset = 0; offset < footprint; offset++) {
    if (consumed.has(offset)) continue;
    const name = slotName[offset];
    if (name == null) {
      caps.push(makeCap({ cap: "Generic", coarse: offset, sourceName: "(unused)", unmapped: true }));
      continue;
    }
    if (!doc.availableChannels[name]) {
      if (switchTargets.has(name)) {
        warnings.push(`"${name}" is a mode-dependent switched channel (OFL switchChannels); represented as Generic -- a static .fdef can't express a byte whose meaning depends on another channel's value at runtime`);
      } else {
        warnings.push(`mode "${modeName}" references undefined channel "${name}"`);
      }
      caps.push(makeCap({ cap: "Generic", coarse: offset, sourceName: name, unmapped: true }));
      continue;
    }
    const fine = fineForCoarseOffset.has(offset) ? fineForCoarseOffset.get(offset) : null;
    caps.push(buildCapFromChannel(doc, name, offset, fine, warnings));
  }

  const isHead = caps.some((c) => c.cap === "Pan") && caps.some((c) => c.cap === "Tilt");
  const panCh = findAngleChannel(doc, "Pan");
  const tiltCh = findAngleChannel(doc, "Tilt");
  const model = {
    name: doc.name || "Imported Fixture",
    footprint,
    isHead,
    panRangeDeg: panCh ?? 540,
    tiltRangeDeg: tiltCh ?? 270,
    caps,
  };
  return { model, warnings };
}

function parseDeg(s) {
  const m = /^(-?[\d.]+)/.exec(String(s ?? ""));
  return m ? parseFloat(m[1]) : null;
}
function findAngleChannel(doc, attrType) {
  for (const ch of Object.values(doc.availableChannels)) {
    const cap = ch.capability;
    if (cap && cap.type === attrType && cap.angleStart != null && cap.angleEnd != null) {
      const a = parseDeg(cap.angleStart), b = parseDeg(cap.angleEnd);
      if (a != null && b != null) return Math.abs(b - a);
    }
  }
  return null;
}

function buildCapFromChannel(doc, name, coarse, fine, warnings) {
  const ch = doc.availableChannels[name];
  const def = parseDefaultValue(ch.defaultValue);

  // Single-capability whole-channel form: linear, no ranges.
  if (ch.capability && !ch.capabilities) {
    const cap = ch.capability;
    const capName = resolveCapabilityType(cap.type, ch.wheel || name, doc.wheels, cap);
    return makeCap({
      cap: capName, coarse, fine,
      default: def ?? 0,
      sourceName: name, unmapped: capName === "Generic",
    });
  }

  const capsList = ch.capabilities || [];
  const single = capsList.length <= 1 && capsList.every((c) => c.dmxRange[0] === 0 && c.dmxRange[1] === 255);
  // Determine the channel's one Capability from its *meaningful* (non-
  // NoFunction) sub-ranges. A wheel channel typically mixes several OFL
  // `type`s (WheelSlot + WheelRotation + WheelShake) that all resolve to
  // the SAME capability (ColorWheel/Gobo) -- that's fine, one answer. A
  // channel whose meaningful sub-ranges resolve to genuinely DIFFERENT
  // capabilities (e.g. slimpar-pro-h-usb.json's "Mode": strobe effects
  // mixed with generic macro effects) is truly ambiguous on one physical
  // byte -- Generic, per "do not guess" in the importer spec, rather than
  // arbitrarily picking whichever sub-range happened to come first.
  const meaningful = capsList.filter((c) => c.type !== "NoFunction");
  const resolvedTypes = new Set(meaningful.map((c) => resolveCapabilityType(c.type, ch.wheel || name, doc.wheels, c)));
  const capName = resolvedTypes.size === 1 ? [...resolvedTypes][0] : "Generic";
  if (resolvedTypes.size > 1) {
    warnings.push(`channel "${name}": mixes capability types (${[...resolvedTypes].join(", ")}) on one DMX byte; mapped to Generic`);
  }

  let ranges = [];
  if (!single) {
    ranges = capsList.map((c) => ({
      from: c.dmxRange[0], to: c.dmxRange[1],
      name: capabilityRangeName(doc.wheels, ch.wheel || name, c.type, c),
      continuous: isContinuousCapability(c.type, c),
    }));
  }

  let defaultValue = def ?? 0;
  if (def == null && capName === "ShutterStrobe") {
    const openEntry = capsList.find((c) => c.shutterEffect === "Open");
    defaultValue = openEntry ? clampByte((openEntry.dmxRange[0] + openEntry.dmxRange[1]) / 2) : 255;
  }

  if (capName === "Generic" && resolvedTypes.size === 0 && capsList.length) {
    warnings.push(`channel "${name}": no known capability for type "${capsList[0].type}"; mapped to Generic`);
  }

  return makeCap({
    cap: capName, coarse, fine, default: defaultValue,
    sourceName: name, unmapped: capName === "Generic", ranges,
  });
}
