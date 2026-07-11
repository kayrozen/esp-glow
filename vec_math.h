#ifndef VEC_MATH_H
#define VEC_MATH_H

struct Vec3 {
  float x, y, z;
};

struct Mat3 {
  float m[9];  // row-major: m[row*3 + col]
};

// Vec3 operations
Vec3 add(Vec3 a, Vec3 b);
Vec3 sub(Vec3 a, Vec3 b);
Vec3 scale(Vec3 a, float s);
float dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
float length(Vec3 a);
Vec3 normalize(Vec3 a);  // returns {0,0,0} if length < 1e-6f

// Mat3 operations
Vec3 mul(const Mat3& R, Vec3 v);      // R * v
Mat3 mul(const Mat3& A, const Mat3& B);  // A * B
Mat3 transpose(const Mat3& R);
Mat3 identity3();

// Elementary rotations (angle in radians), each local->world for a positive angle
Mat3 rotX(float a);  // Rotation about X axis
Mat3 rotY(float a);  // Rotation about Y axis
Mat3 rotZ(float a);  // Rotation about Z axis

// Build orientation from Euler angles in DEGREES. Order (intrinsic Y-X-Z):
//   R = rotY(yaw) * rotX(pitch) * rotZ(roll)
// yaw = heading about world +Y, pitch = nod about X, roll = bank about Z
Mat3 mat3FromEuler(float yawDeg, float pitchDeg, float rollDeg);

#endif  // VEC_MATH_H
