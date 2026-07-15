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

`.mdef` also carries an optional **INIT SYSEX** blob table (v3, below): a
controller like the Akai APC40 needs a SysEx handshake sent on connect
before its pads/LEDs behave the way the rest of this file assumes (its
"Generic Mode" mode-set message -- see "INIT Blob Table (v3)"). The
firmware never interprets these bytes, only sends them -- this is what
keeps a controller's own init handshake a `.mdef` fact instead of a C++
special case (`controller_init.h`).

The on-device runtime type is `MidiControllerProfile` (`mdef.h`): fixed-size
arrays, no heap, copied by value into `LoadedShow::controllers`
(`show_bundle.h`) -- the same discipline as `FixtureProfile`. The text
grammar (`.mdef`, see `provision.h`) and its encoder (`controller_encoder.h`'s
`ControllerBuilder`) are host-tool-only and never linked into firmware.

The format has three versions, all sharing the `"MDF1"` magic (the version
byte, not the magic, is what changes) -- the same convention PFX1/PFX2 use:

- **Version 1**: every PAD/FADER/LED range is channel-agnostic -- the
  note/CC number alone addresses it, and `parseMidi`'s `ControlEvent::channel`
  is reported but never consulted for binding/LED lookups. Still fully
  supported -- every `.mdef` written before version 2 existed parses
  byte-identically.
- **Version 2**: adds an optional channel range (`channelFrom`/`channelTo`,
  0..15) to each PAD/FADER/LED record, so one note/CC number can be
  multiplexed across several MIDI channels to address several distinct
  physical controls -- e.g. the APC40's clip-launch grid, which shares 5
  note values across 8 channels/tracks instead of using 40 distinct notes.
  See "Per-Range Channel Significance (v2)" below.
- **Version 3**: adds an optional init-blob table (`INIT SYSEX` lines,
  P1.1) -- zero or more opaque outbound MIDI messages sent, in order, once
  the controller's MIDI-out path is up. v3 always uses v2's wider
  (channel-carrying) PAD/FADER/LED records too, even if no range is
  actually channel-significant -- v3 is additive on top of v2's record
  shape, not a parallel format, so the parser only ever needs one
  "wide vs. narrow records" branch, not a version-by-version matrix. See
  "INIT Blob Table (v3)" below.

`parseMidiController` accepts all three versions. `ControllerBuilder::encode`
emits version 1 bytes -- byte-identical to the original format -- for any
controller with no `CH` ranges and no `INIT` lines; version 2 once at least
one range has a `CH`; version 3 once at least one `INIT SYSEX` line exists
(regardless of whether any `CH` range is also present).

## Format Specification

Little-endian throughout, like PFX1.

### Header (13 bytes, +1 for v3, + name)

