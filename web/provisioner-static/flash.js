//
// flash.js — USB flashing for the esp-glow provisioner, via esptool-js
// (Espressif's official JS port of esptool, built on the Web Serial API).
//
// Not ESP Web Tools: ESP Web Tools wants one merged binary, which can't
// carry a freshly-compiled SHW1 show bundle. esptool-js flashes an
// arbitrary list of {data, address} parts, so the browser-compiled bundle
// can be written straight at the "show" partition's offset alongside the
// bootloader/partition-table/otadata/app parts fetched from firmware/.
//
// Offsets are never hardcoded here: they come from firmware/flasher_args.json,
// which the firmware build emits. The show-partition offset for the user's
// own compiled bundle is read from the same file (see SHOW_PARTITION_LABEL).

import { ESPLoader, Transport } from "https://unpkg.com/esptool-js/bundle.js";

export function serialSupported() {
  return typeof navigator !== "undefined" && "serial" in navigator;
}

export function secureContextOk() {
  return typeof window !== "undefined" && window.isSecureContext;
}

// Fetches firmware/flasher_args.json and firmware/show_partition.json (if
// present) and resolves the flash parts. Throws with a descriptive message
// on any fetch failure so the caller can surface it in the UI.
async function loadFlasherArgs(baseUrl) {
  const res = await fetch(new URL("flasher_args.json", baseUrl));
  if (!res.ok) {
    throw new Error(
      `Could not fetch firmware/flasher_args.json (${res.status}). ` +
        `This deploy may not have published firmware artifacts yet.`,
    );
  }
  return res.json();
}

async function fetchBinaryString(url) {
  const res = await fetch(url);
  if (!res.ok) throw new Error(`Fetch failed for ${url}: ${res.status}`);
  const buf = new Uint8Array(await res.arrayBuffer());
  return toBinaryString(buf);
}

// esptool-js wants each part's `data` as a binary string, not a Uint8Array.
export function toBinaryString(u8) {
  let s = "";
  const chunk = 0x8000;
  for (let i = 0; i < u8.length; i += chunk) {
    s += String.fromCharCode.apply(null, u8.subarray(i, i + chunk));
  }
  return s;
}

// Resolve the flash offset of the "show" partition. flasher_args.json
// already lists the CI-built demo bundle under its own offset (the build
// writes data/show.shw1 into that partition; see main/CMakeLists.txt), so
// we read the offset back out of flash_files instead of hardcoding it.
function findShowPartitionOffset(args) {
  for (const [addr, file] of Object.entries(args.flash_files ?? {})) {
    if (/\.shw1$/i.test(file)) return parseInt(addr, 16);
  }
  return null;
}

// Find partition-table.bin's own flash offset+filename from flasher_args.json
// (unlike the "scripts" partition, the partition table itself always has a
// CI-built file — it's part of every ESP-IDF build).
function findPartitionTableFile(args) {
  for (const [, file] of Object.entries(args.flash_files ?? {})) {
    if (/partition-table\.bin$/i.test(file)) return file;
  }
  return null;
}

// Parses an ESP-IDF binary partition table (gen_esp32part.py's format:
// repeated 32-byte little-endian entries, magic 0xAA 0x50, until an
// all-0xFF or all-0x00 entry). Returns [{type, subtype, offset, size, label}].
// See ESP-IDF's esp_partition_info_t / docs/en/api-guides/partition-tables.rst.
function parsePartitionTable(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const entries = [];
  for (let off = 0; off + 32 <= bytes.length; off += 32) {
    if (view.getUint8(off) === 0xaa && view.getUint8(off + 1) === 0x50) {
      const type = view.getUint8(off + 2);
      const subtype = view.getUint8(off + 3);
      const offset = view.getUint32(off + 4, true);
      const size = view.getUint32(off + 8, true);
      const labelBytes = bytes.subarray(off + 12, off + 28);
      const nul = labelBytes.indexOf(0);
      const label = new TextDecoder().decode(labelBytes.subarray(0, nul < 0 ? 16 : nul));
      entries.push({ type, subtype, offset, size, label });
    } else {
      break;  // MD5 checksum entry (type 0xEB) or end-of-table padding
    }
  }
  return entries;
}

// Resolves the "scripts" partition's flash offset by fetching and parsing
// the real partition table binary the firmware was built with — not
// hardcoded, since a board's actual partition layout is the only source
// of truth for where writing "scripts region" bytes is safe (see
// firmware/partitions.csv, which is what generates this binary at build
// time). Returns { offset, size } or null if not found.
async function findScriptsPartition(args, baseUrl) {
  const file = findPartitionTableFile(args);
  if (!file) return null;
  const res = await fetch(new URL(file, baseUrl));
  if (!res.ok) return null;
  const bytes = new Uint8Array(await res.arrayBuffer());
  const entries = parsePartitionTable(bytes);
  const scripts = entries.find((e) => e.label === "scripts");
  return scripts ? { offset: scripts.offset, size: scripts.size } : null;
}

