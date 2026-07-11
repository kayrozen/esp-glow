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
)

# Sources shared by both test and editor builds. Mirrors the
# PROVISION_SOURCES list in the Makefile, minus provision_main.cpp
# (CLI driver, not needed here) and test_provision.cpp (test-only).
LIB_SOURCES=(
  "$REPO_ROOT/vec_math.cpp"
  "$REPO_ROOT/fixture_profile.cpp"
  "$REPO_ROOT/profile_encoder.cpp"
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
    -s ALLOW_MEMORY_GROWTH=1

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
