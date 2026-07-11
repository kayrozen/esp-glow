#include "aim.h"
#include "vec_math.h"
#include <cstdio>
#include <cmath>
#include <cstdlib>

const float EPSILON = 1e-4f;
const float PI = 3.14159265358979f;

#define CHECK(cond, msg)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL: %s\n", (msg));                                    \
      return false;                                                            \
    }                                                                          \
  } while (0)

#define CHECK_FLOAT(actual, expected, msg)                                    \
  do {                                                                         \
    float diff = fabsf((actual) - (expected));                                \
    if (diff > EPSILON) {                                                      \
      fprintf(stderr,                                                          \
              "FAIL: %s (expected %.6f, got %.6f, diff %.6e)\n",               \
              (msg),                                                           \
              (expected),                                                      \
              (actual),                                                        \
              diff);                                                           \
      return false;                                                            \
    }                                                                          \
  } while (0)

// Test 1: Straight ahead
bool test_straight_ahead() {
  MovingHeadConfig cfg = {
      {0.0f, 0.0f, 0.0f},  // position
      identity3(),          // orientation
      540.0f,               // panRangeDeg
      270.0f,               // tiltRangeDeg
      0.5f,                 // panCenterNorm
      0.5f,                 // tiltCenterNorm
      false,                // invertPan
      false                 // invertTilt
  };

  AimResult result = aimDirection(cfg, {0.0f, 0.0f, 10.0f});

  CHECK(result.reachable, "Test 1: Should be reachable");
  CHECK_FLOAT(result.panNorm, 0.5f, "Test 1: Pan should be 0.5");
  CHECK_FLOAT(result.tiltNorm, 0.5f, "Test 1: Tilt should be 0.5");

  return true;
}

// Test 2: Stage right, level
bool test_stage_right_level() {
  MovingHeadConfig cfg = {
      {0.0f, 0.0f, 0.0f},  // position
      identity3(),          // orientation
      540.0f,               // panRangeDeg
      270.0f,               // tiltRangeDeg
      0.5f,                 // panCenterNorm
      0.5f,                 // tiltCenterNorm
      false,                // invertPan
      false                 // invertTilt
  };

  AimResult result = aimDirection(cfg, {10.0f, 0.0f, 0.0f});

  // pan angle = +90° = +π/2 rad
  // panNorm = 0.5 + (π/2) / (3π) = 0.5 + 1/6 ≈ 0.66667
  CHECK(result.reachable, "Test 2: Should be reachable");
  CHECK_FLOAT(result.panNorm, 0.5f + 1.0f / 6.0f, "Test 2: Pan should be ~0.66667");
  CHECK_FLOAT(result.tiltNorm, 0.5f, "Test 2: Tilt should be 0.5");

  return true;
}

// Test 3: Straight up
bool test_straight_up() {
  MovingHeadConfig cfg = {
      {0.0f, 0.0f, 0.0f},  // position
      identity3(),          // orientation
      540.0f,               // panRangeDeg
      270.0f,               // tiltRangeDeg
      0.5f,                 // panCenterNorm
      0.5f,                 // tiltCenterNorm
      false,                // invertPan
      false                 // invertTilt
  };

  AimResult result = aimDirection(cfg, {0.0f, 10.0f, 0.0f});

  // tilt angle = +90° = +π/2 rad
  // tiltNorm = 0.5 + (π/2) / (1.5π) = 0.5 + 1/3 ≈ 0.83333
  CHECK(result.reachable, "Test 3: Should be reachable");
  CHECK_FLOAT(result.panNorm, 0.5f, "Test 3: Pan should be 0.5");
  CHECK_FLOAT(result.tiltNorm, 0.5f + 1.0f / 3.0f, "Test 3: Tilt should be ~0.83333");

  return true;
}

// Test 4: aimAtPoint == aimDirection
bool test_aim_at_point() {
  MovingHeadConfig cfg = {
      {5.0f, 2.0f, 0.0f},  // position
      identity3(),          // orientation
      540.0f,               // panRangeDeg
      270.0f,               // tiltRangeDeg
      0.5f,                 // panCenterNorm
      0.5f,                 // tiltCenterNorm
      false,                // invertPan
      false                 // invertTilt
  };

  // Target {5,2,10} from position {5,2,0} = direction {0,0,10}
  AimResult result1 = aimAtPoint(cfg, {5.0f, 2.0f, 10.0f});
  AimResult result2 = aimDirection(cfg, {0.0f, 0.0f, 1.0f});

  CHECK(result1.reachable, "Test 4: aimAtPoint should be reachable");
  CHECK(result2.reachable, "Test 4: aimDirection should be reachable");
  CHECK_FLOAT(result1.panNorm, result2.panNorm, "Test 4: Pan should match");
  CHECK_FLOAT(result1.tiltNorm, result2.tiltNorm, "Test 4: Tilt should match");

  return true;
}

