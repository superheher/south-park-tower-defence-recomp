// HLE graphics — variant B incremental migration (escape the LLE command-processor floor).
//
// Background: RexGlue's graphics runtime is Xenia's GPU emulator verbatim — the title's
// statically-linked XDK Direct3D builds PM4 packets into a ring buffer that a single-threaded
// command processor re-translates to Vulkan every frame (~72% of a heavy combat frame; GPU
// 17-26% idle). The escape (docs/HLE-GRAPHICS-SPIKE-REPORT.md) is to stop emulating the GPU and
// render natively by intercepting the title's render functions — overriding guest functions with
// host C++ that drives the existing Vulkan backend directly.
//
// This file is the host-side HLE shim layer. It is built INTO the south_park_td executable
// alongside the recompiled guest code, so a strong symbol here replaces the generated *weak*
// alias for any guest function (DEFINE_REX_FUNC emits `__imp__<name>` strong + `<name>` weak;
// the per-module dispatch table also binds the weak `<name>` — verified in
// generated/default/south_park_td_init.cpp:2882 — so a single strong override here captures BOTH
// direct `bl` callers AND indirect/vtable dispatch). The original body stays reachable as
// `__imp__<name>`, so we can pass through to it (true incremental hybrid: migrated funcs render
// natively, everything else stays on the PM4 path, reversible per-function).
#include "generated/default/south_park_td_init.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

// getenv-gated so normal runs stay quiet (matches the project's existing diagnostic pattern).
const bool g_trace_present = std::getenv("REX_HLE_PRESENT_TRACE") != nullptr;

// Frame counter, observed host-side to confirm the override actually sits on the per-frame path.
std::atomic<uint64_t> g_present_count{0};

// Reference: skipping sub_821CC830's body blanks the WHOLE frame + stops swaps — it is the D3D
// frame-begin (allocates 7 command-buffer ring segments via sub_821C5BA8, emits one setup packet),
// NOT the dominant draw group. Kept gated for reference. See docs/HLE-PHASE0-PROGRESS.md.
const bool g_stub_cc830 = std::getenv("REX_HLE_STUB_CC830") != nullptr;

}  // namespace

// ============================================================================================
// Phase-1 — render-pass attribution probe (guest-side, deterministic; sidesteps the async wall).
//
// Goal: localize the guest function that emits the dominant 72.5% draw group, so it can be
// overridden and rendered natively (the variant-B escape). RE established that draws are inline
// PM4 stores (no per-draw guest call) and sub_821CC830 is only frame-begin — so the hook target
// is a higher-level RENDER PASS, not a per-draw function. The clean guest-side signal: every
// command-buffer segment flush writes the CP_RB_WPTR doorbell via sub_821C6600 (the "kick";
// the provenance probe measured 100% of doorbell writes come from it, on the render thread,
// synchronously). So the kick-count accrued *during* a pass is proportional to the PM4 bytes
// (≈ draw calls) that pass emitted. We wrap each top-level render pass — the direct children of
// the frame-render sub_82150970, which itself calls Present sub_821BFF48 — and attribute the
// kick-delta across the pass's whole subtree (a thread-local depth guard credits nested
// instrumented calls to the outermost pass only). The pass with the largest per-frame kick-delta
// is the dominant draw emitter = the native-render hook target. Deterministic, dumped to run.log,
// no screenshots, no async correlation. Gated by REX_HLE_PASSPROBE.
// ============================================================================================
namespace {
const bool g_passprobe = std::getenv("REX_HLE_PASSPROBE") != nullptr;

// Visual stub-bisection: REX_HLE_STUB=<guest hex addr> makes that one pass return immediately,
// so a heavy-combat screenshot shows what it rendered. The dominant UI/sprite group's pass is the
// one whose stub removes the on-screen sprites/text/HUD while leaving the rest. (No rebuild needed
// to switch passes — just change the env var.) Kicks are NOT draw-proportional (measured: a fixed
// ~11 doorbell flushes/frame regardless of 180..1301 draws), so visual stubbing is the ground truth.
const uint32_t g_stub_addr =
    std::getenv("REX_HLE_STUB") ? uint32_t(std::strtoul(std::getenv("REX_HLE_STUB"), nullptr, 16)) : 0;

std::atomic<uint64_t> g_kicks{0};  // CP_RB_WPTR doorbell writes (sub_821C6600)

// The pre-Present render passes of sub_82150970, in call order. (sub_821A0B58 is called twice.)
constexpr int kNumPasses = 10;
const char* const kPassNames[kNumPasses] = {
    "sub_8229C360", "sub_8229C650", "sub_821652F0", "sub_821A0B58", "sub_8212DFB8",
    "sub_82167248", "sub_8210DD58", "sub_82150D78", "sub_821BF298", "sub_821BFA10",
};
std::atomic<uint64_t> g_pass_kicks[kNumPasses];
std::atomic<uint64_t> g_pass_calls[kNumPasses];

// Frame-render total: kicks accrued during the whole sub_82150970 (the frame-render fn that calls
// Present). Lets us see how much of the frame the 10 child passes actually cover. Independent guard.
std::atomic<uint64_t> g_frame_kicks{0};
thread_local int g_in_frame = 0;

// Recursion guard: attribute kicks to the OUTERMOST instrumented pass on the call stack, so a
// pass that internally calls another instrumented function does not double-count.
thread_local int g_pass_depth = 0;

// Windowed-delta snapshots (touched only in the Present dump, single render thread -> plain ints).
uint64_t s_last_total = 0, s_last_frame = 0, s_last_present = 0, s_last_pass[kNumPasses] = {0};
}  // namespace

