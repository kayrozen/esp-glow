<!-- GENERATED FILE -- do not hand-edit.
Produced by docs/build/gen-reference.mjs from provision.cpp.
Run `node docs/build/gen-reference.mjs` to regenerate; CI fails the build
if this file doesn't match what the generator produces (drift guard). -->

# Text-format grammar reference

Keywords each text format's compiler (`provision.cpp`) recognizes, in `cmd == "..."` source order. Grammar/argument shape for each keyword is not auto-extracted (freeform token parsing per keyword) -- see FORMAT.md and `provision.cpp`'s own parsing code for exact syntax.

## `.fdef` (`parseFixtureDef`)

`FIXTURE`, `FOOTPRINT`, `HEAD`, `PANRANGE`, `TILTRANGE`, `CAP`, `SLOT`, `RANGE`

## `.mdef` (`parseControllerDef`)

`CONTROLLER`, `MIDI_CHANNEL`, `PAD`, `FADER`, `ENCODER`, `LED`, `COLOR`, `INIT`

## `.show` (`compileShow`)

`UNIVERSE`, `FIXTURE`, `POS`, `ROT`, `CENTER`, `INVERT`, `MATRIX`, `CONTROLLER`, `WLED`

