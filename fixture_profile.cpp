#include "fixture_profile.h"
#include <cmath>
#include <cstring>
#include <algorithm>

namespace {

bool parseCapabilityRecords(const uint8_t* data, size_t capTableOffset, uint8_t capCount,
                            uint8_t footprint, FixtureProfile& out) {
  for (int i = 0; i < capCount; ++i) {
    size_t capOffset = capTableOffset + (i * 5);
    uint8_t type = data[capOffset];
    uint8_t coarseOffset = data[capOffset + 1];
    uint8_t fineOffset = data[capOffset + 2];
    uint8_t defaultValue = data[capOffset + 3];
    uint8_t recFlags = data[capOffset + 4];

    if (coarseOffset >= footprint) return false;
    if (fineOffset != 0xFF && fineOffset >= footprint) return false;

    out.channels[i].cap = static_cast<Capability>(type);
    out.channels[i].coarse = coarseOffset;
    out.channels[i].fine = fineOffset;
    out.channels[i].defaultValue = defaultValue;
    out.channels[i].flags = recFlags;
  }
  return true;
}

// Scans [start, start+len) within `data` for a NUL byte. Returns true if found.
bool hasNulWithin(const uint8_t* data, size_t start, size_t len) {
  const void* found = std::memchr(data + start, 0, len);
  return found != nullptr;
}

}  // namespace

bool parseProfile(const uint8_t* data, size_t len, FixtureProfile& out) {
  // Minimum header size: 9 bytes (magic + version + flags + footprint + capCount + nameLen)
  if (len < 9) {
    return false;
  }

  // Check magic
  if (data[0] != 'P' || data[1] != 'F' || data[2] != 'X' || data[3] != '1') {
    return false;
  }

  uint8_t version = data[4];
  if (version != 1 && version != 2) {
    return false;
  }

  // Check flags (must be 0)
  if (data[5] != 0) {
    return false;
  }

  uint8_t footprint = data[6];
  uint8_t capCount = data[7];
  uint8_t nameLen = data[8];

  if (capCount > MAX_CAPS) {
    return false;
  }

  if (version == 1) {
    size_t totalSize = 9 + nameLen + (5 * capCount);
    if (len < totalSize) {
      return false;
    }

    size_t capTableOffset = 9 + nameLen;
    if (!parseCapabilityRecords(data, capTableOffset, capCount, footprint, out)) {
      return false;
    }

    out.footprint = footprint;
    out.channelCount = capCount;
    out.rangeCount = 0;
    out.rangeNameBlobLen = 0;
    return true;
  }

  // version == 2: header grows by a trailing uint16 rangeCount field.
  if (len < 11) {
    return false;
  }
  uint16_t rangeCount = static_cast<uint16_t>(data[9]) | (static_cast<uint16_t>(data[10]) << 8);
  if (rangeCount > MAX_RANGES) {
    return false;
  }

  size_t capTableOffset = 11 + nameLen;
  size_t rangeTableOffset = capTableOffset + (5 * capCount);
  size_t nameBlobOffset = rangeTableOffset + (7 * static_cast<size_t>(rangeCount));

  if (len < nameBlobOffset) {
    return false;
  }
  size_t nameBlobLen = len - nameBlobOffset;
  if (nameBlobLen > static_cast<size_t>(MAX_RANGE_NAME_BLOB)) {
    return false;
  }

  if (!parseCapabilityRecords(data, capTableOffset, capCount, footprint, out)) {
    return false;
  }

  for (uint16_t i = 0; i < rangeCount; ++i) {
    size_t rOffset = rangeTableOffset + (i * 7);
    uint8_t capIndex = data[rOffset];
    uint8_t dmxFrom = data[rOffset + 1];
    uint8_t dmxTo = data[rOffset + 2];
    uint8_t flags = data[rOffset + 3];
    uint16_t nameOff = static_cast<uint16_t>(data[rOffset + 4]) |
                       (static_cast<uint16_t>(data[rOffset + 5]) << 8);
    uint8_t semantic = data[rOffset + 6];

    if (capIndex >= capCount) return false;
    if (dmxFrom > dmxTo) return false;
    if (nameOff != 0xFFFF) {
      if (nameOff >= nameBlobLen) return false;
      if (!hasNulWithin(data, nameBlobOffset + nameOff, nameBlobLen - nameOff)) return false;
    }

    out.ranges[i].capIndex = capIndex;
    out.ranges[i].dmxFrom = dmxFrom;
    out.ranges[i].dmxTo = dmxTo;
    out.ranges[i].continuous = (flags & 0x01) != 0;
    out.ranges[i].nameOff = nameOff;
    out.ranges[i].semantic = semantic;
  }

  out.footprint = footprint;
  out.channelCount = capCount;
  out.rangeCount = rangeCount;
  out.rangeNameBlobLen = static_cast<uint16_t>(nameBlobLen);
  if (nameBlobLen > 0) {
    std::memcpy(out.rangeNameBlob, data + nameBlobOffset, nameBlobLen);
  }
  return true;
}

const ChannelMap* findCapability(const FixtureProfile& p, Capability cap) {
  for (int i = 0; i < p.channelCount; ++i) {
    if (p.channels[i].cap == cap) {
      return &p.channels[i];
    }
  }
  return nullptr;
}

