// zip-lite.js -- dependency-free ZIP reader for the GDTF importer.
//
// GDTF files are ZIPs containing description.xml (+ 3D models/thumbnails
// this importer never reads -- see fixture_profile geometry being out of
// scope). The original importer spec suggested vendoring fflate; this
// repo doesn't vendor via npm (no package.json anywhere -- every other
// vendored bundle here, e.g. web/vendor/wasmoon-bundle.mjs, was hand-built
// from a pinned upstream commit by a scripts/vendor_*.sh step) and fflate
// ships as TypeScript source only, not a prebuilt browser bundle, in its
// repo. Rather than adding a build step for one 8kB library, this reads
// the ZIP central directory itself (a well-documented, stable binary
// format -- about the same size as the OSC/DJ-Link parsers already
// hand-rolled in this codebase) and delegates the one genuinely nontrivial
// piece, DEFLATE decompression, to a platform primitive: the browser's
// native `DecompressionStream("deflate-raw")`. Node has the equivalent
// (`zlib.inflateRawSync`) built in too, so tests inject that instead of
// vendoring anything.

const EOCD_SIG = 0x06054b50;
const CENTRAL_SIG = 0x02014b50;
const LOCAL_SIG = 0x04034b50;

function findEndOfCentralDirectory(bytes) {
  // The EOCD record is at least 22 bytes and can be followed by a
  // variable-length comment (up to 65535 bytes) -- scan backward for the
  // signature rather than assuming it's the last 22 bytes.
  const maxBack = Math.min(bytes.length, 22 + 0xffff);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  for (let pos = bytes.length - 22; pos >= bytes.length - maxBack && pos >= 0; pos--) {
    if (dv.getUint32(pos, true) === EOCD_SIG) return pos;
  }
  throw new Error("zip-lite: end-of-central-directory record not found (not a ZIP file?)");
}

async function inflateRawDefault(bytes) {
  if (typeof DecompressionStream === "undefined") {
    throw new Error(
      "zip-lite: this entry is DEFLATE-compressed and no DecompressionStream is available " +
      "(pass an `inflateRaw` option -- e.g. zlib.inflateRawSync in Node)",
    );
  }
  const ds = new DecompressionStream("deflate-raw");
  const stream = new Blob([bytes]).stream().pipeThrough(ds);
  const buf = await new Response(stream).arrayBuffer();
  return new Uint8Array(buf);
}

// Reads a ZIP archive and returns a Map<string, Uint8Array> of every
// (non-directory) entry's decompressed bytes, keyed by its path inside the
// archive (forward slashes, as stored).
//
// opts.inflateRaw(bytes) -> Uint8Array | Promise<Uint8Array>: overrides the
// DEFLATE decompressor (tests use Node's zlib; the browser default uses
// DecompressionStream). Entries stored with method 0 (no compression, the
// common case for small GDTF/MVR packages) never need it.
export async function readZip(bytes, opts = {}) {
  const inflateRaw = opts.inflateRaw || inflateRawDefault;
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const dec = new TextDecoder("utf-8");

  const eocdPos = findEndOfCentralDirectory(bytes);
  const entryCount = dv.getUint16(eocdPos + 10, true);
  let centralOffset = dv.getUint32(eocdPos + 16, true);

  const out = new Map();
  for (let e = 0; e < entryCount; e++) {
    if (dv.getUint32(centralOffset, true) !== CENTRAL_SIG) {
      throw new Error("zip-lite: malformed central directory entry");
    }
    const method = dv.getUint16(centralOffset + 10, true);
    const compressedSize = dv.getUint32(centralOffset + 20, true);
    const uncompressedSize = dv.getUint32(centralOffset + 24, true);
    const nameLen = dv.getUint16(centralOffset + 28, true);
    const extraLen = dv.getUint16(centralOffset + 30, true);
    const commentLen = dv.getUint16(centralOffset + 32, true);
    const localHeaderOffset = dv.getUint32(centralOffset + 42, true);
    const nameStart = centralOffset + 46;
    const name = dec.decode(bytes.subarray(nameStart, nameStart + nameLen));

    if (dv.getUint32(localHeaderOffset, true) !== LOCAL_SIG) {
      throw new Error(`zip-lite: malformed local file header for ${name}`);
    }
    const localNameLen = dv.getUint16(localHeaderOffset + 26, true);
    const localExtraLen = dv.getUint16(localHeaderOffset + 28, true);
    const dataStart = localHeaderOffset + 30 + localNameLen + localExtraLen;
    const compressed = bytes.subarray(dataStart, dataStart + compressedSize);

    if (!name.endsWith("/")) {
      let data;
      if (method === 0) {
        data = new Uint8Array(compressed); // copy: subarray still views the input buffer
      } else if (method === 8) {
        data = await inflateRaw(compressed);
      } else {
        throw new Error(`zip-lite: unsupported compression method ${method} for ${name}`);
      }
      if (data.length !== uncompressedSize) {
        throw new Error(`zip-lite: ${name} decompressed to ${data.length} bytes, expected ${uncompressedSize}`);
      }
      out.set(name, data);
    }

    centralOffset = nameStart + nameLen + extraLen + commentLen;
  }
  return out;
}
