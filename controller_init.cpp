#include "controller_init.h"

void sendControllerInit(const MidiControllerProfile& profile, IRawMidiOutput& output) {
  for (uint8_t i = 0; i < profile.initCount; ++i) {
    const MdefInitBlob& blob = profile.initBlobs[i];
    output.sendRaw(blob.data, blob.len);
  }
}
