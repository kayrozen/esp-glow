#include "fixture_profile.h"
#include <cmath>
#include <algorithm>

bool parseProfile(const uint8_t* data, size_t len, FixtureProfile& out) {
  // Minimum header size: 9 bytes (magic + version + flags + footprint + capCount + nameLen)
  if (len < 9) {
    return false;
  }

  // Check magic
  if (data[0] != 'P' || data[1] != 'F' || data[2] != 'X' || data[3] != '1') {
    return false;
  }

  // Check version
  if (data[4] != 1) {
    return false;
  }

  // Check flags (must be 0)
  if (data[5] != 0) {
    return false;
  }

  uint8_t footprint = data[6];
  uint8_t capCount = data[7];
  uint8_t nameLen = data[8];

  // Check capCount doesn't exceed MAX_CAPS
  if (capCount > MAX_CAPS) {
    return false;
  }

  // Calculate total required size
  size_t totalSize = 9 + nameLen + (5 * capCount);
  if (len < totalSize) {
    return false;
  }

  // Parse capability records
  uint8_t parsedChannelCount = 0;
  for (int i = 0; i < capCount; ++i) {
    size_t capOffset = 9 + nameLen + (i * 5);
    uint8_t type = data[capOffset];
    uint8_t coarseOffset = data[capOffset + 1];
    uint8_t fineOffset = data[capOffset + 2];
    uint8_t defaultValue = data[capOffset + 3];
    uint8_t recFlags = data[capOffset + 4];

    // Validate offsets: coarse must be < footprint
    if (coarseOffset >= footprint) {
      return false;
    }

    // Validate fine offset: if not 0xFF (8-bit), must be < footprint
    if (fineOffset != 0xFF && fineOffset >= footprint) {
      return false;
    }

    out.channels[parsedChannelCount].cap = static_cast<Capability>(type);
    out.channels[parsedChannelCount].coarse = coarseOffset;
    out.channels[parsedChannelCount].fine = fineOffset;
    out.channels[parsedChannelCount].defaultValue = defaultValue;
    out.channels[parsedChannelCount].flags = recFlags;
    parsedChannelCount++;
  }

  out.footprint = footprint;
  out.channelCount = parsedChannelCount;
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
