# `lua` ESP-IDF component — vendored Lua 5.4.6 (third_party/lua/, patched
# LUA_32BITS=1; see scripts/vendor_lua.sh at the repo root).
#
# Curated to exactly the sources the sandbox needs (see lua_vm.cpp): the
# core VM plus base/math/string/table and their aux lib. No io/os/debug/
# package(loadlib)/utf8/coroutine/ltests/onelua/lua.c — the interactive
# `lua` CLI's main() — are compiled in at all. This is a second line of
# defense on top of the runtime sandboxing in lua_vm.cpp: even a bug that
# tried to open one of those libraries would fail to LINK, not silently
# reopen a hole in the sandbox.
#
# This is a plain library component (no app logic): glow_core links
# against it and calls the vendored lua.h/lauxlib.h/lualib.h API directly
# (see lua_glow_include.h).

set(LUA_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../third_party/lua)

idf_component_register(
    SRCS
        ${LUA_ROOT}/lapi.c
        ${LUA_ROOT}/lcode.c
        ${LUA_ROOT}/lctype.c
        ${LUA_ROOT}/ldebug.c
        ${LUA_ROOT}/ldo.c
        ${LUA_ROOT}/ldump.c
        ${LUA_ROOT}/lfunc.c
        ${LUA_ROOT}/lgc.c
        ${LUA_ROOT}/llex.c
        ${LUA_ROOT}/lmem.c
        ${LUA_ROOT}/lobject.c
        ${LUA_ROOT}/lopcodes.c
        ${LUA_ROOT}/lparser.c
        ${LUA_ROOT}/lstate.c
        ${LUA_ROOT}/lstring.c
        ${LUA_ROOT}/ltable.c
        ${LUA_ROOT}/ltm.c
        ${LUA_ROOT}/lundump.c
        ${LUA_ROOT}/lvm.c
        ${LUA_ROOT}/lzio.c
        ${LUA_ROOT}/lauxlib.c
        ${LUA_ROOT}/lbaselib.c
        ${LUA_ROOT}/lmathlib.c
        ${LUA_ROOT}/lstrlib.c
        ${LUA_ROOT}/ltablib.c
    INCLUDE_DIRS
        ${LUA_ROOT}
)

# Matches how the same sources are compiled for `make test` on the host
# (see the root Makefile's LUA_CFLAGS) -- verified clean under
# -Wall -Wextra -Werror there, so no warning suppression needed here.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -std=c99
    -Wall
    -Wextra
    -Werror
)
