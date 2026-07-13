#include "profile_encoder.h"

ProfileBuilder& ProfileBuilder::setFootprint(uint8_t f) {
  footprint = f;
  return *this;
}

ProfileBuilder& ProfileBuilder::add(Capability cap, uint8_t coarse, uint8_t fine,
                                     uint8_t def, bool inverted) {
  ChannelMap cm;
  cm.cap = cap;
  cm.coarse = coarse;
  cm.fine = fine;
  cm.defaultValue = def;
  cm.flags = inverted ? 0x01 : 0x00;
  caps.push_back(cm);
  return *this;
}

ProfileBuilder& ProfileBuilder::addRange(uint8_t capIndex, uint8_t dmxFrom, uint8_t dmxTo,
                                         bool continuous, const std::string& rangeName) {
  RangeSpec r;
  r.capIndex = capIndex;
  r.dmxFrom = dmxFrom;
  r.dmxTo = dmxTo;
  r.continuous = continuous;
  r.name = rangeName;
  ranges.push_back(r);
  return *this;
}

std::vector<uint8_t> ProfileBuilder::encode() const {
  std::vector<uint8_t> blob;

  // Magic
  blob.push_back('P');
  blob.push_back('F');
  blob.push_back('X');
  blob.push_back('1');

  // Version: v1 (byte-identical to the pre-v2 encoder) unless ranges are used.
  blob.push_back(ranges.empty() ? 1 : 2);

  // Flags
  blob.push_back(0);

  // Footprint
  blob.push_back(footprint);

  // Capability count
  blob.push_back(static_cast<uint8_t>(caps.size()));

  // Name length
  uint8_t nameLen = static_cast<uint8_t>(name.size());
  blob.push_back(nameLen);

  if (!ranges.empty()) {
    // rangeCount (uint16 LE)
    uint16_t rangeCountVal = static_cast<uint16_t>(ranges.size());
    blob.push_back(rangeCountVal & 0xFF);
    blob.push_back((rangeCountVal >> 8) & 0xFF);
  }

  // Name
  for (char c : name) {
    blob.push_back(static_cast<uint8_t>(c));
  }

  // Capability records
  for (const auto& cm : caps) {
    blob.push_back(static_cast<uint8_t>(cm.cap));
    blob.push_back(cm.coarse);
    blob.push_back(cm.fine);
    blob.push_back(cm.defaultValue);
    blob.push_back(cm.flags);
  }

  if (ranges.empty()) {
    return blob;
  }

  // Range records + trailing name blob (NUL-separated UTF-8, offsets
  // assigned in range order; unnamed ranges get nameOff = 0xFFFF).
  std::vector<uint8_t> nameBlob;
  std::vector<uint16_t> nameOffs;
  nameOffs.reserve(ranges.size());
  for (const auto& r : ranges) {
    if (r.name.empty()) {
      nameOffs.push_back(0xFFFF);
      continue;
    }
    nameOffs.push_back(static_cast<uint16_t>(nameBlob.size()));
    for (char c : r.name) nameBlob.push_back(static_cast<uint8_t>(c));
    nameBlob.push_back(0);
  }

  for (size_t i = 0; i < ranges.size(); ++i) {
    const RangeSpec& r = ranges[i];
    blob.push_back(r.capIndex);
    blob.push_back(r.dmxFrom);
    blob.push_back(r.dmxTo);
    blob.push_back(r.continuous ? 0x01 : 0x00);
    blob.push_back(nameOffs[i] & 0xFF);
    blob.push_back((nameOffs[i] >> 8) & 0xFF);
    blob.push_back(0);  // semantic: reserved, always 0 today
  }

  for (uint8_t b : nameBlob) {
    blob.push_back(b);
  }

  return blob;
}
