// devcfg.js — CFG1 flash-time device config: the browser-side twin of
// device_config.h/.cpp (encoder + decoder). Plain JS, no build step, no
// WASM — unlike PFX1/MDF1 (compiled from a text grammar via provision.cpp),
// CFG1 is just a fixed-field binary struct, trivial to encode/decode
// directly in JS. Used by:
//   - web/provisioner-static/flash.js (encodes the config form into bytes
//     written at the "devcfg" partition offset at flash time)
//   - the device console's reconfigure page (web/console, embedded into
//     firmware/main/data/console) -- decodes the config read back from
//     GET /devcfg, and encodes an edited one for POST /devcfg
//
// Byte layout (little-endian, fixed-size) -- see FORMAT.md's "CFG1 device
// config format" section and device_config.h for the authoritative spec;
// keep this file and that header byte-identical. web/shared/test-devcfg.mjs
// proves it via devcfg_check.cpp (the host build of device_config.cpp's
// parser) and a committed golden blob.

export const DEVCFG_BLOB_SIZE = 150;
export const DEVCFG_CRC_OFFSET = 146;
export const DEVCFG_SSID_MAX = 32;
export const DEVCFG_PASS_MAX = 64;

export const DEVCFG_FLAG_USB_MIDI_HOST = 0x01;
export const DEVCFG_FLAG_SKIP_WIFI = 0x02;

