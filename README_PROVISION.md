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

**Format version 2 (current): fully 1-indexed and human-facing.** Every universe number and
DMX address you write is the number printed on the fixture's own display or the console —
never a memory offset. A `.show` must declare this explicitly with a `SHOW 2` header as its
first non-comment line; see "Format Version / Migration" below.

```
SHOW 2                              # REQUIRED first non-comment line.
UNIVERSE <idx> <DMX|ARTNET|SACN> [<ip>] [<wireUniverse>]
                                    # Sets transport for universe idx (1..8, 1-indexed).
                                    # <ip>/<wireUniverse> are ARTNET-only, both optional --
                                    # see "Universes -- the Art-Net Wire Mapping" below.
FIXTURE  <deffile> <universe> <address> # Patch an instance of a fixture type.
                                    # <universe> 1-indexed (as above). <address> is the
                                    # 1-indexed DMX address (1..512) printed on the fixture.
POS      <x> <y> <z>               # head only: world position (meters, float).
ROT      <yaw> <pitch> <roll>      # head only: degrees. Euler angles for fixture orientation.
CENTER   <panNorm> <tiltNorm>      # head only, optional (default 0.5 0.5). Pan/tilt DMX centers.
INVERT   <0|1> <0|1>               # head only, optional (default 0 0). Pan/tilt invert flags.
MATRIX   <startU> <startAddr> <w> <h> <SERP|PROG> <H|V> <ORDER>
         # Pixel matrix: starts at universe startU, DMX address startAddr (both 1-indexed,
         # same convention as UNIVERSE/FIXTURE above).
         # w × h pixels. SERP = serpentine scan, PROG = progressive.
         # H = horizontal layout, V = vertical.
         # ORDER in {RGB, GRB, BRG, RBG, GBR, BGR}.
```

### Show Semantics

- **UNIVERSE**: Optional per universe; if omitted, defaults to `Unused`.
- **FIXTURE**: Loads `<deffile>` (resolved via filesystem callback), instances it at
  `<universe>:<address>`. The compiler converts `<universe>` and `<address>` to the internal
  0-indexed representation (`universe-1`, `address-1`) exactly once, here — `PatchEntry.base`
  and everything downstream stays 0-indexed.
- **POS/ROT/CENTER/INVERT**: Modify the most recent `FIXTURE` line. Error if the fixture is not a head.
- **universeCount** = (max internal universe index referenced) + 1 ≤ 8.
- **Validation**: universe `< 1` or `> 8` errors ("universes are 1-indexed" / "out of range");
  address `< 1` or `> 512` errors ("DMX addresses are 1..512"); a *fixture* whose
  `address + footprint - 1 > 512` errors ("runs past the end of universe N") — a fixture never
  spans universes. A `MATRIX`'s `w × h × 3` channels are allowed to run past 512 and roll over
  into the next universe(s) (`startUniverse + 1`, `+2`, ...), matching what `PixelMatrix`
  (`pixel_matrix.cpp`) actually does on-device; there's no "past the end" error for matrices.
- **Overlap detection**: after parsing, the compiler builds each universe's occupied-channel
  set from every fixture's `(address, footprint)` and every matrix's `(address, w×h×3)` —
  splitting a matrix's range across each universe it spans — and reports **every** colliding
  pair, naming both fixtures/matrices and the exact overlapping channel range. This is a
  compile-time check for the most common real-world patching mistake — two things fighting
  over the same channels — and does not change the binary output.
- **Gap warning**: if a patched universe has unused trailing channels (e.g. only channels
  1..120 of 512 are occupied), the compiler returns a non-fatal warning
  (`CompileResult::warnings`), not an error.

### Universes — the Art-Net Wire Mapping

`.show` universe numbers are 1-based (`U1`, `U2`, `U3`, ... — what a lighting person says out
loud). The compiler converts to a 0-based **internal** universe index (`UNIVERSE 1` → index 0,
`UNIVERSE 2` → index 1, ...), and that 0-based index is what's stored in the `SHW1` bundle and
used everywhere on-device (`PatchEntry.universe`, `MatrixMap.startUniverse`).

**Art-Net universes are 0-based on the wire** (Art-Net's own `SubUni`/`Net` addressing starts
at 0). By default (no explicit routing given -- see below), this lines up with the internal
index without another offset: `UNIVERSE 1 ARTNET` → internal index `0` → wire universe `0`
(the *first* Art-Net universe, sometimes written `0.0` or `Net 0 / SubUni 0`). `UNIVERSE 2
ARTNET` → internal index `1` → wire universe `1`. In general, absent an explicit wire universe:
**wire universe = `.show` universe number − 1**, the same arithmetic as a DMX address, just
applied to the whole universe instead of a channel within it. DMX and sACN universes don't have
this wire-level 0-basing concern (DMX is a physical port, not a numbered universe on a shared
wire), so this mapping only matters for `ARTNET` transport universes.

