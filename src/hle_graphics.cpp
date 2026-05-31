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

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// getenv-gated so normal runs stay quiet (matches the project's existing diagnostic pattern).
const bool g_trace_present = std::getenv("REX_HLE_PRESENT_TRACE") != nullptr;

// Frame counter, observed host-side to confirm the override actually sits on the per-frame path.
std::atomic<uint64_t> g_present_count{0};

}  // namespace

// ============================================================================================
// Phase-0 increment 2 (TEMP DIAGNOSTIC) — draw-provenance probe.
//
// Goal: find the guest function(s) that issue the dominant 72.5% draw group so they can be
// rendered natively (report step 3). The command processor is asynchronous (a worker thread
// parses the PM4 ring), the CP_RB_WPTR doorbell is coarse (segment-flush, not per-draw), and the
// guest CPU profile is spin-dominated (sub_821B9270 = 55%) — so none of those isolate the draw
// call. The clean signal is RUNTIME CALL FREQUENCY: the D3D draw is invoked ~1700x per heavy
// frame (all groups) / ~729x for the dominant group (docs/DRAW-GROUP-BREAKDOWN.md), far above any
// ordinary function. Static call-site count is a poor predictor (a high-fan-in function is a leaf
// util — e.g. sub_8242BF10 is memcpy — not a draw loop), so we count at runtime.
//
// Method: the doorbell-RIP + kick-backtrace probe (graphics_system.cpp) walked the mid-draw-loop
// kick stack and mapped the render call tree: render fn -> sub_821CC830 (XDK packet EMITTER) ->
// sub_821C6D58 (reserve/flush-check) -> ... -> sub_821C6600 (kick/doorbell). sub_821CC830 is the
// per-packet emitter called by the title's render functions, so histogramming ctx.lr at its entry
// (the recomp sets ctx.lr to the caller's guest return address before each `bl`) ranks the render
// functions by packet volume — the DOMINANT draw group's render function is the top caller. Map
// the winning lr to its enclosing sub_<addr> offline. Temporary — removed once draw is identified.
// ============================================================================================
namespace {
const bool g_drawprobe = std::getenv("REX_HLE_DRAWPROBE") != nullptr;
constexpr uint64_t kProbeWindow = 60;  // dump every 60 frames
constexpr int kLrSlots = 256;
std::atomic<uint32_t> g_lr[kLrSlots];      // caller guest return address, 0 = empty
std::atomic<uint64_t> g_lrcnt[kLrSlots];   // per-caller hit count
std::atomic<uint64_t> g_resv_calls{0};

// Lock-free open-addressed insert (override runs on guest threads, normal context — but keep it
// cheap on this per-packet hot path).
inline void RecordCaller(uint32_t lr) {
  g_resv_calls.fetch_add(1, std::memory_order_relaxed);
  for (int i = 0; i < kLrSlots; ++i) {
    uint32_t cur = g_lr[i].load(std::memory_order_relaxed);
    if (cur == lr) {
      g_lrcnt[i].fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (cur == 0) {
      uint32_t expected = 0;
      if (g_lr[i].compare_exchange_strong(expected, lr, std::memory_order_relaxed) ||
          g_lr[i].load(std::memory_order_relaxed) == lr) {
        g_lrcnt[i].fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
}
}  // namespace

// sub_821CC830 — a per-frame render pass on the dominant mid-loop kick path (see
// docs/HLE-PHASE0-PROGRESS.md). Measured: called ~1x/frame from a single site, sub_821BEF00+0x2DC
// (caller lr 0x821BF1DC) — so it is NOT a per-packet emitter; it performs the inline draw loop
// itself. This is the leading candidate for the dominant group's render function and the planned
// native-render interception point. Currently pass-through + caller histogram (confirms the
// single caller). Next: confirm it renders pixel shader adf7088205c03df9, then render it natively.
REX_EXTERN(sub_821CC830) {
  if (g_drawprobe) {
    RecordCaller(static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_821CC830(ctx, base);
}

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

  if (g_drawprobe && (n % kProbeWindow == 0)) {
    // Snapshot the caller histogram, sort, log the top callers + the per-frame reserve rate.
    std::vector<std::pair<uint32_t, uint64_t>> v;
    for (int i = 0; i < kLrSlots; ++i) {
      const uint32_t lr = g_lr[i].load(std::memory_order_relaxed);
      if (lr) {
        v.emplace_back(lr, g_lrcnt[i].load(std::memory_order_relaxed));
      }
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    const uint64_t total = g_resv_calls.load(std::memory_order_relaxed);
    std::string s = "[hle-drawprobe] emitter sub_821CC830 calls=" + std::to_string(total) +
                    " (~" + std::to_string(total / n) + "/frame) distinct_callers=" +
                    std::to_string(v.size()) + " top-lr:";
    for (size_t i = 0; i < v.size() && i < 8; ++i) {
      char buf[40];
      std::snprintf(buf, sizeof(buf), " %08X=%llu", v[i].first, (unsigned long long)v[i].second);
      s += buf;
    }
    REXLOG_INFO("{}", s);
  }
}