// Test 5: Orientation
bool test_orientation() {
  MovingHeadConfig cfg = {
      {0.0f, 0.0f, 0.0f},               // position
      mat3FromEuler(90.0f, 0.0f, 0.0f), // yawed +90°
      540.0f,                            // panRangeDeg
      270.0f,                            // tiltRangeDeg
      0.5f,                              // panCenterNorm
      0.5f,                              // tiltCenterNorm
      false,                             // invertPan
      false                              // invertTilt
  };

  // After yaw +90°, world +Z maps to local -X
  // So atan2f(dl.x, dl.z) = atan2f(-1, 0) ≈ -π/2
  // panNorm = 0.5 + (-π/2) / (3π) = 0.5 - 1/6 ≈ 0.33333
  AimResult result = aimDirection(cfg, {0.0f, 0.0f, 10.0f});

  CHECK(result.reachable, "Test 5: Should be reachable");
  CHECK_FLOAT(result.panNorm, 0.5f - 1.0f / 6.0f, "Test 5: Pan should shift by -90°");

  return true;
}

// Test 6: Degenerate (target == position)
bool test_degenerate() {
  MovingHeadConfig cfg = {
      {5.0f, 2.0f, 3.0f},  // position
      identity3(),          // orientation
      540.0f,               // panRangeDeg
      270.0f,               // tiltRangeDeg
      0.5f,                 // panCenterNorm
      0.5f,                 // tiltCenterNorm
      false,                // invertPan
      false                 // invertTilt
  };

  AimResult result = aimAtPoint(cfg, {5.0f, 2.0f, 3.0f});

  CHECK(!result.reachable, "Test 6: Should not be reachable");
  CHECK_FLOAT(result.panNorm, 0.5f, "Test 6: Pan should be centerNorm");
  CHECK_FLOAT(result.tiltNorm, 0.5f, "Test 6: Tilt should be centerNorm");

  return true;
}

// Test 7: Unreachable clamp
bool test_unreachable_clamp() {
  MovingHeadConfig cfg = {
      {0.0f, 0.0f, 0.0f},  // position
      identity3(),          // orientation
      90.0f,                // panRangeDeg (only 90°, narrow)
      270.0f,               // tiltRangeDeg
      0.5f,                 // panCenterNorm
      0.5f,                 // tiltCenterNorm
      false,                // invertPan
      false                 // invertTilt
  };

  // Target directly behind on pan axis (beyond the narrow range)
  // Direction {0,0,-10} => pan angle = atan2(0, -10) = π
  // panNorm = 0.5 + π / (π/2) = 0.5 + 2 = 2.5
  // This is clearly out of bounds and should clamp to [0,1]
  AimResult result = aimDirection(cfg, {0.0f, 0.0f, -10.0f});

  CHECK(!result.reachable, "Test 7: Should not be reachable");
  CHECK(result.panNorm >= 0.0f && result.panNorm <= 1.0f, "Test 7: Pan should be clamped");
  CHECK(result.tiltNorm >= 0.0f && result.tiltNorm <= 1.0f, "Test 7: Tilt should be clamped");

  return true;
}

// Test 8: Invert pan
bool test_invert_pan() {
  MovingHeadConfig cfg = {
      {0.0f, 0.0f, 0.0f},  // position
      identity3(),          // orientation
      540.0f,               // panRangeDeg
      270.0f,               // tiltRangeDeg
      0.5f,                 // panCenterNorm
      0.5f,                 // tiltCenterNorm
      true,                 // invertPan
      false                 // invertTilt
  };

  AimResult result = aimDirection(cfg, {10.0f, 0.0f, 0.0f});

  // With inversion: panNorm = 0.5 - 1/6 ≈ 0.33333
  CHECK(result.reachable, "Test 8: Should be reachable");
  CHECK_FLOAT(result.panNorm, 0.5f - 1.0f / 6.0f, "Test 8: Pan should be inverted");

  return true;
}

