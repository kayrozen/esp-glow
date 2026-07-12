//
// fennel-check.js — in-browser Fennel compile check (B2 of the Fennel
// scripting UI plan). Runs the REAL vendored fennel.lua against a REAL
// Lua 5.4 VM (wasmoon, WASM) and calls fennel.compileString, so it reports
// exactly the errors the device would -- because it is literally the same
// compiler, not a JS reimplementation.
//
// Scope, honestly: this checks SYNTAX AND COMPILATION, not behaviour.
// glow.* is stubbed (see buildGlowStub below) with every known field name
// as a no-op function, so a call to a *misspelled* field (`glow.st`)
// throws "attempt to call a nil value" at eval time and is caught here
// too -- but argument types, fixture ids, and everything else that only
// the real render loop can validate are NOT checked. Callers must present
// this as a syntax check, not a dry run (see app.js's "Check syntax"
// button copy).
//

import { LuaFactory } from "../vendor/wasmoon-bundle.mjs";

let enginePromise = null;

// Every field GlowLuaApi::install() registers (glow_lua_api.cpp), stubbed
// as a no-op so a real call compiles and runs without error, but a typo'd
// field name is still nil and throws when called -- the one behavioural
// signal this checker can give for free.
const GLOW_STUB_SRC = `
local function noop() return nil end
local function stubtable(names)
  local t = {}
  for _, n in ipairs(names) do t[n] = noop end
  return t
end
glow = stubtable({"set", "aim"})
glow.CAP = setmetatable({}, { __index = function() return 0 end })
glow.cue = stubtable({"define", "go", "release"})
glow.scene = stubtable({"define", "go", "release"})
glow.fx = stubtable({"hue-rotate", "chase", "sweep"})
glow.matrix = stubtable({"pattern", "brightness"})
`;

async function getEngine() {
  if (enginePromise) return enginePromise;
  enginePromise = (async () => {
    const glueUrl = new URL("../vendor/glue.wasm", import.meta.url).href;
    const factory = new LuaFactory(glueUrl);
    const lua = await factory.createEngine();

    const fennelSrc = await (await fetch(new URL("../vendor/fennel.lua", import.meta.url))).text();
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
    // misspelled field like `glow.st` compiles fine either way (it's
    // valid Lua once compiled) and only fails when that call actually
    // executes -- see this file's header. eval catches it for any
    // top-level call; a typo buried inside a function body that's never
    // invoked here still won't be caught, same as it wouldn't be on a
    // bare `eval` on the real device.
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
