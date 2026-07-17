# esp-glow

*Afterglow* firmware for the ESP32-S3: DMX/Art-Net/sACN/WLED output, a pixel-matrix
engine, MIDI/OSC control surfaces, and a Fennel live-coding layer for writing shows.

## How these docs are built

Every page here is hand-written prose, reviewed in PRs like any other code change — no page
is generated from source. [Architecture](architecture.html) and [Bring-up](bring-up.html)
explain *why* the system is shaped the way it is; [Writing a show](authoring.html) teaches
the Fennel authoring layer concept-first; [API reference](reference.html) and
[Grammar reference](grammar.html) document every `glow.*` call and every `.fdef`/`.show`/
`.mdef` keyword in plain language.

What replaces auto-generation is a **completeness guard**, not generated text: CI extracts
the real `glow.*` names straight from `glow_lua_api.cpp` and the real grammar keywords
straight from `provision.cpp`, and fails the build if the reference/grammar pages document
anything different — missing a name that exists, or documenting one that doesn't
(`docs/build/gen-reference.mjs`). A `glow.*` or grammar change without a matching docs update
can't merge; the prose itself stays human-written and human-reviewed.

## Where to start

- New to the project? Read [Architecture](architecture.html) first.
- Writing a show? Start with [Writing a show](authoring.html).
- Looking up a `glow.*` call or a `.fdef`/`.show`/`.mdef` keyword? Jump straight to the
  [API reference](reference.html) or [Grammar reference](grammar.html).
- Bringing up new hardware? See [Bring-up](bring-up.html).
