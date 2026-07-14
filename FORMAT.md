# PFX1 Fixture Profile Format

## Overview

PFX1 is a compact binary format for DMX fixture profiles. It encodes the channel layout, capabilities, and default values for a single DMX fixture in a space-efficient way suitable for embedded devices with limited storage.

The format has two versions, both sharing the `"PFX1"` magic (the version byte, not the magic, is what changes):

- **Version 1**: every capability is a linear ramp (`norm01` -> raw DMX byte/word). Still fully supported -- profiles already in the field, and the browser flasher's existing blobs, must keep parsing.
- **Version 2**: adds an optional **function range table** per profile, so a capability's raw DMX byte range can be carved into named discrete slots ("gobo 2", "red") and continuous sub-ranges ("strobe 1->10 Hz"), on top of the same linear channel layout. See "Function Ranges (v2)" below.

`parseProfile` accepts both versions. The encoder (`ProfileBuilder`/the `.fdef` compiler) emits version 1 bytes -- byte-identical to the original format -- for any fixture with no ranges, and only emits version 2 once at least one range has been added.

## Format Specification

The format is little-endian. All multi-byte fields use little-endian byte order.

### Header

| Offset | Size (bytes) | Name       | Type    | Description |
|--------|--------------|------------|---------|-------------|
| 0      | 4            | magic      | uint8[] | ASCII "PFX1" |
| 4      | 1            | version    | uint8   | Format version: 1 or 2 |
| 5      | 1            | flags      | uint8   | Reserved, must be 0 |
| 6      | 1            | footprint  | uint8   | Number of DMX channels (1..255) |
| 7      | 1            | capCount   | uint8   | Number of capability records (0..MAX_CAPS=24) |
| 8      | 1            | nameLen    | uint8   | Length of fixture name (0 allowed) |
| 9      | nameLen      | name       | uint8[] | UTF-8 fixture name, NOT null-terminated |

Version 1's header is exactly the 9 bytes above (**9 + nameLen** total before the capability records).

Version 2 inserts one extra field between `nameLen` and `name`:

| Offset | Size (bytes) | Name       | Type    | Description |
|--------|--------------|------------|---------|-------------|
| 9      | 2            | rangeCount | uint16  | Number of function-range records (0..MAX_RANGES=192) |
| 11     | nameLen      | name       | uint8[] | UTF-8 fixture name, NOT null-terminated |

So version 2's header is **11 + nameLen** bytes before the capability records. `rangeCount` is a genuine header field (not part of the range table), which is why it comes before `name` -- everything from `name` onward shifts by 2 bytes relative to v1.

### Capability Records (5 bytes each, capCount entries)

Immediately after the name (offset `9 + nameLen` for v1, `11 + nameLen` for v2), identical layout in both versions:

| Byte | Type    | Name         | Description |
|------|---------|--------------|-------------|
| 0    | uint8   | type         | Capability enum value |
| 1    | uint8   | coarseOffset | 0-based channel offset within footprint |
| 2    | uint8   | fineOffset   | 0-based offset of fine channel, or 0xFF if 8-bit only |
| 3    | uint8   | defaultValue | Idle value written by applyDefaults (e.g., shutter open) |
| 4    | uint8   | recFlags     | bit0 = inverted flag, bits 1-7 reserved (must be 0) |

**Version 1 total blob size:** `9 + nameLen + (5 * capCount)` bytes.

### Function Ranges (v2)

Immediately after the capability records, `rangeCount` entries of 7 bytes each:

| Byte | Type    | Name      | Description |
|------|---------|-----------|-------------|
| 0    | uint8   | capIndex  | Which capability record this range narrows -- an index into the capability-records array above (`channels[]`), not a `Capability` enum value |
| 1    | uint8   | dmxFrom   | Raw DMX byte value, start of the range (inclusive) |
| 2    | uint8   | dmxTo     | Raw DMX byte value, end of the range (inclusive); must be >= dmxFrom |
| 3    | uint8   | flags     | bit0: 1 = continuous sub-range, 0 = discrete slot. Bits 1-7 unused. |
| 4-5  | uint16  | nameOff   | Offset into the trailing name blob, or `0xFFFF` if the range is unnamed |
| 6    | uint8   | semantic  | Reserved for v3 (e.g. "nearest red" colour-wheel mapping). Always 0 today; parser does not reject other values, but nothing currently reads it. |

A **discrete slot** (`flags` bit0 = 0) represents one named position, e.g. a colour wheel's "red" filter at DMX 10-19. Selecting it writes the centre of `[dmxFrom, dmxTo]` -- the safe value, since the edges of a slot can bleed into the neighbouring slot on a miscalibrated fixture.

