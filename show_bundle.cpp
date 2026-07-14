#include "show_bundle.h"
#include "vec_math.h"
#include <cstring>
#include <cstddef>

// Strict little-endian readers with bounds checking
// All operations check bounds before reading and return false on OOB

class BundleReader {
public:
  BundleReader(const uint8_t* data, size_t len) : data_(data), len_(len), pos_(0) {}

  bool readU8(uint8_t& out) {
    if (pos_ + 1 > len_) return false;
    out = data_[pos_];
    pos_++;
    return true;
  }

  bool readU16(uint16_t& out) {
    if (pos_ + 2 > len_) return false;
    out = static_cast<uint16_t>(data_[pos_]) |
          (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return true;
  }

  bool readF32(float& out) {
    if (pos_ + 4 > len_) return false;
    uint32_t bits = static_cast<uint32_t>(data_[pos_]) |
                    (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
                    (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
                    (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    std::memcpy(&out, &bits, sizeof(float));
    pos_ += 4;
    return true;
  }

  bool readBytes(uint8_t* out, size_t count) {
    if (pos_ + count > len_) return false;
    std::memcpy(out, data_ + pos_, count);
    pos_ += count;
    return true;
  }

  bool ensureRemaining(size_t count) {
    return pos_ + count <= len_;
  }

  size_t getPos() const { return pos_; }

private:
  const uint8_t* data_;
  size_t len_;
  size_t pos_;
};

bool loadShow(const uint8_t* data, size_t len, LoadedShow& out) {
  out = LoadedShow();

  BundleReader reader(data, len);

  // Read the fixed 6-byte prefix (magic + version + universeCount); the
  // per-field readU16 calls below self-bounds-check the rest, same as
  // parseProfile's v1/v2 branch (fixture_profile.cpp).
  if (!reader.ensureRemaining(6)) return false;

  uint8_t magic[4];
  if (!reader.readBytes(magic, 4)) return false;
  if (magic[0] != 'S' || magic[1] != 'H' || magic[2] != 'W' || magic[3] != '1') {
    return false;
  }

  // Zero-initialized: readU8 always sets `version` before returning true,
  // but the ESP-IDF toolchain's GCC (12.2.0, -Og) can't see that across the
  // reference-parameter call and flags line 86 as -Werror=maybe-uninitialized
  // otherwise (a false positive -- the host build's GCC/-O0 doesn't hit it).
  uint8_t version = 0;
  if (!reader.readU8(version)) return false;
  if (version != 1 && version != 2) return false;

  uint8_t universeCount;
  if (!reader.readU8(universeCount)) return false;
  if (universeCount > 8) return false;

  uint16_t profileCount, fixtureCount, matrixCount, mdefCount = 0;
  if (!reader.readU16(profileCount)) return false;
  if (!reader.readU16(fixtureCount)) return false;
  if (!reader.readU16(matrixCount)) return false;
  if (version == 2) {
    if (!reader.readU16(mdefCount)) return false;
  }

  out.universeCount = universeCount;

  // Read universe table
  for (int i = 0; i < universeCount; i++) {
    uint8_t transport;
    if (!reader.readU8(transport)) return false;
    if (transport > 3) return false;
    out.transport[i] = static_cast<UniverseTransport>(transport);
  }

  // Read profile table and build profiles
  std::vector<FixtureProfile> profiles;
  for (int i = 0; i < profileCount; i++) {
    uint16_t blobLen;
    if (!reader.readU16(blobLen)) return false;

    if (!reader.ensureRemaining(blobLen)) return false;

    // Extract blob and parse it
    const uint8_t* blobStart = data + reader.getPos();
    FixtureProfile profile;
    if (!parseProfile(blobStart, blobLen, profile)) {
      return false;
    }
    profiles.push_back(profile);

    // Advance reader past blob
    uint8_t dummy;
    for (int j = 0; j < blobLen; j++) {
      if (!reader.readU8(dummy)) return false;
    }
  }

  // Read fixture table (46 bytes per entry)
  for (int i = 0; i < fixtureCount; i++) {
    if (!reader.ensureRemaining(46)) return false;

    uint16_t profileIndex;
    if (!reader.readU16(profileIndex)) return false;
    if (profileIndex >= profiles.size()) return false;

    uint8_t universe = 0;
    if (!reader.readU8(universe)) return false;
    if (universe >= 8) return false;

    uint16_t base;
    if (!reader.readU16(base)) return false;

    uint8_t isHeadByte = 0;
    if (!reader.readU8(isHeadByte)) return false;
    bool isHead = (isHeadByte != 0);

    float posX, posY, posZ;
    if (!reader.readF32(posX)) return false;
    if (!reader.readF32(posY)) return false;
    if (!reader.readF32(posZ)) return false;

    float yaw, pitch, roll;
    if (!reader.readF32(yaw)) return false;
    if (!reader.readF32(pitch)) return false;
    if (!reader.readF32(roll)) return false;

    float panRangeDeg, tiltRangeDeg;
    if (!reader.readF32(panRangeDeg)) return false;
    if (!reader.readF32(tiltRangeDeg)) return false;

    float panCenterNorm, tiltCenterNorm;
    if (!reader.readF32(panCenterNorm)) return false;
    if (!reader.readF32(tiltCenterNorm)) return false;

    uint8_t invertPanByte, invertTiltByte;
    if (!reader.readU8(invertPanByte)) return false;
    if (!reader.readU8(invertTiltByte)) return false;

    PatchEntry entry;
    entry.profile = profiles[profileIndex];
    entry.universe = universe;
    entry.base = base;
    entry.isHead = isHead;

    if (isHead) {
      entry.head.position = {posX, posY, posZ};
      entry.head.orientation = mat3FromEuler(yaw, pitch, roll);
      entry.head.panRangeDeg = panRangeDeg;
      entry.head.tiltRangeDeg = tiltRangeDeg;
      entry.head.panCenterNorm = panCenterNorm;
      entry.head.tiltCenterNorm = tiltCenterNorm;
      entry.head.invertPan = (invertPanByte != 0);
      entry.head.invertTilt = (invertTiltByte != 0);
    }

    out.fixtures.push_back(entry);
  }

  // Read matrix table
  for (int i = 0; i < matrixCount; i++) {
    if (!reader.ensureRemaining(8)) return false;

    uint16_t width, height;
    if (!reader.readU16(width)) return false;
    if (!reader.readU16(height)) return false;

    uint8_t serpentineByte, verticalByte;
    if (!reader.readU8(serpentineByte)) return false;
    if (!reader.readU8(verticalByte)) return false;

    uint8_t orderByte;
    if (!reader.readU8(orderByte)) return false;
    if (orderByte > 5) return false;

    uint8_t startUniverse;
    if (!reader.readU8(startUniverse)) return false;
    if (startUniverse >= 8) return false;

    uint16_t startChannel;
    if (!reader.readU16(startChannel)) return false;

    MatrixMap m;
    m.width = width;
    m.height = height;
    m.serpentine = (serpentineByte != 0);
    m.vertical = (verticalByte != 0);
    m.order = static_cast<ColorOrder>(orderByte);
    m.startUniverse = startUniverse;
    m.startChannel = startChannel;

    out.matrices.push_back(m);
  }

  // Controller (mdef) table -- v2 only.
  for (int i = 0; i < mdefCount; i++) {
    uint16_t blobLen;
    if (!reader.readU16(blobLen)) return false;

    if (!reader.ensureRemaining(blobLen)) return false;

    const uint8_t* blobStart = data + reader.getPos();
    MidiControllerProfile controller;
    if (!parseMidiController(blobStart, blobLen, controller)) {
      return false;
    }
    out.controllers.push_back(controller);

    uint8_t dummy;
    for (int j = 0; j < blobLen; j++) {
      if (!reader.readU8(dummy)) return false;
    }
  }

  return true;
}
