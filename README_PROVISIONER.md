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
    styles.css                     # IDE-dark theme
    static-server.js               # optional Node dev server
    wasm/                          # generated, gitignored, built in CI
      provision-wasm.js
      provision-wasm.wasm
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

## Phase 5b (not yet built)

WebSerial flash to the device. The editor will gain a "Flash to device"
button that:
1. Requests a WebSerial port.
2. Talks the device's serial bootloader protocol (TBD).
3. Writes the `.shw1` bundle to LittleFS.

This is a separate scope — device-side serial protocol, LittleFS image
packing in the browser. Same static-site deployment model.
