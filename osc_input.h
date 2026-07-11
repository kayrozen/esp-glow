// osc_input.h — device-only UDP OSC receiver -> parseOsc -> LiveControl.
//
// Listens on UDP 8000 (configurable). Each datagram is parsed by the
// host-tested parseOsc() and dispatched to LiveControl::handleOsc() with the
// current show time. Bundles are unwrapped to their first element.
#pragma once

#include "live_control.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct OscInputConfig {
  uint16_t port;        // typically 8000
  LiveControl* live;    // borrowed
};

bool osc_input_start(const struct OscInputConfig* cfg);
void osc_input_stop(void);

#ifdef __cplusplus
}
#endif