A **continuous sub-range** (`flags` bit0 = 1) represents a linear function carved out of part of the channel, e.g. a shutter's "strobe" sub-range at DMX 64-95 (slow to fast). Selecting it maps a `[0,1]` value linearly across just that sub-range, not the full 0-255 channel.

Ranges are a **coarse-channel concept**: even on a 16-bit capability, a range write only ever touches the coarse byte; the fine byte is untouched.

### Range Name Blob (v2)

Immediately after the range records: the rest of the blob is a UTF-8, NUL-separated name blob. Its length is not stored explicitly -- it is every remaining byte up to the end of the buffer (`len - (11 + nameLen + 5*capCount + 7*rangeCount)`). A `nameOff` in a range record indexes into this blob and must land on (or before) a NUL terminator within its bounds; `nameOff = 0xFFFF` means the range is unnamed and no blob lookup happens.

**Version 2 total blob size:** `11 + nameLen + (5 * capCount) + (7 * rangeCount) + nameBlobLen` bytes, where `nameBlobLen` is whatever is left once the fixed-size sections are accounted for.

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

- **Discrete range slots (v2)**: centre = `dmxFrom + (dmxTo - dmxFrom) / 2` using integer division (floors). E.g. `[32, 63]` -> `32 + 31/2 = 32 + 15 = 47`, not 48.

- **Continuous ranges (v2)**: `dmxFrom + round(clamp01(value01) * (dmxTo - dmxFrom))`, same round-half-away-from-zero convention as 8-bit channels. E.g. `[64, 95]`, `value01 = 0.5` -> `64 + round(0.5 * 31) = 64 + round(15.5) = 64 + 16 = 80`.

## Worked Example: "Torrent" 16-Channel Fixture (v1)

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

## Worked Example: "Lyre" Moving Head With Function Ranges (v2)

A minimal moving head with an 8-bit colour wheel (channel 0) carved into 3 discrete slots, and a shutter/strobe channel (channel 1) with a discrete "closed" slot and a continuous "strobe" sub-range. No fixture name.

### Fixture Definition

- Footprint: 2 channels
- Capabilities:
  - ColorWheel (8-bit) at channel 0, capIndex 0
    - SLOT 0-9 "open", SLOT 10-19 "red", SLOT 20-29 "blue"
  - ShutterStrobe (8-bit) at channel 1, capIndex 1
    - SLOT 0-31 "closed", RANGE 32-63 "strobe" (continuous)

### Encoded Blob (Hexadecimal)

```
50 46 58 31                          // "PFX1" (magic)
02                                   // version = 2
00                                   // flags = 0
02                                   // footprint = 2
02                                   // capCount = 2
00                                   // nameLen = 0
05 00                                // rangeCount = 5 (uint16 LE)
                                      // name: (empty, nameLen=0)

// Capability records (2 × 5 bytes = 10 bytes)
12 00 ff 00 00                       // ColorWheel: type=18, coarse=0, fine=0xFF, default=0, flags=0
0c 01 ff 00 00                       // ShutterStrobe: type=12, coarse=1, fine=0xFF, default=0, flags=0

// Range records (5 × 7 bytes = 35 bytes)
00 00 09 00 00 00 00                 // capIndex=0, [0,9],   discrete, nameOff=0  ("open")
00 0a 13 00 05 00 00                 // capIndex=0, [10,19], discrete, nameOff=5  ("red")
00 14 1d 00 09 00 00                 // capIndex=0, [20,29], discrete, nameOff=9  ("blue")
01 00 1f 00 0e 00 00                 // capIndex=1, [0,31],  discrete, nameOff=14 ("closed")
01 20 3f 01 15 00 00                 // capIndex=1, [32,63], continuous, nameOff=21 ("strobe")

// Name blob (28 bytes, NUL-separated)
6f 70 65 6e 00                       // "open\0"    (offset 0)
72 65 64 00                          // "red\0"     (offset 5)
62 6c 75 65 00                       // "blue\0"    (offset 9)
63 6c 6f 73 65 64 00                 // "closed\0"  (offset 14)
73 74 72 6f 62 65 00                 // "strobe\0"  (offset 21)
```

**Total size:** 11 (header) + 0 (name) + 10 (caps) + 35 (ranges) + 28 (name blob) = 84 bytes

## Validation Rules

The parser (`parseProfile`) enforces these strict rules for both versions:

1. Buffer must be at least 9 bytes (magic + version + flags + footprint + capCount + nameLen).
2. Magic must be `"PFX1"` (0x50 0x46 0x58 0x31).
3. Version must be 1 or 2.
4. Flags must be 0 (reserved).
5. capCount must be ≤ MAX_CAPS (24).
6. Buffer must be at least the declared total size for that version (see above).
7. For each capability:
   - `coarseOffset` must be < footprint.
   - `fineOffset` must be 0xFF, OR must be < footprint.

Version 2 additionally enforces:

