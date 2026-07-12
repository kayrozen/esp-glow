#pragma once

// Vendored Lua 5.4.6 (third_party/lua/, LUA_32BITS=1 — see
// scripts/vendor_lua.sh) is a C library; wrap its headers in extern "C" once
// here so the rest of the Lua/Fennel layer can just #include this file.
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