// Test 9: rotY(π/2) * {0,0,1} ≈ {1,0,0}
bool test_rot_y() {
  Mat3 R = rotY(PI / 2.0f);
  Vec3 input = {0.0f, 0.0f, 1.0f};
  Vec3 v = mul(R, input);

  CHECK_FLOAT(v.x, 1.0f, "Test 9a: rotY(π/2)*{0,0,1} should give x=1");
  CHECK_FLOAT(v.y, 0.0f, "Test 9b: rotY(π/2)*{0,0,1} should give y=0");
  CHECK_FLOAT(v.z, 0.0f, "Test 9c: rotY(π/2)*{0,0,1} should give z=0");

  return true;
}

// Test 10: rotX(π/2) * {0,1,0} ≈ {0,0,1}
bool test_rot_x() {
  Mat3 R = rotX(PI / 2.0f);
  Vec3 input = {0.0f, 1.0f, 0.0f};
  Vec3 v = mul(R, input);

  CHECK_FLOAT(v.x, 0.0f, "Test 10a: rotX(π/2)*{0,1,0} should give x=0");
  CHECK_FLOAT(v.y, 0.0f, "Test 10b: rotX(π/2)*{0,1,0} should give y=0");
  CHECK_FLOAT(v.z, 1.0f, "Test 10c: rotX(π/2)*{0,1,0} should give z=1");

  return true;
}

// Test 11: transpose(R) * (R * v) ≈ v
bool test_transpose_inverse() {
  Mat3 R = mat3FromEuler(30.0f, 20.0f, 10.0f);
  Vec3 v = {1.5f, 2.3f, -0.7f};

  Vec3 Rv = mul(R, v);
  Vec3 RT_Rv = mul(transpose(R), Rv);

  CHECK_FLOAT(RT_Rv.x, v.x, "Test 11a: transpose inverse x");
  CHECK_FLOAT(RT_Rv.y, v.y, "Test 11b: transpose inverse y");
  CHECK_FLOAT(RT_Rv.z, v.z, "Test 11c: transpose inverse z");

  return true;
}

// Test 12: mat3FromEuler(0,0,0) ≈ identity3()
bool test_euler_identity() {
  Mat3 R = mat3FromEuler(0.0f, 0.0f, 0.0f);
  Mat3 I = identity3();

  for (int i = 0; i < 9; ++i) {
    CHECK_FLOAT(R.m[i], I.m[i], "Test 12: Euler(0,0,0) should be identity");
  }

  return true;
}

// Test 13: normalize({0,0,0}) returns {0,0,0} (no NaN)
bool test_normalize_zero() {
  Vec3 v = normalize({0.0f, 0.0f, 0.0f});

  CHECK_FLOAT(v.x, 0.0f, "Test 13a: normalize({0,0,0}).x should be 0");
  CHECK_FLOAT(v.y, 0.0f, "Test 13b: normalize({0,0,0}).y should be 0");
  CHECK_FLOAT(v.z, 0.0f, "Test 13c: normalize({0,0,0}).z should be 0");

  return true;
}

int main() {
  bool all_pass = true;

  // Aim tests
  printf("Test 1: Straight ahead... ");
  if (!test_straight_ahead()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 2: Stage right, level... ");
  if (!test_stage_right_level()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 3: Straight up... ");
  if (!test_straight_up()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 4: aimAtPoint == aimDirection... ");
  if (!test_aim_at_point()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 5: Orientation... ");
  if (!test_orientation()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 6: Degenerate... ");
  if (!test_degenerate()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 7: Unreachable clamp... ");
  if (!test_unreachable_clamp()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 8: Invert pan... ");
  if (!test_invert_pan()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  // Math primitive tests
  printf("Test 9: rotY(π/2) * {0,0,1}... ");
  if (!test_rot_y()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 10: rotX(π/2) * {0,1,0}... ");
  if (!test_rot_x()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 11: transpose(R) * (R * v) ≈ v... ");
  if (!test_transpose_inverse()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 12: mat3FromEuler(0,0,0) ≈ identity... ");
  if (!test_euler_identity()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("Test 13: normalize({0,0,0})... ");
  if (!test_normalize_zero()) {
    all_pass = false;
  } else {
    printf("PASS\n");
  }

  printf("\n");
  if (all_pass) {
    printf("All tests passed!\n");
    return 0;
  } else {
    printf("Some tests failed!\n");
    return 1;
  }
}
