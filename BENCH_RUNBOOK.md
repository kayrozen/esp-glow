# Bench Runbook

Constraints worth checking on the bench, before a rig leaves for a venue --
things that pass on a known home network and then misbehave on a router
nobody chose. This is not the HIL suite (`tests/hil/README.md` covers
automated validation); it's the list of "why does it do that" answers so a
gig doesn't turn into a live debugging session.

## Known constraints

### WiFi 6 (802.11ax) access points: AMPDU RX disabled

**Symptom:** Art-Net/OSC UDP looks fine, then stalls for a beat and resumes
in a burst, on some networks but not others -- home network tests clean,
venue WiFi (often newer, often WiFi 6) does not.

**Cause:** AMPDU RX (block-ack frame aggregation) on the ESP32-S3 WiFi
driver has a reorder-buffer interaction that doesn't line up with some
802.11ax APs' aggregation behavior. It presents like the modem-sleep power-
save issue this project already works around (`wifi_manager.cpp`'s
`esp_wifi_set_ps(WIFI_PS_NONE)` after association -- some APs drop UDP for
a "sleeping" station instead of queuing it), but has a different root cause
and no telemetry line to point at it.

**Fix:** `firmware/sdkconfig.defaults` sets `CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=n`.
This is compile-time -- there's no runtime `esp_wifi_*` call to toggle it,
so it can't be conditioned on the AP actually being WiFi 6; it's off for
every build. AMPDU TX stays enabled; only RX reassembly was implicated.

**Why this belongs here, not just in a commit message:** the point of this
fix isn't "it works on my network now" -- it's that the device will land on
networks nobody controls or has tested against beforehand, and it needs to
just work. Treat this as a permanent constraint, not a workaround to
revisit: don't re-enable AMPDU RX to chase a throughput number without
re-running a WiFi 6 AP bench check first.

**How to bench it:** join a WiFi 6 AP (ASUS/Ubiquiti/Eero/etc. running
802.11ax, not just labeled "WiFi 6 router" -- check it's actually
negotiating ax, not falling back to ac), run the L2 Art-Net / L4 OSC HIL
layers or a soak against it, and watch for any gap in frame delivery. Watch
`GLOW-TEST: wifi state=` telemetry too -- if drops correlate with
`state=retrying`, that's the reconnect path (README_FIRMWARE.md's F5
section), a different problem from this one.

### SoftAP + DMX timing: not HIL-verified

Flagged in `wifi_manager.h` (`WifiStaConfig::ap_fallback`) and
`README_FIRMWARE.md`'s F5 section. AP mode adds beacon/probe-response
airtime on the same radio driver that carries Art-Net; DMX itself is a
dedicated UART and should be structurally unaffected (core 1, pinned, never
touches WiFi), but this has not been soak-tested on hardware with the
SoftAP fallback actually active. Bench before trusting it at a gig where
the venue WiFi is expected to be unreliable enough to trigger the fallback.

### Other known limitations

See `docs/bring-up.md`'s "Known limitations" section (plaintext WiFi
password in flash, no console auth, unsigned OTA images) -- documented
choices, not bench findings, but relevant to the same "what does this rig
do on someone else's network" question.
