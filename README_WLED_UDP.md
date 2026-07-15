# WLED UDP Notifier Integration

**Version:** 1.0.0  
**Target WLED Version:** 0.14.x (compatible with 0.8.0+)  
**Protocol:** UDP Notifier (port 21324)

---

## Overview

This document describes the integration of WLED fixtures into `esp-glow` using the **WLED UDP Notifier protocol**. This implementation replaces HTTP/JSON with a zero-allocation, sub-millisecond UDP broadcast mechanism that respects `esp-glow`'s core constraint: **the render task never blocks on I/O**.

### Why UDP Notifier?

| Feature | HTTP/JSON | UDP Notifier |
|---------|-----------|--------------|
| Latency | 50-200ms | **<1ms** |
| Heap allocation | cJSON + strings | **Zero** |
| Background task | Required | **Not needed** |
| Packet size | 200-500 bytes | **24 bytes fixed** |
| Broadcast | N/A (unicast only) | **Native support** |
| Reliability | TCP guaranteed | UDP best-effort (acceptable for lighting) |

---

## Quick Start

### 1. Enable in Build

Add to your firmware build configuration:

```cmake
# In CMakeLists.txt or idf_component.yml
add_compile_definitions(GLOW_WLED_UDP_NOTIFIER=1)
```

### 2. Declare WLED Fixtures in Your Show File

```text
SHOW 2
UNIVERSE 1 DMX

# WLED "<logical_name>" "<ip_or_hostname>" [sync_group]
WLED "main_matrix" "192.168.1.100" 1
WLED "accent_strip" "192.168.1.101" 1
WLED "room_ambient" "255.255.255.255" 1   # Broadcast to all WLEDs in group 1

FIXTURE samples/head.fdef 1 22
POS 2.0 1.0 0.0
```

### 3. Control from Fennel Scripts

```fennel
;; Set an effect with parameters
(glow.wled.fx "main_matrix" :rainbow-cycle
  {:speed 150 :intensity 200 :brightness 255 :palette :rainbow-bands})

;; Set a solid color
(glow.wled.color "accent_strip" 255 0 0
  {:brightness 255 :transition 500})

;; Power control
(glow.wled.on "main_matrix")
(glow.wled.off "main_matrix")

;; Broadcast to all WLED devices on the network
(glow.wled.fx-broadcast :pacifica
  {:speed 100 :intensity 150 :brightness 180 :palette :ocean})
```

---

## API Reference

### `(glow.wled.fx target-name effect-keyword opts-table)`

Set an effect on a named WLED target.

**Parameters:**
- `target-name`: String name matching a `WLED` declaration in the show file
- `effect-keyword`: Kebab-case effect name (e.g., `:fire-2012`, `:twinklefox`)
- `opts-table`: Optional table with keys:
  - `:speed` (0-255, default 128) - Animation speed
  - `:intensity` (0-255, default 128) - Effect intensity
  - `:brightness` (0-255, default 255) - Master brightness
  - `:palette` (keyword, default `:default`) - Color palette name
  - `:transition` (ms, default 0) - Fade duration

**Example:**
```fennel
(glow.wled.fx "tree" :meteor-rain
  {:speed 180
   :intensity 220
   :brightness 200
   :palette :colorful
   :transition 1000})
```

### `(glow.wled.color target-name r g b opts-table)`

Set a solid RGB color on a named WLED target.

**Parameters:**
- `target-name`: String name matching a `WLED` declaration
- `r`, `g`, `b`: Color values (0-255)
- `opts-table`: Optional table with keys:
  - `:brightness` (0-255, default 255)
  - `:transition` (ms, default 0)

**Example:**
```fennel
(glow.wled.color "strip" 255 128 0
  {:brightness 200 :transition 500})
```

### `(glow.wled.on target-name)` / `(glow.wled.off target-name)`

Power on/off a WLED target.

**Example:**
```fennel
(glow.wled.on "matrix")
(glow.wled.off "matrix")
```

### `(glow.wled.fx-broadcast effect-keyword opts-table)`

Broadcast an effect to ALL WLED devices on the network (filtered by sync group on the receiver side).

**Parameters:**
- `effect-keyword`: Kebab-case effect name
- `opts-table`: Same as `glow.wled.fx`

**Example:**
```fennel
(glow.wled.fx-broadcast :candle-multi
  {:brightness 150 :palette :fire})
```

---

## Available Effects

All 187 built-in WLED effects are available. Common ones include:

| Keyword | Description |
|---------|-------------|
| `:solid` | Static color (no animation) |
| `:blink` | Blinking color |
| `:breathe` | Breathing fade |
| `:wipe` | Wipe pattern |
| `:random-colors` | Random color generator |
| `:rainbow` | Rainbow cycle |
| `:scan` | Scanner pattern |
| `:theater` | Theater chase |
| `:running` | Running dots |
| `:twinkle` | Twinkle stars |
| `:sparkle` | Sparkle effect |
| `:strobe` | Strobe light |
| `:chase` | Chase pattern |
| `:fire-2012` | Fire simulation |
| `:twinklefox` | Christmas twinkle |
| `:plasma` | Plasma effect |
| `:pacifica` | Ocean waves |
| `:meteor-rain` | Meteor shower |
| `:matrix` | Matrix rain |
| `:candle` | Candle flicker |
| `:noise-pal` | Noise + palette |
| `:ripple` | Ripple effect |

See `wled_effect_map.h` for the complete list of 187 effects.

---

## Available Palettes

All 71 built-in WLED palettes are available by kebab-case name:

| Keyword | Description |
|---------|-------------|
| `:default` | Default gradient |
| `:rainbow` | Classic rainbow |
| `:rainbow-bands` | Rainbow bands |
| `:sunset` | Sunset colors |
| `:ocean` | Ocean blues |
| `:forest` | Forest greens |
| `:lava` | Lava reds/oranges |
| `:fire` | Fire palette |
| `:cloud` | Cloudy pastels |
| `:party` | Party colors |
| `:aurora` | Aurora borealis |
| `:fairy-reef` | Fairy reef colors |
| `:c9` | C9 Christmas |
| `:sakura` | Cherry blossom |
| `:temperature` | Color temperature |
| `:candy` | Candy colors |

See `wled_effect_map.h` for the complete list of 71 palettes.

---

## Show File Syntax

### WLED Directive

```text
WLED "<name>" "<ip_or_hostname>" [sync_group]
```

**Fields:**
- `name`: Unique logical name (used in Fennel API calls)
- `ip_or_hostname`: Static IP, mDNS hostname (e.g., `wled-a1b2.local`), or broadcast address (`255.255.255.255`)
- `sync_group`: Optional integer 1-8 (default: 1). WLED devices filter incoming sync packets by group.

**Examples:**
```text
WLED "matrix" "192.168.1.100" 1
WLED "tree" "wled-tree.local" 2
WLED "all_lights" "255.255.255.255" 1
```

---

## Architecture

### Zero-Allocation Design

The WLED manager uses:
- **Pre-allocated packet buffer**: 24-byte aligned buffer, never reallocates
- **Static effect/palette maps**: Generated at build time from WLED source
- **Fire-and-forget UDP**: No background task, no queue, no backpressure

### Packet Format

The UDP Notifier packet is exactly 24 bytes:

| Byte | Field | Value |
|------|-------|-------|
| 0 | `packet_purpose` | `0x00` (Notifier protocol) |
| 1 | `callMode` | `0x06` (Effect changed) or `0x01` (Direct change) |
| 2 | `bri` | Master brightness (0-255) |
| 3-5 | `col[0-2]` | Primary RGB |
| 6 | `nightlightActive` | 0/1 |
| 7 | `nightlightDelayMins` | 0-255 |
| 8 | `effectCurrent` | Effect ID (0-186) |
| 9 | `effectSpeed` | Speed (0-255) |
| 10 | `white` | White channel (RGBW strips) |
| 11 | `version` | `0x05` (Protocol version 5) |
| 12-14 | `colSec[0-2]` | Secondary RGB |
| 15 | `whiteSec` | Secondary white |
| 16 | `effectIntensity` | Intensity (0-255) |
| 17-18 | `transitionDelay` | Transition duration (big-endian) |
| 19 | `effectPalette` | Palette ID (0-70) |
| 20-23 | Reserved | Zeros |

### callMode Values

- `0x01`: Direct Change (color/brightness without effect change)
- `0x06`: Effect Changed (effect ID, speed, intensity, palette)

---

## Build-Time Map Generation

Effect and palette IDs are locked to a specific WLED version. The map is generated at build time:

```bash
python tools/generate_wled_maps.py \
    --fx-cpp deps/wled/wled00/FX.cpp \
    --palettes-h deps/wled/wled00/palettes.h \
    --out src/wled_effect_map.h \
    --wled-version 0.14.0
```

**When to regenerate:**
- When pinning a new WLED version
- In CI to verify map freshness against a known WLED git tag

**Never at runtime** — no filesystem access, no JSON parsing on device.

---

## WLED Device Configuration

On each WLED device:

1. **Enable UDP Notifier:**
   - Settings → Sync Interfaces → UDP Notifier
   - Check "Receive" (and optionally "Send" if you want bidirectional sync)

2. **Set Sync Group:**
   - Settings → Sync Interfaces → UDP Notifier
   - Set "Group" to match your show file (1-8, default: 1)

3. **Optional: Configure Receive Port:**
   - Default port is 21324
   - Can be changed in WLED settings if needed

---

## Troubleshooting

### WLED not responding

1. **Check network connectivity:**
   ```bash
   ping 192.168.1.100
   ```

2. **Verify UDP Notifier is enabled** on the WLED device (Settings → Sync Interfaces)

3. **Check sync group matches** between show file and WLED device

4. **Try broadcast address** to test all devices:
   ```fennel
   (glow.wled.fx-broadcast :solid {:brightness 255})
   ```

### Unknown effect/palette warnings

If you see warnings like `"Unknown effect ':xyz', falling back to solid"`:

1. Check the effect name spelling (must match kebab-case names in `wled_effect_map.h`)
2. Verify your WLED version matches the generated map version
3. Regenerate the map if you've updated WLED

### Transition delays not working

Transition delays are supported in WLED 0.8.0+. Older versions will apply changes instantly.

---

## Limitations

- **No per-segment control**: Use HTTP API for advanced segment-specific features
- **No custom palettes**: Only the 71 built-in palettes are supported
- **No preset activation**: Use effect+palette+parameters instead
- **Best-effort delivery**: UDP packets may be lost (acceptable for lighting)

For advanced features, fall back to the HTTP API (not covered in this integration).

---

## Files

| File | Purpose |
|------|---------|
| `wled_target.h` | WLED target data model |
| `wled_effect_map.h` | Auto-generated effect/palette ID maps |
| `wled_manager.h/.cpp` | UDP manager implementation |
| `tools/generate_wled_maps.py` | Map generation script |
| `samples/wled-demo.show` | Example show file |

---

## References

- [WLED UDP Notifier Protocol](https://knochen.leimstadt.de/udp-notifier.html)
- [WLED JSON API](https://github.com/Aircoookie/WLED/wiki/JSON-API)
- [WLED FX Source](https://github.com/Aircoookie/WLED/blob/main/wled00/FX.cpp)
