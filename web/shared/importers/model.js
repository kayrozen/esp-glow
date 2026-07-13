// model.js -- the intermediate fixture model every importer (QLC+, OFL,
// GDTF) builds, and the one function that turns it into `.fdef` text:
// the stable seam described in provision.h / FORMAT.md. Nothing past
// emitFdef() cares which source format produced the model.
//
// Shape (mirrors the spec in the importer task doc):
//   {
//     name, footprint, isHead, panRangeDeg, tiltRangeDeg,
//     caps: [ { cap, coarse, fine, default, inverted,
//               sourceName, unmapped, ranges: [ { from, to, name, continuous } ] } ]
//   }
// `fine` is null for 8-bit capabilities. `sourceName` and `unmapped` are
// import-time diagnostics for the UI's channel table -- emitFdef() ignores
// them; they carry no meaning to the compiler.

// Exact spelling required by provision.cpp's capFromName table. Order here
// is display order, not the wire enum order in fixture_profile.h.
export const CAP_NAMES = [
  "Dimmer", "Red", "Green", "Blue", "White", "Amber", "Uv",
  "Cyan", "Magenta", "Yellow", "Pan", "Tilt",
  "ShutterStrobe", "Gobo", "Focus", "Zoom", "Fog", "Fan",
  "ColorWheel", "GoboRotation", "Prism", "PrismRotation", "Frost", "Iris", "CTO",
  "AnimationWheel", "Macro", "Generic",
];
const CAP_SET = new Set(CAP_NAMES);

export function isKnownCap(name) {
  return CAP_SET.has(name);
}

export function clamp(v, lo, hi) {
  return Math.min(hi, Math.max(lo, v));
}

export function clampByte(v) {
  return clamp(Math.round(v), 0, 255);
}

