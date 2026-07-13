// qlcplus.js -- QLC+ .qxf importer.
//
// .qxf is a plain XML fixture definition (<FixtureDefinition> root): a set
// of named <Channel>s (either a single whole-channel <Preset>, or a
// <Group> + a list of <Capability Min Max [Preset]>text</Capability>
// sub-ranges), and a set of <Mode>s, each an ordered list of channel
// references by DMX offset. Modes are QLC+'s "8ch basic / 16ch extended"
// personalities -- the caller (the import UI) must pick one; there is no
// safe default (see the task spec's "never auto-pick" rule).
//
// See testdata/NOTICE.md for the two real fixtures this was built and
// tested against (a simple RGB par and the Clay Paky Sharpy moving head).

import { parseXML } from "./xml-lite.js";
import { makeCap, clampByte } from "./model.js";

// --- whole-channel Preset -> Capability (linear, no ranges) -------------
//
// These are QLC+'s "simple" channels: a single Preset attribute directly
// on <Channel>, no nested <Capability> children. `fineOf` names the base
// preset this is the fine half of (QLC+ appends "Fine" to the coarse
// preset name for the LSB channel).
const WHOLE_CHANNEL_PRESET = {
  IntensityDimmer: "Dimmer", IntensityDimmerFine: { fineOf: "Dimmer" },
  IntensityMasterDimmer: "Dimmer", IntensityMasterDimmerFine: { fineOf: "Dimmer" },
  IntensityRed: "Red", IntensityRedFine: { fineOf: "Red" },
  IntensityGreen: "Green", IntensityGreenFine: { fineOf: "Green" },
  IntensityBlue: "Blue", IntensityBlueFine: { fineOf: "Blue" },
  IntensityWhite: "White", IntensityWhiteFine: { fineOf: "White" },
  IntensityAmber: "Amber", IntensityAmberFine: { fineOf: "Amber" },
  IntensityUV: "Uv", IntensityUVFine: { fineOf: "Uv" },
  IntensityCyan: "Cyan", IntensityCyanFine: { fineOf: "Cyan" },
  IntensityMagenta: "Magenta", IntensityMagentaFine: { fineOf: "Magenta" },
  IntensityYellow: "Yellow", IntensityYellowFine: { fineOf: "Yellow" },
  PositionPan: "Pan", PositionPanFine: { fineOf: "Pan" },
  PositionTilt: "Tilt", PositionTiltFine: { fineOf: "Tilt" },
  BeamFocusNearFar: "Focus", BeamFocusFarNear: "Focus", BeamFocusFine: { fineOf: "Focus" },
  BeamZoomSmallBig: "Zoom", BeamZoomBigSmall: "Zoom", BeamZoomFine: { fineOf: "Zoom" },
  BeamIris: "Iris", BeamIrisFine: { fineOf: "Iris" },
  NoFunction: "Generic",
};

// Per-capability-entry Preset -> discrete/continuous, used inside
// Group-based channels. Anything not listed falls back to a text-based
// heuristic (isContinuousLabel below) -- deliberately biased toward
// "discrete" per the spec: a wrong RANGE call spins a wheel instead of
// selecting a slot, a wrong SLOT call just snaps to a centre.
const CONTINUOUS_PRESETS = new Set([
  "StrobeFrequency", "StrobeFreqRange",
  "RampUpSlowToFast", "RampUpFastToSlow", "RampDownSlowToFast", "RampDownFastToSlow",
  "ColorWheelSpin", "GoboWheelSpin", "PrismRotationSpeed", "PanTiltSpeed",
]);
const DISCRETE_PRESETS = new Set([
  "ColorMacro", "ColorDoubleMacro", "ColorWheelIndex", "ColorWheelShortcut",
  "GoboMacro", "GoboShakeMacro", "GoboWheelIndex", "GoboWheelShortcut",
  "ShutterOpen", "ShutterClose", "PrismOn", "PrismShake", "StrobeRandom", "NoFunction",
]);

function isContinuousLabel(text) {
  return /rotat|spin|speed|\bramp\b|slow.*to.*fast|fast.*to.*slow/i.test(text || "");
}

// Group name / channel name -> base Capability, before the Rotation-only
// refinement below. null means "no textual hint" (caller falls back to
// Generic).
function capHintFromText(text) {
  const t = (text || "").toLowerCase();
  // "Colour Time", "Gobo Time", "Pan-Tilt Time", "Beam Time": QLC+'s
  // generic pattern for "how fast does the previous function move",
  // grouped under Group="Speed". None of the real capabilities carry the
  // word "time", so this guard keeps them out of ColorWheel/Gobo/etc and
  // into Generic (they end up linear -- a single full-width entry, no
  // physical-unit capability exists to carry an RPM/second value; see
  // "Out of scope: physical units" in the importer spec).
  if (/\btime\b/.test(t)) return null;
  if (/colou?r/.test(t)) return "ColorWheel";
  if (/gobo/.test(t)) return "Gobo";
  if (/shutter|strobe/.test(t)) return "ShutterStrobe";
  if (/prism/.test(t)) return "Prism";
  if (/frost/.test(t)) return "Frost";
  if (/iris/.test(t)) return "Iris";
  if (/\bcto\b|colou?r\s*temp/.test(t)) return "CTO";
  if (/animation/.test(t)) return "AnimationWheel";
  if (/macro/.test(t)) return "Macro";
  return null;
}

