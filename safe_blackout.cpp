#include "safe_blackout.h"

void safeBlackoutCore(Show& show, ShowController& controller) {
  controller.stopAll();

  static const uint8_t kZeros[DMX_UNIVERSE_SIZE] = {0};
  for (uint8_t u = 0; u < show.universeCount(); ++u) {
    // No-op for Fixture-mode universes (writeRawUniverse filters by mode;
    // see show.cpp) — safe to call unconditionally for every universe.
    show.writeRawUniverse(u, kZeros, DMX_UNIVERSE_SIZE);
  }
}
