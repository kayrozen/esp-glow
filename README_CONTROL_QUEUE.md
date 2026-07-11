# Control-Event Queue: Decoupling Input Tasks from Render

The control-event queue eliminates a cross-core data race between the
input transports and the render loop. Producers push `ControlEvent`s
into a queue; the render task drains them at the top of each frame and
dispatches to `LiveControl`. After this change, every `ShowController`
mutation happens on the render task — the race is gone by construction,
with no lock on the 44 Hz path.

## Why the queue exists

Without the queue, the three input transports call `LiveControl::handle`
directly:

```
web_input_handle_text_frame  →  LiveControl::handle  →  ShowController::go/release
midi_input_handle_byte       →  LiveControl::handle  →  ShowController::go/release
osc_input_handle_packet      →  LiveControl::handle  →  ShowController::go/release
```

Each transport runs in its own FreeRTOS task. The render task reads the
same `ShowController` at 44 Hz on the **other core** of the ESP32-S3.
That is a data race on real hardware: the Xtensa core has a weaker
memory ordering than x86, so a mutation that looks sequenced on the
host can be reordered on the device. Host tests (ASan/UBSan on x86)
cannot catch this because x86's TSO memory model papers over the race.

The queue fixes it by construction:

```
web_input   ─┐
midi_input  ─┼─→  IControlEventQueue  ─→  pumpControlEvents (render task)  ─→  LiveControl::handle
osc_input   ─┘                                                            ─→  ShowController (same core)
```

The transports no longer touch `LiveControl` or `ShowController`. The
only cross-task shared state is the queue itself, which is synchronized
(FreeRTOS queue on device, `std::mutex` on host).

## Why not hand-rolled lock-free

A hand-rolled lock-free ring with manual `std::atomic` and
acquire/release fences would:

- **Pass on x86** (strong TSO ordering hides the bug).
- **Race on Xtensa** (weaker ordering exposes it).
- Be **untestable** — the race only manifests on real hardware, under
  timing-dependent conditions.

Therefore:

- **Device**: `FreeRtosControlEventQueue` (backed by `xQueueSend` /
  `xQueueReceive`). FreeRTOS handles the memory ordering and
  multi-producer safety correctly.
- **Host**: `RingControlEventQueue` (backed by `std::mutex`-guarded
  ring). Correct and deterministic; the concurrency test validates it
  under `-fsanitize=thread`.

No `std::atomic`-based lock-free ring, no custom fences, anywhere.

## API

```cpp
// control_queue.h

class IControlEventQueue {
public:
  virtual ~IControlEventQueue() = default;
  virtual bool push(const ControlEvent& ev) = 0;  // producer; false if full
  virtual bool pop(ControlEvent& ev) = 0;         // consumer; false if empty
};

class RingControlEventQueue : public IControlEventQueue {
public:
  explicit RingControlEventQueue(size_t capacity);
  bool push(const ControlEvent& ev) override;
  bool pop(ControlEvent& ev) override;
  size_t dropped() const;  // count of push() rejections (diagnostic)
  size_t size() const;     // approximate, for tests
};

// Drain pending events and dispatch each via live.handle(ev, t).
// Returns the number dispatched.
int pumpControlEvents(IControlEventQueue& q, LiveControl& live, float t,
                      int maxPerFrame = 64);
```

Both `push()` and `pop()` are **non-blocking**. `push()` returns false
if the queue is full (the event is dropped); `pop()` returns false if
empty.

## Overflow policy

**Reject the newest** event and increment `dropped()`. Dropping an
already-queued event risks stranding a matched press/release pair —
e.g. a flash cue's press is queued, the release is dropped, and the
light stays on until the next press. Rejecting the newest at worst
loses a rapid double-tap, which is a recoverable UX issue, not a
safety issue.

Size the device queue generously (>= 64 slots) so overflow never
happens at human event rates. `dropped()` exists purely to surface a
misconfiguration (e.g. the render task is stalled and not draining).

## Frame-aligned latency

All dispatched events are applied at the **frame's `t`**, not their
arrival time. This adds at most one frame (~22 ms at 44 Hz) of latency
— imperceptible for lighting — and makes cue timing deterministic
(aligned to frame boundaries). A cue triggered at `t=10.003` is
applied at `t=10.022`, not `t=10.003`; the next frame picks it up.

This is a deliberate tradeoff: deterministic timing (every cue fires
on a frame boundary) beats sub-frame latency (which would require
per-event timestamps and a more complex dispatch path).

## Wiring

### Render task (firmware — not yet implemented)

At the top of each frame, before `show.renderFrame(t)`:

