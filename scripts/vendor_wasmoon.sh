#!/usr/bin/env bash
# vendor_wasmoon.sh — vendor wasmoon (real Lua 5.4 compiled to WASM) for the
# provisioner's in-browser Fennel compile-check (README_PROVISIONER.md,
# "in-browser compile check" — calls the vendored third_party/fennel/fennel.lua's
# fennel.compileString against a real Lua VM, so it reports exactly the
# errors the device would, with no device attached).
#
# wasmoon's own npm package ships a UMD build (dist/index.js + dist/glue.wasm).
# esbuild's static CJS/ESM interop only kicks in when a UMD module is
# *imported* as a dependency (its `typeof exports === 'object'` check runs
# against esbuild's synthesized `__commonJS` shim, not the real environment)
# — bundling dist/index.js directly as the entry point defeats that, so this
# script re-exports it from a one-line entry module instead. Same
# `--alias:X=<stub>` trick as vendor_editor_bundle.sh for the handful of
# Node builtins (path/fs/child_process/crypto/url/module) wasmoon probes
# and only actually uses on its Node/CLI code path.
#
# Re-run this script to reproduce web/vendor/wasmoon-bundle.mjs and
# web/vendor/glue.wasm from scratch.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cd "$WORK"
npm init -y >/dev/null 2>&1
npm install --no-save wasmoon esbuild >/dev/null

echo 'export { LuaFactory, LuaEngine } from "wasmoon";' > entry.js
echo 'export default function(){};' > empty-shim.js

node_modules/.bin/esbuild entry.js --bundle --format=esm --minify \
  --alias:path=./empty-shim.js --alias:fs=./empty-shim.js \
  --alias:child_process=./empty-shim.js --alias:crypto=./empty-shim.js \
  --alias:url=./empty-shim.js --alias:module=./empty-shim.js \
  --outfile=wasmoon-bundle.mjs

mkdir -p "$OLDPWD/web/vendor"
cp wasmoon-bundle.mjs "$OLDPWD/web/vendor/wasmoon-bundle.mjs"
cp node_modules/wasmoon/dist/glue.wasm "$OLDPWD/web/vendor/glue.wasm"

VERSION="$(node -p "require('./node_modules/wasmoon/package.json').version")"
cat > "$OLDPWD/web/vendor/wasmoon-bundle.LICENSE.txt" << LICEOF
web/vendor/wasmoon-bundle.mjs + glue.wasm are a bundled build of wasmoon
$VERSION (real Lua 5.4 compiled to WebAssembly via Emscripten).
Copyright (c) ceifa and contributors. https://github.com/ceifa/wasmoon
MIT License. Full text: https://opensource.org/licenses/MIT
LICEOF

echo "Vendored web/vendor/wasmoon-bundle.mjs ($(wc -c < "$OLDPWD/web/vendor/wasmoon-bundle.mjs") bytes) + glue.wasm ($(wc -c < "$OLDPWD/web/vendor/glue.wasm") bytes), wasmoon $VERSION."

# web/provisioner-static/fennel-check.js loads this Lua source text and
# runs it against the wasmoon engine above -- a copy, not a symlink (Pages
# artifact upload and some checkouts don't preserve symlinks reliably), so
# it needs to travel with wasmoon in the same vendoring step. Source of
# truth stays third_party/fennel/fennel.lua (see vendor_fennel.sh); this
# is refreshed from it every time this script runs.
if [ -f "$OLDPWD/third_party/fennel/fennel.lua" ]; then
  cp "$OLDPWD/third_party/fennel/fennel.lua" "$OLDPWD/web/vendor/fennel.lua"
  echo "Copied third_party/fennel/fennel.lua -> web/vendor/fennel.lua ($(wc -c < "$OLDPWD/web/vendor/fennel.lua") bytes)."
else
  echo "WARNING: third_party/fennel/fennel.lua not found; web/vendor/fennel.lua not refreshed." >&2
fi
