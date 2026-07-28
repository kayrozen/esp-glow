#include "dmx_frame_buffer.h"

#include <cstring>

void DmxFrameBuffer::write(const uint8_t* data, uint16_t len) {
  if (len > DMX_UNIVERSE_SIZE) len = DMX_UNIVERSE_SIZE;

  portENTER_CRITICAL(&mux_);
  if (len > 0) std::memcpy(data_, data, len);
  len_ = len;
  dirty_ = true;
  portEXIT_CRITICAL(&mux_);
}

bool DmxFrameBuffer::takeIfDirty(uint8_t* out, uint16_t* outLen) {
  portENTER_CRITICAL(&mux_);
  bool wasDirty = dirty_;
  if (wasDirty) {
    std::memcpy(out, data_, len_);
    *outLen = len_;
    dirty_ = false;
  }
  portEXIT_CRITICAL(&mux_);
  return wasDirty;
}
