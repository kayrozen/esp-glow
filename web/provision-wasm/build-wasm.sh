#!/usr/bin/env bash
#
# build-wasm.sh — compile provision.cpp + deps to WebAssembly.
#
# Two artifacts:
#
#   1. provision-wasm-test.js  — links test_provision.cpp against the
#      provision library, runs under Node. Used by CI to verify the C++
#      tests pass under WASM. (Local-only; not shipped to the editor.)
#
#   2. provision-wasm.js + provision-wasm.wasm — the editor build. Links
#      wasm-glue.cpp (embind bindings) against the provision library,
#      exports parseFixtureDef / compileShow / loadShow / encodeProfile
#      to JavaScript. Shipped to the editor as a static asset.
#
# Usage:
#   ./build-wasm.sh test     # build + run the test binary
#   ./build-wasm.sh editor   # build the editor glue
#   ./build-wasm.sh all      # both (default)
#
# Requires emcc on PATH. Activate emsdk first:
#   source /path/to/emsdk/emsdk_env.sh
#
set -euo pipefail

# Resolve the repo root from this script's location.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
mkdir -p "$BUILD_DIR"

# emsdk env (idempotent — skips if already on PATH).
if command -v emcc >/dev/null 2>&1; then
  : # already available
elif [ -n "${EMSDK:-}" ] && [ -f "$EMSDK/emsdk_env.sh" ]; then
  # shellcheck disable=SC1090
  source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1
fi

if ! command -v emcc >/dev/null 2>&1; then
  echo "error: emcc not found. Activate emsdk first:" >&2
  echo "  source /path/to/emsdk/emsdk_env.sh" >&2
  exit 1
fi

# --- common compile flags ------------------------------------------------

# Match the host Makefile's strictness so WASM builds don't drift.
CXXFLAGS=(
  -std=c++17 -Wall -Wextra -Werror
  -O2
  # No sanitizers under WASM — ASan/UBSan have a non-trivial WASM runtime
  # cost and complicate embind. The host `make test` gate keeps sanitizers.
  -I"$REPO_ROOT"
  # Emscripten's default stack is 64KB. FixtureProfile/MidiControllerProfile/
  # PatchEntry are copied-by-value value types (deliberately -- see their own
  # headers) sized against real-world worst cases (PFX2's MAX_RANGES=192
  # bumped FixtureProfile past 3.7KB), so a handful of them as locals across
  # a few stack frames (e.g. test_provision.cpp building profiles to feed
  # compileShow) is enough to blow a 64KB stack -- an unhelpful bare
  # `Aborted()` via __abort_js, not a clear "stack overflow" message. The
  # host build never sees this (8MB default stack); ESP32 tasks are sized
  # per-task and unaffected. 1MB is free in a browser tab. In this list (not
  # duplicated per-target) so it can't be forgotten in just one of
  # build_test/build_editor -- see LIB_SOURCES' own history for that mistake.
  -s STACK_SIZE=1048576
)

# Sources shared by both test and editor builds. Mirrors the
# PROVISION_SOURCES list in the Makefile, minus provision_main.cpp
# (CLI driver, not needed here) and test_provision.cpp (test-only).
LIB_SOURCES=(
  "$REPO_ROOT/vec_math.cpp"
  "$REPO_ROOT/fixture_profile.cpp"
  "$REPO_ROOT/profile_encoder.cpp"
  "$REPO_ROOT/mdef.cpp"
  "$REPO_ROOT/controller_encoder.cpp"
  "$REPO_ROOT/show_bundle.cpp"
  "$REPO_ROOT/provision.cpp"
)

build_test() {
  echo "==> building provision-wasm-test (Node runner for test_provision.cpp)"
  emcc "${CXXFLAGS[@]}" \
    "${LIB_SOURCES[@]}" \
    "$REPO_ROOT/test_provision.cpp" \
    -o "$BUILD_DIR/provision-wasm-test.js" \
    -s EXIT_RUNTIME=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s ASSERTIONS=2 \
    -s DEMANGLE_SUPPORT=1 \
    -g2
    # ASSERTIONS=2/DEMANGLE_SUPPORT/-g2: this is the test binary, never
    # shipped to the editor (that's build_editor, below, which stays lean).
    # An abort here should say *why* -- e.g. "stack overflow" or an
    # exception's real (demangled) type -- with a symbolized stack pointing
    # at the offending function, instead of a bare Aborted() via __abort_js.
    # Worth keeping on permanently: the cost is only paid when running the
    # test suite under Node, and a silent abort here has already cost more
    # debugging time than this flag will ever add to CI.

  echo "==> running tests under Node"
  node "$BUILD_DIR/provision-wasm-test.js"
}

build_editor() {
  echo "==> building provision-wasm.js + provision-wasm.wasm (editor build)"
  # ENVIRONMENT=web,node so the same artifact loads in the browser (web)
  # and in Node tests (node). The runtime auto-detects; in Node it uses
  # fs to read the .wasm, in the browser it uses fetch.
  emcc "${CXXFLAGS[@]}" \
    "${LIB_SOURCES[@]}" \
    "$SCRIPT_DIR/wasm-glue.cpp" \
    -o "$SCRIPT_DIR/provision-wasm.js" \
    -s WASM=1 \
    -s MODULARIZE=1 \
    -s EXPORT_ES6=1 \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORT_NAME=createProvisionModule \
    -s ENVIRONMENT=web,node \
    --bind

  # The .js wrapper + .wasm binary both land in this folder. The editor
  # imports provision-wasm.js as an ES module; the wrapper auto-loads
  # provision-wasm.wasm from the same directory.
  ls -la "$SCRIPT_DIR/provision-wasm.js" "$SCRIPT_DIR/provision-wasm.wasm"
  echo "==> editor build complete"
}

case "${1:-all}" in
  test)   build_test ;;
  editor) build_editor ;;
  all)    build_test; build_editor ;;
  *) echo "usage: $0 [test|editor|all]"; exit 1 ;;
esac
