# Provisioning Editor (Phase 5a)

Browser-based `.fdef` / `.show` editor + compiler for the esp-glow
ESP32-S3 DMX lighting project. Runs entirely in the browser via
WebAssembly — no server, no Python, no `esptool`. Deploy to GitHub
Pages as a static site.

This is the ESP-web-flasher pattern: edit a show definition, see live
validation, compile to a `.shw1` bundle, download it. (Phase 5b will
add WebSerial flash direct to the device over USB.)

## Layout

```
web/
  provision-wasm/                  # WASM build of provision.cpp + glue
    build-wasm.sh                  # emcc build script (test + editor targets)
    wasm-glue.cpp                  # embind bindings: parseFixtureDef / compileShow / loadShow
    smoke-test-wasm.mjs            # Node smoke test for the editor build
    provision-wasm.js              # generated editor artifact (gitignored)
    provision-wasm.wasm            # generated editor artifact (gitignored)
    build/                         # test binary output (gitignored)
  provisioner-static/              # the deployable static site
    index.html
    app.js                         # vanilla ESM editor
    import.js                      # fixture importer panel: format dispatch + UI glue
    styles.css                     # IDE-dark theme
    static-server.js               # optional Node dev server
    wasm/                          # generated, gitignored, built in CI
      provision-wasm.js
      provision-wasm.wasm
  shared/
    importers/                     # QLC+/OFL/GDTF -> .fdef (see README_PROVISION.md)
      xml-lite.js, zip-lite.js     # dependency-free XML/ZIP parsing
      model.js                     # intermediate model + emitFdef()
      qlcplus.js, ofl.js, gdtf.js  # per-format parsing + Capability/range mapping
      test-importers.mjs           # `node web/shared/importers/test-importers.mjs`
      testdata/                    # real manufacturer fixture files (see NOTICE.md)
```

The C++ source (`provision.cpp`, `fixture_profile.*`, `profile_encoder.*`,
`show_bundle.*`, `vec_math.*`) is unchanged from the host build — the WASM
port compiles the same files with the same `-Wall -Wextra -Werror` flags.

## How it works

1. `build-wasm.sh test` compiles `provision.cpp` + deps + `test_provision.cpp`
   to a Node-runnable WASM binary. All 14 existing C++ tests run under WASM
   — if they pass, the C++ is good for the browser.
2. `build-wasm.sh editor` compiles `provision.cpp` + deps + `wasm-glue.cpp`
   (embind bindings) to `provision-wasm.{js,wasm}`. The `.js` is an ES
   module that auto-loads the `.wasm`.
3. `provisioner-static/app.js` imports the WASM module, manages a workspace
   (one `.show` + a library of `.fdef` files), and renders a three-pane
   editor: file sidebar / textarea / live preview+diagnostics.

## Editor features

- **File sidebar**: one `.show` file plus a library of `.fdef` files.
  Click to switch, double-click to rename, hover for copy/download/delete.
- **Live preview**: parsed structure of the current file (fixture caps,
  show universes/fixtures/matrices) updates as you type. `.fdef` files
  re-parse on a 250ms debounce; the `.show` file compiles on demand.
- **Diagnostics tab**: per-file ok/error status with details.
- **Bundle size meter**: shows the compiled SHW1 byte size after Compile.
- **Download**: `.fdef` text, `.show` text, or compiled `.shw1` binary.
- **Copy-to-clipboard** for any file's text.
- **Drag-drop import**: drop `.fdef` / `.show` files onto the window.
- **IDE-dark theme** (VS Code-like, `#1e1e1e` background).
- **Fixture importer**: drop a manufacturer's QLC+ `.qxf`, GDTF `.gdtf`, or
  Open Fixture Library `.json` file (or paste a GitHub URL to one) — see
  "Fixture importer" below.

## Fixture importer (QLC+ / OFL / GDTF)

Click the import icon next to "Files" (or drop a `.qxf`/`.gdtf`/`.json`
file anywhere on the window) to bring in a real fixture definition instead
of hand-authoring a `.fdef` from a PDF manual:

1. **Drop a file, or paste a URL** to one on GitHub (QLC+'s and OFL's
   fixture libraries are both public GitHub repos — a `github.com/.../blob/...`
   URL is rewritten to `raw.githubusercontent.com` automatically). Format
   is auto-detected from the extension/content.
2. **Pick a mode** ("8ch basic", "16-bit", ...). Mandatory, no default —
   every real fixture ships several, and patching the wrong one mispatches
   every channel after it.
3. **Review the channel table**: source channel name → mapped Capability
   (a dropdown — fix a wrong guess right there), with its `SLOT`/`RANGE`
   breakdown listed underneath (name, DMX span, discrete/continuous —
   also editable). Unmapped channels are highlighted; the importer never
   drops one silently, even when it can't tell what it does.
4. **Preview the generated `.fdef`** as text, editable, live-updated from
   the table (or vice versa — typing directly in the preview "detaches"
   it from further table edits until you click "Regenerate from table").
5. **Add to fixture library** under a name you choose. It's now a normal
   `.fdef` file in the sidebar — compile, patch, flash like any other.

The mapping logic (QLC+ `Preset`/`Group` heuristics, OFL's structured
capability `type`s, GDTF's `DMXChannel`/`ChannelFunction`/`ChannelSet`
nesting) and the real fixture files it's tested against live in
`web/shared/importers/` — see `README_PROVISION.md` for details and
`web/shared/importers/testdata/NOTICE.md` for where each test fixture
came from.

