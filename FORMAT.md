# PFX1 Fixture Profile Format

## Overview

PFX1 is a compact binary format for DMX fixture profiles. It encodes the channel layout, capabilities, and default values for a single DMX fixture in a space-efficient way suitable for embedded devices with limited storage.

## Format Specification

The format is little-endian. All multi-byte fields (if any are added in future versions) use little-endian byte order. Currently, all fields are single bytes except for the 4-byte magic.

### Header (9 + nameLen bytes)

| Offset | Size (bytes) | Name      | Type    | Description |
|--------|--------------|-----------|---------|-------------|
| 0      | 4            | magic     | uint8[] | ASCII "PFX1" |
| 4      | 1            | version   | uint8   | Format version, must be 1 |
| 5      | 1            | flags     | uint8   | Reserved, must be 0 |
| 6      | 1            | footprint | uint8   | Number of DMX channels (1..255) |
| 7      | 1            | capCount  | uint8   | Number of capability records (0..MAX_CAPS=24) |
| 8      | 1            | nameLen   | uint8   | Length of fixture name (0 allowed) |
| 9      | nameLen      | name      | uint8[] | UTF-8 fixture name, NOT null-terminated |

### Capability Records (5 bytes each, capCount entries)

Immediately after the name, starting at offset `9 + nameLen`:

| Byte | Type    | Name         | Description |
|------|---------|--------------|-------------|
| 0    | uint8   | type         | Capability enum value |
| 1    | uint8   | coarseOffset | 0-based channel offset within footprint |
| 2    | uint8   | fineOffset   | 0-based offset of fine channel, or 0xFF if 8-bit only |
| 3    | uint8   | defaultValue | Idle value written by applyDefaults (e.g., shutter open) |
| 4    | uint8   | recFlags     | bit0 = inverted flag, bits 1-7 reserved (must be 0) |

**Total blob size:** `9 + nameLen + (5 * capCount)` bytes

## 16-Bit Channel Encoding (Coarse/Fine Convention)

For 16-bit capabilities (where `fineOffset != 0xFF`):

1. The normalized input value is in range [0.0, 1.0].
2. Scale to 16-bit by casting (truncation): `v16 = (uint16_t)(norm01 * 65535)`
3. Split into two DMX channels:
   - **Coarse (MSB)**: `v16 >> 8` written to channel `base + coarseOffset`
   - **Fine (LSB)**: `v16 & 0xFF` written to channel `base + fineOffset`
4. If the inverted flag (bit 0 of recFlags) is set, invert the entire 16-bit value BEFORE splitting:
   - `v16 = 65535 - v16`

### Example: Pan with norm=0.5

- `v16 = (uint16_t)(0.5 * 65535) = (uint16_t)(32767.5) = 32767 = 0x7FFF`
- Coarse = `0x7FFF >> 8 = 0x7F = 127`
- Fine = `0x7FFF & 0xFF = 0xFF = 255`

## 8-Bit Channel Encoding

For 8-bit capabilities (where `fineOffset == 0xFF`):

1. Scale to 8-bit: `v8 = round(norm01 * 255)`
2. If the inverted flag is set, invert AFTER scaling:
   - `v8 = 255 - v8`
3. Write to channel `base + coarseOffset`

### Example: Dimmer with norm=0.5

- `v8 = round(0.5 * 255) = 128` (or 127 depending on rounding)
- No inversion → write 128 or 127

### Example: Inverted Dimmer with norm=0.0

- `v8 = round(0.0 * 255) = 0`
- Invert → `255 - 0 = 255`
- Write 255

## Scaling and Rounding Convention

- **8-bit channels**: Use standard rounding (`round()` from `<cmath>`): round half away from zero.
  - `round(0.5 * 255) = round(127.5) = 128`
  - `round(1.0 * 255) = 255`

- **16-bit channels**: Use truncation (cast to `uint16_t`), which floors the result.
  - `(uint16_t)(0.5 * 65535) = (uint16_t)(32767.5) = 32767`
  - `(uint16_t)(1.0 * 65535) = 65535`

## Worked Example: "Torrent" 16-Channel Fixture

### Fixture Definition

- Footprint: 16 channels
- Name: "Torrent"
- Capabilities:
  - Dimmer (8-bit) at channel 0, default 0
  - Red (8-bit) at channel 1, default 0
  - Green (8-bit) at channel 2, default 0
  - Blue (8-bit) at channel 3, default 0
  - Pan (16-bit) coarse at 5, fine at 6, default 0
  - Tilt (16-bit) coarse at 7, fine at 8, default 0
  - ShutterStrobe (8-bit) at channel 10, default 8

### Encoded Blob (Hexadecimal)

```
50 46 58 31                          // "PFX1" (magic)
01                                   // version = 1
00                                   // flags = 0
10                                   // footprint = 16
07                                   // capCount = 7
07                                   // nameLen = 7 ("Torrent" is 7 bytes)
54 6f 72 72 65 6e 74                 // name = "Torrent" (UTF-8)

// Capability records (7 × 5 bytes = 35 bytes)
00 00 ff 00 00                       // Dimmer: type=0, coarse=0, fine=0xFF, default=0, flags=0
01 01 ff 00 00                       // Red: type=1, coarse=1, fine=0xFF, default=0, flags=0
02 02 ff 00 00                       // Green: type=2, coarse=2, fine=0xFF, default=0, flags=0
03 03 ff 00 00                       // Blue: type=3, coarse=3, fine=0xFF, default=0, flags=0
0a 05 06 00 00                       // Pan: type=10, coarse=5, fine=6, default=0, flags=0
0b 07 08 00 00                       // Tilt: type=11, coarse=7, fine=8, default=0, flags=0
0c 0a ff 08 00                       // ShutterStrobe: type=12, coarse=10, fine=0xFF, default=8, flags=0
```

**Total size:** 9 + 7 + 35 = 51 bytes

## Validation Rules

The parser (`parseProfile`) enforces these strict rules:

1. Buffer must be at least 9 bytes (header).
2. Buffer must be at least `9 + nameLen + 5 * capCount` bytes.
3. Magic must be `"PFX1"` (0x50 0x46 0x58 0x31).
4. Version must be 1.
5. Flags must be 0 (reserved).
6. capCount must be ≤ MAX_CAPS (24).
7. For each capability:
   - `coarseOffset` must be < footprint.
   - `fineOffset` must be 0xFF, OR must be < footprint.

If any rule is violated, the parser returns `false` and does not modify the output structure. The parser is a security boundary and never reads out of bounds.

## Notes

- The format does not currently support function ranges (e.g., "shutter open = DMX 8–15"). This is v2 scope.
- The name field is optional and is never used by the apply functions; it is for debugging and tooling only.
- Inverted flags and default values are stored per-capability to support fixtures with varying conventions.
