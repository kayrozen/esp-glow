#include "vec_math.h"
#include <cmath>

const float PI = 3.14159265358979f;

// Vec3 operations
Vec3 add(Vec3 a, Vec3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec3 sub(Vec3 a, Vec3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 scale(Vec3 a, float s) {
  return {a.x * s, a.y * s, a.z * s};
}

float dot(Vec3 a, Vec3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float length(Vec3 a) {
  return sqrtf(a.x * a.x + a.y * a.y + a.z * a.z);
}

Vec3 normalize(Vec3 a) {
  float len = length(a);
  if (len < 1e-6f) {
    return {0.0f, 0.0f, 0.0f};
  }
  return scale(a, 1.0f / len);
}

// Mat3 operations
Vec3 mul(const Mat3& R, Vec3 v) {
  return {R.m[0] * v.x + R.m[1] * v.y + R.m[2] * v.z,
          R.m[3] * v.x + R.m[4] * v.y + R.m[5] * v.z,
          R.m[6] * v.x + R.m[7] * v.y + R.m[8] * v.z};
}

Mat3 mul(const Mat3& A, const Mat3& B) {
  Mat3 C = {};
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) {
      C.m[i * 3 + j] = A.m[i * 3 + 0] * B.m[0 * 3 + j] +
                       A.m[i * 3 + 1] * B.m[1 * 3 + j] +
                       A.m[i * 3 + 2] * B.m[2 * 3 + j];
    }
  }
  return C;
}

Mat3 transpose(const Mat3& R) {
  return {{R.m[0], R.m[3], R.m[6], R.m[1], R.m[4], R.m[7], R.m[2], R.m[5], R.m[8]}};
}

Mat3 identity3() {
  return {{1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}};
}

Mat3 rotX(float a) {
  float c = cosf(a);
  float s = sinf(a);
  return {{1.0f, 0.0f, 0.0f, 0.0f, c, -s, 0.0f, s, c}};
}

Mat3 rotY(float a) {
  float c = cosf(a);
  float s = sinf(a);
  return {{c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c}};
}

Mat3 rotZ(float a) {
  float c = cosf(a);
  float s = sinf(a);
  return {{c, -s, 0.0f, s, c, 0.0f, 0.0f, 0.0f, 1.0f}};
}

Mat3 mat3FromEuler(float yawDeg, float pitchDeg, float rollDeg) {
  float yawRad = yawDeg * (PI / 180.0f);
  float pitchRad = pitchDeg * (PI / 180.0f);
  float rollRad = rollDeg * (PI / 180.0f);

  // R = rotY(yaw) * rotX(pitch) * rotZ(roll)
  Mat3 Ry = rotY(yawRad);
  Mat3 Rx = rotX(pitchRad);
  Mat3 Rz = rotZ(rollRad);

  Mat3 RxRz = mul(Rx, Rz);
  return mul(Ry, RxRz);
}
