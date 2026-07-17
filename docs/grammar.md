# Text-format grammar reference (`.fdef` / `.show` / `.mdef`)

Author-facing grammar for the three text formats the browser provisioner compiles into the
device's binary bundle (`PFX2` fixtures, `SHW1` show, `MDF1` controller). `#` starts a
comment; indentation attaches sub-lines (`SLOT`/`RANGE` under `CAP`, `COLOR` under `LED`) to
the line above them.

These formats declare the **patch** — what fixtures exist, where they physically are, which
controller hardware is wired — not the show itself. Cues, scenes, effects, and every
`glow.bind.*`/`glow.led.*` binding are Fennel, not text-format grammar; see
[the author's guide](authoring.html) for those, and [the API reference](reference.html) for
every `glow.*` call.

**This page's coverage is enforced, not just aspirational**: CI extracts the real `cmd ==
"..."` keywords straight from `provision.cpp` and fails the build if this file documents
anything different (in either direction, per format) — see `docs/build/gen-reference.mjs`.

---

## `.fdef` — fixture definition → `PFX2`

Declares one fixture type: its DMX footprint and the capabilities (colour, dimmer, pan/tilt,
wheels...) at each channel.

```
FIXTURE Moving Head          # display name
FOOTPRINT 16                 # DMX channels this fixture occupies
HEAD                         # optional: this is a moving head (enables aim geometry)
PANRANGE 540                 # degrees (heads only)
TILTRANGE 270
CAP Dimmer 0                 # CAP <Capability> <coarse> [<fine>|-] [<default>] [INVERT]
CAP Pan 1 2                  # 16-bit: coarse=1, fine=2
CAP Tilt 3 4
CAP ColorWheel 5             # a wheel: list its named slots below
  SLOT 0 9    open           # SLOT <from> <to> <name> — discrete, snaps to range centre
  SLOT 10 19  red
  SLOT 20 29  blue
CAP ShutterStrobe 6
  SLOT 0 31   closed
  SLOT 32 63  open
  RANGE 64 95 strobe         # RANGE <from> <to> <name> — continuous, value spread across span
  SLOT 96 127 pulse
```

### FIXTURE
`FIXTURE <display name>` — free text to end of line; shown in the provisioner UI.
```
FIXTURE Moving Head
```

### FOOTPRINT
`FOOTPRINT <n>` — how many consecutive DMX channels this fixture occupies, starting at
whatever address the `.show` patches it to.
```
FOOTPRINT 16
```

### HEAD
`HEAD` — no arguments. Marks this fixture as a moving head: enables `glow.aim` and the
`.show`'s `POS`/`ROT`/`CENTER`/`INVERT` lines for it. A non-`HEAD` fixture rejects all four.
```
HEAD
```

### PANRANGE
`PANRANGE <degrees>` — total mechanical pan sweep (heads only), e.g. `540` for a head that
pans one and a half turns.
```
PANRANGE 540
```

### TILTRANGE
`TILTRANGE <degrees>` — total mechanical tilt sweep (heads only).
```
TILTRANGE 270
```

### CAP
`CAP <Capability> <coarse> [<fine>|-] [<default>] [INVERT]` — declares one capability at a
DMX channel offset. `<coarse>` is the 0-indexed channel within the fixture's footprint; add
`<fine>` for a 16-bit coarse/fine pair (finer resolution — used for pan/tilt on precise
heads), or write `-` to skip straight to `<default>`. `INVERT` reverses the channel's sense
(DMX `0` maps to the capability's *maximum*). A `CAP` with no `SLOT`/`RANGE` lines under it is
linear across its full range — the normal case for `Dimmer`, `Red`, `Pan`, etc.

Capability names: `Dimmer Red Green Blue White Amber Uv Cyan Magenta Yellow Pan Tilt
ShutterStrobe Gobo Focus Zoom Fog Fan ColorWheel GoboRotation Prism PrismRotation Frost Iris
CTO AnimationWheel Macro Generic`.
```
CAP Dimmer 0
CAP Pan 1 2          # 16-bit: coarse=1, fine=2
```

### SLOT
`SLOT <from> <to> <name>`, indented under the `CAP` it belongs to — names one **discrete**
position within that capability's DMX range (a wheel's fixed colour, a fixed gobo pattern).
`glow.slot`'s value snaps to the slot's centre; it doesn't spread across `<from>..<to>`.
```
CAP ColorWheel 5
  SLOT 0 9    open
  SLOT 10 19  red
```

### RANGE
`RANGE <from> <to> <name>`, indented under a `CAP` — like `SLOT`, but **continuous**:
`glow.slot`'s `value` argument spreads linearly across the named span instead of snapping to
its centre. Use this for a variable-speed strobe range, or any span you want to dial within
rather than pick a fixed point in.
```
CAP ShutterStrobe 6
  SLOT 0 31   closed
  RANGE 64 95 strobe
```

---

## `.mdef` — controller definition → `MDF1`

Describes a controller's **hardware** — which notes/CCs exist, and how its LEDs are driven —
not the bindings (those live in Fennel; see [the API reference](reference.html)'s
Controllers & bindings section for `glow.bind.*`). Channel handling is per-range: add
`CH <from> <to>` to a control that multiplexes on MIDI
channel (e.g. the APC40's clip-launch grid, where track is carried on the channel nibble);
omit it and the channel is ignored — the common case for a control that isn't per-track.

```
CONTROLLER Akai APC40
MIDI_CHANNEL 0                          # 0 = any (default)

# Init sent on connect — opaque bytes, emitted verbatim (e.g. APC40 "Mode 0")
INIT SYSEX F0 47 00 73 60 00 04 41 00 00 00 F7

# Inputs
PAD 53                                  # single pad, channel-agnostic
PAD 53 CH 0 7                           # channel-multiplexed: one pad per channel 0..7 at note 53
FADER CC 7                              # a fader on CC 7
FADER CC 48 CH 0 8                      # per-track faders (channel = track)
ENCODER CC 16                           # relative encoder

# LED feedback
LED NOTE 53 92 velocity                 # pad LEDs: colour = note-on velocity
  COLOR off 0                           # name the controller's palette (like SLOT names a DMX range)
  COLOR green 1
  COLOR red 3
LED CC 48 55 value                      # fader-ring LEDs driven by CC value
```

### CONTROLLER
`CONTROLLER <display name>` — free text to end of line; shown in the provisioner UI.
```
CONTROLLER Akai APC40
```

### MIDI_CHANNEL
`MIDI_CHANNEL <n>` — the MIDI channel this controller talks on by default; `0` (the default
if the line is omitted) means "any channel."
```
MIDI_CHANNEL 0
```

### PAD
`PAD <note> [CH <from> <to>]` — a button-type control at MIDI note `<note>`. Add
`CH <from> <to>` for a control repeated per channel at the same note (a channel-multiplexed
grid, like the APC40's 8-track clip launcher); omit it for an ordinary, channel-agnostic pad.
```
PAD 53
PAD 53 CH 0 7
```

### FADER
`FADER CC <cc#> [CH <from> <to>]` — a continuous control on MIDI CC `<cc#>`, with the same
optional channel-multiplexing as `PAD`.
```
FADER CC 7
FADER CC 48 CH 0 8
```

### ENCODER
`ENCODER CC <cc#>` — a relative (incremental, not absolute-position) encoder on CC `<cc#>`.
```
ENCODER CC 16
```

### LED
`LED NOTE <note> <cc> <mode>` (pad LED, echoed back on CC `<cc>`) or
`LED CC <cc> <feedback-cc> <mode>` (fader-ring LED) — declares that a control has LED
feedback, with `COLOR` lines underneath naming its palette. `<mode>` (e.g. `velocity`,
`value`) describes how the controller's firmware expects the colour byte encoded.
```
LED NOTE 53 92 velocity
  COLOR off 0
  COLOR green 1
LED CC 48 55 value
```

### COLOR
`COLOR <name> <raw-value>`, indented under a `LED` line — names one entry in that LED's
colour palette (the same idea as `SLOT` naming a DMX range). `glow.led.set`/`glow.led.auto`
address LEDs by these names.
```
LED NOTE 53 92 velocity
  COLOR off 0
  COLOR green 1
  COLOR red 3
```

### INIT
`INIT SYSEX <hex bytes...>` — opaque bytes sent verbatim on connect, e.g. a controller's own
mode-switch handshake (the APC40's "Generic Mode" SysEx). Send-only — no response is read
back.
```
INIT SYSEX F0 47 00 73 60 00 04 41 00 00 00 F7
```

---

## `.show` — the patch → `SHW1`

The top-level file: which universes exist, which fixtures sit where, and (optionally) which
controller is wired. **Must begin with `SHOW 2`** — a file without the header is rejected
with a migration message, so an old un-migrated file can't silently patch with shifted
addresses. Addresses and universes are **1-indexed** — write the number printed on the
fixture.

```
SHOW 2

# Universes: 1-indexed. Transport per universe.
UNIVERSE 1 DMX                          # local RS-485
UNIVERSE 2 ARTNET 192.168.1.50 0        # ARTNET [dest-ip [wire-universe]]; omit ip → fallback/broadcast
UNIVERSE 3 ARTNET 192.168.1.50 1        # same node, second output — the normal multi-output case
UNIVERSE 4 SACN 1                        # SACN [wire-universe]
UNIVERSE 5 WLED wled-strip 192.168.1.60 # WLED <name> <ip> — pushes to a WLED device

# Fixtures: FIXTURE <deffile> <universe> <address>   (1-indexed address)
FIXTURE samples/head.fdef 1 1
POS 2.0 1.0 0.0                          # metres (world frame) — enables aim-at-a-point
ROT 0 0 0                                # degrees (yaw pitch roll) — MUST match the real hang

# Matrices: MATRIX <universe> <start-addr> <w> <h> <SERP|PROG> <H|V> <colour-order>
MATRIX 2 1 10 16 SERP H RGB

# Controller (optional): pulls in an .mdef
CONTROLLER samples/apc40.mdef
```

The compiler **errors on address collisions** (two fixtures overlapping in a universe, naming
both and the exact channels) and on **duplicate `(ip, wire-universe)`** routes.

### UNIVERSE
`UNIVERSE <index> <transport> [args...]` — declares one DMX-sized (512-channel) universe and
how it's carried:
- `DMX` — the local RS-485 output, no further args.
- `ARTNET [dest-ip [wire-universe]]` — Art-Net; a bare `ARTNET` broadcasts/falls back, an
  explicit `dest-ip` with no `wire-universe` defaults the wire universe to the internal index
  (and warns — write it explicitly if that's not what you mean).
- `SACN [wire-universe]`
- `WLED <name> <ip>` — an inline shorthand for a WLED target; the dedicated `WLED` line below
  is the same idea with an explicit sync group.
```
UNIVERSE 1 DMX
UNIVERSE 2 ARTNET 192.168.1.50 0
UNIVERSE 4 SACN 1
```

### FIXTURE
`FIXTURE <deffile> <universe> <address>` — patches one instance of a `.fdef` fixture at a
1-indexed DMX address in a 1-indexed universe.
```
FIXTURE samples/head.fdef 1 1
```

### POS
`POS <x> <y> <z>` — metres, world frame; must directly follow the `FIXTURE` line it applies
to, and that fixture must be a `HEAD`. Enables `glow.aim` at a world point for this fixture.
Must match where the fixture physically hangs — `glow.aim` computes pan/tilt from this
position, so a wrong `POS` aims at the wrong spot even though the script is correct.
```
POS 2.0 1.0 0.0
```

### ROT
`ROT <yaw> <pitch> <roll>` — degrees, world frame; same placement rule as `POS`. Must match
the fixture's real mounting orientation for the same reason.
```
ROT 0 0 0
```

### CENTER
`CENTER <panNorm> <tiltNorm>` — the normalized DMX value (`0..1`) that corresponds to
pan/tilt angle `0°`, i.e. where the head points when "centred" on its own mechanical zero.
Defaults to `0.5 0.5` if omitted; override it if a fixture's electrical centre isn't its DMX
range's midpoint.
```
CENTER 0.5 0.5
```

### INVERT
`INVERT <panFlag> <tiltFlag>` — `0`/`1`. Negates the pan and/or tilt angle before it's mapped
to a DMX value, for a head that's mounted upside-down or mirrored relative to how `PANRANGE`/
`TILTRANGE` assume it moves.
```
INVERT 0 1
```

### MATRIX
`MATRIX <universe> <start-addr> <w> <h> <SERP|PROG> <H|V> <colour-order>` — patches a pixel
matrix. Wiring is serpentine (`SERP`, alternating rows/columns reversed) or progressive
(`PROG`), run horizontally (`H`) or vertically (`V`), with a byte order like `RGB`/`GRB`. A
large matrix can span multiple universes; the compiler splits its footprint across them
automatically.
```
MATRIX 2 1 10 16 SERP H RGB
```

### CONTROLLER
`CONTROLLER <deffile>` — pulls in an `.mdef` so this show's `glow.bind.*`/`glow.led.*` calls
have real controller hardware to resolve against.
```
CONTROLLER samples/apc40.mdef
```

### WLED
`WLED <name> <ip> [sync-group]` — declares a named WLED target `glow.wled.*` can address.
`sync-group` (`1..8`, default `1`) groups targets under WLED's own sync protocol.
```
WLED wled-strip 192.168.1.60
```