// `.fdef` lines are whitespace-tokenized and '#' starts a comment anywhere
// in the line (see provision.cpp's stripComments, which runs BEFORE
// tokenizing) -- a stray '#' or newline in a source-supplied name would
// silently truncate or split the line. Strip/collapse both.
export function sanitizeName(s) {
  return String(s ?? "")
    .replace(/#/g, "")
    .replace(/\s+/g, " ")
    .trim();
}

// Builds an empty cap entry with sensible defaults; importers fill in the
// rest. Keeping construction in one place means every importer's cap
// entries carry the same fields (no accidental omissions the UI then has
// to guard against).
export function makeCap({
  cap, coarse, fine = null, default: def = 0, inverted = false,
  sourceName = "", unmapped = false, ranges = [],
}) {
  return { cap, coarse, fine, default: def, inverted, sourceName, unmapped, ranges };
}

// Mirrors fixture_profile.h's MAX_RANGES/MAX_RANGE_NAME_BLOB exactly --
// PFX2's function-range table is a fixed-size, no-heap array on the
// device (FixtureProfile is a value type copied around freely), so these
// are hard device-side ceilings, not a knob this importer gets to move.
// A handful of real commercial moving heads (see fitRangeBudget below)
// exceed MAX_RANGES once every discrete slot and speed sub-range is
// carried through -- that's a real gap between today's PFX2 budget and
// what the industry's own fixture files describe, worth raising in a
// follow-up that also re-verifies the device-side memory budget (every
// patched fixture in a show carries its own copy of this table); it is
// NOT something to silently paper over by inflating these constants from
// the importer side without that review.
export const MAX_RANGES = 64;
export const MAX_RANGE_NAME_BLOB = 512;

const utf8Encoder = new TextEncoder();
function utf8Length(s) {
  return utf8Encoder.encode(s).length;
}

function totalRanges(caps) {
  return caps.reduce((n, c) => n + c.ranges.length, 0);
}
function nameBlobBytes(caps) {
  // Mirrors ProfileBuilder::encode's NUL-separated blob (undeduplicated --
  // every named range gets its own entry even if the text repeats).
  let bytes = 0;
  for (const c of caps) {
    for (const r of c.ranges) {
      const name = sanitizeName(r.name);
      if (name) bytes += utf8Length(name) + 1;
    }
  }
  return bytes;
}

// Trims a model's ranges down to what PFX2 can actually encode, if it
// doesn't fit already. Never touches capabilities, offsets, footprint, or
// defaults -- only removes named ranges, so a trimmed capability just
// degrades to a plainer (or partially named) linear channel; nothing
// about DMX safety changes. Priority, most-disposable first:
//  1. Generic (housekeeping/unmapped) channels' ranges -- the capability
//     byte still works, it just loses its named breakdown.
//  2. Once no Generic ranges remain, the tail of whichever remaining
//     capability currently has the most ranges -- for a real fixture,
//     source formats list primary functions (discrete colour/gobo slots)
//     first and advanced/rare ones (shake effects, fine rotation speed
//     sub-ranges) last, so trimming from the end keeps the ranges a user
//     is most likely to actually reach for.
// Returns { model, dropped } where `dropped` is a per-channel summary for
// the import UI to surface (never silent).
export function fitRangeBudget(model) {
  const caps = model.caps.map((c) => ({ ...c, ranges: (c.ranges || []).slice() }));
  if (totalRanges(caps) <= MAX_RANGES && nameBlobBytes(caps) <= MAX_RANGE_NAME_BLOB) {
    return { model, dropped: [] };
  }

  const droppedByOffset = new Map();
  function dropOneFrom(idx) {
    const c = caps[idx];
    c.ranges.pop();
    const rec = droppedByOffset.get(c.coarse) || { cap: c.cap, coarse: c.coarse, count: 0 };
    rec.count++;
    droppedByOffset.set(c.coarse, rec);
  }
  function mostRangesIndex() {
    let best = -1;
    for (let i = 0; i < caps.length; i++) {
      if (caps[i].ranges.length > 0 && (best === -1 || caps[i].ranges.length > caps[best].ranges.length)) best = i;
    }
    return best;
  }

  while (totalRanges(caps) > MAX_RANGES || nameBlobBytes(caps) > MAX_RANGE_NAME_BLOB) {
    let idx = caps.findIndex((c) => c.cap === "Generic" && c.ranges.length > 0);
    if (idx === -1) idx = mostRangesIndex();
    if (idx === -1) break; // nothing left to drop
    dropOneFrom(idx);
  }

  return { model: { ...model, caps }, dropped: [...droppedByOffset.values()] };
}

// Convenience wrapper for importers' buildModel: applies fitRangeBudget
// and turns `dropped` into ready-to-append warning strings.
export function fitRangeBudgetWithWarnings(model) {
  const { model: fitted, dropped } = fitRangeBudget(model);
  const warnings = dropped.map(
    (d) => `channel "${d.cap}" (offset ${d.coarse}): dropped ${d.count} range name(s) that didn't fit in the fixture profile's ${MAX_RANGES}-range/${MAX_RANGE_NAME_BLOB}-byte budget -- the channel still works, just with fewer named states`,
  );
  return { model: fitted, warnings };
}

function validate(model) {
  const errors = [];
  if (!model.footprint || model.footprint < 1 || model.footprint > 255) {
    errors.push(`footprint out of range (1..255): ${model.footprint}`);
  }
  for (const c of model.caps) {
    if (!isKnownCap(c.cap)) errors.push(`unknown capability: ${c.cap}`);
    if (c.coarse < 0 || c.coarse >= model.footprint) {
      errors.push(`${c.cap}: coarse offset ${c.coarse} outside footprint ${model.footprint}`);
    }
    if (c.fine != null && (c.fine < 0 || c.fine >= model.footprint)) {
      errors.push(`${c.cap}: fine offset ${c.fine} outside footprint ${model.footprint}`);
    }
    for (const r of c.ranges || []) {
      if (r.from < 0 || r.from > 255 || r.to < 0 || r.to > 255 || r.from > r.to) {
        errors.push(`${c.cap}: bad range [${r.from},${r.to}]`);
      }
    }
  }
  return errors;
}

// Emits `.fdef` text for `model`. Throws if the model fails validation
// (callers -- the importer UI -- should validate ahead of time so this is
// never reached with user-facing malformed input; it's the last line of
// defense against an importer bug, not a UI-facing error path).
export function emitFdef(model) {
  const errors = validate(model);
  if (errors.length) {
    throw new Error(`emitFdef: invalid model:\n  ${errors.join("\n  ")}`);
  }

  const lines = [];
  lines.push(`FIXTURE ${sanitizeName(model.name) || "Imported Fixture"}`);
  lines.push(`FOOTPRINT ${model.footprint}`);
  if (model.isHead) {
    lines.push("HEAD");
    lines.push(`PANRANGE ${numToToken(model.panRangeDeg ?? 540)}`);
    lines.push(`TILTRANGE ${numToToken(model.tiltRangeDeg ?? 270)}`);
  }
  for (const c of model.caps) {
    const fineTok = c.fine == null ? "-" : String(c.fine);
    const invTok = c.inverted ? " inv" : "";
    lines.push(`CAP ${c.cap} ${c.coarse} ${fineTok} ${clampByte(c.default)}${invTok}`);
    for (const r of c.ranges || []) {
      const cmd = r.continuous ? "RANGE" : "SLOT";
      const name = sanitizeName(r.name);
      lines.push(name ? `  ${cmd} ${r.from} ${r.to} ${name}` : `  ${cmd} ${r.from} ${r.to}`);
    }
  }
  return lines.join("\n") + "\n";
}

function numToToken(n) {
  // PANRANGE/TILTRANGE parse with std::stof -- plain decimal is fine, but
  // avoid emitting "540.000000"-style noise or scientific notation for
  // very small/large inputs.
  if (!Number.isFinite(n)) return "0";
  return String(Math.round(n * 1000) / 1000);
}
