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

    // How many whole periods elapsed since `base` before this frame finally
    // ran. One period late is ordinary scheduling jitter (already captured
    // by `behind`); anything beyond that is a period nothing rendered in at
    // all -- e.g. a GC pause that ate several frame budgets in one gulp.
    // targetPeriodUs == 0 can't express "a period", so there's nothing to
    // count (also avoids a divide by zero).
    if (targetPeriodUs != 0) {
      uint64_t elapsed = nowUs - base;
      uint64_t periods = elapsed / targetPeriodUs;
      r.droppedFrames = (periods > 1) ? static_cast<uint32_t>(periods - 1) : 0;
    } else {
      r.droppedFrames = 0;
    }
  } else {
    r.behind = false;
    r.droppedFrames = 0;
    r.sleepUs = static_cast<int32_t>(next - nowUs);
    r.nextDeadlineUs = next;
  }
  return r;
}

}  // namespace glow
