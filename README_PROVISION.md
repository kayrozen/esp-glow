# Provisioning: Compiler + Loader

## Overview

Provisioning is the off-device tooling that compiles human-readable fixture and show definitions into compact binary artifacts the ESP32-S3 firmware loads at boot:

- **Compiler** (`provision.{h,cpp}`): Runs on the PC/host. Parses `.fdef` (fixture type definitions) and `.show` (patch specifications) text files, produces a `SHW1` binary show bundle.
- **Loader** (`show_bundle.{h,cpp}`): Portable, bounds-checked, runs on the device at boot and in host tests. Reads a `SHW1` bundle into in-memory `LoadedShow` structures.

Both compile clean under `g++ -std=c++17 -Wall -Wextra -Werror`.

## `.fdef` Grammar (Fixture Definition)

One fixture type per file. Lines only; `#` starts a comment; blank lines ignored. Tokens are whitespace-separated.

```
FIXTURE  <name...>              # rest of line is the name (can include spaces)
FOOTPRINT <n>                   # 1..255, required. Total DMX footprint of this fixture type.
HEAD                            # optional flag: this is a moving head
PANRANGE  <deg>                 # head only, e.g. 540. Total mechanical pan sweep in degrees.
TILTRANGE <deg>                 # head only, e.g. 270. Total mechanical tilt sweep in degrees.
CAP <Name> <coarse> [<fine>|-] [<default>] [inv]
```

### CAP Line Details

- `<Name>`: Capability name (exact string match). Valid names: `Dimmer`, `Red`, `Green`, `Blue`, `White`, `Amber`, `Uv`, `Cyan`, `Magenta`, `Yellow`, `Pan`, `Tilt`, `ShutterStrobe`, `Gobo`, `Focus`, `Zoom`, `Fog`, `Fan`, `Generic`.
- `<coarse>`: Required, 0..255. The coarse (MSB) DMX channel offset within the fixture's footprint.
- `<fine>`: Optional, int or `-`. If `-` or omitted, the channel is 8-bit only (no fine channel). If an int, it's the fine (LSB) channel offset.
- `<default>`: Optional, 0..255. The default DMX value for this channel (used at boot to set idle state). Defaults to 0.
- `inv`: Optional flag. If present, the channel value is inverted after scaling.

### Example `.fdef`

```
FIXTURE Torrent F1
FOOTPRINT 16
HEAD
PANRANGE 540
TILTRANGE 270
CAP Dimmer 0
CAP Red 1
CAP Green 2
CAP Blue 3
CAP Pan 5 6
CAP Tilt 7 8
CAP ShutterStrobe 10 - 8 inv
```

## `.show` Grammar (Patch / Show Definition)

Describes the universe/transport setup, fixture placements (patch), and pixel matrix layouts.

```
UNIVERSE <idx> <DMX|ARTNET|SACN>   # Sets transport for universe idx (0..7).
FIXTURE  <deffile> <universe> <base> # Patch an instance of a fixture type.
POS      <x> <y> <z>               # head only: world position (meters, float).
ROT      <yaw> <pitch> <roll>      # head only: degrees. Euler angles for fixture orientation.
CENTER   <panNorm> <tiltNorm>      # head only, optional (default 0.5 0.5). Pan/tilt DMX centers.
INVERT   <0|1> <0|1>               # head only, optional (default 0 0). Pan/tilt invert flags.
MATRIX   <startU> <startCh> <w> <h> <SERP|PROG> <H|V> <ORDER>
         # Pixel matrix: starts at universe startU, channel startCh.
         # w × h pixels. SERP = serpentine scan, PROG = progressive.
         # H = horizontal layout, V = vertical.
         # ORDER in {RGB, GRB, BRG, RBG, GBR, BGR}.
```

### Show Semantics

- **UNIVERSE**: Optional per universe; if omitted, defaults to `Unused`.
- **FIXTURE**: Loads `<deffile>` (resolved via filesystem callback), instances it at `<universe>:<base>`.
- **POS/ROT/CENTER/INVERT**: Modify the most recent `FIXTURE` line. Error if the fixture is not a head.
- **universeCount** = (max universe index referenced) + 1 ≤ 8.

### Example `.show`

```
UNIVERSE 0 DMX
UNIVERSE 1 ARTNET

# Torrent moving head at (1, 2, 3) in world space, pan/tilt at identity.
FIXTURE torrent.fdef 0 1
POS 1 2 3
ROT 0 0 0

# Par fixture, plain dimmer + RGB
FIXTURE par.fdef 0 20

# 16×16 LED matrix on universe 1, starting at channel 0
MATRIX 1 0 16 16 SERP H GRB
```

## `SHW1` Binary Bundle Format

Little-endian encoding. Assumes all platforms (x86-64 host and ESP32-S3 Xtensa device) are little-endian. Floating-point numbers are IEEE-754 32-bit (native byte order). Document any assumption changes.

### Layout

