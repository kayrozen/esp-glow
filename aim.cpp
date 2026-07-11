#include "aim.h"
#include <cmath>

const float PI = 3.14159265358979f;

static float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

AimResult aimDirection(const MovingHeadConfig& cfg, Vec3 dirWorld) {
  // Normalize the direction vector
  Vec3 d = normalize(dirWorld);

  // Check for degenerate input
  if (length(dirWorld) < 1e-6f) {
    return {cfg.panCenterNorm, cfg.tiltCenterNorm, false};
  }

  // Transform world direction to local coordinates
  Mat3 RT = transpose(cfg.orientation);
  Vec3 dl = mul(RT, d);

  // Compute pan angle (heading in local XZ plane)
  float panRad = atan2f(dl.x, dl.z);

  // Compute tilt angle (elevation above pan plane)
  float h = sqrtf(dl.x * dl.x + dl.z * dl.z);
  float tiltRad = atan2f(dl.y, h);

  // Apply inversion flags
  if (cfg.invertPan) {
    panRad = -panRad;
  }
  if (cfg.invertTilt) {
    tiltRad = -tiltRad;
  }

  // Convert angle ranges to radians
  float panRangeRad = cfg.panRangeDeg * (PI / 180.0f);
  float tiltRangeRad = cfg.tiltRangeDeg * (PI / 180.0f);

  // Map angles to normalized [0,1] range
  float panNorm = cfg.panCenterNorm + panRad / panRangeRad;
  float tiltNorm = cfg.tiltCenterNorm + tiltRad / tiltRangeRad;

  // Check if within bounds before clamping
  bool reachable =
      (panNorm >= 0.0f && panNorm <= 1.0f && tiltNorm >= 0.0f && tiltNorm <= 1.0f);

  // Clamp to [0,1]
  panNorm = clampf(panNorm, 0.0f, 1.0f);
  tiltNorm = clampf(tiltNorm, 0.0f, 1.0f);

  return {panNorm, tiltNorm, reachable};
}

AimResult aimAtPoint(const MovingHeadConfig& cfg, Vec3 targetWorld) {
  return aimDirection(cfg, sub(targetWorld, cfg.position));
}
