#pragma once
// Force-included before ppc_context.h (which guards PPC_CALL_INDIRECT_FUNC with #ifndef): override the
// indirect-call macro to LOG a null dispatch target (a jump-table case-label the recompiler emitted as a
// call because XenonAnalyse missed the table) instead of crashing at PC=0, then continue. One run reveals
// every missing table target (and how far boot gets) so they can be batch-recovered. See PPCIndirectNull
// in kernel.cpp.
#include <cstdint>

void PPCIndirectNull(uint32_t target, uint32_t lr);

#define PPC_CALL_INDIRECT_FUNC(x)                                              \
    do {                                                                       \
        auto _rexFn = PPC_LOOKUP_FUNC(base, x);                                \
        if (_rexFn) _rexFn(ctx, base);                                         \
        else PPCIndirectNull(static_cast<uint32_t>(x), static_cast<uint32_t>(ctx.lr)); \
    } while (0)