// Strong override of every instrumented pass. When the probe is off this is a zero-behaviour
// pass-through (one predictable branch). NAME's body subtree's kicks are credited to slot IDX.
#define PASS_PROBE(IDX, ADDR, NAME)                                            \
  REX_EXTERN(NAME) {                                                           \
    if (g_stub_addr == uint32_t(ADDR)) {                                       \
      return; /* visual stub-bisection: skip this pass entirely */             \
    }                                                                          \
    if (!g_passprobe || g_pass_depth) {                                        \
      __imp__##NAME(ctx, base);                                               \
      return;                                                                  \
    }                                                                          \
    g_pass_depth = 1;                                                          \
    const uint64_t k0 = g_kicks.load(std::memory_order_relaxed);              \
    __imp__##NAME(ctx, base);                                                 \
    g_pass_kicks[IDX].fetch_add(g_kicks.load(std::memory_order_relaxed) - k0, \
                                std::memory_order_relaxed);                    \
    g_pass_calls[IDX].fetch_add(1, std::memory_order_relaxed);                 \
    g_pass_depth = 0;                                                          \
  }

PASS_PROBE(0, 0x8229C360, sub_8229C360)
PASS_PROBE(1, 0x8229C650, sub_8229C650)
PASS_PROBE(2, 0x821652F0, sub_821652F0)
PASS_PROBE(3, 0x821A0B58, sub_821A0B58)
PASS_PROBE(4, 0x8212DFB8, sub_8212DFB8)
PASS_PROBE(5, 0x82167248, sub_82167248)
PASS_PROBE(6, 0x8210DD58, sub_8210DD58)
PASS_PROBE(7, 0x82150D78, sub_82150D78)
PASS_PROBE(8, 0x821BF298, sub_821BF298)
PASS_PROBE(9, 0x821BFA10, sub_821BFA10)

// Deeper bisection: stub-only gates (no kick attribution) for sub-passes / inline emitters INSIDE
// the structural top-level passes, to localize the dominant sprite group at finer grain.
//   sub_821BC3E8 / sub_821BC738 : the two non-frame-begin children of the main-scene pass
//                                 sub_8212DFB8 (both call the shared sub_821B97C0).
//   sub_821B97C0                : shared helper called by both -> candidate sprite-draw primitive.
//   sub_822C1190/1300/1418      : leaf inline emitters under sub_82167248 (0x822C subsystem).
#define STUB_ONLY(ADDR, NAME)                       \
  REX_EXTERN(NAME) {                                \
    if (g_stub_addr == uint32_t(ADDR)) return;      \
    __imp__##NAME(ctx, base);                       \
  }