### Universes — Explicit Destination Routing (Wave 3)

A real rig usually has several Art-Net nodes, and a node usually has several DMX outputs. Since
"internal universe index" and "Art-Net wire universe" are conflated by default (previous
section), there was no way to say "wire universe 0 on node A" and "wire universe 0 on node B" --
same number, different IP. `UNIVERSE ... ARTNET` takes two more optional arguments for this:

```
UNIVERSE <idx> ARTNET [<ip>] [<wireUniverse>]
```

```
SHOW 2
UNIVERSE 1 DMX
UNIVERSE 2 ARTNET 192.168.1.50 0     # node A, wire universe 0
UNIVERSE 3 ARTNET 192.168.1.50 1     # SAME node, second DMX output -- the normal case
UNIVERSE 4 ARTNET 192.168.1.51 0     # node B, also wire universe 0 -- different IP, no conflict
UNIVERSE 5 ARTNET                    # no IP -> the CFG1 fallback, or broadcast if that's 0
```

- A multi-output node (2/4/8 DMX ports) is the normal case, not an exception: same IP, different
  wire universes across two `UNIVERSE` lines.
- `<ip>` omitted → the destination is left as the "no explicit route" marker; the device
  resolves it to CFG1's `artnetFallbackIp`, or broadcasts if that's `0` too (see FORMAT.md's CFG1
  section for the full precedence chain: **explicit `.show` route > `artnetFallbackIp` >
  broadcast**). Existing shows that specify no IPs keep working unchanged.
- `<wireUniverse>` omitted but `<ip>` given → defaults to the universe's own internal index (the
  previous section's mapping), and the compiler emits a non-fatal warning -- an explicit number
  is almost always what's meant, but this isn't an error.
- Both omitted (bare `ARTNET`) → today's implicit behavior exactly, no warning.
- **Compile-time validation**: two `UNIVERSE ARTNET` lines resolving to the same `(ip,
  wireUniverse)` is an error naming both universes (two streams fighting over one node output);
  the same IP with two *different* wire universes is accepted (that's the multi-output-node
  case). A malformed `<ip>` (not four dot-separated `0..255` octets) is an error naming the line.

See FORMAT.md's "Art-Net Wire Universe & Destination Routing" section for the full byte-level
Net/SubNet/Universe decomposition, the `ArtNetSink`/`ArtNetRouter` API, ArtSync, and Phase 3's
ArtPoll/ArtPollReply discovery (which fills in exactly this same table when a `.show` doesn't
give an IP at all).

### Format Version / Migration

`.show` text is versioned via the required `SHOW 2` header line. There is **no v1 fallback**:
a pre-v2 `.show` (0-indexed, no header) is a hard parse error rather than being silently
reinterpreted, because flipping the addressing semantics without a version marker would shift
every address by one channel with no error — a rig that mostly works with one fixture subtly
wrong. If you have an old `.show`:

1. Add `SHOW 2` as the first non-comment line.
2. Add 1 to every `UNIVERSE` index and every `FIXTURE`/`MATRIX` address.

### Example `.show`

```
SHOW 2

UNIVERSE 1 DMX
UNIVERSE 2 ARTNET

# Torrent moving head at (1, 2, 3) in world space, pan/tilt at identity.
FIXTURE torrent.fdef 1 2
POS 1 2 3
ROT 0 0 0

# Par fixture, plain dimmer + RGB
FIXTURE par.fdef 1 21

# 16×16 LED matrix on universe 2, starting at address 1
MATRIX 2 1 16 16 SERP H GRB
```

## `SHW1` Binary Bundle Format

Little-endian encoding. Assumes all platforms (x86-64 host and ESP32-S3 Xtensa device) are little-endian. Floating-point numbers are IEEE-754 32-bit (native byte order). Document any assumption changes.

Note: this `version` byte (1; 2 once a `CONTROLLER` is compiled in; 3 once a `.show` gives at
least one `UNIVERSE ... ARTNET` line an explicit IP and/or wire universe) is the **binary
bundle** format version, unrelated to the `.show` text's `SHOW 2` header above — the text format
moved to 1-indexed addressing without touching this byte at all; a `SHOW 2` source file with
neither a `CONTROLLER` nor explicit Art-Net routing still compiles to `version = 1` bytes,
byte-identical to a pre-migration bundle. Versions only ever go up when the feature that needs
the extra bytes is actually used -- see "Universe Table" below for what v3 adds.

