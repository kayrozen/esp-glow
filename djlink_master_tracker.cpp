#include "djlink_master_tracker.h"

namespace glow {

DjLinkMasterTracker::Entry* DjLinkMasterTracker::find(uint8_t deviceNumber) {
  for (auto& e : entries_) {
    if (e.valid && e.deviceNumber == deviceNumber) return &e;
  }
  return nullptr;
}

const DjLinkMasterTracker::Entry* DjLinkMasterTracker::find(uint8_t deviceNumber) const {
  for (const auto& e : entries_) {
    if (e.valid && e.deviceNumber == deviceNumber) return &e;
  }
  return nullptr;
}

void DjLinkMasterTracker::update(uint8_t deviceNumber, bool isMaster) {
  Entry* e = find(deviceNumber);
  if (e == nullptr) {
    // Prefer an empty slot; if the (small, fixed) table is somehow full
    // of OTHER devices, fall back to overwriting the first slot rather
    // than silently dropping the update -- a real booth never has enough
    // simultaneous devices to hit this in practice.
    for (auto& slot : entries_) {
      if (!slot.valid) {
        e = &slot;
        break;
      }
    }
    if (e == nullptr) e = &entries_[0];
  }
  e->deviceNumber = deviceNumber;
  e->isMaster = isMaster;
  e->valid = true;
}

bool DjLinkMasterTracker::hasKnownMaster() const {
  for (const auto& e : entries_) {
    if (e.valid && e.isMaster) return true;
  }
  return false;
}

bool DjLinkMasterTracker::shouldAccept(uint8_t deviceNumber) const {
  const Entry* e = find(deviceNumber);
  if (e != nullptr && e->isMaster) return true;   // this device IS the known master
  if (!hasKnownMaster()) return true;             // no master known at all -- permissive fallback
  return false;                                   // some OTHER device is the known master
}

}  // namespace glow
