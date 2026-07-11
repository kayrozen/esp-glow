// dmx_sink.h — device-only DMX output sink.
//
// Declares DmxSink, a concrete IUniverseSink that wraps the esp_dmx driver.
// The implementation is in dmx_sink.cpp and is `#ifdef ESP_PLATFORM`-guarded;
// on a host build this header still parses (the class body is hidden) so
// device-facing main.cpp can be written against a stable interface.
#pragma once

#include "show.h"

#ifdef ESP_PLATFORM
#include "esp_dmx.h"
#include <cstdint>

class DmxSink : public IUniverseSink {
public:
  // dmxPort: DMX_NUM_1 / DMX_NUM_2.
  // tx/rx/rts: GPIO numbers. rts drives the RS485 DE+RE pair (tied together).
  DmxSink(dmx_port_t dmxPort, int tx, int rx, int rts);

  // Install the driver. Call exactly once. Returns false on failure.
  bool begin();

  void send(uint8_t universeIndex, const uint8_t* data, uint16_t len) override;

private:
  dmx_port_t port_;
  int tx_, rx_, rts_;
  bool ready_ = false;
};

#endif  // ESP_PLATFORM