### Layout

```
Header (11 bytes):
  magic         4 bytes       "SHW1"
  version       u8            = 1
  universeCount u8            (0..8)
  profileCount  u16           count of unique PFX1 profiles
  fixtureCount  u16           count of fixtures in patch
  matrixCount   u16           count of pixel matrices

Universe Table (universeCount entries):
  v1/v2 -- 1 byte each:
    transport[i]  u8            0=Dmx, 1=ArtNet, 2=Sacn, 3=Unused
  v3 -- 7 bytes each (see FORMAT.md's "Art-Net Wire Universe & Destination
  Routing" for the full writeup):
    transport[i]     u8         (as above)
    destIp[i]        u32        packed host-byte-order IPv4; 0 = no explicit route
    wireUniverse[i]  u16        0..32767, already resolved by the compiler

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
  - Bad magic or version not in {1, 2, 3} → false
  - Truncated bundle (size too small) → false
  - profileIndex out of range → false
  - Any PFX1 blob that fails `parseProfile` → false
- **No exceptions**: Portable, uses return values for errors.
- Testable under `-fsanitize=address` to detect OOB reads.

## GDTF / QLC+ / OFL Importers

Implemented, entirely in the browser: `web/shared/importers/` parses QLC+
`.qxf` (XML), Open Fixture Library `.json`, and GDTF `.gdtf` (a ZIP
containing `description.xml`) fixture definitions and maps them to the
intermediate model in `web/shared/importers/model.js`, which
`emitFdef()` turns into `.fdef` text — including PFX2 `SLOT`/`RANGE`
lines, so a real fixture's colour wheel, gobo wheel, prism, etc. come
through named, not just as bare linear bytes. See the import panel in the
provisioner editor (`web/provisioner-static/import.js`) and
`web/shared/importers/testdata/NOTICE.md` for the real manufacturer files
this was built and tested against.

None of this runs on the device or needs a native ZIP/XML library: a tiny
dependency-free XML parser (`xml-lite.js`) and ZIP reader (`zip-lite.js`,
STORE + DEFLATE via `DecompressionStream`/`node:zlib`) are enough for
these formats. The `.fdef` and `.show` text formats are the stable seam
the importers target — same as any hand-written `.fdef` — which is what
keeps the device-side loader simple and the importers entirely
PC/browser-side.

Test suite: `node web/shared/importers/test-importers.mjs` (or
`make test-importers`) — pure-JS parsing/mapping tests against the
committed real fixture files, plus a round-trip through `fdef_check` (a
host build of this same `parseFixtureDef`/`encodeProfile`/`parseProfile`
pipeline) that proves an imported `.fdef` actually compiles to the
channel map and ranges expected, not just plausible-looking text.

## API

### Compiler (Host-Only)

```cpp
// Parse a .fdef file
bool parseFixtureDef(const std::string& text, FixtureDef& out, std::string& err);

// Map capability name to enum
bool capFromName(const std::string& name, Capability& out);

// Encode a FixtureDef to a PFX1 blob
std::vector<uint8_t> encodeProfile(const FixtureDef& def);

// Compile a .show to a SHW1 bundle. CompileResult.warnings carries non-fatal
// notices (e.g. unused trailing channels in a patched universe); always
// empty when !ok.
CompileResult compileShow(const std::string& showText,
                          const std::function<std::string(const std::string&)>& readFile);
```

### Loader (Portable)

```cpp
struct ArtNetDest { uint32_t ip; uint16_t wireUniverse; };  // ip == 0 -> fallback/broadcast

struct LoadedShow {
  uint8_t universeCount;
  UniverseTransport transport[8];
  ArtNetDest artnetDest[8];  // always populated -- v1/v2 bundles get today's
                             // implicit default (ip=0, wireUniverse=index)
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
- `SHOW 2` 1-indexed addressing: the address/universe conversion (`address 17` → `base 16`),
  a missing-header file failing with the migration message, out-of-range universes/addresses,
  and a footprint that runs past the end of a universe.
- Address collision detection: two overlapping fixtures, three-way overlaps (all reported, not
  just the first), and a matrix overlapping a fixture.
- Golden test: `samples/demo.show` compiles to a byte-identical `SHW1` bundle before and after
  the 1-indexing migration — proves the text semantics moved without moving the bytes.
