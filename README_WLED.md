# WLED UDP Notifier Fixture Support

## Overview

`esp-glow` can drive [WLED](https://kno.wled.ge/) devices (0.14.x) directly, alongside DMX/Art-Net fixtures, using WLED's UDP Notifier protocol (port 21324). Each command is a fixed 24-byte UDP packet: one `sendto()`, no heap allocation, no background task, no TCP handshake -- the render task never blocks on it.

WLED targets are declared once, by name, in the `.show` file, and controlled from Fennel via `glow.wled.*`. They are independent of the DMX/Art-Net universe system -- a WLED target is not patched to a universe or channel.

## Declaring targets: the `WLED` directive

```text
SHOW 2
UNIVERSE 1 DMX

# WLED <name> <ip> [syncGroup]
WLED "main_matrix"    "192.168.1.100"   1
WLED "christmas_tree" "192.168.1.101"   1
WLED "room_ambient"   "255.255.255.255" 1   # broadcast to every WLED on the LAN

FIXTURE samples/head.fdef 1 22
```

- `name`: unique within the show; this is what Fennel addresses. Double quotes are optional (stripped if present) -- neither `name` nor `ip` may contain whitespace.
- `ip`: an IPv4 dotted-quad, or `255.255.255.255` for a LAN broadcast. **mDNS hostnames (`wled-a1b2.local`) are not resolved** by the compiler or the device today -- use a static IP, or resolve one ahead of time (a documented gap, not silently broken: `wled_udp_sink.cpp` logs a warning and drops the send if you pass one).
- `syncGroup`: 1..8, default 1. Informational only -- WLED's own receiver does any sync-group filtering; esp-glow always addresses a target by name, never by group.

At least one `WLED` line bumps the compiled `SHW1` bundle to version 3 (see `show_bundle.h`); a show with no `WLED` line stays byte-identical to today's v1/v2 layout.

## Fennel API

```fennel
;; Effect with parameters (opts table is optional; every key has a default)
(glow.wled.fx "main_matrix" :fire-2012
  {:speed 180 :intensity 220 :brightness 200 :palette :fire :transition 500})

;; Solid color override (forces effect 0, sent as a direct color change)
(glow.wled.color "christmas_tree" 255 0 0 {:brightness 255 :transition 500})

;; Power
(glow.wled.off "main_matrix")
(glow.wled.on "main_matrix")

;; Broadcast an effect to every WLED device on the LAN, no named target
(glow.wled.fx-broadcast :pacifica {:speed 100 :intensity 150 :palette :ocean})
```

`glow.wled.*` calls are fire-and-forget: they are not frame-context-gated like `glow.set`/`glow.aim`, so they're callable from a cue's effect function, `boot.fnl`, or the live REPL alike.

An unknown target name is a Lua error (`glow.wled.fx: unknown WLED target 'x'`) -- a typo in a script fails loudly. An unknown effect or palette *name* instead falls back to `solid`/`default` with a logged warning, matching WLED's own "ignore what you don't understand" tolerance for a mistyped or version-skewed name; see `wled_effect_map.h`'s `effectIdFromName`/`paletteIdFromName`.

A device with no `WLED` directives in its show has `glow.wled.*` disabled -- every call is a clear Lua error ("no WLED targets on this device"), the same convention `glow.matrix.*`/`glow.ranges` use for a device with no matrices/no fixture registry.

## Packet format

24 bytes, WLED UDP Notifier protocol version 5 (palette byte honored):

| Byte  | Field | Notes |
|-------|-------|-------|
| 0     | packet_purpose | always `0x00` (notifier protocol) |
| 1     | callMode | `0x06` effect changed, `0x01` direct change (color/power) |
| 2     | brightness | master brightness 0-255 |
| 3-5   | col[0..2] | primary R/G/B |
| 6-7   | nightlight | always 0 (unused) |
| 8     | effectCurrent | 0-186 |
| 9     | effectSpeed | `sx`, 0-255 |
| 10    | white | primary W channel, always 0 |
| 11    | version | always `0x05` |
| 12-15 | colSec/whiteSec | always 0 (unused) |
| 16    | effectIntensity | `ix`, 0-255 |
| 17-18 | transitionDelay | big-endian ms |
| 19    | effectPalette | 0-70 |
| 20-23 | reserved | always 0 |

See `wled_packet.h`/`wled_packet.cpp` for the builder (host-tested, `test_wled.cpp`) and `wled_manager.h`/`wled_manager.cpp` for the named-target runtime (also host-tested via `MockWledSink`, mirroring `show.h`'s `MockSink`).

## Effect / palette maps

`wled_effect_map.h` has all 187 effects and 71 palettes from WLED 0.14.x's `FX.cpp`/`palettes.h`, as kebab-case names (`:fire-2012`, `:fairy-reef`, ...). It's version-locked: effect/palette IDs are stable within a WLED minor version but can shift across major versions. Regenerate it with `tools/generate_wled_maps.py` against a checked-out WLED source tree when bumping the target version, and re-review any `.show`/Fennel scripts that name effects by string.

## Architecture notes

- **No background task, no queue.** Every `glow.wled.*` call builds one packet on the caller's stack and hands it to a UDP socket (`WledUdpSink`, `#ifdef ESP_PLATFORM` guarded) with a bounded `SO_SNDTIMEO`, same discipline as `artnet_sink.cpp`. A slow/unreachable WLED device can never stall the render loop.
- **One socket for every target.** Unlike `ArtNetSink` (one `connect()`ed bridge), `WledUdpSink` uses `sendto()` per call since a show's WLED targets are typically different IPs plus the broadcast address.
- **Host-testable core.** `WledManager`/`buildWledPacket` have zero ESP-IDF dependency -- they take an injected `IWledSink` (mirroring `show.h`'s `IUniverseSink`), so `test_wled.cpp`/`test_apply_loaded_show.cpp`/`test_glow_lua_api.cpp` exercise the real packet bytes without a socket.
