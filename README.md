# Moving Head Pan/Tilt Aim Geometry

Pure C++17 implementation of pan/tilt angle computation for DMX moving head fixtures on ESP32-S3.

## Coordinate Conventions

### World Frame (Right-Handed)
- **+X**: Stage right
- **+Y**: Up (vertical)
- **+Z**: Toward audience (front)
- **Units**: Any consistent length unit (meters, feet, etc.); for aiming, only direction matters.

### Fixture-Local Frame (at pan=0°, tilt=0°)
- **+Z_local**: Beam direction (forward)
- **+Y_local**: Up-post (vertical)
- **+X_local**: Right from fixture perspective

### Fixture Orientation
The fixture's mounting orientation is represented as a **3×3 rotation matrix** `R` that maps **local → world** coordinates:
```
v_world = R * v_local
```

To transform a world direction into the fixture's local frame (required by the aiming algorithm):
```
v_local = Rᵀ * v_world  (Rᵀ = inverse for rotation matrices)
```

### Euler Angle Convention
Euler angles are applied in the order **(intrinsic Y-X-Z)**:
```
R = rotY(yaw) * rotX(pitch) * rotZ(roll)
```

Where:
- **yaw** (in degrees): Heading rotation about world +Y axis
- **pitch** (in degrees): Nod/tilt rotation about the intermediate X axis
- **roll** (in degrees): Bank rotation about the final Z axis

## Pan and Tilt Angles

### Pan Angle
- Rotation about the vertical (Y) axis of the fixture
- Computed from local X and Z components: `pan = atan2f(dl.x, dl.z)`
- Maps to normalized DMX value via: `panNorm = panCenter + pan / panRange`

### Tilt Angle
- Rotation about the horizontal (X) axis after pan
- Computed from local Y and horizontal distance: `tilt = atan2f(dl.y, sqrt(dl.x² + dl.z²))`
- Maps to normalized DMX value via: `tiltNorm = tiltCenter + tilt / tiltRange`

## Worked Example

**Setup:**
- Fixture at world position (2, 1, 0)
- Fixture orientation: yaw=0°, pitch=0°, roll=0° (identity orientation)
- Pan range: 540°, tilt range: 270°
- Centers: pan=0.5, tilt=0.5
- No inversions

**Target:** Point at world position (2, 6, 10)

**Calculation:**

1. Direction vector: (2, 6, 10) - (2, 1, 0) = (0, 5, 10)
2. Normalize: d ≈ (0, 0.447, 0.894)
3. Transform to local (R = identity, so Rᵀ = identity): dl = (0, 0.447, 0.894)
4. Pan angle: atan2(0, 0.894) ≈ 0 rad
5. Tilt angle: sqrt(0² + 0.894²) ≈ 0.894; atan2(0.447, 0.894) ≈ 0.464 rad (≈ 26.6°)
6. Convert ranges: panRange = 540° = 3π rad, tiltRange = 270° = 1.5π rad
7. Pan norm: 0.5 + 0 / 3π = 0.5
8. Tilt norm: 0.5 + 0.464 / 1.5π ≈ 0.5 + 0.0988 ≈ 0.599
9. Result: **panNorm ≈ 0.5**, **tiltNorm ≈ 0.599**, **reachable = true**

## API Overview

### `aimDirection(config, directionVector) → AimResult`
Compute pan/tilt angles to point the fixture in a given world direction.

### `aimAtPoint(config, targetPosition) → AimResult`
Compute pan/tilt angles to point the fixture at a world position. Internally computes direction as `target - fixture_position` and calls `aimDirection`.

### `AimResult`
```cpp
struct AimResult {
  float panNorm;    // [0, 1] normalized DMX value for pan
  float tiltNorm;   // [0, 1] normalized DMX value for tilt
  bool reachable;   // false if clamped or degenerate input
};
```

### `MovingHeadConfig`
```cpp
struct MovingHeadConfig {
  Vec3 position;        // World position
  Mat3 orientation;     // Local→world rotation matrix
  float panRangeDeg;    // Total pan sweep (e.g., 540°)
  float tiltRangeDeg;   // Total tilt sweep (e.g., 270°)
  float panCenterNorm;  // DMX value at pan=0° (typically 0.5)
  float tiltCenterNorm; // DMX value at tilt=0° (typically 0.5)
  bool invertPan;       // Negate pan angle before mapping
  bool invertTilt;      // Negate tilt angle before mapping
};
```

## Building and Testing

```bash
make test
```

Builds with `-std=c++17 -Wall -Wextra -Werror` and runs all tests. All assertions must pass.

## Integration with Profile Subsystem

The output values `panNorm` and `tiltNorm` are plain floats in `[0, 1]`, ready to pass to the profile system:
```cpp
applyCapability(profile, Capability::Pan, panNorm, ...);
applyCapability(profile, Capability::Tilt, tiltNorm, ...);
```