```
Header (11 bytes):
  magic         4 bytes       "SHW1"
  version       u8            = 1
  universeCount u8            (0..8)
  profileCount  u16           count of unique PFX1 profiles
  fixtureCount  u16           count of fixtures in patch
  matrixCount   u16           count of pixel matrices

Universe Table (universeCount entries, 1 byte each):
  transport[i]  u8            0=Dmx, 1=ArtNet, 2=Sacn, 3=Unused

Profile Table (profileCount entries):
  blobLen       u16           length of this PFX1 blob (bytes)
  blob[blobLen] u8[blobLen]   PFX1 fixture profile blob

Fixture Table (fixtureCount entries, 46 bytes fixed per entry):
  profileIndex  u16           index into profile table
  universe      u8            (0..7)
  base          u16           start channel within universe
  isHead        u8            (0=false, 1=true)
  [if isHead:]
    posX        f32           world x (meters)
    posY        f32           world y (meters)
    posZ        f32           world z (meters)
    yaw         f32           degrees, rotation about Y
    pitch       f32           degrees, rotation about X
    roll        f32           degrees, rotation about Z
    panRangeDeg f32           total pan sweep (degrees)
    tiltRangeDeg f32          total tilt sweep (degrees)
    panCenterNorm  f32        pan DMX center (0..1)
    tiltCenterNorm f32        tilt DMX center (0..1)
    invertPan   u8            (0=false, 1=true)
    invertTilt  u8            (0=false, 1=true)
  [if not head, all fields above are zero]

Matrix Table (matrixCount entries):
  width         u16           pixels wide
  height        u16           pixels tall
  serpentine    u8            (0=progressive, 1=serpentine)
  vertical      u8            (0=horizontal, 1=vertical)
  order         u8            ColorOrder enum (0=RGB, 1=GRB, etc.)
  startUniverse u8            (0..7)
  startChannel  u16           start channel within startUniverse
```

### Worked Example

A show with 1 universe (Dmx), 1 fixture (5-channel par, footprint=5), 0 matrices:

```
Offset  Data                              Description
------  ----                              -----------
0-3     "SHW1"                            magic
4       0x01                              version
5       0x01                              universeCount = 1
6-7     0x0100 (little-endian)            profileCount = 1
8-9     0x0100                            fixtureCount = 1
10-11   0x0000                            matrixCount = 0

12      0x00                              transport[0] = Dmx

13-14   <len> (e.g. 0x1200 = 18)          PFX1 blob length
15+     [18 bytes of PFX1 blob]           PFX1 profile for the par

33      0x0000                            profileIndex = 0
35      0x00                              universe = 0
36-37   0x0000                            base = 0
38      0x00                              isHead = 0
39-82   [44 bytes of zeros]               (head fields zeroed for non-head)
```

### Byte-Ordering Guarantees

- Multi-byte integers are written/read in native little-endian order using `memcpy`.
- IEEE-754 32-bit floats are written/read in native byte order via `memcpy`.
- All host and target platforms are little-endian; if this ever changes, update the read/write functions.

## Profile Deduplication

If two `FIXTURE` lines in the `.show` reference the same `.fdef` file, only one entry is written to the profile table, and both fixtures reference the same `profileIndex`.

## Loader Security & Strictness

The loader (`loadShow`) is the security boundary for device-side loading:

- **Never reads out of bounds**: All array accesses are bounds-checked before dereferencing.
- **Rejects malformed input**:
  - Bad magic or version != 1 → false
  - Truncated bundle (size too small) → false
  - profileIndex out of range → false
  - Any PFX1 blob that fails `parseProfile` → false
- **No exceptions**: Portable, uses return values for errors.
- Testable under `-fsanitize=address` to detect OOB reads.

## Future: GDTF / QLC+ Importers

This agent implements **no** GDTF, QLC+ (.qxf), or Open Fixture Library parsing. Those are complex (ZIP + XML) and belong in separate PC-side tools that:

1. Read GDTF/QLC+/OFL files using proper libraries (libzip, libxml2).
2. Emit `.fdef` and `.show` text files in the formats described above.
3. Run provisioning on the emitted text.

The `.fdef` and `.show` text formats are the stable seam — all importers target them, and all firmware loads from `SHW1` bundles compiled from them. This separation keeps the device-side loader simple and the text formats forward-compatible.

## API

### Compiler (Host-Only)

```cpp
// Parse a .fdef file
bool parseFixtureDef(const std::string& text, FixtureDef& out, std::string& err);

// Map capability name to enum
bool capFromName(const std::string& name, Capability& out);

// Encode a FixtureDef to a PFX1 blob
std::vector<uint8_t> encodeProfile(const FixtureDef& def);

// Compile a .show to a SHW1 bundle
CompileResult compileShow(const std::string& showText,
                          const std::function<std::string(const std::string&)>& readFile);
```

### Loader (Portable)

```cpp
struct LoadedShow {
  uint8_t universeCount;
  UniverseTransport transport[8];
  std::vector<PatchEntry> fixtures;
  std::vector<MatrixMap> matrices;
};

// Load a SHW1 bundle
bool loadShow(const uint8_t* data, size_t len, LoadedShow& out);
```

## CLI

Optional: `provision_main.cpp` wraps the compiler in a thin CLI:

```bash
provision <show.show> <output.shw1>
```

Reads `<show.show>`, resolves `.fdef` files from the filesystem, and writes the compiled bundle to `<output.shw1>`.

## Testing

Run tests with:

```bash
make test_provision
```

Tests cover:
- `.fdef` parsing (valid/invalid inputs, missing fields, malformed values).
- `capFromName` mapping (valid/invalid names).
- `.show` compilation (round-trip with `loadShow`).
- Profile deduplication.
- Error cases (POS on non-head, etc.).
- Loader strictness (bad magic, truncation, OOB checks).
