// import.js -- format detection + dispatch for the fixture importer panel
// (QLC+ .qxf / Open Fixture Library .json / GDTF .gdtf). The actual
// per-format parsing and Capability/range mapping lives in
// web/shared/importers/{qlcplus,ofl,gdtf}.js -- shared with the Node test
// suite (test-importers.mjs) so the browser and the tests run the exact
// same code. This file is just the "which parser do I call" glue plus a
// little UI-facing convenience (GitHub blob-URL normalization).

import * as qlcplus from "./shared/importers/qlcplus.js";
import * as ofl from "./shared/importers/ofl.js";
import * as gdtf from "./shared/importers/gdtf.js";

export const FORMAT_LABEL = {
  qlcplus: "QLC+",
  ofl: "Open Fixture Library",
  gdtf: "GDTF",
};

// `bytes`: Uint8Array of the raw file (works for both text and binary
// formats -- GDTF is a ZIP, the other two are read as UTF-8 text).
// Detection prefers the file extension; if that's missing or ambiguous
// (e.g. a URL with no extension), it sniffs the actual bytes.
export async function detectAndParse(name, bytes) {
  const lower = (name || "").toLowerCase();
  const isZip = bytes.length >= 4 && bytes[0] === 0x50 && bytes[1] === 0x4b && bytes[2] === 0x03 && bytes[3] === 0x04;

  if (lower.endsWith(".gdtf") || (isZip && !lower.endsWith(".qxf") && !lower.endsWith(".json"))) {
    const parsed = await gdtf.parseGdtf(bytes);
    return { format: "gdtf", parsed };
  }

  const text = new TextDecoder("utf-8").decode(bytes);
  if (lower.endsWith(".qxf")) return { format: "qlcplus", parsed: qlcplus.parseQlcPlusQxf(text) };
  if (lower.endsWith(".json")) return { format: "ofl", parsed: ofl.parseOflJson(text) };

  const trimmed = text.trim();
  if (trimmed.startsWith("{")) return { format: "ofl", parsed: ofl.parseOflJson(text) };
  if (trimmed.startsWith("<")) return { format: "qlcplus", parsed: qlcplus.parseQlcPlusQxf(text) };
  throw new Error(
    "Unrecognized file -- expected a QLC+ .qxf, GDTF .gdtf (ZIP), or Open Fixture Library .json fixture file",
  );
}

export function listImportModes(format, parsed) {
  if (format === "qlcplus") return qlcplus.listModes(parsed);
  if (format === "ofl") return ofl.listModes(parsed);
  if (format === "gdtf") return gdtf.listModes(parsed);
  throw new Error(`unknown import format: ${format}`);
}

export function buildImportModel(format, parsed, modeName) {
  if (format === "qlcplus") return qlcplus.buildModel(parsed, modeName);
  if (format === "ofl") return ofl.buildModel(parsed, modeName);
  if (format === "gdtf") return gdtf.buildModel(parsed, modeName);
  throw new Error(`unknown import format: ${format}`);
}

// QLC+ and OFL's fixture libraries live on GitHub. A pasted "view this
// file" URL (github.com/.../blob/<ref>/<path>) serves an HTML page, not
// the file -- rewrite it to the raw.githubusercontent.com equivalent
// (which also sends CORS headers a browser fetch can actually read).
// Anything else (already a raw URL, a different host) passes through
// unchanged.
export function normalizeFixtureUrl(url) {
  const m = /^https?:\/\/github\.com\/([^/]+)\/([^/]+)\/blob\/([^/]+)\/(.+)$/.exec(url.trim());
  if (!m) return url.trim();
  const [, owner, repo, ref, path] = m;
  return `https://raw.githubusercontent.com/${owner}/${repo}/${ref}/${path}`;
}
