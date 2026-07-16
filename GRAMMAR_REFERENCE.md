# esp-glow — text format reference (`.fdef` / `.show` / `.mdef`)

Author-facing grammar for the three text formats the browser provisioner compiles into the
device's binary bundle (`PFX2` fixtures, `SHW1` show, `MDF1` controller). Verified against the
compiler on `main`. `#` starts a comment; indentation attaches sub-lines to the preceding
`CAP`/`LED`.

---

## `.fdef` — fixture definition → `PFX2`

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
Capabilities: `Dimmer Red Green Blue White Amber Uv Cyan Magenta Yellow Pan Tilt
ShutterStrobe Gobo Focus Zoom Fog Fan ColorWheel GoboRotation Prism PrismRotation Frost Iris
CTO AnimationWheel Macro Generic`. A `CAP` with no `SLOT`/`RANGE` lines is linear (0..255).

---

## `.show` — the patch → `SHW1`

**Must begin with `SHOW 2`.** Addresses and universes are **1-indexed** — write the number
printed on the fixture. (A file without the header is rejected with a migration message.)

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

---

## `.mdef` — controller definition → `MDF1`

Describes the **hardware**, not the bindings (bindings live in Fennel). Channel handling is
per-range: add `CH <from> <to>` to a control that multiplexes on MIDI channel (e.g. the APC40
grid); omit it and the channel is ignored (the common case).

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
ENCODER CC 16 23                        # relative encoders

# LED feedback
LED NOTE 53 92 velocity                 # pad LEDs: colour = note-on velocity
  COLOR off 0                           # name the controller's palette (like SLOT names a DMX range)
  COLOR green 1
  COLOR red 3
LED CC 48 55 value                      # fader-ring LEDs driven by CC value
```

---

## Fennel binding vocabulary (in `boot.fnl` or the REPL)

```fennel
;; discrete
(glow.bind.pad 53 :flash :chorus)          ; note 53 → momentary cue
(glow.bind.pad-xy 0 0 :toggle :verse)      ; grid (col,row) → cue, resolved via the .mdef
(glow.bind.scene 61 :chorus)               ; a pad → a scene
(glow.bind.program :scene)                 ; program-change N → scene N (any keyboard)

;; continuous — named by SOURCE SHAPE, so any controller works
(glow.bind.fader 7 :master)
(glow.bind.pitchbend :param :hue)          ; a pitch wheel → an effect parameter
(glow.bind.pressure :cue-level :chorus)    ; channel pressure → hold a cue's level

;; LED feedback — (col,row) is primary, resolved the same way as bind.pad-xy
(glow.led.set-xy  0 0 :red)                ; grid (col,row) -> pad's LED, on the resolved channel
(glow.led.auto-xy 0 0 :chorus :green :off) ; pad (0,0) tracks the cue: green when active
(glow.led.set  53 :red)                    ; raw note, channel-agnostic — the escape hatch
(glow.led.auto 53 :chorus :green :off)     ; pad tracks the cue: green when active
(glow.bind.clear)                          ; wipe all bindings
```
Every binding is a **no-op** if the controller lacks that control — so one show `.fnl` runs on
any controller, using whatever it has.
