# HIL Test Suite — Implementation Reference

Technical reference for the harness's internals: the telemetry wire format,
the serial query protocol, and how each layer's firmware-side dependency
maps to actual source. See `README.md` for how to run the suite.

## Directory structure

```
tests/hil/
├── conftest.py                # SerialReader, TelemetryLine, fixtures, flash/query helpers
├── fixtures/
│   ├── bundle_a.show          # L3: 2 fixtures, 0 matrices
│   └── bundle_b.show          # L3: 3 fixtures, 1 matrix
├── test_l0_boot.py            # Boot/POST
├── test_l1_dmx.py             # DMX output via ?dmx0
├── test_l2_artnet.py          # Art-Net UDP packet inspection
├── test_l3_show_load.py       # Raw "show" partition load/swap/corrupt
├── test_l4_inputs.py          # Web + OSC round-trip; MIDI skipped
├── test_l5_fennel_repl.py     # Live-coding over WS
├── test_l6_safety.py          # fx_error, infinite loop, OOM
├── test_l7_soak.py            # 10-minute mixed load
├── test_l8_ota_robustness.py  # F5 robustness (mostly skipped -- not implemented)
├── pytest.ini
├── requirements.txt
└── run_hil_tests.sh
```

## The `GLOW-TEST:` telemetry protocol

