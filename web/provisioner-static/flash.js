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
//   eraseFirst  bool — erase the whole flash before writing.
//   baudrate    number — defaults to 460800.
//   onLog(line) called with esptool-js's terminal output.
//   onProgress(fileIndex, fileCount, fraction) per-part progress.
//   onChip(chipDescription) called once the chip is detected.
export async function flash(opts) {
  const {
    baseUrl = new URL("firmware/", window.location.href),
    bundleBytes = null,
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