// Standard CRC-32 (IEEE 802.3 / zlib / PNG): poly 0xEDB88320 (reflected),
// init 0xFFFFFFFF, final XOR 0xFFFFFFFF. Bitwise (no lookup table) --
// 146 bytes is nothing, and this keeps the implementation trivially
// comparable to device_config.cpp's devcfgCrc32 byte-for-byte.
export function devcfgCrc32(bytes) {
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

// A default-shaped config object -- callers spread over this so a partial
// form (e.g. "no WiFi fields touched yet") still encodes something valid.
export function defaultDeviceConfig() {
  return {
    usbMidiHost: false,
    skipWifi: false,
    wifiSsid: "",
    wifiPass: "",
    artnetFallbackIp: 0, // 0 = broadcast
    artnetPort: 6454,
    dmxTxGpio: 17,
    dmxRxGpio: 18,
    dmxRtsGpio: 8,
    ledGpio: 2,
  };
}

// Parses "192.168.1.50" into the packed host-byte-order uint32 device_config.h
// expects ((192<<24)|(168<<16)|(1<<8)|50), matching Kconfig's
// GLOW_ARTNET_BRIDGE_IP convention. Returns 0 (broadcast) for "", null,
// undefined, or "broadcast". Throws on anything else malformed.
export function parseIPv4(text) {
  if (text == null) return 0;
  const s = String(text).trim();
  if (s === "" || s.toLowerCase() === "broadcast") return 0;
  const parts = s.split(".");
  if (parts.length !== 4) throw new Error(`Invalid IPv4 address: "${text}"`);
  let packed = 0;
  for (const p of parts) {
    if (!/^\d{1,3}$/.test(p)) throw new Error(`Invalid IPv4 address: "${text}"`);
    const n = Number(p);
    if (n > 255) throw new Error(`Invalid IPv4 address: "${text}"`);
    packed = ((packed << 8) | n) >>> 0;
  }
  return packed >>> 0;
}

export function formatIPv4(packed) {
  if (!packed) return "";
  return [(packed >>> 24) & 0xff, (packed >>> 16) & 0xff, (packed >>> 8) & 0xff, packed & 0xff].join(".");
}

function writePaddedString(view, offset, fieldLen, str) {
  const bytes = new TextEncoder().encode(str || "");
  const n = Math.min(bytes.length, fieldLen);
  for (let i = 0; i < n; i++) view.setUint8(offset + i, bytes[i]);
  // Remainder stays zero (the buffer is zero-initialized by the caller).
}

function readPaddedString(view, offset, fieldLen) {
  const bytes = new Uint8Array(view.buffer, view.byteOffset + offset, fieldLen);
  const nul = bytes.indexOf(0);
  const trimmed = nul < 0 ? bytes : bytes.subarray(0, nul);
  return new TextDecoder().decode(trimmed);
}

// Encodes `cfg` (any subset of defaultDeviceConfig()'s shape -- missing
// fields fall back to the default) into a DEVCFG_BLOB_SIZE-byte CFG1 blob.
export function encodeDeviceConfig(cfg) {
  const c = { ...defaultDeviceConfig(), ...cfg };
  const buf = new ArrayBuffer(DEVCFG_BLOB_SIZE);
  const view = new DataView(buf);
  const bytes = new Uint8Array(buf);

  bytes[0] = 0x43; // 'C'
  bytes[1] = 0x46; // 'F'
  bytes[2] = 0x47; // 'G'
  bytes[3] = 0x31; // '1'
  bytes[4] = 1; // version
  let flags = 0;
  if (c.usbMidiHost) flags |= DEVCFG_FLAG_USB_MIDI_HOST;
  if (c.skipWifi) flags |= DEVCFG_FLAG_SKIP_WIFI;
  bytes[5] = flags;
  view.setUint16(6, 0, true); // reserved

  let off = 8;
  writePaddedString(view, off, DEVCFG_SSID_MAX, c.wifiSsid);
  off += DEVCFG_SSID_MAX;
  writePaddedString(view, off, DEVCFG_PASS_MAX, c.wifiPass);
  off += DEVCFG_PASS_MAX;

  view.setUint32(off, c.artnetFallbackIp >>> 0, true);
  off += 4;
  view.setUint16(off, c.artnetPort & 0xffff, true);
  off += 2;

  bytes[off++] = c.dmxTxGpio & 0xff;
  bytes[off++] = c.dmxRxGpio & 0xff;
  bytes[off++] = c.dmxRtsGpio & 0xff;
  bytes[off++] = c.ledGpio & 0xff;

  // reserved2 (32 bytes) stays zero.

  const crc = devcfgCrc32(bytes.subarray(0, DEVCFG_CRC_OFFSET));
  view.setUint32(DEVCFG_CRC_OFFSET, crc, true);

  return bytes;
}

// Parses a CFG1 blob. Returns { ok: true, cfg } on success, or
// { ok: false, error } on any failure (short buffer, bad magic, bad
// version, CRC mismatch) -- mirrors parseDeviceConfig's strictness
// (device_config.cpp), including rejecting an all-0xFF erased partition
// via the magic check before any field is ever read.
export function decodeDeviceConfig(bytes) {
  if (!bytes || bytes.length < DEVCFG_BLOB_SIZE) {
    return { ok: false, error: `blob too short (${bytes ? bytes.length : 0} < ${DEVCFG_BLOB_SIZE} bytes)` };
  }
  if (bytes[0] !== 0x43 || bytes[1] !== 0x46 || bytes[2] !== 0x47 || bytes[3] !== 0x31) {
    return { ok: false, error: "bad magic (expected \"CFG1\")" };
  }
  const version = bytes[4];
  if (version !== 1) {
    return { ok: false, error: `unsupported version ${version}` };
  }

  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const storedCrc = view.getUint32(DEVCFG_CRC_OFFSET, true);
  const computedCrc = devcfgCrc32(bytes.subarray(0, DEVCFG_CRC_OFFSET));
  if (storedCrc !== computedCrc) {
    return { ok: false, error: `CRC mismatch (stored 0x${storedCrc.toString(16)}, computed 0x${computedCrc.toString(16)})` };
  }

  const flags = bytes[5];
  let off = 8;
  const wifiSsid = readPaddedString(view, off, DEVCFG_SSID_MAX);
  off += DEVCFG_SSID_MAX;
  const wifiPass = readPaddedString(view, off, DEVCFG_PASS_MAX);
  off += DEVCFG_PASS_MAX;

  const artnetFallbackIp = view.getUint32(off, true);
  off += 4;
  const artnetPort = view.getUint16(off, true);
  off += 2;

  const dmxTxGpio = bytes[off++];
  const dmxRxGpio = bytes[off++];
  const dmxRtsGpio = bytes[off++];
  const ledGpio = bytes[off++];

  return {
    ok: true,
    cfg: {
      usbMidiHost: (flags & DEVCFG_FLAG_USB_MIDI_HOST) !== 0,
      skipWifi: (flags & DEVCFG_FLAG_SKIP_WIFI) !== 0,
      wifiSsid,
      wifiPass,
      artnetFallbackIp,
      artnetPort,
      dmxTxGpio,
      dmxRxGpio,
      dmxRtsGpio,
      ledGpio,
    },
  };
}
