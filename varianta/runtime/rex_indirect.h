#pragma once
// Force-included before ppc_context.h (which guards PPC_CALL_INDIRECT_FUNC with #ifndef): override the
// indirect-call macro to route every guest bctr/bctrl through PPCInvokeGuest (kernel.cpp), which
// bounds-checks the target against the recompiled code range, dispatches mapped targets, and logs+skips
// unmapped/out-of-range ones instead of crashing at PC=0 (a jump-table case-label the recompiler emitted
// as a call because XenonAnalyse missed the table, or a wild pointer from a data divergence). Keeping the
// body a one-line call means tweaking the dispatch policy only recompiles kernel.cpp, not all ~90 TUs.
// See PPCInvokeGuest + PPCIndirectNull in kernel.cpp.
#include <cstdint>

struct PPCContext;
void PPCIndirectNull(uint32_t target, uint32_t lr);
void PPCInvokeGuest(PPCContext& ctx, uint8_t* base, uint32_t target);

#define PPC_CALL_INDIRECT_FUNC(x) PPCInvokeGuest(ctx, base, static_cast<uint32_t>(x))
