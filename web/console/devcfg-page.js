// devcfg-page.js — §6 reconfigure-without-reflashing: reads the device's
// current CFG1 config over GET /devcfg, lets the operator edit it in the
// plain form in devcfg.html, and writes it back over POST /devcfg (which
// reboots the device to apply it -- see device_config_web.h). No
// framework: this is one form, not worth pulling in Preact for.
import { encodeDeviceConfig, decodeDeviceConfig, parseIPv4, formatIPv4 } from "./shared/devcfg.js";

const $ = (id) => document.getElementById(id);
const statusEl = $("status");

function setStatus(kind, text) {
  statusEl.className = kind;
  statusEl.textContent = text;
}

function fillForm(cfg) {
  $("skipWifi").checked = !!cfg.skipWifi;
  $("wifiSsid").value = cfg.wifiSsid || "";
  $("wifiPass").value = cfg.wifiPass || "";
  $("artnetFallbackIp").value = formatIPv4(cfg.artnetFallbackIp || 0);
  $("artnetPort").value = cfg.artnetPort || 6454;
  $("dmxTxGpio").value = cfg.dmxTxGpio;
  $("dmxRxGpio").value = cfg.dmxRxGpio;
  $("dmxRtsGpio").value = cfg.dmxRtsGpio;
  $("ledGpio").value = cfg.ledGpio;
  $("usbMidiHost").checked = !!cfg.usbMidiHost;
}

function readForm() {
  return {
    skipWifi: $("skipWifi").checked,
    wifiSsid: $("wifiSsid").value,
    wifiPass: $("wifiPass").value,
    artnetFallbackIp: parseIPv4($("artnetFallbackIp").value),
    artnetPort: Number($("artnetPort").value) || 6454,
    dmxTxGpio: Number($("dmxTxGpio").value) & 0xff,
    dmxRxGpio: Number($("dmxRxGpio").value) & 0xff,
    dmxRtsGpio: Number($("dmxRtsGpio").value) & 0xff,
    ledGpio: Number($("ledGpio").value) & 0xff,
    usbMidiHost: $("usbMidiHost").checked,
  };
}

async function loadCurrent() {
  setStatus("info", "Reading current config…");
  try {
    const res = await fetch("/devcfg", { cache: "no-store" });
    if (!res.ok) throw new Error(`GET /devcfg failed: ${res.status}`);
    const bytes = new Uint8Array(await res.arrayBuffer());
    const decoded = decodeDeviceConfig(bytes);
    if (!decoded.ok) throw new Error(`device returned an unparseable config: ${decoded.error}`);
    fillForm(decoded.cfg);
    setStatus("ok", "Loaded the device's current config.");
  } catch (e) {
    setStatus("err", `Could not load current config: ${e.message}`);
  }
}

async function save(ev) {
  ev.preventDefault();
  const saveBtn = $("save-btn");
  let cfg;
  try {
    cfg = readForm();
  } catch (e) {
    setStatus("err", `Invalid field: ${e.message}`);
    return;
  }

  if (!cfg.skipWifi && !cfg.wifiSsid) {
    setStatus("err", "WiFi SSID is empty. Tick \"No WiFi\" if that's intentional, or fill in an SSID.");
    return;
  }

  const bytes = encodeDeviceConfig(cfg);
  // Sanity-check our own encoding before sending it -- the device will
  // reject a bad blob too (device_config_web.cpp validates before ever
  // writing flash), but catching an encoder bug here is a better user
  // experience than a 400 with no context.
  const check = decodeDeviceConfig(bytes);
  if (!check.ok) {
    setStatus("err", `Refusing to send: encoded config failed its own validation (${check.error}). This is a bug -- please report it.`);
    return;
  }

  saveBtn.disabled = true;
  setStatus("info", "Writing config and rebooting the device…");
  try {
    const res = await fetch("/devcfg", { method: "POST", body: bytes });
    const text = await res.text();
    if (!res.ok) {
      setStatus("err", `Device rejected the config (${res.status}): ${text}`);
      saveBtn.disabled = false;
      return;
    }
    setStatus("ok", `${text}\nThe device is rebooting -- this page will lose its connection shortly. `
      + "If you changed the SSID/password, reconnect to the new network before reloading.");
  } catch (e) {
    // A fetch error here is actually the EXPECTED outcome once the device
    // reboots mid-response on a WiFi change -- still worth telling the
    // operator, since "did it work?" is the natural next question.
    setStatus("info", `Request ended (${e.message}) -- likely because the device rebooted. `
      + "If you changed WiFi settings, reconnect to the new network and reload this page to confirm.");
  } finally {
    saveBtn.disabled = false;
  }
}

$("reload-btn").addEventListener("click", loadCurrent);
$("devcfg-form").addEventListener("submit", save);

loadCurrent();
