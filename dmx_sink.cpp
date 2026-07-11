// Device-only. Excluded from the host test build (see Makefile).
// Requires the esp_dmx driver; not buildable/testable on host.
#ifdef ESP_PLATFORM

#include "show.h"

class DmxSink : public IUniverseSink {
public:
  explicit DmxSink(int dmxPort) : port_(dmxPort) {}

  void send(uint8_t /*universeIndex*/, const uint8_t* data, uint16_t len) override {
    // TODO: dmx_write(port_, data, len);
    // TODO: dmx_send(port_);
    (void)data;
    (void)len;
  }

private:
  int port_;
};

#endif  // ESP_PLATFORM