function resolveGroupChannelCapability(channelName, groupName) {
  const base = capHintFromText(`${groupName || ""} ${channelName}`);
  // Gobo/Prism have a dedicated *Rotation capability for fixtures that put
  // selection and rotation on two SEPARATE DMX channels (e.g. Sharpy's
  // "Prism Insertion" vs "Prism Rotation") -- the channel's own name is
  // the reliable signal for that split, because a fixture that instead
  // mixes selection and rotation into one physical byte (Sharpy's colour
  // and gobo wheels) names that one channel after the selection function
  // ("Colour Wheel", "Static Gobo Change"), never after rotation; the
  // rotation portion shows up only as one capability entry's label
  // ("Rotation (...)") within that single channel, carried as a
  // continuous named range under the base capability instead. There's no
  // "ColorWheelRotation" enum, so ColorWheel channels never get split.
  if ((base === "Gobo" || base === "Prism") && /rotat|spin/i.test(channelName)) {
    return base + "Rotation";
  }
  return base || "Generic";
}

// --- XML -> parsed fixture -----------------------------------------------

export function parseQlcPlusQxf(xmlText) {
  const root = parseXML(xmlText);
  const fixtureDef = root.tag === "FixtureDefinition" ? root : root.find("FixtureDefinition");
  if (!fixtureDef) throw new Error("qlcplus: not a <FixtureDefinition> document");

  const manufacturer = fixtureDef.child("Manufacturer")?.text || "";
  const modelName = fixtureDef.child("Model")?.text || "";
  const type = fixtureDef.child("Type")?.text || "";

  const channels = new Map();
  for (const ch of fixtureDef.children("Channel")) {
    const name = ch.attr("Name", "");
    const preset = ch.attr("Preset", null);
    const defaultAttr = ch.attr("Default", null);
    const group = ch.child("Group");
    const groupName = group ? group.text : null;
    const groupByte = group ? parseInt(group.attr("Byte", "0"), 10) : null;
    const caps = ch.children("Capability").map((c) => ({
      min: parseInt(c.attr("Min", "0"), 10),
      max: parseInt(c.attr("Max", "0"), 10),
      preset: c.attr("Preset", null),
      text: c.text,
      res1: c.attr("Res1", null),
      res2: c.attr("Res2", null),
    }));
    channels.set(name, {
      name, preset, default: defaultAttr != null ? parseInt(defaultAttr, 10) : null,
      groupName, groupByte, caps,
    });
  }

  const modes = fixtureDef.children("Mode").map((m) => ({
    name: m.attr("Name", ""),
    channels: m.children("Channel")
      .map((c) => ({ number: parseInt(c.attr("Number", "0"), 10), name: c.text }))
      .sort((a, b) => a.number - b.number),
  }));

  const physical = fixtureDef.child("Physical");
  const focus = physical?.child("Focus");
  const panRangeDeg = focus ? parseFloat(focus.attr("PanMax", "540")) : 540;
  const tiltRangeDeg = focus ? parseFloat(focus.attr("TiltMax", "270")) : 270;

  return { manufacturer, model: modelName, type, channels, modes, panRangeDeg, tiltRangeDeg };
}

export function listModes(parsed) {
  return parsed.modes.map((m) => ({
    name: m.name,
    footprint: m.channels.length ? m.channels[m.channels.length - 1].number + 1 : 0,
  }));
}

// --- mode -> intermediate model ------------------------------------------

