// render_pacing.cpp — implementation of the frame-pacing math.
//
// The whole point: the render loop's only "real logic" is here, host-tested,
// instead of buried in a FreeRTOS task that only runs on hardware.
#include "render_pacing.h"

namespace glow {

PaceResult paceNextFrame(uint32_t targetPeriodUs, uint64_t nowUs,
                         uint64_t prevDeadlineUs) {
  PaceResult r;
  r.nowSec = tickToSeconds(nowUs);

  // Seed the deadline on the first frame (prevDeadlineUs == 0 sentinel).
  // We anchor to the current time so the loop doesn't try to retroactively
  // "make up" the entire history of deadlines.
  uint64_t base = (prevDeadlineUs == 0) ? nowUs : prevDeadlineUs;
  uint64_t next = base + targetPeriodUs;

  if (nowUs >= next) {
    // We are behind. Don't sleep; re-base the deadline off nowUs so the next
    // frame targets one period ahead of the current time (drift-correcting,
    // not catch-up).
    r.behind = true;
    r.sleepUs = 0;
    r.nextDeadlineUs = nowUs + targetPeriodUs;
  } else {
    r.behind = false;
    r.sleepUs = static_cast<int32_t>(next - nowUs);
    r.nextDeadlineUs = next;
  }
  return r;
}

}  // namespace glow
