#!/usr/bin/env bash
# vendor_lua.sh — record exactly how third_party/lua/ was produced.
#
# Vendors upstream Lua 5.4.6 (MIT license) and patches luaconf.h so
# lua_Number/lua_Integer are float/int32 instead of double/int64 (see
# README_LUA_FENNEL.md for why this matters on the ESP32-S3's FPU).
#
# Re-run this script to reproduce third_party/lua/ from scratch.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

git clone --depth 1 --branch v5.4.6 https://github.com/lua/lua.git "$WORK/lua"

rm -rf third_party/lua
mkdir -p third_party/lua
cp "$WORK"/lua/*.c "$WORK"/lua/*.h third_party/lua/

# LUA_32BITS must be set by editing luaconf.h, not with -DLUA_32BITS=1 — the
# latter only redefines a macro that luaconf.h has already defined to 0 by
# the time a command-line -D would apply in some build setups, and silently
# leaves lua_Number as double. Edit the source of truth instead.
sed -i 's/^#define LUA_32BITS\s*0/#define LUA_32BITS\t1/' third_party/lua/luaconf.h
grep -n '^#define LUA_32BITS' third_party/lua/luaconf.h

cat > third_party/lua/LICENSE << 'EOF'
Lua 5.4.6 — copied verbatim from the end of lua.h (upstream license text).

Copyright (C) 1994-2023 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
EOF

echo "Vendored Lua 5.4.6 into third_party/lua/ (LUA_32BITS=1)."
echo "Verify: gcc -std=c99 -Ithird_party/lua -c third_party/lua/lapi.c -o /tmp/lapi.o"