8. Buffer must be at least 11 bytes before `rangeCount` is read.
9. `rangeCount` must be ≤ MAX_RANGES (192).
10. The trailing name blob must fit within MAX_RANGE_NAME_BLOB (2048 bytes).
11. For each range record:
    - `capIndex` must be < capCount.
    - `dmxFrom` must be ≤ `dmxTo`.
    - `nameOff` must be `0xFFFF`, or a valid offset within the name blob at which a NUL terminator exists before the blob's end.

If any rule is violated, the parser returns `false` and does not modify the output structure. The parser is a security boundary and never reads out of bounds.

## Notes

- v2 (function ranges) is implemented. See "Function Ranges (v2)" above and `applyRangeByName`/`applyRangeByIndex`/`rangeCount`/`rangeName`/`rangeIsContinuous` in `fixture_profile.h`.
- The `semantic` byte on each v2 range record is reserved for a future "point this at the nearest red" colour-wheel-to-RGB mapping layer -- v3 scope, unused today.
- The name field is optional and is never used by the apply functions; it is for debugging and tooling only. The same is true of range names for `applyRangeByIndex` (only `applyRangeByName` reads them); both are read by introspection (`rangeName`, Lua's `glow.ranges`).
- Inverted flags and default values are stored per-capability to support fixtures with varying conventions.

# MDF1 Controller Definition Format

## Overview

MDF1 is the MIDI twin of PFX1: a compact binary format describing MIDI
*hardware* -- which pads/faders/encoders exist, at what note/CC number, and
how each one's LED is driven. It deliberately encodes no cue/scene bindings
("note 60 -> cue chorus"); that mapping is show-specific and belongs in
Fennel (`glow.bind.*`, live-editable), the same split `.fdef`/`.show`
already draws between fixture hardware and the show that drives it.

Without an MDF1 profile, LED feedback is impossible -- you cannot light a
pad if you don't know it exists, what note addresses it, or how its LED is
driven. Bindings alone (`glow.bind.pad`/`glow.bind.fader`) need no format at
all: they address a note/CC number directly, exactly like `parseMidi`
already does (`live_control.h`).

The on-device runtime type is `MidiControllerProfile` (`mdef.h`): fixed-size
arrays, no heap, copied by value into `LoadedShow::controllers`
(`show_bundle.h`) -- the same discipline as `FixtureProfile`. The text
grammar (`.mdef`, see `provision.h`) and its encoder (`controller_encoder.h`'s
`ControllerBuilder`) are host-tool-only and never linked into firmware.

## Format Specification

Little-endian throughout, like PFX1.

### Header (13 bytes + name)

| Offset | Size | Name         | Type    | Description |
|--------|------|--------------|---------|-------------|
| 0      | 4    | magic        | uint8[] | ASCII "MDF1" |
| 4      | 1    | version      | uint8   | Format version: 1 |
| 5      | 1    | flags        | uint8   | Reserved, must be 0 |
| 6      | 1    | midiChannel  | uint8   | 0 = any (0..16); parseMidi already ignores channel |
| 7      | 1    | nameLen      | uint8   | Length of controller name (0 allowed) |
| 8      | 1    | padCount     | uint8   | 0..MDEF_MAX_PADS=32 |
| 9      | 1    | faderCount   | uint8   | 0..MDEF_MAX_FADERS=16 |
| 10     | 1    | encoderCount | uint8   | 0..MDEF_MAX_ENCODERS=16 |
| 11     | 1    | ledCount     | uint8   | 0..MDEF_MAX_LED_RANGES=8 |
| 12     | 1    | colorCount   | uint8   | Total COLOR entries across every LED range, 0..MDEF_MAX_COLORS=96 |
| 13     | nameLen | name      | uint8[] | UTF-8 controller name, NOT null-terminated. Parsed over but never stored in `MidiControllerProfile` (debugging/tooling only, same convention as PFX1's fixture name) |

### Pad Records (2 bytes each, padCount entries)

| Byte | Type  | Name     | Description |
|------|-------|----------|-------------|
| 0    | uint8 | noteFrom | 0..127 |
| 1    | uint8 | noteTo   | 0..127, inclusive, >= noteFrom. A single pad has noteFrom == noteTo |

### Fader Records (4 bytes each, faderCount entries)

| Byte | Type   | Name    | Description |
|------|--------|---------|-------------|
| 0    | uint8  | ccFrom  | 0..127 |
| 1    | uint8  | ccTo    | 0..127, inclusive, >= ccFrom |
| 2-3  | uint16 | nameOff | Offset into the trailing name blob, or 0xFFFF if unnamed |

### Encoder Records (3 bytes each, encoderCount entries)

| Byte | Type  | Name   | Description |
|------|-------|--------|-------------|
| 0    | uint8 | ccFrom | 0..127 |
| 1    | uint8 | ccTo   | 0..127, inclusive, >= ccFrom |
| 2    | uint8 | mode   | 0 = absolute, 1 = relative-2c (two's complement), 2 = relative-signmag (sign+magnitude) |

Relative encoders send deltas, not positions, and the encoding is
vendor-specific -- see `decodeEncoderDelta` (`mdef.h`/`mdef.cpp`). `absolute`
is the safe default when unsure: it degrades to a plain fader instead of
jumping wildly or running backwards.

### LED Records (7 bytes each, ledCount entries)

| Byte | Type   | Name        | Description |
|------|--------|-------------|-------------|
| 0    | uint8  | msgType     | 0 = Note (note-on), 1 = Cc (control change) |
| 1    | uint8  | addrFrom    | Note or CC number, 0..127 |
| 2    | uint8  | addrTo      | Note or CC number, 0..127, inclusive, >= addrFrom |
| 3    | uint8  | semantic    | 0 = velocity (data byte selects a colour from the palette below), 1 = value (data byte is a raw level, e.g. an LED ring) |
| 4-5  | uint16 | colorOffset | Index into the profile-wide colour table (below) where this range's palette starts |
| 6    | uint8  | colorCount  | Number of colour entries in this range's palette |

An LED record declares how a block of pads/faders (typically the same
address range as a preceding PAD/FADER record, though this isn't enforced)
lights up. `colorOffset`/`colorCount` slice into the global colour table in
declaration order -- each LED range owns a contiguous, non-overlapping
slice.

### Colour Records (3 bytes each, colorCount entries, profile-wide)

| Byte | Type   | Name    | Description |
|------|--------|---------|-------------|
| 0-1  | uint16 | nameOff | Offset into the trailing name blob; always present (COLOR always names its colour) |
| 2    | uint8  | value   | MIDI data byte (0..127) that selects this colour when sent as the LED message's velocity/value byte |

### Name Blob

Immediately after the colour records: the rest of the blob is a UTF-8,
NUL-separated name blob (fader names and colour names share it, in
declaration order, undeduplicated -- same convention as PFX2's range name
blob). Its length is not stored explicitly -- it is every remaining byte to
the end of the buffer.

**Total blob size:** `13 + nameLen + 2*padCount + 4*faderCount + 3*encoderCount + 7*ledCount + 3*colorCount + nameBlobLen` bytes.

## Validation Rules

`parseMidiController` enforces (same strict, never-reads-out-of-bounds
security-boundary contract as `parseProfile`):

1. Buffer at least 13 bytes; magic "MDF1"; version 1; flags 0; midiChannel <= 16.
2. Each of padCount/faderCount/encoderCount/ledCount/colorCount within its MDEF_MAX_*.
3. Buffer at least the declared header + table sizes.
4. Every pad/fader/encoder/LED `addrFrom`/`ccFrom` <= its `addrTo`/`ccTo`, both <= 127.
5. Encoder `mode` in {0,1,2}; LED `msgType` in {0,1}; LED `semantic` in {0,1}.
6. LED `colorOffset`/`colorCount` fits within the profile-wide colour table.
7. Every fader (if not 0xFFFF) and colour `nameOff` lands on a NUL-terminated string within the trailing name blob.

## `.mdef` Text Grammar

Compiled by `provision.cpp`'s `parseControllerDef`/`encodeController` (see
`provision.h`), the same two-step "text -> builder struct -> binary blob"
pipeline as `.fdef`/`ProfileBuilder`:

```
CONTROLLER Akai APC40 mkII
MIDI_CHANNEL 1                  # 0 = any (default)
PAD  53 92                      # a contiguous block of pads: note 53..92
PAD  0                          # a single pad at note 0
FADER CC 48 55                  # faders on CC 48..55
FADER CC 7   master             # a named single fader
ENCODER CC 16 23                # relative encoders default to absolute
ENCODER CC 16 23 relative-2c    # or: relative-signmag | absolute
LED NOTE 53 92 velocity         # pads 53..92: LED colour = note-on velocity
  COLOR off    0
  COLOR green  1
  COLOR red    3
LED CC 48 55 value              # fader LED rings driven by CC value 0..127
```

A `.show` file embeds one with `CONTROLLER <deffile>` (mirrors `FIXTURE
<deffile> <universe> <base>`), folding the compiled MDF1 blob into the SHW1
bundle's v2 controller table -- see the "SHW1 bundle format" section
(`show_bundle.h`).

## Out of Scope

- Binding pads/faders/encoders to cues/scenes -- that's `glow.bind.*`
  (Fennel, live-editable, no file format; see README_LIVE_CONTROL.md).
- Bidirectional display feedback (text on a controller's own LCD).
- MIDI 2.0 / MPE.