Emitted only when `CONFIG_GLOW_SELFTEST` is on (`firmware/main/Kconfig.projbuild`).
One line per event, format `GLOW-TEST: <event> key=value key=value...`.
`conftest.py`'s `TelemetryLine.parse()` splits the first token off as
`event` and the rest into a `key_values` dict, with one exception: an
`err=` field (fx_disabled's Lua error message) takes everything to
end-of-line verbatim, since error messages contain spaces that would break
naive whitespace tokenization.

| Event | Fields | Emitted | Firmware source |
|---|---|---|---|
| `boot` | `core`, `hz` | once, after the render task starts | `main.cpp`, right after `render_task_start()` |
| `dmx` | `begin=ok` | once, after `DmxSink::begin()` succeeds | `main.cpp`, `app_main()` |
| `artnet` | `tx=ok` | once, after `ArtNetSink::begin()` succeeds | `main.cpp`, `app_main()` |
| `bundle` | `fixtures`, `matrices` | once per successful bundle load | `main.cpp`, `setup_show_from_bundle()` |
| `scripts` | `mount=ok` | once, after the "scripts" LittleFS partition mounts | `main.cpp`, `setup_lua()` |
| `stats` | `frames`, `behind`, `dropped`, `heap`, `lua_mem` | ~once/sec (every 44 render frames) | `main.cpp`'s `render_tick_hooks` Post phase, via `render_task_get_and_reset_stats()` |
| `fx_disabled` | `name`, `err` | once per newly-disabled Lua effect | `main.cpp`'s `send_fx_error_to_ws` (extended `FxErrorReplyFn`) |

`stats`' `behind` vs `dropped` is the important distinction for L7:
`behind` (existing counter) means this frame missed its deadline but still
ran; `dropped` (new, `render_pacing.h`'s `PaceResult::droppedFrames`) means
one or more whole frame periods elapsed with *no* render call at all --
e.g. a GC pause. `dropped` staying 0 through a 10-minute soak is the GC
regression test; nothing else in the stack measures this.

## The serial query protocol

Sent as a bare line (`<cmd>\n`) to the same UART the console/log already
uses; answered synchronously by a dedicated low-priority FreeRTOS task
(`main.cpp`'s `selftest_query_task`, blocked on `fgetc(stdin)`). Reply
format matches the telemetry protocol (`GLOW-TEST: <event> ...`), where
`<event>` is the command with its `?` stripped.

| Command | Reply | Notes |
|---|---|---|
| `?dmx0` | `GLOW-TEST: dmx0 bytes=<b0>,<b1>,...,<b7>` | First 8 bytes of universe 0 (`Show::universeData(0)`), comma-joined |
| `?state` | `GLOW-TEST: state cues=<id,id,...>` or `cues=none` | Active `ShowController` cue ids (`ShowController::activeCueIds`) |
| `?lua` | `GLOW-TEST: lua mem=<used> highwater=<highwater>` | `LuaVM::memUsed()` / `memHighWater()` |

`conftest.py`'s `SerialReader.query(cmd)` sends the command and returns the
matching `TelemetryLine`, retrying nothing (a missing reply is a real
failure, not flaky serial noise -- see the agent guardrails in README.md).

## The selftest fixture

When `CONFIG_GLOW_SELFTEST` is on and no valid bundle is in the raw `show`
partition, `main.cpp`'s `setup_selftest_fixture()` (in place of the
ordinary demo fallback) patches exactly one fixture -- universe 0, channel
0, a Dimmer capability -- and goes a cue that drives it to `200/255`
(`round(200/255 * 255) == 200` exactly, no rounding slop; see
`test_show.cpp`'s regression test for the same constant). This is what
L0/L1 assert `?dmx0`'s first byte against, and what L5/L6 use as fixture id
0 for `glow.set 0 :dimmer ...` in live-coded effects.

L3 flashing a real bundle temporarily replaces this (a bundle load takes
priority over the selftest fallback, same as it would in a release build)
-- L3's own module-scoped teardown fixture blanks the partition again
afterward so every later layer gets the deterministic fixture back,
regardless of run order.

## The live-coding (WS) protocol

Fully specified in `web_protocol.h`; the harness talks it directly, no
firmware-side test-only shim:

```
UI -> device: {"type":"eval","src":"...","seq":N}
device -> UI (broadcast to every client): {"type":"eval_result","seq":N,"ok":bool,"err":"..."?}

UI -> device: {"type":"script_save","name":"...","src":"..."}   -> reply (this client only): {"type":"scripts","names":[...]}
UI -> device: {"type":"script_list"}                             -> reply: {"type":"scripts","names":[...]}
UI -> device: {"type":"script_load","name":"..."}                -> reply: {"type":"script","name":"...","src":"..."}
UI -> device: {"type":"script_delete","name":"..."}              -> reply: {"type":"scripts","names":[...]}

device -> UI (unsolicited, broadcast): {"type":"fx_error","effect":"<cue>#<index>","err":"..."}
```

`config` is sent automatically on WS connect (`web_input.cpp`'s
`ws_handler`, the initial GET handshake) -- no `hello` round-trip needed.
`conftest.py`'s `ws_eval()`/`ws_recv_json()`/`ws_drain()` wrap the pieces
L5/L6 need.

## Why L7 re-triggers one cue instead of calling `glow.cue.define` repeatedly

`ShowController` has no cue-removal API (`show_control.h`) -- every
`glow.cue.define` call allocates a new cue slot that lives until reboot.
Hammering `define` at the spec's 50-100 msg/s for 10 minutes would itself
manufacture heap/`lua_mem` growth, which is exactly the signal L7 is
trying to catch as a leak -- a false positive from the load generator, not
the firmware. L7 defines one cue once, then only `go()`/`release()`s it
(idempotent, no allocation) for the eval portion of the mixed load. See
`test_l7_soak.py`'s module docstring.

## Adapting this suite to different firmware

- Telemetry format is a straight text protocol; if you add a new
  `GLOW-TEST:` event, no harness change is needed beyond
  `TelemetryLine.parse` (which is generic) and a new
  `read_telemetry(event="...")` call in whatever test needs it.
- The show partition offset (`conftest.py`'s `SHOW_PARTITION_OFFSET`/`SHOW_PARTITION_SIZE`)
  is hand-kept in sync with `firmware/partitions.csv`; if that layout
  changes, update both.
- The `provision` compiler build command in `compile_show_bundle()` mirrors
  `scripts/build_sample_bundle.sh` exactly -- if that script's source list
  changes, update both together.
