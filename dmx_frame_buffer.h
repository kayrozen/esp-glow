// dmx_frame_buffer.h — B2: latest-wins single-slot mailbox for one DMX
// universe's frame, shared between the render task (writer, core 1) and the
// dmx_tx task (reader, core 0 -- see dmx_tx_task.h).
//
// Why a mailbox, not a queue: 513 slots at 250 kbaud is ~22.7ms of wire
// time, exactly the render loop's own ~22.7ms period at 44 Hz -- the two
// cadences are in lockstep, not one a clean multiple of the other. A queue
// between them would need an unbounded (or arbitrarily-bounded) backlog
// policy for the case where dmx_tx falls behind; DMX has no such thing as
// "catching up" on stale slot values, so there's nothing a backlog would
// preserve. Only the most recent write ever matters -- render() overwrites
// whatever was last buffered, consumed or not, and dmx_tx always transmits
// the newest frame it can see.
//
// Host-testable (no ESP_PLATFORM dependency): the portMUX shim below is the
// same pattern artnet_router.h uses (a real spinlock on-device, a no-op on
// host, where nothing is actually concurrent).
#pragma once

#include <cstdint>
#include <cstddef>

#include "show.h"  // DMX_UNIVERSE_SIZE

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#else
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) ((void)(mux))
#define portEXIT_CRITICAL(mux) ((void)(mux))
#endif

class DmxFrameBuffer {
public:
  DmxFrameBuffer() = default;

  // Copies `len` bytes (clamped to DMX_UNIVERSE_SIZE) into the shared slot
  // and marks it dirty. Called from the writer (DmxSink::send(), on the
  // render task). Overwrites whatever was buffered before, whether or not
  // it was ever consumed by takeIfDirty() -- see the file header. The
  // critical section is a fixed-size memcpy, never a driver call or
  // anything else that could block -- this is what keeps it cheap enough
  // to call every render frame without stalling core 1.
  void write(const uint8_t* data, uint16_t len);

  // If a frame has been written since the last successful takeIfDirty()
  // call, copies it into `out` (must have room for DMX_UNIVERSE_SIZE
  // bytes), writes its length to `*outLen`, clears the dirty flag, and
  // returns true. Otherwise leaves `out`/`*outLen` untouched and returns
  // false. Called from the reader (dmx_tx task). Calling this with nothing
  // ever written is well-defined: it returns false, same as any other
  // not-dirty call.
  bool takeIfDirty(uint8_t* out, uint16_t* outLen);

private:
  mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  uint8_t data_[DMX_UNIVERSE_SIZE] = {0};
  uint16_t len_ = 0;
  bool dirty_ = false;
};