STUB_ONLY(0x821BC3E8, sub_821BC3E8)
STUB_ONLY(0x821BC738, sub_821BC738)
STUB_ONLY(0x821B97C0, sub_821B97C0)
STUB_ONLY(0x822C1190, sub_822C1190)
STUB_ONLY(0x822C1300, sub_822C1300)
STUB_ONLY(0x822C1418, sub_822C1418)

// Frame-render fn (calls the 10 passes above, then Present). Measures total kicks under the whole
// frame so we can check the 10 passes' coverage. Separate guard from g_pass_depth.
REX_EXTERN(sub_82150970) {
  if (!g_passprobe || g_in_frame) {
    __imp__sub_82150970(ctx, base);
    return;
  }
  g_in_frame = 1;
  const uint64_t k0 = g_kicks.load(std::memory_order_relaxed);
  __imp__sub_82150970(ctx, base);
  g_frame_kicks.fetch_add(g_kicks.load(std::memory_order_relaxed) - k0, std::memory_order_relaxed);
  g_in_frame = 0;
}

// The kick / CP_RB_WPTR doorbell write. Counting only; pass straight through.
REX_EXTERN(sub_821C6600) {
  if (g_passprobe) {
    g_kicks.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_821C6600(ctx, base);
}

// sub_821CC830 — D3D frame-begin (resets the 7 command-buffer ring segments + one setup packet),
// NOT the dominant draw group (RE: docs/HLE-PHASE0-PROGRESS.md). Pass-through; optional stub.
REX_EXTERN(sub_821CC830) {
  if (g_stub_cc830) {
    return;  // reference: blanks the whole frame + stops swaps (proves it is frame-begin)
  }
  __imp__sub_821CC830(ctx, base);
}

// ---------------------------------------------------------------------------------------------
// Phase-0 increment 1 — Present/Swap interception (proves the override+build+run+observe loop).
//
// sub_821BFF48 is the title's frame Present/Swap routine: it orchestrates the end-of-frame
// command buffer and calls __imp__VdSwap (the kernel video swap export) exactly once per frame
// (generated/default/south_park_td_recomp.5.cpp:76805). We wrap it: pass straight through to the
// original body, observe the per-frame cadence, and dump the pass-attribution histogram.
// ---------------------------------------------------------------------------------------------
REX_EXTERN(sub_821BFF48) {
  const uint64_t n = g_present_count.fetch_add(1, std::memory_order_relaxed) + 1;

  __imp__sub_821BFF48(ctx, base);  // original Present/Swap (PM4 path, unchanged)

  if (g_trace_present && (n % 60 == 0)) {
    REXLOG_INFO("[hle] present sub_821BFF48 count={}", n);
  }

  if (g_passprobe && (n % 120 == 0)) {
    // Windowed deltas over the last ~120 presents -> isolates the CURRENT regime (heavy combat),
    // instead of diluting the heavy signal with thousands of light boot/menu frames.
    const uint64_t total = g_kicks.load(std::memory_order_relaxed);
    const uint64_t frame = g_frame_kicks.load(std::memory_order_relaxed);
    const uint64_t wtotal = total - s_last_total;
    const uint64_t wframe = frame - s_last_frame;
    const uint64_t wpres = (n - s_last_present) ? (n - s_last_present) : 1;
    std::vector<std::pair<int, uint64_t>> v;
    for (int i = 0; i < kNumPasses; ++i) {
      const uint64_t c = g_pass_kicks[i].load(std::memory_order_relaxed);
      v.emplace_back(i, c - s_last_pass[i]);
      s_last_pass[i] = c;
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    std::string s = "[hle-passprobe] win=" + std::to_string(wpres) + "f kicks total=" +
                    std::to_string(wtotal) + " (~" + std::to_string(wtotal / wpres) +
                    "/f) frame970=" + std::to_string(wframe) + " by-pass(win):";
    for (auto& p : v) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), " %s=%llu", kPassNames[p.first], (unsigned long long)p.second);
      s += buf;
    }
    REXLOG_INFO("{}", s);
    s_last_total = total;
    s_last_frame = frame;
    s_last_present = n;
  }
}
