// glow_fennel.h — process-wide Lua/Fennel VM lifecycle and the live-coding
// eval entry points (design doc sections 2, 6, 8; README_LUA_FENNEL.md).
//
// There is exactly one Lua VM, owned by the render task (see
// control_queue.h's rationale — the same single-owner discipline applies
// here; see design doc section 9). This file owns that singleton.
#pragma once

#include <cstddef>
#include <cstdint>

#include "eval_queue.h"

class ShowController;
class IMatrixRegistry;
class GlowLuaApi;
namespace glow {
class LuaVM;
}

namespace glow {

// One-time process-wide init: constructs the VM + GlowLuaApi, installs
// glow.*, loads the vendored Fennel compiler, and reclaims the one-time
// compiler-loading garbage (see LuaVM::collectFullyOnce). Call once, on
// the render task, before the render loop starts and before boot.fnl.
//
// capBytes/frameInstrBudget/evalInstrBudget of 0 mean "use LuaVM's
// defaults"; tests override them to get small, fast-to-trip limits.
//
// Returns false (fills errOut) on failure. The caller's contract per the
// design doc is: fall back to a safe blackout, never run with a
// half-initialized VM.
bool glowLuaInit(ShowController& show, IMatrixRegistry* matrices,
                 const char* fennelSrc, size_t fennelSrcLen,
                 char* errOut, size_t errCap,
                 size_t capBytes = 0, int frameInstrBudget = 0, int evalInstrBudget = 0);

// Host tests only: tears down the singleton so the next glowLuaInit starts
// clean. Never called on device — there is exactly one VM for the
// process's lifetime (see design doc section 9: no per-script/per-VM
// teardown-and-recreate).
void glowLuaShutdown();

bool glowLuaReady();
LuaVM& glowLuaVM();
GlowLuaApi& glowLuaApi();

// Drain up to maxPerFrame pending eval submissions from `q` (eval_queue.h),
// evaluating each via glow_lua_eval_fennel and invoking onResult(ctx,
// requestId, ok, err) for each (err is nullptr when ok). The caller turns
// that into an eval_result WS reply (see web_protocol.h's
// buildEvalResultJson). Bounds work per frame so a flood of submissions
// cannot stall the render loop; call from the render task's frame slack,
// the same discipline as pumpControlEvents (control_queue.h).
using EvalResultFn = void (*)(void* ctx, uint32_t requestId, bool ok, const char* err);
int pumpEvalSubmissions(IEvalSubmissionQueue& q, int maxPerFrame, EvalResultFn onResult, void* ctx);

}  // namespace glow

// Compile + evaluate a Fennel source string against the single process-wide
// VM's sandboxed environment (glowLuaInit must have already succeeded).
// Returns false and fills errOut on failure. Never throws — everything
// goes through lua_pcall. This is also the boot.fnl entry point (design doc
// section 8): reading LittleFS is device-only (see scripts_storage.h); the
// caller there is responsible for the "fall back to blackout on error"
// policy, not this function.
bool glow_lua_eval_fennel(const char* src, size_t len, char* errOut, size_t errCap);