## Local iteration

Two ways to run locally:

### Option A: static server (matches the deployed Pages site)

```bash
# Build the WASM (requires emsdk on PATH).
cd web/provision-wasm && ./build-wasm.sh all

# Copy artifacts into the static site.
cp provision-wasm.{js,wasm} ../provisioner-static/wasm/

# Serve.
cd ../provisioner-static && node static-server.js 8766
# Open http://localhost:8766/
```

### Option B: Next.js dev (richer UI, same WASM)

A richer React/Next.js version lives in the parent project's `src/`
directory. It uses the same WASM artifacts (copied to `public/wasm/`).
See `/home/z/my-project/src/components/editor/ProvisioningEditor.tsx`.

## Deploy to GitHub Pages

The workflow at `.github/workflows/provisioner.yml`:

1. Installs emsdk, builds the WASM, runs the 14 C++ tests + smoke tests.
2. Copies the WASM artifacts into `web/provisioner-static/wasm/`.
3. Uploads `web/provisioner-static/` as a Pages artifact and deploys.

Trigger: push to `main` touching any of the C++ source files, the WASM
build, the static editor, or the workflow itself. Also `workflow_dispatch`.

## Protocol seam

The `.fdef` and `.show` text formats are the stable seam (see
`README_PROVISION.md`). The WASM compiler is the same C++ code the host
CLI uses; the editor is just a browser UI over it. If the C++ changes,
rebuild the WASM and redeploy — the editor picks up the new behavior
automatically.

## Phase 5b — Web flasher (built)

The editor has a "Flash device" button (`web/provisioner-static/flash.js`)
built on [`esptool-js`](https://github.com/espressif/esptool-js) over Web
Serial:
1. Requests a Web Serial port and detects the chip (refuses anything that
   isn't an ESP32-S3).
2. Fetches the CI-built bootloader/partition-table/otadata/app parts from
   `firmware/flasher_args.json` (offsets are read from that file, never
   hardcoded).
3. Writes the freshly-compiled `.shw1` bundle straight at the raw `show`
   partition's flash offset (see `firmware/partitions.csv`) — no filesystem
   image packing needed, because the show partition isn't a filesystem.

Same static-site deployment model: `.github/workflows/provisioner.yml`
downloads the latest `firmware-esp32s3` artifact and publishes it alongside
the editor at `web/provisioner-static/firmware/`.

## Offline Fennel authoring (Show script tab)

Why this lives here and not just in the device console: the console is
served from the device over `ws://`, and this page is served over HTTPS
(GitHub Pages). Browsers block insecure WebSockets from an HTTPS page, and
the device can't present a valid TLS cert for a local IP — there's no
clean workaround. So this half of the feature is deliberately **offline
authoring + compile-check + bake-in**, not a live connection; see
`README_WEB_CONSOLE.md` for the live REPL half.

A "Show script" entry (`boot.fnl`) sits in the sidebar alongside the
`.show`/`.fdef` files, using the same shared CodeMirror component as the
device console (`web/shared/fennel-editor.js` — bracket matching,
auto-close, Parinfer, Clojure-mode highlighting). Three pieces:

1. **Author offline.** Plain text editing, no device needed; Save/
   Download/Copy work the same as the other file types.
2. **Check syntax** (`web/provisioner-static/fennel-check.js`) runs
   [wasmoon](https://github.com/ceifa/wasmoon) (real Lua 5.4 compiled to
   WASM, vendored at `web/vendor/wasmoon-bundle.mjs` + `glue.wasm`) loaded
   with the *actual* vendored `third_party/fennel/fennel.lua` (copied to
   `web/vendor/fennel.lua` by the same vendoring step) and calls
   `fennel.eval` against it — so a compile error is byte-for-byte the same
   error the device would report, because it's literally the same
   compiler. `glow.*` is stubbed with every real field name as a no-op, so
   a top-level typo like `glow.st` throws too (an unresolved field is
   nil), but **this is a syntax check, not a dry run**: argument types,
   fixture ids, and anything inside a function that's never called at the
   top level aren't checked. The UI says so.
3. **Bake into the flash image.** The Flash modal's "Bake boot.fnl into
   the scripts partition" checkbox calls `boot-image.js`'s
   `buildScriptsImage`, which drives `web/vendor/littlefs-image.wasm` — the
   *real* upstream littlefs C library, pinned to the exact commit
   `joltwallet/littlefs` vendors (firmware's LittleFS component), compiled
   freestanding to wasm32 with clang+wasm-ld (no Emscripten, no libc; see
   `web/littlefs-image/shim.c` and `scripts/vendor_littlefs_image_wasm.sh`
   for how its geometry was read directly out of that pin's source rather
   than guessed). `flash.js` resolves the "scripts" partition's flash
   offset by fetching and parsing this build's actual
   `partition-table.bin` (ESP-IDF's binary format, not hardcoded), then
   writes the built image there alongside the bootloader/app/show parts.
   A fresh board comes up already running the authored show — no
   filesystem tooling needed on the flashing machine.

One page: author patch → author show → flash → lights on.

Scope note: baking only writes `boot.fnl`. Additional scripts saved later
via `glow.save`/the console's Script tab live alongside it on the same
LittleFS partition (this bake step doesn't touch or know about them).