```cpp
pumpControlEvents(queue, live, t);   // all controller mutation happens here
show.renderFrame(t);
```

`pumpControlEvents` bounds work per frame (`maxPerFrame` default 64) so
a flood cannot stall the render loop. Leftover events stay queued for
the next frame.

### Input transports (applied to the three scaffolds)

The three transport scaffolds (`web_input.cpp`, `midi_input.cpp`,
`osc_input.cpp`) were changed in exactly one way: the direct
`live.handle(ev, now)` call is replaced with `queue.push(ev)`.

Before:
```cpp
// web_input.cpp
static LiveControl* g_live = nullptr;
void web_input_init(LiveControl& live, ...) { g_live = &live; }
void web_input_handle_text_frame(const char* json, size_t len, float now) {
  // ...parse...
  g_live->handle(ev, now);
}
```

After:
```cpp
// web_input.cpp
static IControlEventQueue* g_queue = nullptr;
void web_input_init(IControlEventQueue& queue, ...) { g_queue = &queue; }
void web_input_handle_text_frame(const char* json, size_t len) {
  // ...parse...
  g_queue->push(ev);
}
```

The same change applies to `midi_input.cpp` and `osc_input.cpp`. The
transports no longer take a `LiveControl&` or a `float now` — they just
parse and push. The `now`/`t` parameter is gone from the transport API
because the pump provides time at dispatch.

### MIDI ISR path

If MIDI bytes arrive from a UART ISR (not a task), use the device-only
`pushFromISR` method on `FreeRtosControlEventQueue`:

```cpp
// In the UART RX ISR:
((FreeRtosControlEventQueue*)g_queue)->pushFromISR(ev);
```

This calls `xQueueSendFromISR` and yields to the render task if it was
blocked on the queue. The base `IControlEventQueue` interface does not
expose `pushFromISR` because it's ISR-specific.

## Device scaffold

`control_queue_freertos.cpp` (excluded from the host build via
`#ifdef ESP_PLATFORM`) provides `FreeRtosControlEventQueue`:

```cpp
class FreeRtosControlEventQueue : public IControlEventQueue {
public:
  explicit FreeRtosControlEventQueue(size_t capacity);  // xQueueCreate
  bool push(const ControlEvent& ev) override;           // xQueueSend(..., 0)
  bool pushFromISR(const ControlEvent& ev);             // xQueueSendFromISR
  bool pop(ControlEvent& ev) override;                  // xQueueReceive(..., 0)
};
```

Non-blocking (`0` ticks) on both ends: producers never block, and the
render task never blocks draining. The `xQueueCreate` / `vQueueDelete`
calls are marked `// TODO` — same convention as the other device
scaffolds.

## Testing

`test_control_queue.cpp` links against the real `LiveControl` +
`ShowController` + deps. Seven tests:

1. **FIFO**: push A,B,C → pop returns A,B,C in order; then pop → false.
2. **Full**: capacity 2, push 3 → third returns false, `dropped()`==1;
   pop yields the first two in order.
3. **Empty**: fresh queue, pop → false.
4. **pump dispatches in order**: CueFlash press → active, release →
   inactive, via a real `ShowController` + `LiveControl`.
5. **maxPerFrame bound**: 5 events, `maxPerFrame=2` → 2 dispatched, 3
   queued; second pump drains the rest.
6. **Equivalence**: same event stream via queue+pump produces the same
   `ShowController` state as calling `live.handle` directly.
7. **Concurrency** (must pass under `-fsanitize=thread`): one
   `std::thread` pushes 10000 events while the main thread pops; assert
   every event received exactly once, in FIFO order, no lost/duplicated,
   TSan reports no race.

### Build flags

`test_control_queue` is built with **`-fsanitize=thread`** (TSan),
not the default `-fsanitize=address,undefined` (ASan/UBSan) used by
the other suites. TSan and ASan cannot be combined in one binary, so
this target gets its own compile rule that compiles all sources in a
single command with TSan flags. The other 9 suites remain on
ASan/UBSan.

Run all tests:
```bash
make test
```

Run just the queue tests (with TSan):
```bash
make test_control_queue && ./test_control_queue
```

## Out of scope

- The actual FreeRTOS task creation / core-pinning / httpd server —
  those are firmware phases (F1, F4). This task provides the queue, the
  pump, and the wiring contract only.
- Priorities, coalescing, or de-duplication of events — a plain FIFO is
  the whole spec.
- Any `std::atomic`-based lock-free ring or custom memory-ordering code
  (forbidden — see above).
- Modifying `live_control.*`, `web_protocol.*`, `show_control.*`, or any
  engine module.
