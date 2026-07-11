#ifndef AIM_H
#define AIM_H

#include "vec_math.h"

// Coordinate conventions:
//   Right-handed world frame: +X = stage right, +Y = up, +Z = toward audience.
//   Fixture-local frame at pan=0, tilt=0: beam points along +Z_local,
//   up-post is +Y_local, +X_local = right (stage-right from fixture perspective).
//   Fixture mounting orientation is a 3×3 rotation matrix R that maps
//   local → world coordinates (v_world = R * v_local).
//   To transform a world direction into the fixture's local frame:
//   v_local = Rᵀ * v_world (Rᵀ = inverse for a rotation).

struct MovingHeadConfig {
  Vec3 position;          // world position of the fixture
  Mat3 orientation;       // local -> world (from mat3FromEuler or identity3)
  float panRangeDeg;      // total mechanical pan sweep, e.g. 540
  float tiltRangeDeg;     // total mechanical tilt sweep, e.g. 270
  float panCenterNorm;    // normalized DMX value at pan = 0 deg, default 0.5
  float tiltCenterNorm;   // normalized DMX value at tilt = 0 deg, default 0.5
  bool invertPan;         // negate the pan angle term before mapping
  bool invertTilt;        // negate the tilt angle term before mapping
};

struct AimResult {
  float panNorm;   // [0,1], feed to profile Capability::Pan
  float tiltNorm;  // [0,1], feed to profile Capability::Tilt
  bool reachable;  // false if clamped or input direction was degenerate
};

// Compute pan/tilt to point in a given world direction.
AimResult aimDirection(const MovingHeadConfig& cfg, Vec3 dirWorld);

// Compute pan/tilt to point at a world position.
AimResult aimAtPoint(const MovingHeadConfig& cfg, Vec3 targetWorld);

#endif  // AIM_H