bool hasCapability(const FixtureProfile& p, Capability cap) {
  return findCapability(p, cap) != nullptr;
}

void applyCapability(const FixtureProfile& p, Capability cap, float norm01,
                     uint8_t* universeBuf, uint16_t base) {
  const ChannelMap* cm = findCapability(p, cap);
  if (!cm) {
    return;
  }

  // Clamp to [0, 1]
  float clamped = std::max(0.0f, std::min(1.0f, norm01));

  if (cm->fine == 0xFF) {
    // 8-bit channel: round to nearest integer
    uint8_t v8 = static_cast<uint8_t>(std::round(clamped * 255.0f));

    // Apply inversion if bit 0 of flags is set
    if (cm->flags & 0x01) {
      v8 = 255 - v8;
    }

    universeBuf[base + cm->coarse] = v8;
  } else {
    // 16-bit channel: truncate (cast) to get floor behavior
    uint16_t v16 = static_cast<uint16_t>(clamped * 65535.0f);

    // Apply inversion if bit 0 of flags is set
    if (cm->flags & 0x01) {
      v16 = 65535 - v16;
    }

    // Write coarse (MSB) and fine (LSB)
    universeBuf[base + cm->coarse] = v16 >> 8;
    universeBuf[base + cm->fine] = v16 & 0xFF;
  }
}

void applyDefaults(const FixtureProfile& p, uint8_t* universeBuf, uint16_t base) {
  for (int i = 0; i < p.channelCount; ++i) {
    const ChannelMap& cm = p.channels[i];
    universeBuf[base + cm.coarse] = cm.defaultValue;

    // For 16-bit channels, write 0 to fine
    if (cm.fine != 0xFF) {
      universeBuf[base + cm.fine] = 0;
    }
  }
}

// --- v2: function ranges ----------------------------------------------------

namespace {

int channelIndexForCap(const FixtureProfile& p, Capability cap) {
  for (int i = 0; i < p.channelCount; ++i) {
    if (p.channels[i].cap == cap) return i;
  }
  return -1;
}

// idx is 0-based among just this capability's own ranges (the order they
// appear in the profile's range table).
const FunctionRange* rangeAtLocalIndex(const FixtureProfile& p, Capability cap, uint8_t idx) {
  int ci = channelIndexForCap(p, cap);
  if (ci < 0) return nullptr;
  uint8_t seen = 0;
  for (uint16_t i = 0; i < p.rangeCount; ++i) {
    if (p.ranges[i].capIndex == ci) {
      if (seen == idx) return &p.ranges[i];
      ++seen;
    }
  }
  return nullptr;
}

uint8_t applyRangeValue(const FunctionRange& r, float value01) {
  if (r.continuous) {
    float clamped = std::max(0.0f, std::min(1.0f, value01));
    float span = static_cast<float>(r.dmxTo - r.dmxFrom);
    return static_cast<uint8_t>(r.dmxFrom + std::round(clamped * span));
  }
  // Discrete: centre of the range, floor((from+to)/2).
  return static_cast<uint8_t>((static_cast<int>(r.dmxFrom) + static_cast<int>(r.dmxTo)) / 2);
}

}  // namespace

bool applyRangeByName(const FixtureProfile& p, Capability cap, const char* name,
                      float value01, uint8_t* universeBuf, uint16_t base) {
  int ci = channelIndexForCap(p, cap);
  if (ci < 0 || name == nullptr) return false;

  for (uint16_t i = 0; i < p.rangeCount; ++i) {
    const FunctionRange& r = p.ranges[i];
    if (r.capIndex != ci || r.nameOff == 0xFFFF) continue;
    const char* rname = reinterpret_cast<const char*>(&p.rangeNameBlob[r.nameOff]);
    if (std::strcmp(rname, name) == 0) {
      universeBuf[base + p.channels[ci].coarse] = applyRangeValue(r, value01);
      return true;
    }
  }
  return false;
}

bool applyRangeByIndex(const FixtureProfile& p, Capability cap, uint8_t rangeIdx,
                       float value01, uint8_t* universeBuf, uint16_t base) {
  const FunctionRange* r = rangeAtLocalIndex(p, cap, rangeIdx);
  if (!r) return false;
  universeBuf[base + p.channels[r->capIndex].coarse] = applyRangeValue(*r, value01);
  return true;
}

size_t rangeCount(const FixtureProfile& p, Capability cap) {
  int ci = channelIndexForCap(p, cap);
  if (ci < 0) return 0;
  size_t n = 0;
  for (uint16_t i = 0; i < p.rangeCount; ++i) {
    if (p.ranges[i].capIndex == ci) ++n;
  }
  return n;
}

const char* rangeName(const FixtureProfile& p, Capability cap, uint8_t rangeIdx) {
  const FunctionRange* r = rangeAtLocalIndex(p, cap, rangeIdx);
  if (!r || r->nameOff == 0xFFFF) return nullptr;
  return reinterpret_cast<const char*>(&p.rangeNameBlob[r->nameOff]);
}

bool rangeIsContinuous(const FixtureProfile& p, Capability cap, uint8_t rangeIdx) {
  const FunctionRange* r = rangeAtLocalIndex(p, cap, rangeIdx);
  return r ? r->continuous : false;
}
