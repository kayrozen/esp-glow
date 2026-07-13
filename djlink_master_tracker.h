// djlink_master_tracker.h — "which player is currently tempo master?"
//
// Beat packets alone never say who is master (see beats.adoc's own
// remark: "it was not possible to tell which player was the master" from
// beat packets). That comes from a separate CDJ Status packet field
// (djlink_parser.h's parseDjLinkMasterFlag). This class is the small,
// pure, bounded table that remembers the last-known master flag per
// device number, so the transport can gate which beat packets actually
// become BeatEvents: "prefer the one flagged as tempo master; ignore the
// rest" (the design doc's T3).
//
// FALLBACK WHEN NO MASTER IS KNOWN
//   Mixer status packets (kind 0x29) also carry a master flag, at a
//   different byte offset, and are NOT parsed by this feature (see
//   djlink_parser.h's header for why) -- so if the mixer is currently
//   tempo master, this table will never learn that. Rather than go
//   silent in that case (worse than the problem it's solving), shouldAccept
//   degrades to "accept beats from anyone" whenever no device is
//   currently known to be master. Once ANY device is confirmed master,
//   only that device's beats are accepted.
#pragma once

#include <cstddef>
#include <cstdint>

namespace glow {

class DjLinkMasterTracker {
public:
  // Record the latest master-flag reading for a device (from a CDJ Status
  // packet). Overwrites any prior reading for the same device number.
  void update(uint8_t deviceNumber, bool isMaster);

  // Whether a beat packet from `deviceNumber` should be accepted:
  //   - true if `deviceNumber` is the currently-known master
  //   - true if no device is currently known to be master at all
  //     (permissive fallback -- see this file's header)
  //   - false if some OTHER device is known to be master
  bool shouldAccept(uint8_t deviceNumber) const;

  bool hasKnownMaster() const;

private:
  static constexpr size_t kCapacity = 8;  // more than enough for a real DJ booth (<=4 CDJs + 1 mixer)

  struct Entry {
    uint8_t deviceNumber = 0;
    bool    isMaster = false;
    bool    valid = false;
  };

  Entry* find(uint8_t deviceNumber);
  const Entry* find(uint8_t deviceNumber) const;

  Entry entries_[kCapacity];
};

}  // namespace glow
