# esp-glow

*Afterglow* firmware for the ESP32-S3: DMX/Art-Net/sACN/WLED output, a pixel-matrix
engine, MIDI/OSC control surfaces, and a Fennel live-coding layer for writing shows —
documented here from the source that actually ships, not from hand-copied notes that
drift out of sync with it.

## How these docs are built

Two kinds of page, deliberately kept apart:

- **Generated** — [API reference](generated/api-reference.html),
  [grammar reference](generated/grammar-reference.html),
  [enumerations](generated/enumerations.html), and [test status](generated/test-status.html)
  are extracted straight from `glow_lua_api.cpp`, `provision.cpp`, `live_control.h`, and the
  `Makefile` by `docs/build/gen-reference.mjs` at build time. Nobody hand-edits these — CI
  regenerates them from source and fails the build if a committed copy doesn't match, so a
  `glow.*` or grammar change without a regenerate can't merge.
- **Hand-written** — [Architecture](architecture.html), [Bring-up](bring-up.html), and the
  [interactive demo walkthrough](interactive/demo-walkthrough.html) are prose, reviewed in
  PRs like any other code change.

The interactive walkthrough is also a test: it loads and compiles the real
`samples/demo-boot.fnl` in your browser using the same Fennel compiler the device runs, so
an API regression that breaks the demo show turns the tutorial red instead of leaving a
doc that quietly lies.

## Where to start

- New to the project? Read [Architecture](architecture.html) first.
- Writing a show? Start with the [interactive demo walkthrough](interactive/demo-walkthrough.html).
- Looking up a `glow.*` call or a `.fdef`/`.show`/`.mdef` keyword? Jump straight to the
  [API reference](generated/api-reference.html) or [grammar reference](generated/grammar-reference.html).
- Bringing up new hardware? See [Bring-up](bring-up.html).
