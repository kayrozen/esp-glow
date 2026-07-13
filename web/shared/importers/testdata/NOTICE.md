# Test fixture provenance

Real-world fixture definitions committed here so the importer tests exercise
actual industry files, not just hand-crafted ones. Each is unmodified from
its source repository except where noted.

## QLC+ (`qlcplus/`)

From [mcallegari/qlcplus](https://github.com/mcallegari/qlcplus)
(`resources/fixtures/`), Apache-2.0 licensed.

- `Generic-RGB.qxf` -- `Generic/Generic-Generic-RGB.qxf`. Simple RGB par,
  5 modes (RGB / GRB / BGR / RGB Dimmer / Dimmer RGB).
- `Clay-Paky-Sharpy.qxf` -- `Clay_Paky/Clay-Paky-Sharpy.qxf`. Moving head:
  16-bit pan/tilt, colour wheel + gobo wheel (each mixing discrete slots
  with a continuous rotation sub-range), prism, frost, 2 modes (Standard
  16ch / Vector 20ch).

## OFL (`ofl/`)

From [FloEdelmann/open-fixture-library](https://github.com/FloEdelmann/open-fixture-library)
(`fixtures/`), MIT licensed.

- `rgb-fader.json` -- `generic/rgb-fader.json`. Simple RGB par with fine
  channel aliases, 3 modes (8/16/24-bit).
- `pan-tilt.json` -- `generic/pan-tilt.json`. Plain pan/tilt, 5 modes
  including double-fine (24-bit) pairing.
- `sharpy.json` -- `clay-paky/sharpy.json`. The same Sharpy fixture as
  above, in OFL's schema (`wheels` + `WheelSlot`/`WheelRotation` capability
  types instead of QLC+'s inline `<Capability Min Max>` ranges) -- lets the
  importer tests cross-check one real fixture against two source formats.
- `slimpar-pro-h-usb.json` -- `chauvet-dj/slimpar-pro-h-usb.json`. Harder
  case: `NoFunction` capabilities, `switchChannels` (virtual mode-dependent
  channel aliasing), 3 modes (6/7/12ch).

## GDTF (`gdtf/`)

- `led-par-64-rgbw.gdtf` -- trimmed (3D models and thumbnail dropped; GDTF
  geometry/meshes are out of scope for this importer) from
  `BlenderDMX@LED_PAR_64_RGBW@v0.3.gdtf` in
  [open-stage/python-gdtf](https://github.com/open-stage/python-gdtf)'s
  test suite (`tests/`), MIT licensed, originally authored by the
  BlenderDMX project. `description.xml`'s bytes are otherwise untouched.
  Simple 5-channel RGBW+dimmer par, 1 mode -- and real-world confirmation
  that GDTF's trivial-ChannelSet convention (every linear channel gets a
  `Min`/`""`/`Max` triplet) must NOT be read as 3 discrete slots.
- `ESP-Glow@Test-Beam.gdtf` -- **not a real fixture**; hand-authored for
  this test suite (following the real GDTF 1.2 schema observed in the file
  above) to cover what no single easily-reachable real sample here did at
  once: 16-bit pan/tilt via a comma-separated `Offset`, a colour wheel and
  a gobo wheel each combining a discrete `ChannelFunction` (with nested,
  meaningfully-named `ChannelSet`s) and a continuous rotation
  `ChannelFunction` sharing the *same* DMX byte, a standalone
  `PrismRotation` channel on its *own* byte (distinct from `Prism`), and
  2 DMX modes (`Basic` 9ch / `Extended` 12ch). Deflate-compressed (the real
  sample above is stored/uncompressed), so the two together exercise both
  ZIP compression methods the importer's ZIP reader supports.
