// HLE graphics — variant B incremental migration (escape the LLE command-processor floor).
//
// Background: RexGlue's graphics runtime is Xenia's GPU emulator verbatim — the title's
// statically-linked XDK Direct3D builds PM4 packets into a ring buffer that a single-threaded
// command processor re-translates to Vulkan every frame (~72% of a heavy combat frame; GPU
// 17-26% idle). The escape (docs/HLE-GRAPHICS-SPIKE-REPORT.md) is to stop emulating the GPU and
// render natively by intercepting the title's Direct3D entry points — overriding guest functions
// with host C++ that drives the existing Vulkan backend directly.
//
// This file is the host-side HLE shim layer. It is built INTO the south_park_td executable
// alongside the recompiled guest code, so a strong symbol here replaces the generated *weak*
// alias for any guest function (DEFINE_REX_FUNC emits `__imp__<name>` strong + `<name>` weak;
// the per-module dispatch table also binds the weak `<name>` — verified in
// generated/default/south_park_td_init.cpp:2882 — so a single strong override here captures BOTH
// direct `bl` callers AND indirect/vtable dispatch). The original body stays reachable as
// `__imp__<name>`, so we can pass through to it (true incremental hybrid: migrated funcs render
// natively, everything else stays on the PM4 path, reversible per-function).
//
// Including the generated init header pulls in PPCContext, the REX_* function macros, the
// `__imp__<name>` declarations (DECLARE_REX_FUNC), and <rex/logging.h> (REXLOG_INFO -> run.log).
#include "generated/default/south_park_td_init.h"

#include <atomic>
#include <cstdlib>

namespace {

// getenv-gated so normal runs stay quiet (matches the project's existing diagnostic pattern).
const bool g_trace_present = std::getenv("REX_HLE_PRESENT_TRACE") != nullptr;

// Frame counter, observed host-side to confirm the override actually sits on the per-frame path.
std::atomic<uint64_t> g_present_count{0};

}  // namespace

// ---------------------------------------------------------------------------------------------
// Phase-0 increment 1 — Present/Swap interception (proves the override+build+run+observe loop).
//
// sub_821BFF48 is the title's frame Present/Swap routine: it orchestrates the end-of-frame
// command buffer and calls __imp__VdSwap (the kernel video swap export) exactly once per frame
// (generated/default/south_park_td_recomp.5.cpp:76805). It is called directly from 4 sites and
// via the indirect dispatch table. We wrap it: pass straight through to the original body, and
// observe the per-frame cadence from host C++. No behaviour change — this increment only
// validates that variant-B interception works on a real graphics-path guest function.
// ---------------------------------------------------------------------------------------------
REX_EXTERN(sub_821BFF48) {
  const uint64_t n = g_present_count.fetch_add(1, std::memory_order_relaxed) + 1;

  __imp__sub_821BFF48(ctx, base);  // original Present/Swap (PM4 path, unchanged)

  if (g_trace_present && (n % 60 == 0)) {
    REXLOG_INFO("[hle] present sub_821BFF48 count={}", n);
  }
}
