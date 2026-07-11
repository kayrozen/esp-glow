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

std::vector<uint8_t> ProfileBuilder::encode() const {
  std::vector<uint8_t> blob;

  // Magic
  blob.push_back('P');
  blob.push_back('F');
  blob.push_back('X');
  blob.push_back('1');

  // Version
  blob.push_back(1);

  // Flags
  blob.push_back(0);

  // Footprint
  blob.push_back(footprint);

  // Capability count
  blob.push_back(static_cast<uint8_t>(caps.size()));

  // Name length
  uint8_t nameLen = static_cast<uint8_t>(name.size());
  blob.push_back(nameLen);

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

  return blob;
}
