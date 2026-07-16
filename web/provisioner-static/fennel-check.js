//
// fennel-check.js — in-browser Fennel compile check (B2 of the Fennel
// scripting UI plan). Runs the REAL vendored fennel.lua against a REAL
// Lua 5.4 VM (wasmoon, WASM) and calls fennel.compileString, so it reports
// exactly the errors the device would -- because it is literally the same
// compiler, not a JS reimplementation.
//
// Scope, deliberately: this checks SYNTAX AND COMPILATION ONLY, never
// behaviour or glow.* name correctness. `glow` is bound to an
// accept-everything proxy (see makeProxy below) -- every field access at
// any depth returns another proxy that is itself callable, so ANY
// syntactically valid glow.* usage (glow.bind.pad-xy, glow.led.auto-xy,
// glow.slot, a brand new subtable added to glow_lua_api.cpp tomorrow,
// anything) type-checks with no maintenance here. A misspelled field name
// is NOT caught by this checker -- the device REPL reports that
// immediately, with a real line number, the moment the script actually
// runs. That's the right trade: this file used to hand-list every glow.*
// subtable name, the list drifted out of sync with glow_lua_api.cpp, and
// the drift showed up as REJECTING valid scripts (e.g. any use of
// glow.bind) -- a worse failure than missing a typo, because it makes the
// tool lie about correct code. Do not reintroduce a hand-maintained name
// list here; if browser-side typo-catching is wanted later, generate the
// allow-list from glow_lua_api.cpp's registerFn calls at build time and
// fail CI on drift, never hand-list it again.
//

import { LuaFactory } from "./vendor/wasmoon-bundle.mjs";

let enginePromise = null;

// A value that is simultaneously:
//   * indexable to any depth        (glow.bind.pad-xy)  -> returns another proxy
//   * callable with any args        ((glow.set) 1 :dimmer 0.5) -> returns a proxy
//   * usable as a number/string sink where the code expects a return
// so ANY syntactically-valid glow.* usage type-checks. This is a SYNTAX
// check; API-name correctness is validated on the device, not here.
// Recursion is bounded by the input, not infinite: a proxy is only
// created when the script actually indexes/calls one.
const GLOW_STUB_SRC = `
local function makeProxy()
  local function noop() return makeProxy() end
  return setmetatable({}, {
    __index    = function() return makeProxy() end,
    __call     = function(...) return makeProxy() end,
    -- arithmetic/concat sinks: glow.beat used in (* 0.5 (glow.beat)) etc.
    __add = noop, __sub = noop, __mul = noop, __div = noop,
    __mod = noop, __pow = noop, __unm = noop, __concat = noop,
    __len = function() return 0 end,
    __tostring = function() return "" end,
  })
end
glow = makeProxy()
`;

async function getEngine() {
  if (enginePromise) return enginePromise;
  enginePromise = (async () => {
    const glueUrl = new URL("./vendor/glue.wasm", import.meta.url).href;
    const factory = new LuaFactory(glueUrl);
    const lua = await factory.createEngine();

    const fennelSrc = await (await fetch(new URL("./vendor/fennel.lua", import.meta.url))).text();
    await lua.doString(GLOW_STUB_SRC);
    await lua.doString(`
      local fennel = (function() ${fennelSrc} end)()
      _G.fennel = fennel
    `);
    return lua;
  })();
  return enginePromise;
}

// Compiles `src` (Fennel source text) and returns
// { ok: true } | { ok: false, err: string }. Never throws -- a VM-level
// failure (e.g. the wasm module itself failing to load) also comes back
// as { ok: false, err }, since this is UI-facing, not a fatal error.
export async function checkFennelSyntax(src) {
  try {
    const lua = await getEngine();
    // Using fennel.eval (compile + run), not just fennel.compileString: a
    // top-level form can be syntactically valid Lua once compiled but
    // still throw when actually run (e.g. calling a real Lua stdlib
    // function with the wrong arity) -- see this file's header for why
    // glow.* itself never throws here, proxy or not. A form buried inside
    // a function body that's never invoked here still won't be caught,
    // same as it wouldn't be on a bare `eval` on the real device.
    //
    // wasmoon's doString only surfaces a Lua chunk's FIRST return value to
    // JS (verified against the real vendored wasmoon build -- a Lua
    // `return ok, err` pair does NOT come back as a JS array), so stash
    // both halves as globals and read them back individually instead.
    lua.global.set("__check_src", src);
    await lua.doString(`
      local checkEnv = setmetatable({ glow = glow }, { __index = _G })
      local ok, result = pcall(fennel.eval, __check_src, { env = checkEnv, ["error-pinpoint"] = false })
      __check_ok = ok
      __check_err = ok and nil or tostring(result)
    `);
    const ok = lua.global.get("__check_ok") === true;
    if (ok) return { ok: true };
    const err = lua.global.get("__check_err");
    return { ok: false, err: typeof err === "string" ? err : String(err) };
  } catch (e) {
    return { ok: false, err: `compile-check unavailable: ${e && e.message ? e.message : e}` };
  }
}