No shared DMX headers or dependencies—this module is purely mathematical geometry.

## Files

- **`vec_math.h`, `vec_math.cpp`**: Vector and matrix primitives (Vec3, Mat3, rotations, Euler angle conversion)
- **`aim.h`, `aim.cpp`**: Main aiming algorithm (`aimDirection`, `aimAtPoint`)
- **`test_aim.cpp`**: Comprehensive test suite (13 tests covering all cases)
- **`Makefile`**: Build configuration
- **`README.md`**: This file

---

# Show / Render Loop

Wires the fixture-profile subsystem (`fixture_profile.h`) and the aim geometry
subsystem (`aim.h`, `vec_math.h`) into a `Show`: a set of patched fixtures and
DMX universes that get resolved and flushed once per frame.

## `renderFrame(t)` algorithm

```
1. For each universe u:
     if mode(u) == Fixture:  zero all 512 bytes of u
     (Raw universes are left as-is)

2. For each patched fixture f (all live in Fixture universes):
     applyDefaults(f.profile, universe[f.universe].data, f.base)

3. Gather intents: clear the reusable member vectors caps_ and aims_ (keep
   capacity), then for each effect in insertion order call
   fx->evaluate(t, caps_, aims_).

4. Resolve aim intents FIRST, then cap intents (so an explicit Pan CapIntent
   could override an aim if authored that way — last write wins):
     for each AimIntent ai:
        f = fixture(ai.fixtureId); if !f or !f->isHead: skip
        AimResult r = ai.isPoint ? aimAtPoint(f->head, ai.target)
                                 : aimDirection(f->head, ai.target)
        applyCapability(f->profile, Capability::Pan,  r.panNorm,  buf(f), f->base)
        applyCapability(f->profile, Capability::Tilt, r.tiltNorm, buf(f), f->base)
        (ignore r.reachable for now; engine may use it later)
     for each CapIntent ci (in list order):
        f = fixture(ci.fixtureId); if !f: skip
        applyCapability(f->profile, ci.cap, ci.norm01, buf(f), f->base)

5. For each universe u: sink(u)->send(u, universe[u].data, 512)
   (skip universes with no configured sink)
```

`Show::caps_` and `Show::aims_` are `std::vector` members reserved once
(construction time) and `clear()`-ed every frame — `renderFrame` never
allocates.

## Fixture vs. Raw universes

Each universe configured via `configureUniverse(idx, mode, sink)` is one of:

- **`UniverseMode::Fixture`** — owned by the patch/effect pipeline. Zeroed at
  the start of every `renderFrame`, then repopulated by `applyDefaults` and
  the resolved effect intents (steps 1–4 above).
- **`UniverseMode::Raw`** — the pixel-effect seam. `renderFrame` never
  touches its bytes. Callers write into it directly with
  `writeRawUniverse(idx, data, len)` before calling `renderFrame` for the
  frame that buffer should appear in; whatever was last written is what gets
  sent. `writeRawUniverse` is a no-op on a `Fixture`-mode universe.

Both universe kinds are flushed identically in step 5: whatever bytes are
currently in `universe[u].data` go to `sink(u)->send(...)`.

## Device sinks

`IUniverseSink` is the hardware seam — the core (`show.h`/`show.cpp`) only
depends on the interface, plus the host-only `MockSink` used by tests.

Two concrete transports plug in on real hardware, each in its own file,
guarded by `#ifdef ESP_PLATFORM` so they compile to nothing (and pull in no
ESP-IDF headers) on a host build:

- **`dmx_sink.cpp` — `DmxSink`**: wraps an `esp_dmx` port. `send()` is meant
  to call `dmx_write()` then `dmx_send()` on that port; the actual esp_dmx
  calls are left as `// TODO` since they can only be verified on hardware.
- **`artnet_sink.cpp` — `ArtNetSink`**: builds an Art-Net DMX packet (8-byte
  `"Art-Net\0"` ID, OpCode 0x5000, ProtVer 0/14, Sequence, Physical,
  15-bit universe as SubUni+Net, 512 length big-endian, then the 512 data
  bytes) and is meant to UDP-send it to a configured bridge IP:port; the
  socket call is left as `// TODO` device wiring.

Neither file is part of `make test` — the `Makefile`'s `SHOW_SOURCES` list
only builds `show.cpp`, the upstream subsystems, and `test_show.cpp`. Wire a
concrete sink in on real firmware by constructing a `DmxSink`/`ArtNetSink`
and passing it to `Show::configureUniverse`.

## Building and testing

```bash
make test
```

Builds `test_aim` and `test_show` under
`-std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined` and runs
both. `renderFrame` performs zero heap allocation per frame.
