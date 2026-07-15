// test-devcfg.mjs -- proves the JS CFG1 encoder/decoder (devcfg.js) is
// byte-identical to the C++ parser the firmware actually links
// (device_config.cpp), via devcfg_check (the host build of that same
// parser -- see Makefile's devcfg_check target). Same pattern as
// web/shared/importers/test-importers.mjs's fdef_check round trip.
//
// Run: node web/shared/test-devcfg.mjs
// (or `make test-devcfg`, which builds devcfg_check first)
//
// testdata/devcfg_golden.bin is a COMMITTED fixture -- this test asserts
// the JS encoder reproduces it byte-for-byte, so any accidental drift in
// devcfg.js's field layout (offset, width, byte order) shows up as a
// failing byte-diff here, not as a silently-mismatched device that boots
// on the wrong GPIOs. See device_config.h's header comment for the format
// spec both sides implement.

import { readFileSync, writeFileSync, unlinkSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { execFileSync } from "node:child_process";

import {
  DEVCFG_BLOB_SIZE,
  encodeDeviceConfig,
  decodeDeviceConfig,
  parseIPv4,
  formatIPv4,
} from "./devcfg.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const REPO_ROOT = join(__dirname, "..", "..");
const DEVCFG_CHECK = join(REPO_ROOT, "devcfg_check");
const GOLDEN_PATH = join(__dirname, "testdata", "devcfg_golden.bin");

let failures = 0;
let count = 0;
function check(name, cond, detail) {
  count++;
  if (!cond) {
    failures++;
    console.log(`  FAIL: ${name}${detail !== undefined ? " -- " + JSON.stringify(detail) : ""}`);
  } else {
    console.log(`  ok:   ${name}`);
  }
}
function section(title) {
  console.log(`\n== ${title} ==`);
}

// The canonical test vector the golden blob was generated from. If this
// object ever changes, regenerate testdata/devcfg_golden.bin to match (see
// the bottom of this file / README note) -- deliberately not automated,
// so a drift is a conscious, reviewed decision.
const GOLDEN_CONFIG = {
  usbMidiHost: true,
  skipWifi: false,
  wifiSsid: "TestNet",
  wifiPass: "hunter2pass",
  artnetFallbackIp: parseIPv4("192.168.1.5"),
  artnetPort: 6454,
  dmxTxGpio: 17,
  dmxRxGpio: 18,
  dmxRtsGpio: 8,
  ledGpio: 2,
};

function runDevcfgCheck(bytes) {
  const tmp = join(__dirname, `.tmp-devcfg-check-${process.pid}.bin`);
  writeFileSync(tmp, Buffer.from(bytes));
  try {
    // devcfg_check exits 1 (with a JSON {"ok":false,...} on stdout) for a
    // rejected blob -- that's an expected outcome for the corruption
    // tests below, not a harness failure, so a nonzero exit must not
    // throw away stdout the way execFileSync's default does.
    const out = execFileSync(DEVCFG_CHECK, [tmp], { encoding: "utf8" });
    return JSON.parse(out);
  } catch (e) {
    if (e.stdout) return JSON.parse(e.stdout);
    throw e;
  } finally {
    unlinkSync(tmp);
  }
}

section("Golden blob: JS encoder matches the committed fixture byte-for-byte");
{
  const encoded = encodeDeviceConfig(GOLDEN_CONFIG);
  check("encoded length matches DEVCFG_BLOB_SIZE", encoded.length === DEVCFG_BLOB_SIZE, encoded.length);

  const golden = new Uint8Array(readFileSync(GOLDEN_PATH));
  check("golden fixture length matches DEVCFG_BLOB_SIZE", golden.length === DEVCFG_BLOB_SIZE, golden.length);

  let firstDiff = -1;
  for (let i = 0; i < Math.min(encoded.length, golden.length); i++) {
    if (encoded[i] !== golden[i]) { firstDiff = i; break; }
  }
  check("encoded bytes == committed golden blob", firstDiff === -1, { firstDiff });
}

section("Golden blob: C++ parser (device_config.cpp, via devcfg_check) accepts it with the expected fields");
{
  const golden = new Uint8Array(readFileSync(GOLDEN_PATH));
  const result = runDevcfgCheck(golden);
  check("devcfg_check: ok", result.ok === true, result);
  if (result.ok) {
    check("usbMidiHost", result.usbMidiHost === GOLDEN_CONFIG.usbMidiHost);
    check("skipWifi", result.skipWifi === GOLDEN_CONFIG.skipWifi);
    check("wifiSsid", result.wifiSsid === GOLDEN_CONFIG.wifiSsid);
    check("wifiPass", result.wifiPass === GOLDEN_CONFIG.wifiPass);
    check("artnetFallbackIp", result.artnetFallbackIp === GOLDEN_CONFIG.artnetFallbackIp);
    check("artnetPort", result.artnetPort === GOLDEN_CONFIG.artnetPort);
    check("dmxTxGpio", result.dmxTxGpio === GOLDEN_CONFIG.dmxTxGpio);
    check("dmxRxGpio", result.dmxRxGpio === GOLDEN_CONFIG.dmxRxGpio);
    check("dmxRtsGpio", result.dmxRtsGpio === GOLDEN_CONFIG.dmxRtsGpio);
    check("ledGpio", result.ledGpio === GOLDEN_CONFIG.ledGpio);
  }
}

section("JS decodeDeviceConfig round-trips its own encoder's output");
{
  const cfg = {
    usbMidiHost: false,
    skipWifi: true,
    wifiSsid: "AnotherSSID",
    wifiPass: "",
    artnetFallbackIp: 0,
    artnetPort: 6454,
    dmxTxGpio: 4,
    dmxRxGpio: 5,
    dmxRtsGpio: 6,
    ledGpio: 48,
  };
  const encoded = encodeDeviceConfig(cfg);
  const decoded = decodeDeviceConfig(encoded);
  check("decode ok", decoded.ok === true, decoded);
  if (decoded.ok) {
    for (const key of Object.keys(cfg)) {
      check(`field ${key} round-trips`, decoded.cfg[key] === cfg[key], { got: decoded.cfg[key], want: cfg[key] });
    }
  }

  const devcfgCheckResult = runDevcfgCheck(encoded);
  check("C++ parser also accepts JS-encoded arbitrary config", devcfgCheckResult.ok === true, devcfgCheckResult);
}

section("Corruption is rejected by both the JS decoder and the C++ parser");
{
  const encoded = encodeDeviceConfig(GOLDEN_CONFIG);

  const badMagic = new Uint8Array(encoded);
  badMagic[0] = 0x58; // 'X'
  check("JS: bad magic rejected", decodeDeviceConfig(badMagic).ok === false);
  check("C++: bad magic rejected", runDevcfgCheck(badMagic).ok === false);

  const badCrc = new Uint8Array(encoded);
  badCrc[146] ^= 0xff;
  check("JS: bad CRC rejected", decodeDeviceConfig(badCrc).ok === false);
  check("C++: bad CRC rejected", runDevcfgCheck(badCrc).ok === false);

  const erased = new Uint8Array(DEVCFG_BLOB_SIZE).fill(0xff);
  check("JS: all-0xFF erased flash rejected", decodeDeviceConfig(erased).ok === false);
  check("C++: all-0xFF erased flash rejected", runDevcfgCheck(erased).ok === false);

  const truncated = encoded.subarray(0, DEVCFG_BLOB_SIZE - 1);
  check("JS: truncated buffer rejected", decodeDeviceConfig(truncated).ok === false);
}

section("IPv4 helpers");
{
  check("parseIPv4 round-trips through formatIPv4", formatIPv4(parseIPv4("192.168.1.5")) === "192.168.1.5");
  check("parseIPv4('') is broadcast (0)", parseIPv4("") === 0);
  check("parseIPv4('broadcast') is 0", parseIPv4("broadcast") === 0);
  check("formatIPv4(0) is empty string", formatIPv4(0) === "");
  let threw = false;
  try { parseIPv4("not-an-ip"); } catch { threw = true; }
  check("parseIPv4 throws on garbage", threw);
}

console.log(`\n${count - failures}/${count} checks passed.`);
if (failures > 0) {
  console.log(`${failures} FAILURE(S)`);
  process.exit(1);
}