| Offset | Size | Name         | Type    | Description |
|--------|------|--------------|---------|-------------|
| 0      | 4    | magic        | uint8[] | ASCII "MDF1" |
| 4      | 1    | version      | uint8   | Format version: 1, 2, or 3 |
| 5      | 1    | flags        | uint8   | Reserved, must be 0 |
| 6      | 1    | midiChannel  | uint8   | 0 = any/unset, 1..16 = a fixed channel (1-indexed). Used only as `LedFeedback`'s default outgoing LED channel when the matched LED range is NOT itself channel-significant (`led_feedback.cpp`) -- it does not filter `parseMidi`'s input, which reports every message's channel regardless of this field (see "Per-Range Channel Significance" below for the 0-indexed field that actually does) |
| 7      | 1    | nameLen      | uint8   | Length of controller name (0 allowed) |
| 8      | 1    | padCount     | uint8   | 0..MDEF_MAX_PADS=32 |
| 9      | 1    | faderCount   | uint8   | 0..MDEF_MAX_FADERS=16 |
| 10     | 1    | encoderCount | uint8   | 0..MDEF_MAX_ENCODERS=16 |
| 11     | 1    | ledCount     | uint8   | 0..MDEF_MAX_LED_RANGES=8 |
| 12     | 1    | colorCount   | uint8   | Total COLOR entries across every LED range, 0..MDEF_MAX_COLORS=96 |
| 13     | 1    | initCount    | uint8   | **v3 only.** Number of `INIT SYSEX` blobs, 0..MDEF_MAX_INIT_BLOBS=8 -- inserted here, same convention as PFX2's `rangeCount` insertion before its name field. Absent entirely in v1/v2 (`name` starts at offset 13 instead) |
| 13 (v1/v2) / 14 (v3) | nameLen | name | uint8[] | UTF-8 controller name, NOT null-terminated. Parsed over but never stored in `MidiControllerProfile` (debugging/tooling only, same convention as PFX1's fixture name) |

### Pad Records (padCount entries; 2 bytes each in v1, 4 in v2/v3)

| Byte | Type  | Name     | Description |
|------|-------|----------|-------------|
| 0    | uint8 | noteFrom | 0..127 |
| 1    | uint8 | noteTo   | 0..127, inclusive, >= noteFrom. A single pad has noteFrom == noteTo |
| 2    | uint8 | channelFrom | **v2/v3 only.** 0..15, or `kChannelAgnostic` (0xFF) -- see "Per-Range Channel Significance" |
| 3    | uint8 | channelTo   | **v2/v3 only.** 0..15 inclusive (>= channelFrom), or 0xFF -- must match channelFrom's agnostic-ness |

### Fader Records (faderCount entries; 4 bytes each in v1, 6 in v2/v3)

| Byte | Type   | Name    | Description |
|------|--------|---------|-------------|
| 0    | uint8  | ccFrom  | 0..127 |
| 1    | uint8  | ccTo    | 0..127, inclusive, >= ccFrom |
| 2-3  | uint16 | nameOff | Offset into the trailing name blob, or 0xFFFF if unnamed |
| 4    | uint8  | channelFrom | **v2/v3 only.** Same convention as the pad record's |
| 5    | uint8  | channelTo   | **v2/v3 only.** |

### Encoder Records (3 bytes each, encoderCount entries)

| Byte | Type  | Name   | Description |
|------|-------|--------|-------------|
| 0    | uint8 | ccFrom | 0..127 |
| 1    | uint8 | ccTo   | 0..127, inclusive, >= ccFrom |
| 2    | uint8 | mode   | 0 = absolute, 1 = relative-2c (two's complement), 2 = relative-signmag (sign+magnitude) |

Relative encoders send deltas, not positions, and the encoding is
vendor-specific -- see `decodeEncoderDelta` (`mdef.h`/`mdef.cpp`). `absolute`
is the safe default when unsure: it degrades to a plain fader instead of
jumping wildly or running backwards. Encoders have no channel-significance
field -- unaffected by v2.

### LED Records (ledCount entries; 7 bytes each in v1, 9 in v2/v3)

| Byte | Type   | Name        | Description |
|------|--------|-------------|-------------|
| 0    | uint8  | msgType     | 0 = Note (note-on), 1 = Cc (control change) |
| 1    | uint8  | addrFrom    | Note or CC number, 0..127 |
| 2    | uint8  | addrTo      | Note or CC number, 0..127, inclusive, >= addrFrom |
| 3    | uint8  | semantic    | 0 = velocity (data byte selects a colour from the palette below), 1 = value (data byte is a raw level, e.g. an LED ring) |
| 4-5  | uint16 | colorOffset | Index into the profile-wide colour table (below) where this range's palette starts |
| 6    | uint8  | colorCount  | Number of colour entries in this range's palette |
| 7    | uint8  | channelFrom | **v2/v3 only.** Same convention as the pad record's -- the LED-OUTPUT side of channel significance, independent of the PAD/FADER range's own flag (see below) |
| 8    | uint8  | channelTo   | **v2/v3 only.** |

An LED record declares how a block of pads/faders (typically the same
address range as a preceding PAD/FADER record, though this isn't enforced)
lights up. `colorOffset`/`colorCount` slice into the global colour table in
declaration order -- each LED range owns a contiguous, non-overlapping
slice.

### Colour Records (3 bytes each, colorCount entries, profile-wide)

| Byte | Type   | Name    | Description |
|------|--------|---------|-------------|
| 0-1  | uint16 | nameOff | Offset into the trailing name blob; always present (COLOR always names its colour) |
| 2    | uint8   | value   | MIDI data byte (0..127) that selects this colour when sent as the LED message's velocity/value byte |

### INIT Blob Table (v3)

Immediately after the colour records, `initCount` entries (v3 only; absent
entirely in v1/v2):

| Byte | Type  | Name | Description |
|------|-------|------|-------------|
| 0    | uint8 | len  | Number of bytes in this blob, 0..MDEF_MAX_INIT_BLOB_BYTES=64 |
| 1..len | uint8[] | data | Opaque bytes -- never interpreted by `parseMidiController`, `sendControllerInit` (`controller_init.h`), or anything else on-device. Typically a complete `F0...F7` SysEx frame, but the format does not require or check that framing |

Entries are sent in declaration order, on connect -- see "INIT SYSEX" under
the `.mdef` grammar below and `controller_init.h`'s `sendControllerInit`.

### Name Blob

Immediately after the colour records (v1/v2) or the init blob table (v3):
the rest of the blob is a UTF-8, NUL-separated name blob (fader names and
colour names share it, in declaration order, undeduplicated -- same
convention as PFX2's range name blob). Its length is not stored explicitly
-- it is every remaining byte to the end of the buffer.

**Total blob size:** `13 + (1 if v3) + nameLen + padRecSize*padCount + faderRecSize*faderCount + 3*encoderCount + ledRecSize*ledCount + 3*colorCount + initTableSize + nameBlobLen` bytes, where `padRecSize`/`faderRecSize`/`ledRecSize` are 2/4/7 (v1) or 4/6/9 (v2/v3), and `initTableSize` is 0 (v1/v2) or the sum of `1 + len` over every init blob (v3).

## Per-Range Channel Significance (v2)

Channel significance is a **per-range** flag, not a per-controller one:
MDF1's existing `midiChannel` header field says which channel the whole
controller nominally lives on (mostly used for LED output defaults, above)
-- it has nothing to do with whether an individual note/CC is multiplexed
across channels. Most controllers are entirely channel-agnostic (every
PAD/FADER/LED range's `channelFrom`/`channelTo` is `kChannelAgnostic`, both
fields, the v1-compatible default). A controller like the Akai APC40
multiplexes several physical controls onto shared note/CC numbers via the
channel nibble -- e.g. all 40 clip-launch pads share only 5 note numbers
(one per scene row), the channel selecting the track/column.

`channelFrom`/`channelTo` (0..15 inclusive, or both `kChannelAgnostic`/0xFF)
name that channel range on a PAD, FADER, or LED record. When set on a PAD or
FADER range, the internal binding id for an event landing in that range is
**packed**: `(channel << 8) | id` (where `id` is the note 0..127 for a pad,
or `128 + cc` for a fader -- the same `+128` offset `parseMidi` already uses
to keep note and CC ids from colliding). `LiveControl::effectiveId`
(`live_control.h`/`.cpp`) computes this packing for an incoming
`ControlEvent` by consulting the wired `MidiControllerProfile`;
`glow.bind.pad-xy` (`glow_lua_api.cpp`, via `resolvePadXY` in `mdef.h`) binds
under the same packed id, so an incoming event and its binding always agree.
A channel-agnostic range's id is never packed -- unpacked, exactly as before
v2 existed.

**⚠️ LED-output channel significance is independent of the PAD/FADER range's
own flag, and can be narrower** (this is a real hardware quirk, not
arbitrary): on the Akai APC40 family, button *input* on notes 0x30-0x49 uses
the channel nibble to select the track, but LED *output* only honours the
channel on the narrower 0x30-0x39 span -- everything else's LED lights on a
fixed channel regardless of which track's button was pressed. This is why
`MdefLedRange` carries its own `channelFrom`/`channelTo`, set independently
via the `LED ... CH <lo> <hi>` grammar line, rather than being inherited from
the PAD/FADER range it happens to overlap. `LedFeedback::set`/`setAuto`'s
channel-aware overloads (`led_feedback.h`) only honour a supplied channel
argument when the *matched LED range* is itself channel-significant;
otherwise they fall back to the ordinary `midiChannel`-derived nibble, same
as the plain (channel-agnostic) overloads.

`resolvePadXY(profile, col, row, &note, &channel)` (`mdef.h`/`mdef.cpp`) is
`glow.bind.pad-xy`'s grid resolver: it walks a profile's PAD declarations in
order, treats each **channel-significant, single-note** one (`noteFrom ==
noteTo`) as one grid row, and maps `row` to the row-th such declaration and
`col` to a channel within that row's span. This is exactly the shape
`samples/apc40.mdef` declares (5 separate `PAD <note> CH 0 7` lines, one per
scene row) -- it is a convention on top of the format, not a distinct field.

## Validation Rules

`parseMidiController` enforces (same strict, never-reads-out-of-bounds
security-boundary contract as `parseProfile`):

1. Buffer at least 13 bytes; magic "MDF1"; version 1, 2, or 3; flags 0; midiChannel <= 16.
2. Each of padCount/faderCount/encoderCount/ledCount/colorCount within its MDEF_MAX_*.
3. Buffer at least the declared header + table sizes (record sizes depend on version -- see above).
4. Every pad/fader/encoder/LED `addrFrom`/`ccFrom` <= its `addrTo`/`ccTo`, both <= 127.
5. Encoder `mode` in {0,1,2}; LED `msgType` in {0,1}; LED `semantic` in {0,1}.
6. LED `colorOffset`/`colorCount` fits within the profile-wide colour table.
7. Every fader (if not 0xFFFF) and colour `nameOff` lands on a NUL-terminated string within the trailing name blob.
8. **v2/v3 only:** every pad/fader/LED `channelFrom`/`channelTo` is either both `kChannelAgnostic` (0xFF), or both a valid 0..15 value with `channelFrom <= channelTo`. A v1 blob has no channel fields at all -- every range parses as `kChannelAgnostic`.
9. **v3 only:** `initCount` within MDEF_MAX_INIT_BLOBS; every init blob's declared `len` within MDEF_MAX_INIT_BLOB_BYTES and not running past the end of the buffer. A v1/v2 blob has no init table at all -- `initCount` parses as 0 (same no-op `sendControllerInit` gives an empty table -- see `controller_init.h`).

## `.mdef` Text Grammar

Compiled by `provision.cpp`'s `parseControllerDef`/`encodeController` (see
`provision.h`), the same two-step "text -> builder struct -> binary blob"
pipeline as `.fdef`/`ProfileBuilder`:

```
CONTROLLER Akai APC40 mkII
MIDI_CHANNEL 1                  # 0 = any (default)
PAD  53 92                      # a contiguous block of pads: note 53..92
PAD  0                          # a single pad at note 0
PAD  53 CH 0 7                  # channel-significant: one logical pad per channel 0..7 at note 53
FADER CC 48 55                  # faders on CC 48..55
FADER CC 7   master             # a named single fader
FADER CC 7   track CH 0 8       # channel-significant, named: track faders, channel per track (8 = master)
ENCODER CC 16 23                # relative encoders default to absolute
ENCODER CC 16 23 relative-2c    # or: relative-signmag | absolute
LED NOTE 53 92 velocity         # pads 53..92: LED colour = note-on velocity
  COLOR off    0
  COLOR green  1
  COLOR red    3
LED CC 48 55 value              # fader LED rings driven by CC value 0..127
LED NOTE 53 57 velocity CH 0 7  # LED-output channel significance (independent of the PAD's own CH -- see above)
  COLOR off 0
  COLOR on  1
INIT SYSEX F0 47 7F 29 60 00 04 40 00 00 00 F7  # opaque bytes, sent on connect (v3)
```

`CH <lo> <hi>` (0..15, 0-indexed -- NOT `MIDI_CHANNEL`'s 1-indexed "any"
scheme) is an optional trailing modifier on `PAD`, `FADER`, and `LED` lines;
omitting it (every `.mdef` written before it existed) keeps that range
channel-agnostic, unchanged. See "Per-Range Channel Significance (v2)"
above for what it means and `samples/apc40.mdef`/`samples/apc40-original.mdef`
for a worked, protocol-doc-cited example (the full 8-track x 5-scene
clip-launch grid).

`INIT SYSEX <hex bytes...>` (P1.1) is a top-level line, zero or more,
order preserved -- each is one complete outbound MIDI message (typically a
full `F0...F7` SysEx frame) sent verbatim, in declaration order, once the
controller's MIDI-out path comes up (`controller_init.h`'s
`sendControllerInit`, wired from `midi_uart_task`/`usb_midi_input.cpp`'s
device-connect handler). Each byte token is exactly two hex digits
(case-insensitive); the grammar validates the hex encoding and the
MDEF_MAX_INIT_BLOB_BYTES-per-line size limit, but never the message's own
framing or meaning -- that stays opaque all the way to the wire, which is
what makes one controller's mode-set handshake and another's LED-enable
message the same mechanism with different data. Omitting `INIT` (every
`.mdef` written before it existed) sends nothing, unchanged. See
`samples/apc40.mdef`/`samples/apc40-original.mdef` for the APC40's real
"Generic Mode" (Mode 0) handshake, cited against Akai's own protocol docs.

A `.show` file embeds one with `CONTROLLER <deffile>` (mirrors `FIXTURE
<deffile> <universe> <address>`), folding the compiled MDF1 blob into the SHW1
bundle's v2 controller table -- see the "SHW1 bundle format" section
(`show_bundle.h`).

**A note on addressing conventions, since this is where the two most
confusable ones meet:** `.show` text (format version `SHOW 2`) is fully
1-indexed and human-facing -- `UNIVERSE 1 ARTNET` and `FIXTURE ... 1 17`
mean "the first universe" and "the address printed on the fixture's
display." Internally (`PatchEntry`, `MatrixMap`, the `SHW1` bytes) every
universe and channel is 0-indexed, and that 0-indexed universe number is
also exactly what gets sent as the **Art-Net wire universe** (Art-Net's own
`SubUni`/`Net` addressing is 0-based) -- so `UNIVERSE 1 ARTNET` -> internal
index `0` -> wire universe `0`, `UNIVERSE 2 ARTNET` -> internal index `1` ->
wire universe `1`, and so on. See README_PROVISION.md's "`.show` Grammar"
section (Format Version / Migration, and the Art-Net wire mapping) for the
full writeup and the compiler's validation/overlap-detection rules.

## Out of Scope

- Binding pads/faders/encoders to cues/scenes -- that's `glow.bind.*`
  (Fennel, live-editable, no file format; see README_LIVE_CONTROL.md).
- Bidirectional display feedback (text on a controller's own LCD).
- MIDI 2.0 / MPE. 14-bit CC (RPN/NRPN). Running status (handled in
  `MidiByteReader`, `midi_realtime.h`, not this format). MIDI Clock/transport
  (`BeatClock`).
- **Inbound/bidirectional SysEx** (device inquiry replies, any handshake
  that reads a response back) -- `INIT SYSEX` (v3, above) is send-only, by
  design: the firmware emits the declared bytes and never waits for or
  parses a reply. This is what let the APC40's "Generic Mode" (Mode 0)
  handshake -- previously a documented follow-up flag here -- become a
  `.mdef` fact instead of a C++ special case: `samples/apc40*.mdef` now
  declare the real Mode-0 `INIT SYSEX` line (cited against Akai's own
  protocol docs), sent from `controller_init.h`'s `sendControllerInit` via
  `midi_uart_task` (DIN, at boot) or `usb_midi_input.cpp`'s device-connect
  handler (USB-MIDI hot-plug). A device inquiry response, or any other
  reply a controller might send back, is still out of scope.

# CFG1 Device Config Format

## Overview

CFG1 is a fixed-size binary blob carrying the device configuration that used to live only in `menuconfig`: WiFi SSID/password, DMX GPIOs, the status LED GPIO, the Art-Net fallback destination, and whether the USB-MIDI host transport is enabled. It's written by the browser flasher (or the device console's reconfigure page) into a raw `devcfg` data partition (`partitions.csv`, subtype `0x40`) and read once at boot -- the same "opaque blob in a raw partition, no filesystem" pattern `SHW1` already uses for the `show` partition, for the same reason: the browser can write raw bytes at a known flash offset, but can't easily construct a filesystem image, and doesn't need to for one fixed-size struct.

Unlike PFX1/MDF1 (compiled from a text grammar via `provision.cpp`), CFG1 has no text form -- it's just a fixed-field struct, encoded/decoded directly by both `device_config.cpp` (the device parser, also usable as an on-device encoder for the reconfigure page's `GET /devcfg`) and `web/shared/devcfg.js` (the browser's independent implementation, proven byte-identical against the C++ side via a committed golden blob -- see `web/shared/test-devcfg.mjs`).

**Kconfig values are defaults, not truth.** A valid `devcfg` partition overrides every compiled-in `CONFIG_GLOW_*` default; an absent or corrupt one falls back to those defaults, so a dev build driven purely by `menuconfig` (no CFG1 ever flashed) behaves exactly as it did before this format existed. See `firmware/main/Kconfig.projbuild` and `main.cpp`'s `defaultDeviceConfigFromKconfig`.

## Format Specification

The format is little-endian throughout, like every other format in this document. Kept deliberately simple and padded rather than compact -- it's read once at boot, and the `devcfg` partition (4 KB) has plenty of room beyond the 150 bytes CFG1 v1 actually uses.

### Header (8 bytes)

| Offset | Size | Name     | Type    | Description |
|--------|------|----------|---------|--------------|
| 0      | 4    | magic    | uint8[] | ASCII "CFG1" |
| 4      | 1    | version  | uint8   | Format version: 1 |
| 5      | 1    | flags    | uint8   | bit0: usbMidiHost, bit1: skipWifi. Bits 2-7 reserved, parsed over but not validated (forward-compatible) |
| 6      | 2    | reserved | uint16  | Must be 0 on encode; parsed over, not validated, on decode |

### Network (102 bytes, offset 8)

| Offset | Size | Name             | Type    | Description |
|--------|------|------------------|---------|--------------|
| 8      | 32   | wifiSsid         | uint8[] | UTF-8, NUL-padded (not necessarily NUL-terminated if it fills all 32 bytes) |
| 40     | 64   | wifiPass         | uint8[] | UTF-8, NUL-padded |
| 104    | 4    | artnetFallbackIp | uint32  | Packed host-byte-order IPv4 (same convention as the old `GLOW_ARTNET_BRIDGE_IP` Kconfig value, e.g. `192.168.1.50` => `(192<<24)\|(168<<16)\|(1<<8)\|50`). **0 = broadcast.** See "The `artnetFallbackIp` field" below for what this value means and does *not* mean. |
| 108    | 2    | artnetPort       | uint16  | Art-Net UDP port |

### Pins (4 bytes, offset 110)

| Offset | Size | Name       | Type  | Description |
|--------|------|------------|-------|--------------|
| 110    | 1    | dmxTxGpio  | uint8 | RS485 DI |
| 111    | 1    | dmxRxGpio  | uint8 | RS485 RO |
| 112    | 1    | dmxRtsGpio | uint8 | RS485 DE/RE (tied together) |
| 113    | 1    | ledGpio    | uint8 | Status LED |

### Tail (36 bytes, offset 114)

| Offset | Size | Name      | Type    | Description |
|--------|------|-----------|---------|--------------|
| 114    | 32   | reserved2 | uint8[] | Must be 0. Room for the next few options -- a future field can be added here with **no version bump needed**, since existing parsers already skip over it as padding. |
| 146    | 4    | crc32     | uint32  | CRC-32 (IEEE 802.3 / zlib / PNG: poly `0xEDB88320` reflected, init `0xFFFFFFFF`, final XOR `0xFFFFFFFF`) over every byte from offset 0 up to (not including) this field |

**Total blob size: 150 bytes** (`DEVCFG_BLOB_SIZE`).

## The `artnetFallbackIp` Field

This field's name and scope were deliberate from the start, specifically so Wave 3 (per-universe Art-Net routing) would not collide with it.

The obvious name -- `artnetIp`, "the bridge IP" -- would have collided with Wave 3, which moves per-universe Art-Net routing into the `.show` itself (a `(IP, wire-universe)` table, so different universes can go to different Art-Net nodes; see "Art-Net Wire Universe & Destination Routing" below). If this field meant "the" Art-Net destination, that meaning would have had to be walked back or overloaded the moment Wave 3 landed.

So it's defined as: **the destination used for Art-Net universes the loaded `.show` does not route explicitly.** `0` means broadcast (`255.255.255.255`). Precedence, stated once and for all:

```
explicit route in the .show   >   CFG1 artnetFallbackIp   >   broadcast
```

Now that Wave 3 has shipped, this is exactly the second tier of that precedence chain: a `.show` with no explicit Art-Net routes still behaves exactly as before (every Art-Net universe goes to `artnetFallbackIp`, or broadcasts if that's 0); a `.show` with explicit routes uses this field only for the universes it left unspecified.

## Validation Rules

`parseDeviceConfig` (`device_config.cpp`) enforces (same strict, never-reads-out-of-bounds security-boundary contract as `parseProfile`/`parseMidiController`/`loadShow`):

1. Buffer must be at least `DEVCFG_BLOB_SIZE` (150) bytes.
2. Magic must be `"CFG1"`. This alone rejects an erased/all-`0xFF` partition (the common case on a fresh, never-configured board) and a half-written one, long before any field -- a GPIO byte of `0xFF`/255, for instance -- would ever be read as a real value.
3. Version must be 1 (the only version defined today).
4. The stored `crc32` must equal the CRC computed over bytes `[0, 146)`. This catches any single flipped bit anywhere in the blob, including a magic/version that happens to survive corruption elsewhere in the payload.

If any rule is violated, the parser returns `false` and does not modify the output struct. **A bad CRC is not a partial config with some fields honored -- it's a total rejection, and the caller falls back to the compiled-in Kconfig defaults.** This is reported loudly, not silently: `main.cpp` logs both a human-readable line (serial) and, under `CONFIG_GLOW_SELFTEST`, a structured `GLOW-TEST: cfg source=devcfg|defaults ...` telemetry line the QEMU/HIL harnesses assert on. Neither line ever includes the WiFi password.

## Boot Ordering

`devcfg` is read at the very start of `app_main`, before the status LED initializes (needs `ledGpio`) and before DMX bring-up (needs the three `dmx*Gpio` fields) -- see `main.cpp`'s header comment on this. The read is local flash only (`esp_partition_read`); it never depends on, or waits for, the network. WiFi bring-up itself stays at the very end of `app_main`, unchanged from before CFG1 existed -- a WiFi stall must never block the lights.

`usbMidiHost` and `skipWifi` are runtime switches, not compile-time gates: the USB-MIDI driver and every WiFi-dependent transport are compiled in unconditionally (mirrored by `CONFIG_GLOW_USB_MIDI_HOST`, default on, purely for people who want a smaller image with the driver removed entirely), and `main.cpp` checks `cfg.usbMidiHost`/`cfg.skipWifi` at the point each would otherwise start. This is what makes both a flash-time checkbox instead of a rebuild.

## Reconfiguring Without Reflashing

The device console exposes `GET /devcfg` (returns the currently-effective config -- whichever source won at boot, devcfg or defaults) and `POST /devcfg` (validates with the exact same `parseDeviceConfig` the boot-time loader uses, writes the partition only if that succeeds, then reboots). A malformed POST is rejected with `400` before any flash write happens -- the CRC-verified fallback-to-defaults path above is the safety net for the case this can't catch (a well-formed config for the wrong network): a corrupt or merely-wrong `devcfg` still boots on Kconfig defaults next time, not a brick.

## Security

The WiFi password sits in plaintext in the `devcfg` partition, exactly like every other field -- anyone with physical access to the board and `esptool` can read it out of flash directly. This is a deliberate, documented tradeoff for a hobby ESP32 project, not an oversight: flash/NVS encryption would close it, but is out of scope here (see the README). If this project is ever adapted into a shipping product, that's the point to revisit.

## Out of Scope

- Flash/NVS encryption (see "Security" above).
- Fleet provisioning (serial numbers, device management).

Per-universe Art-Net routing (the `.show`'s own `(IP, wire-universe)` table) is Wave 3, and it *has* shipped -- see "Art-Net Wire Universe & Destination Routing" below. `artnetFallbackIp` above only ever defines the fallback tier of that routing's precedence, by design.

# Art-Net Wire Universe & Destination Routing

## Overview

A real rig usually has several Art-Net nodes, and a node usually has several DMX outputs (2/4/8 is common). Wave 1/2 conflated "internal universe index" with "Art-Net wire universe" and sent every Art-Net universe to one configured bridge IP (`artnetFallbackIp` above) -- fine for one node, impossible for two nodes that both want to use wire universe 0 (same number, different IP).

Wave 3 fixes this in three parts, all in `ArtNetSink`/`ArtNetRouter` (`artnet_router.h`/`artnet_sink.h`) and the `.show` compiler (`provision.cpp`):

1. **A destination is `(IP, wire universe)`, not just an IP.** Modeled as `ArtNetDest { uint32_t ip; uint16_t wireUniverse; }` (`show.h`) -- `ip == 0` is the "no explicit route" sentinel (falls back to `artnetFallbackIp`, or broadcast).
2. **The `.show` carries an explicit per-universe routing table** (`UNIVERSE ... ARTNET <ip> <wireUniverse>`, below) -- this is rig topology, the same class of fact as a fixture's DMX address, not a device setting.
3. **ArtSync (OpCode 0x5200)** is sent once per frame, after every Art-Net `send()`, so every node latches its outputs simultaneously instead of updating whenever its own packet happens to arrive -- without this, a pixel matrix spanning multiple universes shows visible tearing the instant it's split across more than one Art-Net destination.

## The Wire Universe: One `uint16_t`, Decomposed Net:SubNet:Universe

Art-Net's own addressing is a 15-bit "Port-Address": **Net** (7 bits) : **Sub-Net** (4 bits) : **Universe** (4 bits). This codebase models the whole 15-bit quantity as a single `uint16_t wireUniverse` in the range `0..32767` everywhere above the wire-packet layer (`ArtNetDest`, the SHW1 bundle, the `.show` grammar) -- the Net/Sub-Net/Universe split only happens at the one place a raw ArtDMX/ArtPollReply packet is actually built or parsed:

```
wireUniverse:            0brNNNNNNNSSSSUUUU   (r = reserved, always 0; 15 significant bits)
Net      (bits 14..8):   (wireUniverse >> 8) & 0x7F
SubUni byte (bits 7..0): wireUniverse & 0xFF     (= SubNet<<4 | Universe, but transmitted as one byte)
```

On the wire, an ArtDMX packet's `SubUni` field (byte 14) and `Net` field (byte 15) are exactly those two bytes (see `artnet_router.cpp`'s `buildArtDmxPacket`); an ArtPollReply's `NetSwitch`/`SubSwitch`/`SwOut[]` fields compose back into the same `wireUniverse` the same way (see "ArtPoll / ArtPollReply Discovery" below). Do not invent a different Net/SubNet split -- this is Art-Net's own documented Port-Address structure, not a project convention.

`.show` `UNIVERSE` numbers stay 1-indexed and human-facing as ever (README_PROVISION.md); the internal 0-indexed universe number is *not* the wire universe once an explicit route is given -- it's only the *default* wire universe when none is given (today's pre-Wave-3 behavior, kept for compatibility).

## `.show` Grammar: Explicit Per-Universe Routing

```
SHOW 2
UNIVERSE 1 DMX
UNIVERSE 2 ARTNET 192.168.1.50 0     # node A, wire universe 0
UNIVERSE 3 ARTNET 192.168.1.50 1     # SAME node, second DMX output -- the normal case
UNIVERSE 4 ARTNET 192.168.1.51 0     # node B, also wire universe 0 -- different IP, no conflict
UNIVERSE 5 ARTNET                    # no IP -> the CFG1 fallback, or broadcast if that's 0
```

- **A multi-output node (2/4/8 DMX ports) is the normal case, not an exception**: same IP, different wire universes on different `UNIVERSE` lines.
- **IP omitted** -> the destination is left as the `0` marker in the bundle; the device resolves it to `artnetFallbackIp`, or broadcast if that's also `0` (see the CFG1 section above for the precedence chain). Existing shows that specify no IPs keep working unchanged -- broadcast is genuinely fine for small rigs, and nothing pushes you off it.
- **Wire universe omitted, but IP given** -> defaults to the universe's own internal (0-indexed) number, and the compiler emits a non-fatal warning (`CompileResult::warnings`) -- an explicit number is almost always what's meant, but this is not an error.
- **Wire universe omitted AND IP omitted** (bare `ARTNET`) -> today's implicit behavior exactly, no warning: internal index as the wire universe, fallback/broadcast as the destination.

### Compile-Time Validation

- **Two `UNIVERSE ARTNET` lines resolving to the same `(ip, wireUniverse)`** (after defaulting) is a compile error, naming both universes -- two data streams fighting over one node output is never intentional.
- **Same IP, different wire universes** is accepted -- that's the multi-output-node case.
- **A malformed IP** (not exactly four dot-separated `0..255` octets) is a compile error naming the offending `UNIVERSE` line.
- **A wire universe outside `0..32767`** is a compile error.

## `SHW1` Bundle: Version 4

The universe table (see README_PROVISION.md's `SHW1` bundle section) grows from 1 byte/entry to 7 bytes/entry when the bundle is version 4:

```
v1/v2/v3 universe table entry (1 byte):
  transport      u8    (0=Dmx, 1=ArtNet, 2=Sacn, 3=Unused)

v4 universe table entry (7 bytes):
  transport      u8    (as above)
  destIp         u32   (packed host-byte-order IPv4; 0 = no explicit route)
  wireUniverse   u16   (0..32767; already resolved by the compiler -- either
                         what the .show said, or the internal index default)
```

Version 4 also always carries `mdefCount` (same header shape as v2 -- see README_PROVISION.md), whether or not a `CONTROLLER` was actually compiled in, mirroring the existing "only bump the version once the new feature is actually used" convention: **a `.show` with no explicit Art-Net routes compiles to byte-identical v1/v2/v3 output, never v4**, even if it uses `ARTNET` transport at all (bare `UNIVERSE N ARTNET`, no IP/wire-universe args, never bumps the version).

The loader (`loadShow`) accepts v1/v2/v3/v4 uniformly: for every universe, it first fills in today's implicit default (`destIp=0`, `wireUniverse=`the universe's own index), then -- only for a v4 bundle -- overwrites those two fields from the wire bytes. An already-flashed board's saved show, or a `.show` compiled by an older provisioner, loads with exactly its pre-Wave-3 semantics; nothing about v1/v2/v3 bytes changes.

## `ArtNetSink` / `ArtNetRouter`

Packet building, per-universe destination routing, and per-universe sequence numbering are split into a portable, host-tested module (`artnet_router.h`/`.cpp`) and a thin device-only socket wrapper (`artnet_sink.h`/`.cpp`):

```cpp
struct ArtNetDest { uint32_t ip; uint16_t wireUniverse; };   // ip == 0 -> fallback/broadcast

class ArtNetSink : public IUniverseSink {
public:
  ArtNetSink(uint16_t port, uint32_t fallbackIp);
  bool begin();
  void setDest(uint8_t universeIndex, const ArtNetDest& d);  // from the bundle, later from discovery
  void send(uint8_t universeIndex, const uint8_t* data, uint16_t len) override;
  void frameEnd();                                            // one ArtSync per frame
};
```

One unconnected UDP socket, one `sendto()` per packet -- never a socket per node; that scales to any number of destinations for free.

**Sequence numbers are per destination universe**, not global: each of the (up to 8) internal universes gets its own sequence counter, incremented only when that universe sends. Sharing one global counter across universes/destinations would make a receiving node see gaps in its own sequence whenever *another* universe's packet was sent in between -- indistinguishable from a flaky network, and a nightmare to diagnose (this is called out explicitly because it is exactly the kind of bug that looks like "the WiFi is bad" for days).

**Ordering**: `setDest()` must be called (from the loaded bundle, via `applyLoadedShow`'s `ISinkFactory::configureArtnetDest`) before the first `send()` for that universe. `main.cpp`'s boot order already guarantees this -- the bundle loads, and every universe's destination is set, before the render task starts (see `app_main`'s comments on why DMX/render come up before the network). A universe nobody ever calls `setDest` for still defaults to `{ip=0, wireUniverse=its own index}` -- never a crash, never silently dropped.

## ArtSync

Without ArtSync, a node outputs each universe as it arrives -- invisible for two independent matrices, but the moment a pixel matrix spans multiple Art-Net universes, half the panel updates a frame before the other half: visible horizontal tearing on any moving gradient. `ArtNetSink::frameEnd()` (called once per frame from the render loop, right after every Art-Net `send()` for that frame) broadcasts one ArtSync packet:

```
Offset  Size  Field
0       8     ID = "Art-Net\0"
8       2     OpCode = 0x5200, little-endian (bytes 0x00, 0x52)
10      2     ProtVerHi, ProtVerLo (big-endian; 14 today)
12      1     Aux1 (0, unused)
13      1     Aux2 (0, unused)
```

Always broadcast (`255.255.255.255`), regardless of any individual universe's own (possibly unicast) destination -- it has to reach every node, not just the ones this controller happens to unicast to.

## ArtPoll / ArtPollReply Discovery (Phase 3)

A static IP in a `.show` breaks the moment a venue's DHCP hands the node a different address. Phase 3 fixes this by discovering nodes instead of typing their IPs: the device broadcasts an ArtPoll, nodes reply with ArtPollReply (name, IP, which universes they carry), and those replies fill in exactly the same `setDest()` table Phase 1 built -- discovery is not a parallel mechanism, it's what populates the routing table when the `.show` didn't.

**Precedence holds**: an explicit `.show` route (`ArtNetDest.ip != 0`) is never overwritten by discovery -- a user who typed an address meant it. Discovery only fills in universes the `.show` left as the `ip=0` marker. A node that stops answering ArtPoll (unplugged, powered off, DHCP-relocated and re-polled under a new IP) has its universes revert to the fallback/broadcast marker after a timeout -- never left silently pointed at a now-wrong IP, and never left dark either.

### Byte Layout

Parsed against the fields actually needed (source IP, ShortName/LongName, NetSwitch/SubSwitch, NumPorts, SwOut\[\]) -- every other documented ArtPollReply field (PortTypes/GoodInput/GoodOutput/Status1/Status2/MAC/BindIp/Style/etc.) is skipped over, not guessed at, and every read is bounds-checked regardless of what the packet claims (see `artnet_discovery.cpp`'s `parseArtPollReply`).

```
ArtPollReply (OpCode 0x2100), fixed 239 bytes total:
Offset  Size  Field
0       8     ID = "Art-Net\0"
8       2     OpCode = 0x2100, little-endian
10      4     IP (raw bytes, dotted-quad order)
18      1     NetSwitch (bits 0-6)
19      1     SubSwitch (bits 0-3)
26      18    ShortName (NUL-padded ASCII)
44      64    LongName (NUL-padded ASCII)
172-173 2     NumPorts (Hi, Lo byte -- this parser only reads the Lo byte;
               every real node reports <= 4, so Hi is always 0 in practice)
190-193 4     SwOut[4] (low nibble of each byte = that output port's
               Universe field; combined with NetSwitch/SubSwitch this is the
               same Net:SubNet:Universe composition ArtDMX sends)

ArtPoll (OpCode 0x2000), 14 bytes total:
Offset  Size  Field
0       8     ID = "Art-Net\0"
8       2     OpCode = 0x2000, little-endian
10      2     ProtVerHi, ProtVerLo (big-endian; 14)
12      1     TalkToMe (0 -- no unsolicited updates requested; this device polls periodically instead)
13      1     Priority (0, unused -- no diagnostics requested)
```

Per output port `i` (`0..NumPorts-1`): `wireUniverse[i] = (NetSwitch & 0x7F) << 8 | (SubSwitch & 0x0F) << 4 | (SwOut[i] & 0x0F)`.

**Provenance note**: these byte offsets are cross-checked against a mature, widely-deployed open-source Art-Net implementation (OpenLightingProject/ola, `plugins/artnet/ArtNetPackets.h`'s `artnet_reply_s`/`artnet_poll_s`), not reconstructed from memory alone -- this codebase has already lost real time once to a hallucinated API, and Art-Net is a published spec, not something to guess at. The committed test fixture (`test_artnet_discovery.cpp`'s `makeSampleArtPollReply`) is a byte-accurate *synthetic* packet built field-by-field against that same layout, not a literal hardware capture (no Art-Net node was available to sniff from in the environment this was built in) -- swap in a real capture from an actual node during HIL bring-up if maximum fidelity matters. If you're maintaining this, treat any discrepancy an actual node's reply exposes as a bug to fix here, not evidence the node is wrong.

### Discovery Table

`ArtNetDiscovery` (`artnet_discovery.h`) tracks which nodes are alive: `onReply()` adds/refreshes a node by IP; `expire()` drops any node not heard from within its timeout (default 10s); `resolveDiscoveredDests()` is the pure precedence logic above, taking the `.show`'s own per-universe table plus the current discovery table and producing what to hand `ArtNetSink::setDest()` for every universe the `.show` left unspecified. The device-only glue (`artnet_discovery_task.h`/`.cpp`) broadcasts ArtPoll every few seconds on its own socket, feeds replies into this table, and re-applies it after every cycle.

### Web Console

`GET /artnet_nodes` (see `artnet_nodes_web.h`) returns the currently-discovered nodes as JSON (`{"nodes":[{"ip":..., "shortName":..., "longName":..., "universes":[...]}]}`), read from a thread-safe snapshot published once per poll cycle. The device console's reconfigure page lists them -- this is the screen that turns "why is nothing lighting up" into "ah, the node isn't on the network."

## Testing Reality

QEMU cannot validate any of this end-to-end -- the S3 fork has no 802.11 model (see `skipWifi`/`tests/qemu/README.md`). Everything above the socket layer (packet building, routing/sequencing/ArtSync ordering, ArtPollReply parsing, the discovery table's precedence/timeout logic) is host-tested (`test_artnet_router.cpp`, `test_artnet_discovery.cpp`, plus the `.show`/`SHW1` coverage in `test_provision.cpp`). The actual wire behavior -- real packets landing on real UDP sockets, at the real frame rate -- is a HIL-only gate (`tests/hil/test_l2_artnet.py`).

## Out of Scope

- sACN / E1.31 (a plausible future sink -- same destination-table shape, different packet).
- Art-Net input (receiving DMX from another controller) -- this project is a controller, never a node.
- ArtNzs, RDM-over-Art-Net.
- Being an Art-Net node ourselves (answering ArtPoll, not just sending it).
