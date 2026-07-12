#!/usr/bin/env bash
# vendor_fennel.sh — record exactly how third_party/fennel/fennel.lua was
# produced.
#
# Fennel ships as a self-hosted compiler (Fennel source compiling Fennel
# source) with no pre-built single-file release for a given tag, so it must
# be bootstrapped: build a host `lua` binary, then run Fennel's own Makefile
# with that binary to AOT-compile the compiler into one ~300 KB fennel.lua.
#
# Pin the `1.6.1` release tag. Do not vendor git HEAD — it reports itself as
# "1.7.0-dev" and is not a released, reproducible artifact.
#
# Re-run this script to reproduce third_party/fennel/fennel.lua from scratch.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

if [ ! -f third_party/lua/lapi.c ]; then
  echo "third_party/lua is missing; run scripts/vendor_lua.sh first." >&2
  exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- 1. Build a throwaway host `lua` binary from the vendored sources. ----
# This binary is a build-time tool only; it is never shipped or committed.
# It's fine that it's built with our LUA_32BITS=1 luaconf.h: the pre-flight
# check step below confirms Fennel 1.6.x compiles correctly under 32-bit
# numbers, which is what actually matters (the produced fennel.lua is plain
# Lua source text, not bytecode, so it does not encode the host's number
# representation).
gcc -O2 -std=c99 -o "$WORK/lua" \
  $(ls third_party/lua/*.c | grep -v -E '/(ltests|onelua)\.c$') \
  -lm -ldl

# --- 2. Clone Fennel at the pinned release tag and bootstrap fennel.lua. --
git clone --depth 1 --branch 1.6.1 https://github.com/bakpakin/Fennel.git "$WORK/fennel-src"
make -C "$WORK/fennel-src" LUA="$WORK/lua" fennel.lua

# --- 3. Sanity check: version string + the emit pattern from the design ---
# doc must still work under 32-bit numbers before we commit anything.
VERSION="$("$WORK/lua" -e "local f=dofile(\"$WORK/fennel-src/fennel.lua\"); print(f.version)")"
if [ "$VERSION" != "1.6.1" ]; then
  echo "unexpected Fennel version: $VERSION (expected 1.6.1)" >&2
  exit 1
fi

cat > "$WORK/verify_emit.lua" << 'LUAEOF'
local fennel = dofile(arg[1])
local emitted = {}
local glow = { set = function(id, cap, val)
  emitted[#emitted+1] = string.format("emitted: fixture=%d cap=%-7s val=%.3f", id, cap, val)
end }
local env = setmetatable({ glow = glow, math = math }, { __index = _G })
local src = [[
(fn my-effect [t]
  (glow.set 1 :dimmer (* 0.5 (+ 1 (math.sin (* t 2)))))
  (glow.set 1 :red 1.0))
(my-effect 0)
]]
local ok, err = pcall(fennel.eval, src, { env = env })
assert(ok, err)
assert(#emitted == 2, "expected 2 emitted calls, got " .. #emitted)
print(emitted[1])
print(emitted[2])
LUAEOF
"$WORK/lua" "$WORK/verify_emit.lua" "$WORK/fennel-src/fennel.lua"

# --- 4. Commit the artifact + its license. --------------------------------
mkdir -p third_party/fennel
cp "$WORK/fennel-src/fennel.lua" third_party/fennel/fennel.lua
cp "$WORK/fennel-src/LICENSE" third_party/fennel/LICENSE

echo "Vendored Fennel $VERSION into third_party/fennel/fennel.lua."