export function buildModel(parsed, modeName) {
  const mode = parsed.modes.find((m) => m.name === modeName);
  if (!mode) throw new Error(`qlcplus: no such mode: ${modeName}`);
  const warnings = [];

  const footprint = mode.channels.length ? mode.channels[mode.channels.length - 1].number + 1 : 0;
  // offset -> channel name, from the mode's <Channel Number="N">Name</Channel> refs.
  const nameAtOffset = new Array(footprint).fill(null);
  for (const ref of mode.channels) nameAtOffset[ref.number] = ref.name;

  // Pass 1: find fine-channel offsets and which coarse offset they pair with.
  const fineForCoarse = new Map(); // coarseOffset -> fineOffset
  const consumedAsFine = new Set(); // offsets folded into another cap as `fine`
  for (let offset = 0; offset < footprint; offset++) {
    const name = nameAtOffset[offset];
    if (name == null) continue;
    const def = parsed.channels.get(name);
    if (!def) continue;

    let baseKey = null; // preset or name of the coarse counterpart to search for
    if (def.preset && def.preset.endsWith("Fine")) {
      baseKey = { kind: "preset", value: def.preset.slice(0, -4) };
    } else if (def.groupByte === 1 && def.groupName) {
      baseKey = { kind: "group", value: def.groupName };
    } else if (/\sfine(\^\d+)?$/i.test(name)) {
      baseKey = { kind: "name", value: name.replace(/\sfine(\^\d+)?$/i, "") };
    }
    if (!baseKey) continue;

    const coarseOffset = mode.channels.find((ref) => {
      if (ref.number === offset || consumedAsFine.has(ref.number) || fineForCoarse.has(ref.number)) return false;
      const cd = parsed.channels.get(ref.name);
      if (!cd) return false;
      if (baseKey.kind === "preset") return cd.preset === baseKey.value;
      if (baseKey.kind === "group") return cd.groupByte === 0 && cd.groupName === baseKey.value;
      return ref.name === baseKey.value;
    })?.number;

    if (coarseOffset != null) {
      fineForCoarse.set(coarseOffset, offset);
      consumedAsFine.add(offset);
    } else {
      warnings.push(`"${name}" looks like a fine channel but its coarse counterpart isn't in mode "${modeName}"; kept as its own 8-bit Generic channel`);
    }
  }

  const caps = [];
  for (let offset = 0; offset < footprint; offset++) {
    if (consumedAsFine.has(offset)) continue;
    const name = nameAtOffset[offset];
    if (name == null) {
      caps.push(makeCap({ cap: "Generic", coarse: offset, sourceName: "(unused)", unmapped: true }));
      continue;
    }
    const def = parsed.channels.get(name);
    if (!def) {
      caps.push(makeCap({ cap: "Generic", coarse: offset, sourceName: name, unmapped: true }));
      warnings.push(`mode "${modeName}" references undefined channel "${name}"`);
      continue;
    }
    const fine = fineForCoarse.has(offset) ? fineForCoarse.get(offset) : null;
    caps.push(buildCapFromChannel(def, offset, fine, warnings));
  }

  const isHead = caps.some((c) => c.cap === "Pan") && caps.some((c) => c.cap === "Tilt");
  const model = {
    name: [parsed.manufacturer, parsed.model].filter(Boolean).join(" ") || parsed.model || "Imported Fixture",
    footprint,
    isHead,
    panRangeDeg: parsed.panRangeDeg,
    tiltRangeDeg: parsed.tiltRangeDeg,
    caps,
  };
  return { model, warnings };
}

function buildCapFromChannel(def, coarse, fine, warnings) {
  // Simple whole-channel Preset form: linear, no ranges.
  if (def.preset && WHOLE_CHANNEL_PRESET[def.preset] !== undefined) {
    const mapped = WHOLE_CHANNEL_PRESET[def.preset];
    const cap = typeof mapped === "string" ? mapped : mapped.fineOf; // a *Fine preset landing here means its coarse wasn't found; still map to the same capability, standalone 8-bit
    return makeCap({
      cap, coarse, fine,
      default: def.default != null ? clampByte(def.default) : 0,
      sourceName: def.name, unmapped: false,
    });
  }
  if (def.preset && !def.caps.length) {
    // Unrecognized whole-channel preset string: fall back to a name hint,
    // else Generic. Still linear (no ranges -- a single preset means a
    // single function).
    const hint = capHintFromText(def.name);
    warnings.push(`unrecognized Preset "${def.preset}" on channel "${def.name}"; mapped by name to ${hint || "Generic"}`);
    return makeCap({
      cap: hint || "Generic", coarse, fine,
      default: def.default != null ? clampByte(def.default) : 0,
      sourceName: def.name, unmapped: !hint,
    });
  }

  // Group/Capability form: may be linear (one entry spanning the full
  // byte) or carved into named ranges.
  const capName = resolveGroupChannelCapability(def.name, def.groupName);
  const single = def.caps.length <= 1 && def.caps.every((e) => e.min === 0 && e.max === 255);
  let ranges = [];
  if (!single) {
    ranges = def.caps.map((e) => {
      const continuous = e.preset
        ? CONTINUOUS_PRESETS.has(e.preset) || (!DISCRETE_PRESETS.has(e.preset) && isContinuousLabel(e.text))
        : isContinuousLabel(e.text);
      return { from: e.min, to: e.max, name: e.text, continuous };
    });
  }

  let defaultValue = def.default != null ? clampByte(def.default) : 0;
  if (def.default == null && capName === "ShutterStrobe") {
    const openEntry = def.caps.find((e) => e.preset === "ShutterOpen" || /\bopen\b/i.test(e.text));
    defaultValue = openEntry ? clampByte((openEntry.min + openEntry.max) / 2) : 255;
  }

  return makeCap({
    cap: capName, coarse, fine, default: defaultValue,
    sourceName: def.name, unmapped: capName === "Generic", ranges,
  });
}
