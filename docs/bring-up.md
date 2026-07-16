# Bring-up

## Hardware

ESP32-S3 (dual-core, FPU, PSRAM — the Lua VM lives there). DMX out via an RS-485
transceiver (MAX3485; default GPIOs TX 17 / RX 18 / DE-RTS 8). Pixel matrices via
Art-Net or sACN over WiFi. Inputs: MIDI (DIN/UART or native USB host), OSC (UDP),
DJ-Link, and a device-served web console.

## Flash-time config

The browser flasher writes a `CFG1` blob — WiFi credentials, DMX GPIOs, Art-Net
fallback, and a USB-MIDI checkbox — so a stranger can flash a working rig without
ESP-IDF installed. Config is also editable live from the device console. A
missing/corrupt config boots on compiled-in defaults and says so rather than refusing
to boot.

## Build

ESP-IDF v5.1+. Two vendored dependencies need a specific patch, not a compile flag:

- **Lua 5.4.6**: `luaconf.h`'s `LUA_32BITS` must be flipped to `1` by editing the file
  directly — `-DLUA_32BITS=1` on the command line leaves the compiled library on
  doubles, defeating the point (the ESP32-S3's FPU is single-precision).
- **Fennel 1.6.1**: a self-hosted compiler, bootstrapped via `scripts/vendor_fennel.sh`
  into one AOT-compiled file, pinned to the `1.6.1` release tag (not git HEAD).

## The bring-up ladder

boot → **DMX out** (the milestone) → WiFi/Art-Net → console → live-coding → HIL →
**the soak**. The soak is the one risk no host or QEMU test can answer: whether the Lua
GC, paced in the render task's frame slack, ever drops a DMX frame under sustained real
hardware load.

## Testing

```sh
make test
```

Runs the host-tested suite — see the [generated test status page](generated/test-status.html)
for the exact suite count and pass/fail as of the last docs build (extracted from the
`Makefile`, not hand-copied, so it can't go stale the way a hand-typed count would).

**QEMU** boots the real firmware image in CI (asserts boot telemetry, a 5× anti-flake
loop, addr2line-symbolicated crashes on failure). **HIL** (`tests/hil/`) covers what
neither host tests nor QEMU can: DMX timing, Art-Net on the wire, the Fennel REPL
end-to-end against real hardware, and a soak run. Two things stay human to verify:
whether a moving head physically aims right, and colour.

## Known limitations

Documented choices, not accidents — revisit as a block before shipping a product: WiFi
password stored in plaintext in flash, no console auth, unsigned OTA images.
