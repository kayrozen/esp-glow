//
// zip-writer.js — dependency-free ZIP writer, the write-side counterpart
// to web/shared/importers/zip-lite.js's reader. Same reasoning as that
// file's header: no npm/build step in this repo, so rather than vendoring
// fflate this hand-rolls the (well-documented, stable) ZIP binary format
// directly.
//
// Entries are stored uncompressed (method 0 / STORE), not DEFLATEd. That's
// a deliberate simplification, not an oversight: project exports are
// source text (.show/.fdef/.mdef/.fnl, a project.json manifest, maybe one
// compiled .shw1 bundle) -- kilobytes, not megabytes -- so the size win
// from compression is not worth pulling CompressionStream into the one
// path (§3's export/import round trip) that must be provably lossless.
// zip-lite.js's reader already handles STORE entries with no decompression
// step at all, which keeps the round trip exact by construction: the
// bytes written here are exactly the bytes zip-lite.js hands back.

const LOCAL_SIG = 0x04034b50;
const CENTRAL_SIG = 0x02014b50;
const EOCD_SIG = 0x06054b50;

// Standard CRC-32 (IEEE 802.3 / zlib / PNG), same algorithm as
// web/shared/devcfg.js's devcfgCrc32 -- duplicated rather than imported
// since that file's export is scoped to the CFG1 encoder's own tests, and
// this is a generic, well-known, easily-verified 12-line function.
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

// Packs a JS Date into DOS date/time fields (ZIP's on-disk format).
// Cosmetic only -- zip-lite.js's reader never looks at these -- but real
// zip tools opening an exported project.zip should see a sane timestamp
// rather than the 1980-01-01 epoch every all-zero writer produces.
function dosDateTime(date) {
  const year = Math.max(1980, date.getFullYear());
  const dosDate = (((year - 1980) & 0x7f) << 9) | ((date.getMonth() + 1) << 5) | date.getDate();
  const dosTime = (date.getHours() << 11) | (date.getMinutes() << 5) | (date.getSeconds() >> 1);
  return { dosDate: dosDate & 0xffff, dosTime: dosTime & 0xffff };
}

// entries: [{ name: "project.json", data: Uint8Array }, …]. Returns the
// complete ZIP archive as a Uint8Array.
export function writeZip(entries, { date = new Date() } = {}) {
  const enc = new TextEncoder();
  const { dosDate, dosTime } = dosDateTime(date);

  const chunks = [];
  let offset = 0;
  const central = [];

  for (const { name, data } of entries) {
    const nameBytes = enc.encode(name);
    const crc = crc32(data);
    const localHeader = new DataView(new ArrayBuffer(30));
    localHeader.setUint32(0, LOCAL_SIG, true);
    localHeader.setUint16(4, 20, true); // version needed
    localHeader.setUint16(6, 0, true); // flags
    localHeader.setUint16(8, 0, true); // method: STORE
    localHeader.setUint16(10, dosTime, true);
    localHeader.setUint16(12, dosDate, true);
    localHeader.setUint32(14, crc, true);
    localHeader.setUint32(18, data.length, true); // compressed size == uncompressed (STORE)
    localHeader.setUint32(22, data.length, true);
    localHeader.setUint16(26, nameBytes.length, true);
    localHeader.setUint16(28, 0, true); // extra length

    const localHeaderOffset = offset;
    chunks.push(new Uint8Array(localHeader.buffer), nameBytes, data);
    offset += 30 + nameBytes.length + data.length;

    central.push({ nameBytes, crc, size: data.length, localHeaderOffset });
  }

  const centralStart = offset;
  for (const c of central) {
    const h = new DataView(new ArrayBuffer(46));
    h.setUint32(0, CENTRAL_SIG, true);
    h.setUint16(4, 20, true); // version made by
    h.setUint16(6, 20, true); // version needed
    h.setUint16(8, 0, true); // flags
    h.setUint16(10, 0, true); // method: STORE
    h.setUint16(12, dosTime, true);
    h.setUint16(14, dosDate, true);
    h.setUint32(16, c.crc, true);
    h.setUint32(20, c.size, true);
    h.setUint32(24, c.size, true);
    h.setUint16(28, c.nameBytes.length, true);
    h.setUint16(30, 0, true); // extra length
    h.setUint16(32, 0, true); // comment length
    h.setUint16(34, 0, true); // disk number start
    h.setUint16(36, 0, true); // internal attrs
    h.setUint32(38, 0, true); // external attrs
    h.setUint32(42, c.localHeaderOffset, true);
    chunks.push(new Uint8Array(h.buffer), c.nameBytes);
    offset += 46 + c.nameBytes.length;
  }
  const centralSize = offset - centralStart;

  const eocd = new DataView(new ArrayBuffer(22));
  eocd.setUint32(0, EOCD_SIG, true);
  eocd.setUint16(4, 0, true); // disk number
  eocd.setUint16(6, 0, true); // disk with central dir
  eocd.setUint16(8, entries.length, true); // entries on this disk
  eocd.setUint16(10, entries.length, true); // total entries
  eocd.setUint32(12, centralSize, true);
  eocd.setUint32(16, centralStart, true);
  eocd.setUint16(20, 0, true); // comment length
  chunks.push(new Uint8Array(eocd.buffer));

  const total = chunks.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(total);
  let pos = 0;
  for (const c of chunks) {
    out.set(c, pos);
    pos += c.length;
  }
  return out;
}
