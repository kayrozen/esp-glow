#include "usb_midi_packetizer.h"

void packUsbMidiEventPackets(const uint8_t* bytes, size_t len, std::vector<uint8_t>& out) {
  size_t pos = 0;
  while (pos < len) {
    size_t remaining = len - pos;
    uint8_t cin;
    size_t groupLen;
    if (remaining > 3) {
      cin = 0x4;
      groupLen = 3;
    } else {
      // remaining is 1, 2, or 3 -- the final group, CIN encodes exactly
      // how many of the three data bytes are real (0x5/0x6/0x7).
      cin = static_cast<uint8_t>(0x4 + remaining);
      groupLen = remaining;
    }
    out.push_back(cin);  // cable number 0 (high nibble) | CIN (low nibble)
    for (size_t i = 0; i < 3; ++i) {
      out.push_back(i < groupLen ? bytes[pos + i] : 0x00);
    }
    pos += groupLen;
  }
}
