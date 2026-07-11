// Device-only. Excluded from the host test build (see Makefile).
// Requires a UDP socket; not buildable/testable on host.
#ifdef ESP_PLATFORM

#include "show.h"
#include <cstring>

class ArtNetSink : public IUniverseSink {
public:
  ArtNetSink(uint32_t bridgeIp, uint16_t bridgePort) : ip_(bridgeIp), port_(bridgePort) {}

  void send(uint8_t universeIndex, const uint8_t* data, uint16_t len) override {
    uint8_t pkt[18 + DMX_UNIVERSE_SIZE];
    const char id[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};
    memcpy(pkt, id, 8);
    pkt[8] = 0x00; pkt[9] = 0x50;         // OpCode 0x5000, little-endian
    pkt[10] = 0; pkt[11] = 14;            // ProtVer hi/lo
    pkt[12] = 0;                          // Sequence
    pkt[13] = 0;                          // Physical
    pkt[14] = universeIndex & 0xFF;       // SubUni
    pkt[15] = (universeIndex >> 8) & 0x7F; // Net
    pkt[16] = (len >> 8) & 0xFF;          // Length hi
    pkt[17] = len & 0xFF;                 // Length lo
    memcpy(pkt + 18, data, len);
    // TODO: open/reuse a UDP socket and sendto(ip_, port_, pkt, 18 + len);
    (void)ip_;
    (void)port_;
  }

private:
  uint32_t ip_;
  uint16_t port_;
};

#endif  // ESP_PLATFORM
