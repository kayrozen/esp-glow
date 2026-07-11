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