// Detect the chip's family name from esptool-js's `chip` string, e.g.
// "ESP32-S3 (QFN56) (revision v0.2)" -> "ESP32-S3".
function chipFamily(chipDescription) {
  const m = /ESP32(-[A-Z0-9]+)?/i.exec(chipDescription || "");
  return m ? m[0].toUpperCase() : "";
}

// flash() drives the whole USB flash flow.
//
// opts:
//   baseUrl     URL of the firmware/ directory to fetch parts from (defaults
//               to "./firmware/" relative to the page).
//   bundleBytes Uint8Array | null — the user's freshly-compiled SHW1 bundle.
//               If provided, it's written at the show partition's offset
//               instead of the CI-built demo bundle.
//   scriptsImageBytes Uint8Array | null — a pre-built "scripts" partition
//               LittleFS image (see boot-image.js's buildScriptsImage) to
//               write at the scripts partition's offset, so a fresh board
//               comes up already running the authored boot.fnl. Its size
//               must exactly match the scripts partition's size (it does,
//               if it came from buildScriptsImage against this same
//               firmware build's partition table).
//   eraseFirst  bool — erase the whole flash before writing.
//   baudrate    number — defaults to 460800.
//   onLog(line) called with esptool-js's terminal output.
//   onProgress(fileIndex, fileCount, fraction) per-part progress.
//   onChip(chipDescription) called once the chip is detected.
export async function flash(opts) {
  const {
    baseUrl = new URL("firmware/", window.location.href),
    bundleBytes = null,
    scriptsImageBytes = null,
    eraseFirst = false,
    baudrate = 460800,
    onLog = () => {},
    onProgress = () => {},
    onChip = () => {},
  } = opts;

  if (!serialSupported()) {
    throw new Error(
      "Web Serial isn't available in this browser. Use Chrome or Edge 89+ on desktop " +
        "(or Chrome on Android). Safari and iOS don't support Web Serial.",
    );
  }
  if (!secureContextOk()) {
    throw new Error("Web Serial requires HTTPS (or localhost).");
  }

  const port = await navigator.serial.requestPort(); // must be a user gesture
  const transport = new Transport(port, true);
  const esploader = new ESPLoader({
    transport,
    baudrate,
    terminal: {
      clean() {},
      write: onLog,
      writeLine: onLog,
    },
  });

  const chipDescription = await esploader.main();
  onChip(chipDescription);
  const family = chipFamily(chipDescription);
  if (family !== "ESP32-S3") {
    await transport.disconnect();
    throw new Error(
      `Expected an ESP32-S3, but the connected board reports "${chipDescription}". ` +
        `Refusing to flash — the bootloader offset (0x0) and partition table are S3-specific ` +
        `and would likely brick a different chip.`,
    );
  }

  const args = await loadFlasherArgs(baseUrl);
  const parts = await Promise.all(
    Object.entries(args.flash_files).map(async ([addr, file]) => {
      const address = parseInt(addr, 16);
      // Skip the CI demo show bundle if the caller supplied their own —
      // it gets appended below at the same partition offset instead.
      if (bundleBytes && /\.shw1$/i.test(file)) return null;
      const data = await fetchBinaryString(new URL(file, baseUrl));
      return { address, data };
    }),
  );
  const fileArray = parts.filter(Boolean);

  if (bundleBytes) {
    const offset = findShowPartitionOffset(args);
    if (offset == null) {
      await transport.disconnect();
      throw new Error(
        "Couldn't find the show partition's flash offset in flasher_args.json " +
          `(expected a "*.shw1" entry). The firmware build may be out of date.`,
      );
    }
    fileArray.push({ address: offset, data: toBinaryString(bundleBytes) });
  }

  if (scriptsImageBytes) {
    const scripts = await findScriptsPartition(args, baseUrl);
    if (scripts == null) {
      await transport.disconnect();
      throw new Error(
        "Couldn't find the \"scripts\" partition in this firmware build's partition table " +
          "(expected a partition-table.bin in flasher_args.json). The firmware build may be out of date.",
      );
    }
    if (scriptsImageBytes.length !== scripts.size) {
      await transport.disconnect();
      throw new Error(
        `The built scripts-partition image is ${scriptsImageBytes.length} bytes, but this ` +
          `board's "scripts" partition is ${scripts.size} bytes. The vendored littlefs image ` +
          "builder and this firmware build's partition table have drifted apart — refusing to " +
          "flash a mismatched image.",
      );
    }
    fileArray.push({ address: scripts.offset, data: toBinaryString(scriptsImageBytes) });
  }

  if (eraseFirst) {
    onLog("Erasing flash...\n");
    await esploader.eraseFlash();
  }

  await esploader.writeFlash({
    fileArray,
    flashSize: "keep",
    flashMode: "keep",
    flashFreq: "keep",
    eraseAll: false,
    reportProgress: (fileIndex, written, total) => {
      onProgress(fileIndex, fileArray.length, total > 0 ? written / total : 0);
    },
  });

  await esploader.after(); // hard reset into the app
  await transport.disconnect();
}
