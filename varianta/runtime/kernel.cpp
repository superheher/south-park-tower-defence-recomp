// Variant A — kernel/xam import implementations (strong symbols; override the weak trap-stubs in
// import_stubs.gen.cpp). Behaviour reference: third_party/rexglue-sdk/src/ + Xenia.
// ABI: PPC_FUNC(__imp__Name)(PPCContext& ctx, uint8_t* base); args in ctx.r3,r4,...; NTSTATUS/ret in ctx.r3.
// Guest pointers are guest addresses — dereference via PPC_LOAD_*/PPC_STORE_* (base + addr, byte-swapped).
#include "ppc_recomp_shared.h"
#include "kernel.h"
#include "rex_render.h"   // minimal Vulkan renderer (VdSwap present), active under REX_RENDER=1
#include "rex_texture.h"  // Xenos tiled-texture decode (GPU-RESOURCE-BUILD-PLAN piece 2 / cont.25 R0)
#include <xex.h>     // getOptHeaderPtr, XEX_HEADER_*
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <string>
#include <dirent.h>      // case-insensitive VFS path resolution (Xbox FS is case-insensitive)
#include <strings.h>     // strcasecmp
#include <csignal>       // raise(SIGTRAP) for the REX_TEXWATCH gdb hand-off (R0 keystone)
#include <sys/stat.h>

// Lightweight import trace (set REX_KTRACE=0 to silence).
static const bool g_ktrace = []{ const char* e = getenv("REX_KTRACE"); return !e || e[0] != '0'; }();
// REX_TRACEB740: trace sub_8211B740's indirect calls (transitions-init divergence RE).
static const bool g_traceB740 = getenv("REX_TRACEB740") != nullptr;
// REX_INDDUMP: at each DISTINCT null/unmapped indirect-call site, dump the GPRs + the C++ vtable chain
// (object *(r31+4) / *(r3) / *(*(r3)+N)) once, to RE which slot is null (object vs vtable vs method) for
// the menu/frontend INDIRECT-NULL recovery (cont.12(b)). Prod oracle gives the correct values.
static const bool g_inddump = getenv("REX_INDDUMP") != nullptr;
#define KTRACE(...) do { if (g_ktrace) { fprintf(stderr, "[kernel] " __VA_ARGS__); } } while (0)
// Renderer (decoded-frame shortcut): the VC-1 video decoder allocates its frame-pool buffers from a single
// site (LR 0x8244DD2C). Each FRAME is 3 separate allocations in order: Y plane (req 0x101440, pitch 1344,
// 1280x720) then U then V (req 0x40520 each, pitch 672, 640x360) — planar I420. Capture base+size so the
// render thread can present the decoded frame in color (sidesteps PM4 command-buffer enumeration).
uint32_t g_videoBufs[24]; uint32_t g_videoBufSz[24]; std::atomic<int> g_videoBufN{0};

// Called (via rex_indirect.h's PPC_CALL_INDIRECT_FUNC override) when a guest indirect-call target has no
// recompiled function — i.e. a jump-table case-label the recompiler emitted as a call (XenonAnalyse missed
// the table). Log each distinct target once and continue (skip the call) so one run maps every missing
// table target for batch recovery.
// Race indicator for the pivotal "does a clean run build textured content?" experiment: every INDIRECT-NULL
// except the one known-benign site (lr=0x82292D08, an always-null optional callback) is a render-path miss —
// under preemptive NOTOKEN these are the use-before-init races on pooled render-command objects. A run with
// g_nonBenignInd==0 is RACE-FREE; correlate that with whether device+13568 then carries textured draws.
std::atomic<int> g_nonBenignInd{0};
void PPCIndirectNull(uint32_t target, uint32_t lr)
{
    if (lr != 0x82292D08u) g_nonBenignInd.fetch_add(1, std::memory_order_relaxed);
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard<std::mutex> lk(m);
    if (seen.insert(target).second)
        fprintf(stderr, "[INDIRECT-NULL] target=0x%08X (caller lr=0x%08X)\n", target, lr);
}

// Resolve a guest code address to its recompiled host fn via the dispatch table. cont.22: the table now lives
// in a SEPARATE host allocation (g_funcTableBase, allocated out of guest space by runtime.cpp) — NOT at the
// old g_base+PPC_IMAGE_BASE+PPC_IMAGE_SIZE (which was inside the guest 4 GiB mmap, right after the image, and
// got overwritten by the title's own post-image data → garbage fn pointers → SIGSEGV). Byte layout unchanged:
// one PPCFunc* per 4 guest code bytes, byte-indexed by (addr-CODE_BASE)*2. Out-of-code-range addr -> null.
static inline PPCFunc* HostFnAt(uint32_t guestAddr) {
    if (guestAddr < PPC_CODE_BASE || guestAddr >= PPC_CODE_BASE + PPC_CODE_SIZE) return nullptr;
    return *reinterpret_cast<PPCFunc**>(g_funcTableBase + (uint64_t(guestAddr - PPC_CODE_BASE) * 2));
}

// Belt-and-suspenders after the cont.22 relocation: a slot must still hold one of the recompiled function
// pointers. Compute the valid host-pointer range [lo,hi] once from PPCFuncMappings[]; a slot outside it is
// corruption, not a real function. (The table now lives out of guest space, so a guest store can no longer
// corrupt it — this should never fire; if it ever does, something else did. The old in-guest table at
// g_base+0x82930000 was overwritten by the title's post-image data, garbaging the slots for its render/
// frontend fns and crashing every indirect call through them — almost certainly the root of the cont.10–21
// render-path "string-as-code" crashes.) Cheap sanity net; the default boot never trips it.
static bool ValidHostFn(PPCFunc* fn) {
    static const std::pair<uintptr_t, uintptr_t> bounds = []{
        uintptr_t lo = ~uintptr_t(0), hi = 0;
        for (size_t i = 0; PPCFuncMappings[i].host; i++) {
            uintptr_t h = reinterpret_cast<uintptr_t>(PPCFuncMappings[i].host);
            if (h < lo) lo = h;
            if (h > hi) hi = h;
        }
        return std::make_pair(lo, hi);
    }();
    uintptr_t p = reinterpret_cast<uintptr_t>(fn);
    return p >= bounds.first && p <= bounds.second;
}

// Every guest indirect branch/call (bctr/bctrl) routes here via rex_indirect.h's PPC_CALL_INDIRECT_FUNC.
// CRITICAL: bounds-check the target against the recompiled code range BEFORE indexing the function table.
// A recovered switch-table's out-of-range fallback (or any corrupted code pointer) can hand us a wild
// address; PPC_LOOKUP_FUNC would then dereference base + IMAGE_SIZE + (target-CODE_BASE)*2, which for a
// target far below CODE_BASE wraps to a slot gigabytes past the 4 GiB guest mapping — faulting the lookup
// READ itself. So: dispatch only in-range, mapped targets; log+skip everything else (in-range-but-unmapped
// = a still-missing jump-table case; out-of-range = a data divergence the title would also fault on).
// ⚠ The upper bound is CODE_BASE+CODE_SIZE (the table's real extent: 1 PPCFunc* per 4 guest code bytes),
// NOT IMAGE_BASE+IMAGE_SIZE — a target in [code_end 0x825F0C18, image_end 0x82930000) would index PAST the
// table (HostFnAt's own range check enforces this too). Valid code targets are all < code_end, so legit
// dispatch and the default cooperative boot are unaffected. (cont.22: the table now lives out of guest space
// — HostFnAt — so it can't be corrupted by guest stores; ValidHostFn below is a remaining sanity net.)
void PPCInvokeGuest(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    // REX_TRACEB740: log every indirect (bctrl) call made from within sub_8211B740 (its call sites'
    // return-LRs fall in [0x8211B748, 0x8211C000)) WITH the call's return value (the post-bctrl branches
    // gate on it). Reveals how far the 718-line transitions-init handler gets and whether the indirect
    // dispatch to sub_8210AF90 (the 0x828E82A6 setter) ever fires.
    uint32_t lr = static_cast<uint32_t>(ctx.lr);
    bool trace = g_traceB740 && lr >= 0x8211B748u && lr < 0x8211C000u;
    // REX_ADVGATE (cont.25 R2): the intro→menu advance gate. sub_8211B740's middle-block dispatch at
    // lr=0x8211B91C is vtable[33] of the screen-state global *(0x828E83C0) — the path to sub_8210AF90 (the
    // 0x828E82A6 setter). It appears in NEITHER the [b740] success trace NOR as INDIRECT-NULL ⇒ likely not
    // reached (the direct sub_82267630 just before it not returning). Log it the instant it IS reached, with
    // the computed target, to confirm whether it fires + where it points. Default-off.
    static const bool s_advgate = getenv("REX_ADVGATE") != nullptr;
    if (s_advgate && (lr == 0x8211B91Cu || lr == 0x8211B964u)) {
        static std::atomic<int> ag{0}; if (ag.fetch_add(1) < 8)
            fprintf(stderr, "[advgate] REACHED dispatch lr=0x%08X target=0x%08X (vtable method -> %s)\n",
                    lr, target, target == 0x8210AF90u ? "sub_8210AF90 THE SETTER!" : "other");
    }
    // REX_UITRACE (deep build, task #5): capture the D3D dynamic-VB Lock/fill/Unlock method targets in the text
    // renderer sub_821F8E60 (the un-emitted bind+draw). Its 3 indirect call sites by return-lr: vtable+200
    // factory (0x821F906C, returns r30=VB obj), vtable+120 Lock (0x821F9098), vtable+124 Unlock/submit
    // (0x821F90C4, r3=r30). One-shot each: target = the method, r3 = the object, rd(r3) = its vtable. The
    // Unlock/submit method (= vtable[31]) is the stubbed GPU-submit to decode + reconstruct.
    static const bool g_uitrace = getenv("REX_UITRACE") != nullptr;
    if (g_uitrace && (lr == 0x821F906Cu || lr == 0x821F9098u || lr == 0x821F90C4u)) {
        static std::atomic<uint32_t> seen{0};
        uint32_t bit = lr==0x821F906Cu?1u : lr==0x821F9098u?2u : 4u;
        if (!(seen.load() & bit)) { seen.fetch_or(bit);
            auto rd = [&](uint32_t a){ uint32_t b; memcpy(&b, base + a, 4); return __builtin_bswap32(b); };
            const char* site = lr==0x821F906Cu ? "factory(vt+200)" : lr==0x821F9098u ? "Lock(vt+120)" : "Unlock/submit(vt+124)";
            uint32_t r3 = ctx.r3.u32, vt = r3 ? rd(r3) : 0;
            fprintf(stderr, "[uitrace] %-20s lr=0x%08X target=sub_%08X | r3(obj)=0x%08X vtable=0x%08X vt[120]=0x%08X vt[124]=0x%08X\n",
                    site, lr, target, r3, vt, vt?rd(vt+120):0, vt?rd(vt+124):0);
        }
    }
    // REX_TASKDIAG (cont.22 loop-iter 8): classify the transition's leaf async sub-ops. sub_82248010 (the
    // per-child state machine) advances each stage on a polymorphic sub-op call returning status 3 / waits on
    // status 2; log those calls (lr inside sub_82248010 = [0x82248010,0x82248260)) + the returned status, to
    // see which sub-op stays "pending" (status 2) and whether its target is I/O (sub_8244xxxx) or GPU/render
    // (sub_821Bxxxx/821Cxxxx) code — that classifies the completion variant A must drive. Gated, default-off.
    static const bool g_taskdiag = getenv("REX_TASKDIAG") != nullptr;
    bool taskdiag = g_taskdiag && lr >= 0x82248010u && lr < 0x82248260u;
    // REX_POLLDIAG (cont.22 loop-iter 6): the intro→menu transition worker blocks in sub_822481E0's async-
    // completion delay-poll `while(*(r30+136)∉{1,12}){ (*(r31-4412))->vtable[8](); KeDelayExecutionThread(10);}`.
    // The pump call is the indirect bctrl at guest lr=0x82248224. Log the live ctx there (frame-reads of the
    // blocked worker are unreliable — non-volatiles are saved/restored down the call chain) to identify the
    // stuck state value + the pumped subsystem's object/vtable. Gated, default boot unaffected.
    static const bool g_polldiag = getenv("REX_POLLDIAG") != nullptr;
    if (g_polldiag && lr == 0x82248224u) {
        static std::mutex pm; static int pn = 0; static uint32_t seen = 0; static bool doneLog = false;
        std::lock_guard<std::mutex> lk(pm);
        auto rd = [&](uint32_t a){ uint32_t b; memcpy(&b, base + a, 4); return __builtin_bswap32(b); };
        uint32_t r30 = ctx.r30.u32, st = rd(r30 + 136);
        if (pn++ < 8) {
            uint32_t r31 = ctx.r31.u32, cvt = rd(r30), poll9 = cvt ? rd(cvt + 36) : 0;
            fprintf(stderr, "[polldiag] #%d state=*(r30+136)=%u *(child+208)=%u(prod=1=ready) child[0]_vtable=0x%08X poll9=sub_%08X | r30=0x%08X r31=0x%08X\n",
                    pn, st, rd(r30 + 208), cvt, poll9, r30, r31);
        }
        // Decisive: does the loader EVER reach done (state 1 or 12), or is it truly stuck cycling? Track each
        // NEW state value (uncapped) + a one-time DONE flag, over a long run — answers "stuck vs merely slow".
        uint32_t bit = st < 31 ? (1u << st) : 0x80000000u;
        if (!(seen & bit)) { seen |= bit; fprintf(stderr, "[polldiag] NEW state=%u (mask=0x%X) at poll #%d\n", st, seen, pn); }
        if ((st == 1 || st == 12) && !doneLog) { doneLog = true; fprintf(stderr, "[polldiag] *** LOADER REACHED DONE state=%u after %d polls ***\n", st, pn); }
        // task #7 (deep build): survey ALL 20 children (cont.22 focused on child[0]). child[i]=0x82657578+i*216,
        // state @+136, ready-flag @+208. Periodic dump (every 200 polls) → which children COMPLETE (state 1/12)
        // vs STICK, and what each loads (the leaf resource type) — pinpoints the SPECIFIC stuck resource.
        static int surveyN = 0;
        if ((pn % 200) == 1 && surveyN < 6) { surveyN++;
            char buf[640]; size_t o=0; int done=0,stuck=0;
            for (int i=0;i<20;i++){ uint32_t c=0x82657578u + i*216u; uint32_t cs=rd(c+136), cr=rd(c+208);
                bool d=(cs==1||cs==12); if(d)done++; else stuck++;
                o += snprintf(buf+o, sizeof(buf)-o, " [%d]s%u/r%u%s", i, cs, cr, d?"D":""); }
            fprintf(stderr, "[children] poll#%d: %d done / %d stuck:%s\n", pn, done, stuck, buf);
        }
    }
    // (cont.22 loop-iter 6: a REX_POLLFORCE experiment that forced *(r30+136)=done + skipped the pump did NOT
    // clear the transition — the state re-cycled and the title stalled, i.e. forcing an incomplete async result
    // breaks the worker. Removed: stopgaps don't substitute for real completion here. See NIGHT-LOG cont.22.)
    if (target >= PPC_CODE_BASE && target < (PPC_CODE_BASE + PPC_CODE_SIZE))
    {
        PPCFunc* fn = HostFnAt(target);
        if (ValidHostFn(fn)) {
            fn(ctx, base);
            if (trace) fprintf(stderr, "[b740] bctrl lr=0x%08X -> 0x%08X returned r3=0x%08X%s\n",
                               lr, target, ctx.r3.u32, target == 0x8210AF90u ? "  <== sub_8210AF90!" : "");
            if (taskdiag) { static std::atomic<int> tn{0}; if (tn.fetch_add(1) < 60)
                fprintf(stderr, "[task] lr=0x%08X sub-op->0x%08X status=%u\n", lr, target, ctx.r3.u32); }
            return;
        }
        if (fn) {   // non-null but outside the valid host-fn range = a CORRUPTED table slot (cont.22) —
            static std::mutex cm; static std::unordered_set<uint32_t> cseen;   // log once/target, then skip
            std::lock_guard<std::mutex> clk(cm);
            if (cseen.insert(target).second)
                fprintf(stderr, "[DISPATCH-CORRUPT] guest 0x%08X slot=%p (lr=0x%08X) — table overwritten, skipped\n",
                        target, (void*)fn, lr);
        }
    }
    // Recompiler jump-table-gap recovery: XenonAnalyse misidentified some switch jump-tables as code and
    // decoded the table dwords as instructions, absorbing the FIRST case handler (which sits immediately
    // after the table) into that mis-decoded "function" so it was never emitted as its own callable entry.
    // The dispatcher's `bctr jumptable[r3]` then lands on an unregistered in-range address -> INDIRECT-NULL.
    // Re-supply those tiny handlers here, transcribed from the recomp's own decode of the absorbed bytes, so
    // the dispatch resolves correctly. (Always-on: each target is otherwise a guaranteed INDIRECT-NULL; none
    // is hit on the default cooperative boot, which never reaches these paths — so the default boot is
    // unaffected.) sub_82367BD8 = `li r3,0 ; blr` — case 0 of sub_82367B88's 12-way switch (table at
    // 0x82367BA8 was mis-decoded as sub_82367BA8); reached from 4 sites in the sub_82368xxx format chain.
    switch (target) {
        case 0x82367BD8u: ctx.r3.s64 = 0; return;   // li r3,0 ; blr
    }
    if (trace) fprintf(stderr, "[b740] bctrl lr=0x%08X -> 0x%08X (NULL/unmapped, SKIPPED)\n", lr, target);
    if (g_inddump) {   // dump GPRs + the C++ vtable chain once per distinct null/unmapped call site
        static std::mutex dm; static std::unordered_set<uint32_t> dseen;
        std::lock_guard<std::mutex> dlk(dm);
        if (dseen.insert(lr).second) {
            auto rd = [&](uint32_t a){ uint32_t b; memcpy(&b, base + a, 4); return __builtin_bswap32(b); };
            uint32_t r3 = ctx.r3.u32, r31 = ctx.r31.u32, obj = rd(r31 + 4);
            fprintf(stderr, "[inddump] lr=0x%08X tgt=0x%08X | r3=0x%08X r10=0x%08X r11=0x%08X r30=0x%08X r31=0x%08X | "
                    "*(r31+4)=0x%08X *(r3)=0x%08X *(*r3+0)=0x%08X *(*r3+4)=0x%08X *(obj)=0x%08X *(obj+0)=0x%08X\n",
                    lr, target, r3, ctx.r10.u32, ctx.r11.u32, ctx.r30.u32, r31,
                    obj, rd(r3), rd(rd(r3) + 0), rd(rd(r3) + 4), (obj ? rd(obj) : 0), (obj ? rd(rd(obj)) : 0));
        }
    }
    PPCIndirectNull(target, lr);
}

// ---- guest virtual memory manager ------------------------------------------------------------------
// Backs Nt{Allocate,Query,Free}VirtualMemory. The full 4 GiB is already mmap'd lazily by the host
// (runtime.cpp), so a "commit" is essentially free — we only TRACK reservations so that
// NtQueryVirtualMemory can answer (the guest RtlCreateHeap = sub_8244B380 reserves a region then
// queries it back). Reference: rexglue-sdk src/kernel/xboxkrnl/xboxkrnl_memory.cpp + the Xenia heap
// model. ⚠ Xbox 360 ABI: NtAllocateVirtualMemory has 4 (+1 debug) args and NO ProcessHandle —
// r3=*BaseAddress, r4=*RegionSize, r5=AllocType, r6=Protect, r7=DebugMemory (NOT the Win32 6-arg form).
namespace {
constexpr uint32_t kMemCommit = 0x1000, kMemReserve = 0x2000, kMemDecommit = 0x4000;
constexpr uint32_t kMemRelease = 0x8000, kMemFree = 0x10000, kMemPrivate = 0x20000;
constexpr uint32_t kMemLargePages = 0x20000000;
constexpr uint32_t kPageReadWrite = 0x04;
constexpr uint32_t kStatusSuccess = 0x00000000;
constexpr uint32_t kStatusInvalidParameter = 0xC000000Du;
constexpr uint32_t kStatusMemoryNotAllocated = 0xC00000A0u;

struct VRegion { uint32_t size; bool reserved; bool committed; uint32_t protect; };
std::map<uint32_t, VRegion> g_regions;   // base -> region (kept non-overlapping)
uint32_t g_virtNext = 0x00010000;        // bump from LOW memory so the title's big heap reserve
                                          // (everything below its ~0x70000000 stack) ends below the
                                          // image (0x82000000) instead of overwriting it.
                                          // NOTE (2026-06-01): the .ptc-load device corruption that used to
                                          // overwrite the live GPU device here is FIXED — it was the
                                          // NtFreeVirtualMemory(MEM_DECOMMIT) writeback returning the whole
                                          // reservation instead of the decommitted sub-range (see that fn
                                          // below + NIGHT-LOG "DECOMMIT WRITEBACK"). It was base-independent
                                          // (a heap UnCommittedRanges divergence), so this base stays 0x10000.
std::mutex g_memMutex;                    // guards g_regions / g_virtNext (multiple guest threads alloc)

inline uint32_t RoundUp(uint32_t v, uint32_t a) { return (v + (a - 1)) & ~(a - 1); }

// Largest tracked region whose [base, base+size) contains addr.
std::pair<uint32_t, VRegion*> FindRegion(uint32_t addr)
{
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin()) return {0, nullptr};
    --it;
    if (addr >= it->first && addr < it->first + it->second.size) return {it->first, &it->second};
    return {0, nullptr};
}
} // namespace

// NTSTATUS NtAllocateVirtualMemory(*BaseAddress r3, *RegionSize r4, AllocType r5, Protect r6, Debug r7)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    std::lock_guard<std::mutex> lk(g_memMutex);
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32;
    uint32_t allocType = ctx.r5.u32, protect = ctx.r6.u32;
    if (!pBase || !pSize) { ctx.r3.u64 = kStatusInvalidParameter; return; }
    uint32_t reqBase = PPC_LOAD_U32(pBase);
    uint32_t reqSize = PPC_LOAD_U32(pSize);
    if (reqSize == 0) { ctx.r3.u64 = kStatusInvalidParameter; return; }

    uint32_t pageSize = (allocType & kMemLargePages) ? 0x10000u : 0x1000u;
    uint32_t gbase, size;
    if (reqBase) {                                    // caller chose the base (e.g. commit-on-reserve)
        gbase = reqBase & ~(pageSize - 1);
        size = RoundUp(reqSize + (reqBase - gbase), pageSize);
    } else {                                          // kernel chooses — 64 KiB granular bump
        size = RoundUp(reqSize, 0x10000);
        g_virtNext = RoundUp(g_virtNext, 0x10000);
        gbase = g_virtNext;
        g_virtNext += size;
    }
    if (protect == 0) protect = kPageReadWrite;

    auto [rb, r] = FindRegion(gbase);
    if (r) {                                          // grow/recommit an existing reservation
        if (allocType & kMemReserve) r->reserved = true;
        if (allocType & kMemCommit) { r->reserved = true; r->committed = true; }
        r->protect = protect;
        if (gbase + size > rb + r->size) r->size = (gbase - rb) + size;
    } else {
        VRegion nr{};
        nr.size = size;
        nr.reserved = (allocType & (kMemReserve | kMemCommit)) != 0;
        nr.committed = (allocType & kMemCommit) != 0;
        nr.protect = protect;
        g_regions[gbase] = nr;
    }
    PPC_STORE_U32(pBase, gbase);
    PPC_STORE_U32(pSize, size);
    if (ctx.lr == 0x8244DD2Cull) {                 // VC-1 decoder frame-pool allocation site (Y/U/V triplets)
        int i = g_videoBufN.load();
        if (i < 24) { g_videoBufs[i] = gbase; g_videoBufSz[i] = reqSize; g_videoBufN.store(i + 1); }
    }
    KTRACE("NtAllocateVirtualMemory(req=0x%X sz=0x%X type=0x%X prot=0x%X) -> base=0x%X size=0x%X lr=0x%llX\n",
        reqBase, reqSize, allocType, protect, gbase, size, (unsigned long long)ctx.lr);
    ctx.r3.u64 = kStatusSuccess;
}

// ---- Kernel pool allocator: ExAllocatePoolTypeWithTag / ExAllocatePoolWithTag / ExFreePool -----------
// The default weak stubs return NULL, so EVERY guest pool allocation fails with E_OUTOFMEMORY. That is
// fatal for the menu/frontend: e.g. its singleton-manager construction (getter sub_8248F4C8 -> sub_82497720
// -> sub_82497678 -> sub_824A5E50 -> sub_824A5DD0 -> ExAllocatePoolTypeWithTag) returned 0x8007000E, so the
// manager object was never built; the menu then virtual-calls through its null pointer (sub_82292CE0) =>
// the INDIRECT-NULL cascade + SIGSEGV (cont.12(c)). Implement a real pool: a fine-grained bump allocator
// over arenas carved from the guest VM bump cursor. The title rarely frees pool during a session, so
// ExFreePool is a no-op leak (acceptable for bring-up); every block is fresh demand-zero guest memory.
namespace {
std::mutex g_poolMutex;
uint32_t g_poolCur = 0, g_poolEnd = 0;
uint32_t PoolAlloc(uint32_t size) {
    if (!size) return 0;
    std::lock_guard<std::mutex> lk(g_poolMutex);
    size = RoundUp(size, 16);
    if (g_poolCur == 0 || g_poolCur + size > g_poolEnd) {        // carve a new arena from the guest VM
        uint32_t arena = RoundUp(size > 0x800000u ? size : 0x800000u, 0x10000);   // >= 8 MiB
        std::lock_guard<std::mutex> mk(g_memMutex);
        g_virtNext = RoundUp(g_virtNext, 0x10000);
        g_poolCur = g_virtNext; g_poolEnd = g_virtNext + arena; g_virtNext += arena;
        g_regions[g_poolCur] = VRegion{arena, true, true, kPageReadWrite};
    }
    uint32_t p = g_poolCur; g_poolCur += size; return p;
}
} // namespace
// PVOID ExAllocatePoolTypeWithTag(SIZE_T NumberOfBytes r3, ULONG Tag r4, ULONG PoolType r5)
PPC_FUNC(__imp__ExAllocatePoolTypeWithTag) { ctx.r3.u64 = PoolAlloc(ctx.r3.u32); }
// PVOID ExAllocatePoolWithTag(SIZE_T NumberOfBytes r3, ULONG Tag r4)  — Xbox 360 2-arg form (size first)
PPC_FUNC(__imp__ExAllocatePoolWithTag)     { ctx.r3.u64 = PoolAlloc(ctx.r3.u32); }
// void ExFreePool(PVOID Block r3) — bump allocator leaks; nothing to do
PPC_FUNC(__imp__ExFreePool)                { ctx.r3.u64 = 0; }

// NTSTATUS NtQueryVirtualMemory(BaseAddress r3, *MEMORY_BASIC_INFORMATION r4, RegionType r5)
PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    std::lock_guard<std::mutex> lk(g_memMutex);
    uint32_t addr = ctx.r3.u32, pMbi = ctx.r4.u32;
    auto [rb, r] = FindRegion(addr);
    if (!r) {
        KTRACE("NtQueryVirtualMemory(0x%X) -> INVALID (not tracked)\n", addr);
        ctx.r3.u64 = kStatusInvalidParameter;
        return;
    }
    uint32_t state = r->committed ? kMemCommit : (r->reserved ? kMemReserve : kMemFree);
    PPC_STORE_U32(pMbi + 0x00, rb);                  // base_address
    PPC_STORE_U32(pMbi + 0x04, rb);                  // allocation_base
    PPC_STORE_U32(pMbi + 0x08, r->protect);          // allocation_protect
    PPC_STORE_U32(pMbi + 0x0C, rb + r->size - addr); // region_size (addr .. end of region)
    PPC_STORE_U32(pMbi + 0x10, state);               // state
    PPC_STORE_U32(pMbi + 0x14, r->protect);          // protect
    PPC_STORE_U32(pMbi + 0x18, kMemPrivate);         // type
    KTRACE("NtQueryVirtualMemory(0x%X) -> base=0x%X size=0x%X state=0x%X\n", addr, rb, r->size, state);
    ctx.r3.u64 = kStatusSuccess;
}

// NTSTATUS NtFreeVirtualMemory(*BaseAddress r3, *RegionSize r4, FreeType r5, Debug r6)
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    std::lock_guard<std::mutex> lk(g_memMutex);
    uint32_t pBase = ctx.r3.u32, pSize = ctx.r4.u32, freeType = ctx.r5.u32;
    if (!pBase) { ctx.r3.u64 = kStatusMemoryNotAllocated; return; }
    uint32_t gbase = PPC_LOAD_U32(pBase);
    uint32_t reqSize = pSize ? PPC_LOAD_U32(pSize) : 0;
    auto [rb, r] = FindRegion(gbase);
    if (!r) { ctx.r3.u64 = kStatusMemoryNotAllocated; return; }
    // 🎯 Writeback semantics matter: the guest heap reads *BaseAddress / *RegionSize BACK after the call to
    // update its UnCommittedRanges (RtlpDeCommitFreeBlock sub_8244B018 → sub_82449D58 inserts the returned
    // [base,size) as an uncommitted range). Real NtFreeVirtualMemory(MEM_DECOMMIT) writes back the
    // PAGE-ALIGNED REQUESTED sub-range — NOT the whole reservation. Returning the whole region (rb/r->size)
    // made the heap believe the ENTIRE heap was decommitted, so its UCR reset to the heap base; the next
    // heap-extend then committed at the base, overwrote the live heap header + GPU device, and the
    // backward-coalesce (sub_8244A108: prev = block − PreviousSize*16) underflowed below the base. Only
    // MEM_RELEASE returns the whole allocation. (See NIGHT-LOG "HEAP FREE-LIST"; the prior zero-on-decommit
    // alone did not fix it — the writeback was the root.)
    //
    // Xbox/NT MEM_DECOMMIT/RELEASE also LOSE the pages' contents (a later MEM_COMMIT of the same VA returns
    // ZEROED pages). variant A keeps host pages mapped (lazy), so zero the freed range to match.
    uint32_t outBase, outSize;
    if (freeType & kMemRelease) {                          // release frees the whole reservation
        r->reserved = false; r->committed = false;
        outBase = rb;
        outSize = r->size;
    } else {                                               // MEM_DECOMMIT: a page-aligned sub-range
        uint32_t aligned = gbase & ~0xFFFu;
        outBase = aligned;
        outSize = reqSize ? RoundUp((gbase - aligned) + reqSize, 0x1000u)
                          : (rb + r->size - aligned);      // size 0 => decommit to end of region
        if (outBase < rb) outBase = rb;
        if (outBase + outSize > rb + r->size) outSize = rb + r->size - outBase;
        if (outBase == rb && outSize >= r->size) r->committed = false;  // whole region decommitted
    }
    if (outSize && outBase >= rb && outBase + outSize <= rb + r->size)
        std::memset(base + outBase, 0, outSize);
    PPC_STORE_U32(pBase, outBase);
    if (pSize) PPC_STORE_U32(pSize, outSize);
    KTRACE("NtFreeVirtualMemory(req=0x%X reqsz=0x%X type=0x%X) -> writeback base=0x%X size=0x%X lr=0x%llX\n",
        gbase, reqSize, freeType, outBase, outSize, (unsigned long long)ctx.lr);
    ctx.r3.u64 = kStatusSuccess;
}

// DWORD KeGetCurrentProcessType(void) -> 1 (X_PROCTYPE_TITLE)
PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    KTRACE("KeGetCurrentProcessType -> 1\n");
    // cont.34 (REX_SPINTRACE): KeGetCurrentProcessType is called 100k+ times when the title stalls (a hot wait
    // loop polls it). ctx.lr = the guest caller's return address; sampling it every 20000 calls reveals the
    // DOMINANT caller in the late (post-Level-1) phase => the spin-loop location. Gated; default boot no-op.
    static const bool spintrace = getenv("REX_SPINTRACE") != nullptr;
    if (spintrace) { static std::atomic<uint64_t> n{0}; uint64_t c = n.fetch_add(1);
        if ((c % 20000) == 0) fprintf(stderr, "[spintrace] KeGetCurrentProcessType call #%llu from lr=0x%08X\n",
                                      (unsigned long long)c, (uint32_t)ctx.lr); }
    ctx.r3.u64 = 1;
}

// void HalReturnToFirmware(routine) — the guest is asking to reboot/exit. Log and stop.
PPC_FUNC(__imp__HalReturnToFirmware)
{
    KTRACE("HalReturnToFirmware(r3=0x%X) — guest requested reboot/exit; stopping.\n", ctx.r3.u32);
    fflush(stderr);
    exit(0);
}

// ---- critical sections -----------------------------------------------------------------------------
// Single-threaded bring-up: Enter/Leave are no-ops (no contention yet); make them real host mutexes
// once threading lands. Layout (Xenia X_RTL_CRITICAL_SECTION): X_DISPATCHER_HEADER header @0x00 (size
// 0x10; type@0x00 u8, absolute@0x01 u8, signal_state@0x04 s32), lock_count@0x10 s32, recursion_count
// @0x14 s32, owning_thread@0x18 u32.
static void InitCriticalSection(uint8_t* base, uint32_t cs)
{
    PPC_STORE_U8(cs + 0x00, 1);             // header.type = EventSynchronizationObject
    PPC_STORE_U8(cs + 0x01, 0);             // header.absolute (spin count / 256)
    PPC_STORE_U32(cs + 0x04, 0);            // header.signal_state
    PPC_STORE_U32(cs + 0x10, 0xFFFFFFFFu);  // lock_count = -1 (unowned)
    PPC_STORE_U32(cs + 0x14, 0);            // recursion_count
    PPC_STORE_U32(cs + 0x18, 0);            // owning_thread
}

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    KTRACE("RtlInitializeCriticalSection(cs=0x%X) caller-lr=0x%llX r2=0x%X r13=0x%X\n",
        ctx.r3.u32, (unsigned long long)ctx.lr, ctx.r2.u32, ctx.r13.u32);
    InitCriticalSection(base, ctx.r3.u32);
}

// NTSTATUS RtlInitializeCriticalSectionAndSpinCount(cs, spin_count)
PPC_FUNC(__imp__RtlInitializeCriticalSectionAndSpinCount)
{
    InitCriticalSection(base, ctx.r3.u32);
    uint32_t spin = (ctx.r4.u32 + 255) >> 8;
    if (spin > 255) spin = 255;
    PPC_STORE_U8(ctx.r3.u32 + 0x01, static_cast<uint8_t>(spin)); // header.absolute = spin/256
    ctx.r3.u64 = 0; // STATUS_SUCCESS
}

// Real per-CS host locks (now that ExCreateThread spawns concurrent guest threads). Keyed by the
// guest CS address; recursive to match Win32 critical-section re-entry semantics.
namespace {
std::mutex g_csMapMutex;
std::unordered_map<uint32_t, std::recursive_mutex*> g_csLocks;
std::recursive_mutex* CsLock(uint32_t cs) {
    std::lock_guard<std::mutex> lk(g_csMapMutex);
    auto& p = g_csLocks[cs];
    if (!p) p = new std::recursive_mutex();
    return p;
}
// Orphaned-critical-section handling (preemptive mode). A guest thread that EXITS while still owning a
// critical section orphans its host recursive_mutex (std::recursive_mutex is NOT released on thread exit)
// -> a later RtlEnterCriticalSection on it dead-locks forever. Under the cooperative token this never
// surfaces (one thread runs at a time, so a held CS is released before another can contend); under
// REX_NOTOKEN it does — the natural-transition worker tid=10 hangs at sub_82435C48 on a CS that the GPU/
// video-init thread sub_8242B4A8 acquired (0x82818628) and exited without releasing (its CS-release branch
// is skipped because the fence-forward stopgap doesn't reproduce the real GPU sequence). Track the CSes
// held by THIS host thread (thread-local, so no start-addr aliasing) and, on exit, release any still held
// (the exiting thread owns those host mutexes, so unlock() is valid on it; it returned normally, so the
// state they guard is consistent) — and/or report under REX_CSLEAK. Tracking runs only when needed
// (preemptive or the diagnostic); the default cooperative path is untouched (zero cost). [STOPGAP — the
// clean fix is a real CP so sub_8242B4A8 takes its normal CS-release path; see the fence-forward note.]
const bool g_csleak = getenv("REX_CSLEAK") != nullptr;
const bool g_csPreempt = getenv("REX_NOTOKEN") != nullptr;   // == g_preempt (defined later in the file); local copy for this earlier section
const bool g_csTrack = g_csPreempt || g_csleak;     // track per-thread held CSes (preemptive or diagnostic)
thread_local uint32_t tl_guestStart = 0;            // owning guest thread's start addr (set in GuestThreadRun)
thread_local std::unordered_map<uint32_t,int>* tl_heldCS = nullptr;   // guest CS addr -> recursion depth held by THIS host thread
void CsHeldEnter(uint32_t cs) { if (!g_csTrack) return;
    if (!tl_heldCS) tl_heldCS = new std::unordered_map<uint32_t,int>(); (*tl_heldCS)[cs]++; }
void CsHeldLeave(uint32_t cs) { if (!g_csTrack || !tl_heldCS) return;
    auto it = tl_heldCS->find(cs); if (it != tl_heldCS->end() && --it->second <= 0) tl_heldCS->erase(it); }
void CsExitCleanup(uint32_t start) {                // on guest-thread exit: release CSes it still holds
    if (!tl_heldCS) return;
    for (auto& kv : *tl_heldCS) { if (kv.second <= 0) continue;
        if (g_csleak) fprintf(stderr, "[CS-LEAK] guest thread start=0x%X EXITED owning CS=0x%X (depth=%d)%s\n",
                              start, kv.first, kv.second, g_csPreempt ? " — releasing (orphan fix)" : " — orphans it");
        if (g_csPreempt) for (int i = 0; i < kv.second; i++) CsLock(kv.first)->unlock(); }   // unlock on the owning thread
    delete tl_heldCS; tl_heldCS = nullptr; }
} // namespace

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    auto* m = CsLock(ctx.r3.u32);
    if (!m->try_lock()) {                 // held by a thread that yielded while owning it: release the
        kernel::UnlockGuestExecution();   // execution token while we block, then re-acquire it.
        m->lock();
        kernel::LockGuestExecution();
    }
    CsHeldEnter(ctx.r3.u32);
}
PPC_FUNC(__imp__RtlLeaveCriticalSection) { CsHeldLeave(ctx.r3.u32); CsLock(ctx.r3.u32)->unlock(); }

// BOOLEAN RtlTryEnterCriticalSection(cs)
PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    bool got = CsLock(ctx.r3.u32)->try_lock();
    if (got) CsHeldEnter(ctx.r3.u32);
    ctx.r3.u64 = got ? 1 : 0;
}

// ====================================================================================================
// Boot environment: title X_KPROCESS + main-thread X_KTHREAD + X_KPCR + static TLS block.
// Layouts are 1:1 with rexglue-sdk (Xenia-derived): include/rex/system/{xthread.h,kernel_state.h};
// behaviour mirrors KernelState::{Setup,InitializeProcess,SetProcessTLSVars} +
// XThread::{Create,InitializeGuestObject}. The recompiled CRT reads r13=KPCR → current_thread (KTHREAD)
// → process (KPROCESS), the per-thread TLS block, the stack bounds, and the process TLS slot bitmap.
// ====================================================================================================
namespace {
// Kernel arena ABOVE the image + dispatch table (image 0x82000000..~0x83500000): the guest reserves
// its heap as everything BELOW its stack (~0x70000000), so guest allocations must stay below the
// image and our structures must stay above it, or a big heap reserve overwrites the image's .data.
// The full 4 GiB is lazily mmap'd, so these high addresses are already backed.
constexpr uint32_t kProcessAddr = 0x90000000;
constexpr uint32_t kThreadAddr  = 0x90001000;
constexpr uint32_t kKpcrAddr    = 0x90002000;
constexpr uint32_t kTlsAddr     = 0x90003000;  // per-thread TLS block (main thread)
constexpr uint32_t kDefaultTlsSlots = 1024;
constexpr uint32_t kProctypeUser = 1;          // X_PROCTYPE_USER (KeGetCurrentProcessType == 1)

// Big-endian guest stores/loads via the raw base (used during setup, no PPCContext in scope).
inline void GST8 (uint32_t a, uint8_t  v) { g_base[a] = v; }
inline void GST16(uint32_t a, uint16_t v) { uint16_t b = __builtin_bswap16(v); memcpy(g_base + a, &b, 2); }
inline void GST32(uint32_t a, uint32_t v) { uint32_t b = __builtin_bswap32(v); memcpy(g_base + a, &b, 4); }
inline void GST64(uint32_t a, uint64_t v) { uint64_t b = __builtin_bswap64(v); memcpy(g_base + a, &b, 8); }
inline uint32_t GLD32(uint32_t a) { uint32_t b; memcpy(&b, g_base + a, 4); return __builtin_bswap32(b); }
inline uint16_t GLD16(uint32_t a) { uint16_t b; memcpy(&b, g_base + a, 2); return __builtin_bswap16(b); }
inline uint64_t GLD64(uint32_t a) { uint64_t b; memcpy(&b, g_base + a, 8); return __builtin_bswap64(b); }

// ---- KeTimeStampBundle: the guest millisecond clock GetTickCount() reads ----------------------------
// The title's GetTickCount-equivalent (CRT sub_82448748) returns KeTimeStampBundle->TickCount (+16).
// On Xbox 360 the kernel advances this 24-byte structure continuously; code that polls with a timeout
// (e.g. the boot-logo "press A/START" screen, which auto-proceeds after 5 s) relies on it ticking.
// KeTimeStampBundle is kernel-export VARIABLE ordinal 0x00AD; variant A leaves that data import as its
// unresolved placeholder 0xAD000100 (the address the title actually dereferences — verified at runtime
// and clear of every MmAllocatePhysical region), so the structure effectively lives there. Nothing
// advanced +16, so GetTickCount() was pinned at 0 → every "elapsed = now - start" stayed 0 < timeout →
// the logo worker (tid 4, sub_8242B4A8 → sub_8214F730) spun forever and the main thread, which joins it
// during CRT static-init, deadlocked. Fix: maintain +16 = guest uptime in ms from a dedicated ~1 ms
// host thread — a plain guest-memory write needing no execution token — mirroring rexglue-sdk
// xboxkrnl_module.cpp, which zeroes +0/+8 and updates +16 with QueryGuestUptimeMillis() every 1 ms.
constexpr uint32_t kKeTimeStampBundle = 0xAD000100;   // xboxkrnl.exe variable import ordinal 0x00AD
std::chrono::steady_clock::time_point g_bootTime;
std::atomic<bool> g_timestampRunning{false};

uint32_t GuestUptimeMs() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_bootTime).count());
}

void TimestampPump() {
    while (g_timestampRunning.load()) {
        GST32(kKeTimeStampBundle + 16, GuestUptimeMs());          // TickCount (ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Start the 1 ms KeTimeStampBundle updater (once). Called from SetupEnvironment, before _xstart, so the
// guest clock is already ticking when the CRT static-init logo poll captures its start tick.
void StartTimestampPump() {
    if (g_timestampRunning.exchange(true)) return;
    g_bootTime = std::chrono::steady_clock::now();
    GST64(kKeTimeStampBundle + 0, 0);    // InterruptTime  (rexglue leaves these 0)
    GST64(kKeTimeStampBundle + 8, 0);    // SystemTime
    GST32(kKeTimeStampBundle + 16, 0);   // TickCount
    GST32(kKeTimeStampBundle + 20, 0);
    std::thread(TimestampPump).detach();
}

// Read a XEX optional header that stores its value INLINE (key low byte 0) as a BE u32.
uint32_t XexOptU32(const uint8_t* xex, uint32_t key, uint32_t fallback) {
    const void* p = getOptHeaderPtr(xex, key);
    if (!p) return fallback;
    uint32_t b; memcpy(&b, p, 4); return __builtin_bswap32(b);
}

// Fill an X_KTHREAD (XThread::InitializeGuestObject) and tail-link it into the process thread list.
void FillKThread(uint32_t T, uint32_t kpcr, uint32_t P, uint32_t stackBase, uint32_t stackLimit,
                 uint32_t tlsDynamic, uint32_t threadId, uint32_t startAddr) {
    memset(g_base + T, 0, 0xAB0);
    GST8 (T + 0x00, 6);                                      // header.type = ThreadObject
    GST32(T + 0x10, T + 0x10); GST32(T + 0x14, T + 0x10);
    GST32(T + 0x40, T + 0x20); GST32(T + 0x44, T + 0x20);   // wait_timeout_block.list_entry
    GST32(T + 0x48, T);        GST32(T + 0x4C, T + 0x18);
    GST16(T + 0x54, 0x0100);   GST16(T + 0x56, 0x0201);
    GST32(T + 0x5C, stackBase); GST32(T + 0x60, stackLimit); GST32(T + 0x64, stackBase - 240);
    GST32(T + 0x68, tlsDynamic);                             // tls_address (dynamic TLS base)
    GST8 (T + 0x6C, 2);                                      // thread_state = RUNNING
    GST32(T + 0x74, T + 0x74); GST32(T + 0x78, T + 0x74);   // apc_lists[0]
    GST32(T + 0x7C, T + 0x7C); GST32(T + 0x80, T + 0x7C);   // apc_lists[1]
    GST8 (T + 0x72, kProctypeUser); GST8(T + 0x73, kProctypeUser);
    GST32(T + 0x84, P);                                      // process
    GST8 (T + 0x8B, 1);                                      // may_queue_apcs
    GST32(T + 0x9C, 0xFDFFD7FFu);                            // msr_mask
    GST32(T + 0xC0, kpcr + 0x100); GST32(T + 0xC4, kpcr + 0x100);
    GST32(T + 0xD0, stackBase);                              // stack_alloc_base
    GST32(T + 0x144, T + 0x144); GST32(T + 0x148, T + 0x144); // timer_list
    GST32(T + 0x14C, threadId);
    GST32(T + 0x150, startAddr);
    GST32(T + 0x154, T + 0x154); GST32(T + 0x158, T + 0x154);
    GST32(T + 0x17C, 1);
    // XeInsertTailList(head = P+0x04, entry = T+0x110): correct for empty(self-ref) or populated list.
    uint32_t head = P + 0x04, oldBlink = GLD32(head + 4);
    GST32(T + 0x110, head); GST32(T + 0x114, oldBlink);
    GST32(oldBlink + 0, T + 0x110); GST32(head + 4, T + 0x110);
    GST32(P + 0x14, GLD32(P + 0x14) + 1);                    // thread_count++
}

void FillKPcr(uint32_t K, uint32_t tlsPtr, uint32_t T, uint32_t stackBase, uint32_t stackLimit) {
    memset(g_base + K, 0, 0x2D8);
    GST32(K + 0x00, tlsPtr);              // tls_ptr (this is *(r13))
    GST8 (K + 0x18, 0);                   // current_irql = PASSIVE
    GST64(K + 0x30, K);                   // pcr_ptr
    GST32(K + 0x70, stackBase); GST32(K + 0x74, stackLimit);
    GST32(K + 0x100, T);                  // prcb_data.current_thread
    GST32(K + 0x2A8, K + 0x100);          // prcb -> &prcb_data
}
} // namespace

namespace kernel {
uint32_t g_kpcrAddr = 0, g_mainThreadAddr = 0, g_processAddr = 0;

uint32_t SetupEnvironment(const uint8_t* xexFile, uint32_t stackBase, uint32_t stackLimit,
                          uint32_t startAddress)
{
    // ---- XEX TLS directory (XEX_HEADER_TLS_INFO = 4 BE dwords) --------------------------------------
    uint32_t tlsSlots = kDefaultTlsSlots, tlsRawAddr = 0, tlsDataSize = 0, tlsRawSize = 0;
    if (const void* ti = getOptHeaderPtr(xexFile, XEX_HEADER_TLS_INFO)) {
        const uint8_t* p = static_cast<const uint8_t*>(ti);
        uint32_t v[4]; memcpy(v, p, 16);
        tlsSlots    = __builtin_bswap32(v[0]);
        tlsRawAddr  = __builtin_bswap32(v[1]);
        tlsDataSize = __builtin_bswap32(v[2]);
        tlsRawSize  = __builtin_bswap32(v[3]);
    }
    if (tlsSlots == 0) tlsSlots = kDefaultTlsSlots;
    uint32_t slotsPadded = (tlsSlots + 3) & ~3u;
    uint32_t tlsSlotSize = tlsSlots * 4;
    uint32_t tlsTotal    = tlsSlotSize + tlsDataSize;
    uint32_t tlsDynamic  = kTlsAddr + tlsDataSize;     // dynamic slots follow the static (extended) data

    // Build the TLS block: zero, then copy the XEX raw template into the static (extended) region.
    memset(g_base + kTlsAddr, 0, tlsTotal ? tlsTotal : 0x1000);
    if (tlsDataSize && tlsRawAddr && tlsRawSize)
        memcpy(g_base + kTlsAddr, g_base + tlsRawAddr, tlsRawSize);

    uint32_t defaultStack = XexOptU32(xexFile, XEX_HEADER_DEFAULT_STACK_SIZE, 0);
    uint32_t kernStack = defaultStack ? ((defaultStack + 0xFFF) & ~0xFFFu) : 0;
    if (kernStack < 16 * 1024) kernStack = 16 * 1024;

    // ---- X_KPROCESS (title process) -----------------------------------------------------------------
    const uint32_t P = kProcessAddr;
    memset(g_base + P, 0, 0x60);
    GST32(P + 0x04, P + 0x04); GST32(P + 0x08, P + 0x04);   // thread_list: self-ref LIST_ENTRY
    GST32(P + 0x0C, 60);                                     // quantum
    GST32(P + 0x14, 0);                                      // thread_count (++ when main thread links)
    GST8 (P + 0x18, 10); GST8(P + 0x19, 13); GST8(P + 0x1A, 17); GST8(P + 0x1B, 6);
    GST32(P + 0x1C, kernStack);                              // kernel_stack_size
    GST32(P + 0x20, tlsRawAddr);                             // tls_static_data_address (XEX template)
    GST32(P + 0x24, tlsDataSize);                            // tls_data_size
    GST32(P + 0x28, tlsRawSize);                             // tls_raw_data_size
    GST16(P + 0x2C, static_cast<uint16_t>(4 * slotsPadded)); // tls_slot_size
    GST8 (P + 0x2F, kProctypeUser);                          // process_type
    GST32(P + 0x54, P + 0x54); GST32(P + 0x58, P + 0x54);   // unk_54: self-ref LIST_ENTRY
    // tls_slot_bitmap[8]: 1 = available slot (high bits first), matching SetProcessTLSVars.
    uint32_t bitmapSlots = slotsPadded / 32;
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t v;
        if (i < bitmapSlots) v = 0xFFFFFFFFu;
        else if (i == bitmapSlots) { uint32_t rem = slotsPadded % 32; v = rem ? (~0u << (32 - rem)) : 0; }
        else v = 0;
        GST32(P + 0x30 + i * 4, v);
    }

    // ---- main-thread X_KTHREAD + X_KPCR (shared fill helpers; also used by CreateGuestThreadContext)
    const uint32_t T = kThreadAddr, K = kKpcrAddr;
    g_processAddr = P;   // FillKThread links into the process; set this first
    FillKThread(T, K, P, stackBase, stackLimit, tlsDynamic, /*threadId=*/1, startAddress);
    FillKPcr(K, kTlsAddr, T, stackBase, stackLimit);

    g_kpcrAddr = K; g_mainThreadAddr = T; g_processAddr = P;
    fprintf(stderr, "[env] KPROCESS=0x%X KTHREAD=0x%X KPCR=0x%X TLS=0x%X (slots=%u dataSize=0x%X "
        "rawAddr=0x%X rawSize=0x%X dyn=0x%X)\n", P, T, K, kTlsAddr, tlsSlots, tlsDataSize, tlsRawAddr,
        tlsRawSize, tlsDynamic);
    StartTimestampPump();   // advance KeTimeStampBundle->TickCount so GetTickCount()-timed waits expire
    // GPU-RESOURCE-BUILD-PLAN piece 2: verify the Xenos texture decoder (tiler round-trip + BC1 vector +
    // rat.dds 8888 -> /tmp/rat_decoded.ppm). Gated, default-off; no effect on the boot path.
    if (getenv("REX_TEXSELFTEST")) rex_tex::SelfTest();
    return K;
}
} // namespace kernel

// ---- dynamic TLS slots (KeTls*) --------------------------------------------------------------------
// Slot bitmap lives in the current process (1 = free, high-bit-first). Slot values live in the current
// thread's dynamic TLS array. Current thread/process resolved via r13 (KPCR) so this works per-thread.
PPC_FUNC(__imp__KeTlsAlloc)
{
    uint32_t thread = PPC_LOAD_U32(ctx.r13.u32 + 0x100);   // KPCR.prcb_data.current_thread
    uint32_t proc   = PPC_LOAD_U32(thread + 0x84);          // KTHREAD.process
    uint32_t slot = 0xFFFFFFFFu;
    for (uint32_t bi = 0; bi < 8; bi++) {
        uint32_t bm = PPC_LOAD_U32(proc + 0x30 + bi * 4);
        if (bm == 0) continue;
        uint32_t lz = __builtin_clz(bm);
        uint32_t s = bi * 32 + lz;
        if (s >= 256) continue;
        PPC_STORE_U32(proc + 0x30 + bi * 4, bm & ~(1u << (31 - lz)));
        slot = s;
        break;
    }
    if (slot != 0xFFFFFFFFu) {
        uint32_t tlsDyn = PPC_LOAD_U32(thread + 0x68);
        PPC_STORE_U32(tlsDyn + slot * 4, 0);
    }
    KTRACE("KeTlsAlloc -> %u\n", slot);
    ctx.r3.u64 = slot;
}

PPC_FUNC(__imp__KeTlsFree)
{
    uint32_t idx = ctx.r3.u32;
    if (idx < 256) {
        uint32_t thread = PPC_LOAD_U32(ctx.r13.u32 + 0x100);
        uint32_t proc   = PPC_LOAD_U32(thread + 0x84);
        uint32_t bi = idx / 32, lz = idx % 32;
        uint32_t bm = PPC_LOAD_U32(proc + 0x30 + bi * 4);
        PPC_STORE_U32(proc + 0x30 + bi * 4, bm | (1u << (31 - lz)));
    }
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__KeTlsGetValue)
{
    uint32_t idx = ctx.r3.u32;
    uint32_t thread = PPC_LOAD_U32(ctx.r13.u32 + 0x100);
    uint32_t tlsDyn = PPC_LOAD_U32(thread + 0x68);
    ctx.r3.u64 = (idx < 0x4000) ? PPC_LOAD_U32(tlsDyn + idx * 4) : 0;
}

PPC_FUNC(__imp__KeTlsSetValue)
{
    uint32_t idx = ctx.r3.u32, val = ctx.r4.u32;
    uint32_t thread = PPC_LOAD_U32(ctx.r13.u32 + 0x100);
    uint32_t tlsDyn = PPC_LOAD_U32(thread + 0x68);
    if (idx < 0x4000) PPC_STORE_U32(tlsDyn + idx * 4, val);
    ctx.r3.u64 = 1;
}

// ---- time ------------------------------------------------------------------------------------------
// DWORD KeQueryPerformanceFrequency() — Xbox 360 timebase = 50 MHz (avoid guest div-by-zero).
PPC_FUNC(__imp__KeQueryPerformanceFrequency) { ctx.r3.u64 = 50000000u; }

// void KeQuerySystemTime(PLARGE_INTEGER) — 100ns units since 1601. Fixed plausible value (~2024).
PPC_FUNC(__imp__KeQuerySystemTime)
{
    if (ctx.r3.u32) PPC_STORE_U64(ctx.r3.u32, 0x01DA80F800000000ull);
}

// BOOL XexCheckExecutablePrivilege(DWORD privilege) — title has no special privileges during bring-up.
PPC_FUNC(__imp__XexCheckExecutablePrivilege) { ctx.r3.u64 = 0; }

// ---- video mode (goal step: video) -----------------------------------------------------------------
// void XGetVideoMode(X_VIDEO_MODE* mode) — report a fixed 1280x720 widescreen hi-def NTSC mode.
// Layout (rexglue X_VIDEO_MODE): display_width@0x0, display_height@0x4, is_interlaced@0x8,
// is_widescreen@0xC, is_hi_def@0x10, refresh_rate@0x14 (float), video_standard@0x18,
// unknown_0x8a@0x1C, unknown_0x01@0x20, reserved[3]@0x24.
PPC_FUNC(__imp__XGetVideoMode)
{
    uint32_t m = ctx.r3.u32;
    if (!m) return;
    for (uint32_t i = 0; i < 0x30; i += 4) PPC_STORE_U32(m + i, 0);
    PPC_STORE_U32(m + 0x00, 1280);          // display_width
    PPC_STORE_U32(m + 0x04, 720);           // display_height
    PPC_STORE_U32(m + 0x08, 0);             // is_interlaced
    PPC_STORE_U32(m + 0x0C, 1);             // is_widescreen
    PPC_STORE_U32(m + 0x10, 1);             // is_hi_def
    { float hz = 60.0f; uint32_t b; memcpy(&b, &hz, 4); PPC_STORE_U32(m + 0x14, b); } // refresh_rate
    PPC_STORE_U32(m + 0x18, 1);             // video_standard = NTSC
    PPC_STORE_U32(m + 0x1C, 0x4A);          // unknown_0x8a
    PPC_STORE_U32(m + 0x20, 0x01);          // unknown_0x01
    KTRACE("XGetVideoMode -> 1280x720\n");
}

// void VdQueryVideoMode(X_VIDEO_MODE* mode r3) — SAME fill as XGetVideoMode. The device build calls
// THIS (not XGetVideoMode); a zeroed mode → div-by-zero on the display dimensions (sub_821D29A8).
PPC_FUNC(__imp__VdQueryVideoMode)
{
    uint32_t m = ctx.r3.u32;
    if (!m) return;
    for (uint32_t i = 0; i < 0x30; i += 4) PPC_STORE_U32(m + i, 0);
    PPC_STORE_U32(m + 0x00, 1280); PPC_STORE_U32(m + 0x04, 720);   // display_width/height
    PPC_STORE_U32(m + 0x08, 0); PPC_STORE_U32(m + 0x0C, 1); PPC_STORE_U32(m + 0x10, 1); // interlaced/wide/hidef
    { float hz = 60.0f; uint32_t b; memcpy(&b, &hz, 4); PPC_STORE_U32(m + 0x14, b); }    // refresh_rate
    PPC_STORE_U32(m + 0x18, 1);            // video_standard = NTSC
    PPC_STORE_U32(m + 0x1C, 0x4A); PPC_STORE_U32(m + 0x20, 0x01);
    KTRACE("VdQueryVideoMode -> 1280x720\n");
}

// ---- physical memory (GPU buffers) -----------------------------------------------------------------
namespace { uint32_t g_physNext = 0xA0000000; }  // Xbox physical-address window (all lazily mmap'd)

// R0 keystone (REX_TEXWATCH): non-static so an attached gdb can read it by name after the SIGTRAP, to set a
// hardware write-watchpoint on g_base + g_texWatchAddr and catch the texture-populate writer (or confirm none).
uint32_t g_texWatchAddr = 0;
// REX_TEXSCAN (cont.38): the COMPLETE, bind-independent sweep cont.25 R0 never did (it sampled only the FIRST
// 924KB block). Tracks every >=256KB physical alloc so the vblank pump can sample each for population — to
// rigorously settle whether ANY tiled texture data exists in guest GPU memory at the attract state.
struct TexAlloc { uint32_t addr, size, lr; };
std::vector<TexAlloc> g_texAllocs;
std::mutex g_texAllocsMtx;
// PVOID MmAllocatePhysicalMemoryEx(flags r3, size r4, protect r5, min r6, max r7, align r8)
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    uint32_t size = ctx.r4.u32, protect = ctx.r5.u32, align = ctx.r8.u32;
    uint32_t pageSize = 0x1000;
    if (protect & 0x20000000u)      pageSize = 0x10000;     // X_MEM_LARGE_PAGES
    else if (protect & 0x80000000u) pageSize = 0x1000000;   // X_MEM_16MB_PAGES
    uint32_t a = align < pageSize ? pageSize : ((align + pageSize - 1) & ~(pageSize - 1));
    size = (size + pageSize - 1) & ~(pageSize - 1);
    g_physNext = (g_physNext + (a - 1)) & ~(a - 1);
    uint32_t addr = g_physNext;
    g_physNext += size ? size : pageSize;
    KTRACE("MmAllocatePhysicalMemoryEx(sz=0x%X prot=0x%X) -> 0x%X\n", size, protect, addr);
    // REX_TEXSCAN: record texture-sized blocks (>=256KB) + the guest CALLER (ctx.lr) so the vblank pump can
    // sweep them for population AND so the texture-create function (the inlined-D3D create that allocates the
    // GPU block — cont.38 keystone) is identified by its alloc site. Log the GPU-window (0xA4-0xA5) callers now.
    { static const bool s_texscan = getenv("REX_TEXSCAN") != nullptr;
      if (s_texscan && size >= 0x40000u) { uint32_t lr = static_cast<uint32_t>(ctx.lr);
          std::lock_guard<std::mutex> lk(g_texAllocsMtx);
          if (g_texAllocs.size() < 256) g_texAllocs.push_back({addr, size, lr});
          // The immediate caller (lr) is a generic allocator wrapper; walk the PPC back-chain 3 levels to find
          // the texture-CREATE (the distinct caller of the GPU-window texture-block allocs vs working buffers).
          uint32_t sp = ctx.r1.u32, bc1 = sp ? GLD32(sp) : 0, lr1 = bc1 ? GLD32(bc1 + 4) : 0;
          uint32_t bc2 = bc1 ? GLD32(bc1) : 0, lr2 = bc2 ? GLD32(bc2 + 4) : 0;
          uint32_t bc3 = bc2 ? GLD32(bc2) : 0, lr3 = bc3 ? GLD32(bc3 + 4) : 0;
          bool gpu = (addr >= 0xA4000000u && addr < 0xA6000000u);
          fprintf(stderr, "[texalloc] 0x%08X sz=0x%X lr=%08X up=[%08X %08X %08X]%s\n", addr, size, lr,
                  lr1, lr2, lr3, gpu ? "  <- GPU-window" : ""); } }
    // R0 keystone (REX_TEXWATCH): GPU texture memory IS allocated but never populated (atlas zeros). Flag the
    // FIRST big-texture block (0xE1000 ≈ 924KB) so an attached gdb can hardware-watchpoint it and catch WHO
    // writes it (the decode/upload = "model the upload" territory) — or confirm NOTHING does (decode stubbed =
    // "implement decode"). raise(SIGTRAP) hands control to gdb with g_texWatchAddr set (default-off, gdb-only).
    static const bool s_texwatch = getenv("REX_TEXWATCH") != nullptr;
    static const bool s_textrap = getenv("REX_TEXTRAP") != nullptr;   // separate: raise SIGTRAP (gdb-only)
    if (s_texwatch && size == 0xE1000u && g_texWatchAddr == 0) {
        g_texWatchAddr = addr;
        fprintf(stderr, "[texwatch] flagged big-texture alloc 0x%X (sz=0x%X)%s\n", addr, size,
                s_textrap ? " — trapping for gdb watchpoint" : " — will sample for population");
        if (s_textrap) raise(SIGTRAP);
    }
    ctx.r3.u64 = addr;
}

// PVOID MmGetPhysicalAddress(addr r3) — identity within the physical window for bring-up.
PPC_FUNC(__imp__MmGetPhysicalAddress) { /* keep r3 as-is */ }

// ---- XConfig --------------------------------------------------------------------------------------
// NTSTATUS ExGetXConfigSetting(cat r3, setting r4, buffer r5, buffer_size r6, *required_size r7).
// Return success with sane defaults so config-driven init paths proceed.
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    uint16_t cat = static_cast<uint16_t>(ctx.r3.u32);
    uint16_t setting = static_cast<uint16_t>(ctx.r4.u32);
    uint32_t buf = ctx.r5.u32, bufSize = ctx.r6.u32, reqPtr = ctx.r7.u32;
    uint32_t val = 0; uint16_t valSize = 4;
    // Common settings the CRT/title queries early.
    if (cat == 0x0003) {                 // XCONFIG_USER category
        if (setting == 0x0009) val = 1;  // USER_LANGUAGE = English
        if (setting == 0x000A) val = 0;  // USER_VIDEO_FLAGS
        if (setting == 0x0007) { val = 0; valSize = 1; }   // USER_AV_REGION-ish
    } else if (cat == 0x0002) {          // XCONFIG_CONSOLE category
        valSize = 4;                     // default 0
    }
    if (buf && bufSize) {
        for (uint32_t i = 0; i < bufSize && i < valSize; i++)
            PPC_STORE_U8(buf + i, static_cast<uint8_t>(val >> (8 * (valSize - 1 - i))));
    }
    if (reqPtr) PPC_STORE_U16(reqPtr, valSize);
    ctx.r3.u64 = 0;  // STATUS_SUCCESS
}

// ---- strings ---------------------------------------------------------------------------------------
// void RtlInitAnsiString(ANSI_STRING* dst r3, const char* src r4)
// ANSI_STRING: length@0x0 (u16), maximum_length@0x2 (u16), buffer@0x4 (u32).
PPC_FUNC(__imp__RtlInitAnsiString)
{
    uint32_t dst = ctx.r3.u32, src = ctx.r4.u32;
    if (!dst) return;
    uint16_t len = 0;
    if (src) { while (PPC_LOAD_U8(src + len) != 0 && len < 0xFFFE) len++; }
    PPC_STORE_U16(dst + 0x00, len);
    PPC_STORE_U16(dst + 0x02, src ? static_cast<uint16_t>(len + 1) : 0);
    PPC_STORE_U32(dst + 0x04, src);
}

// ====================================================================================================
// Threading + GPU/vblank pump (goal steps: import cascade -> video).
// ExCreateThread spawns a host std::thread that runs the guest start routine on its own
// X_KTHREAD/X_KPCR/TLS/stack. A vblank pump thread fires the guest graphics interrupt callback at
// ~60 Hz so the main thread's GPU ring-buffer/vblank spin (sub_821B9270) makes progress.
// Reference: rexglue-sdk XThread::{Create,Execute} + GraphicsSystem::{MarkVblank,DispatchInterruptCallback}.
// ====================================================================================================
namespace {

// Resolve a guest code address to its recompiled host fn via the relocated dispatch table (HostFnAt reads
// g_funcTableBase, the separate out-of-guest-space allocation — cont.22; returns null for out-of-range addr).
PPCFunc* DispatchLookup(uint32_t addr) { return HostFnAt(addr); }
void CallGuest(uint32_t addr, PPCContext& ctx) {
    PPCFunc* fn = DispatchLookup(addr);
    if (ValidHostFn(fn)) fn(ctx, g_base);     // ValidHostFn rejects a corrupted/garbage slot (cont.22) so
    else fprintf(stderr, "[thread] CallGuest: corrupt/missing slot for 0x%X (fn=%p) — skipped\n",
                 addr, (void*)fn);             // we log+skip instead of calling a wild pointer -> SIGSEGV
}

// Per-thread arena (KTHREAD+KPCR+TLS) + worker stacks, all ABOVE the image so the title's big
// guest-heap reserve (which spans low memory up to ~0x70000000) never overlaps them.
std::mutex g_threadMutex;
uint32_t g_threadArenaNext = 0x90100000;
uint32_t g_threadStackNext = 0x98000000;

// Allocate + fill a new guest thread context (KTHREAD/KPCR/TLS/stack). Returns the KPCR address.
uint32_t CreateGuestThreadContext(uint32_t stackSize, uint32_t startAddr, uint32_t threadId,
                                  uint32_t* outThreadAddr, uint32_t* outStackTop) {
    std::lock_guard<std::mutex> lk(g_threadMutex);
    uint32_t P = kernel::g_processAddr;
    uint32_t tlsSlotSize = GLD16(P + 0x2C);
    uint32_t tlsDataSize = GLD32(P + 0x24);
    uint32_t tlsRawAddr  = GLD32(P + 0x20);
    uint32_t tlsRawSize  = GLD32(P + 0x28);
    uint32_t tlsTotal = tlsSlotSize + tlsDataSize;

    auto bump = [&](uint32_t n){ uint32_t a = g_threadArenaNext;
                                 g_threadArenaNext = (a + n + 0xFFFu) & ~0xFFFu; return a; };
    uint32_t T   = bump(0xB00);
    uint32_t K   = bump(0x300);
    uint32_t tls = bump(tlsTotal ? tlsTotal : 0x1000);

    if (stackSize == 0) stackSize = 0x40000;            // 256 KiB default
    stackSize = (stackSize + 0xFFFFu) & ~0xFFFFu;
    uint32_t stackLimit = g_threadStackNext;
    uint32_t stackBase  = stackLimit + stackSize;       // high address (stack grows down)
    g_threadStackNext   = stackBase + 0x10000;          // + guard gap

    memset(g_base + tls, 0, tlsTotal ? tlsTotal : 0x1000);
    if (tlsDataSize && tlsRawAddr && tlsRawSize) memcpy(g_base + tls, g_base + tlsRawAddr, tlsRawSize);
    uint32_t tlsDynamic = tls + tlsDataSize;

    FillKThread(T, K, P, stackBase, stackLimit, tlsDynamic, threadId, startAddr);
    FillKPcr(K, tls, T, stackBase, stackLimit);
    if (outThreadAddr) *outThreadAddr = T;
    if (outStackTop) *outStackTop = stackBase;
    return K;
}

// ---- guest thread objects + handle table ----------------------------------------------------------
struct ThreadRec {
    uint32_t kpcr = 0, threadAddr = 0, stackTop = 0;
    uint32_t startAddr = 0, startContext = 0, xapiStartup = 0;
    std::thread th;
    std::atomic<bool> started{false};
};
std::mutex g_handleMutex;
std::unordered_map<uint32_t, ThreadRec*> g_handles;
uint32_t g_nextHandle = 0xF1000000;          // private handle space (avoids 0xFFFFFFxx pseudo-handles)
std::atomic<uint32_t> g_nextThreadId{2};     // main thread is id 1
const bool g_noSpawn = getenv("REX_NOSPAWN") != nullptr;  // diagnostic: create handles but don't run

// Non-thread dispatch objects (events, etc.): handle -> guest dispatch-header address. Backed by a
// small arena in the kernel region (above the thread structs / worker stacks).
std::unordered_map<uint32_t, uint32_t> g_objHandles;
uint32_t g_objArenaNext = 0x90400000;
uint32_t AllocObject(uint32_t size) { uint32_t a = g_objArenaNext; g_objArenaNext += (size + 0x1F) & ~0x1Fu; return a; }

// Resolve a guest handle to its dispatch-header address (thread KTHREAD or event object); 0 if unknown.
uint32_t ResolveObject(uint32_t handle) {
    std::lock_guard<std::mutex> lk(g_handleMutex);
    auto t = g_handles.find(handle);    if (t != g_handles.end()) return t->second->threadAddr;
    auto o = g_objHandles.find(handle); if (o != g_objHandles.end()) return o->second;
    return 0;
}

// ---- dispatch-object wait/signal (events, semaphores, mutants, thread-exit) ------------------------
// Xbox dispatch objects carry an X_DISPATCH_HEADER (type@0x00 u8, signal_state@0x04 s32). We back
// waits with one global condition variable; the signal_state lives in guest memory. Coarse but
// correct. Reference: rexglue-sdk xboxkrnl_threading.cpp.
// g_waitMutex IS the cooperative guest-execution token: the running guest thread holds it; waits
// release it (cv.wait) so another thread can run. So SignalObject (called by the running thread)
// must NOT re-lock it, and WaitObject adopts the already-held lock.
std::mutex g_waitMutex;
std::condition_variable g_waitCv;
// Cooperative token by default; REX_NOTOKEN=1 runs guest threads PREEMPTIVELY (relies on the title's
// own critical sections/atomics for correctness — needed when a pure busy-wait would deadlock the token).
const bool g_coop = (getenv("REX_NOTOKEN") == nullptr);
const bool g_preempt = !g_coop;   // REX_NOTOKEN: guest threads run preemptively on real cores (no run-token)
const bool g_evtrace = (getenv("REX_EVTRACE") != nullptr);

// ---- Fair cooperative scheduler (REX_FAIRSCHED) ------------------------------------------------------
// The default token (g_waitMutex, a plain std::mutex) is UNFAIR: when the main loop yields it and re-locks,
// it wins the race, so resumed/ready worker threads parked at g_waitMutex.lock() (GuestThreadRun startup,
// LockGuestExecution) can starve forever (observed: tid=10 sub_82250420, which runs the screen-transitions-
// enable teardown, never ran its entry). REX_FAIRSCHED swaps in a FIFO run-token so every ready thread gets
// a turn. Self-contained: when off, the existing g_waitMutex/g_waitCv path below runs UNCHANGED (zero
// default regression). The run-token (g_tok) is held by the one running guest thread; a thread blocked on a
// guest dispatch object releases g_tok and re-acquires it FIFO-fairly when SignalObject wakes it (g_objM/
// g_objCv). g_fair implies g_coop. Full rationale: NIGHT-LOG 'cont. 8/9'.
class FairMutex {
public:
    std::mutex m_; std::condition_variable cv_; uint64_t next_ = 0, serving_ = 0; bool held_ = false;
    void lock()   { std::unique_lock<std::mutex> lk(m_); uint64_t my = next_++;
                    cv_.wait(lk, [&]{ return !held_ && serving_ == my; }); held_ = true; }
    void unlock() { { std::lock_guard<std::mutex> lk(m_);
                    if (!held_ && getenv("REX_INITDIAG")) fprintf(stderr, "[BUG] spurious g_tok.unlock (serving=%llu next=%llu)\n", (unsigned long long)serving_, (unsigned long long)next_);
                    held_ = false; serving_++; } cv_.notify_all(); }
};
const bool g_fair = g_coop && (getenv("REX_FAIRSCHED") != nullptr);
FairMutex g_tok;                  // the fair run-token: one guest thread runs at a time, granted FIFO
std::mutex g_objM;                // guards guest dispatch-object signal_state for fair object-waits
std::condition_variable g_objCv;  // notified by SignalObject (fair mode)
// Caller holds the run-token. If pred() (a guest object is signaled) isn't already true, release the token
// so other threads run, block on g_objCv until pred()/timeout, then re-acquire the token FIFO-fairly.
// Returns true iff pred satisfied. pred reads guest state under g_objM (SignalObject writes it under g_objM).
template <class Pred> bool FairWaitUntil(Pred pred, int64_t timeoutMs) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;) {
        { std::unique_lock<std::mutex> ol(g_objM); if (pred()) return true; }  // satisfied while holding token
        g_tok.unlock();                                                        // yield the run-token
        bool sig;
        { std::unique_lock<std::mutex> ol(g_objM);
          if (timeoutMs < 0) { g_objCv.wait(ol, pred); sig = true; }
          else sig = g_objCv.wait_until(ol, deadline, pred); }
        g_tok.lock();                                                          // re-acquire FIFO-fairly
        if (timeoutMs >= 0 && !sig) { std::unique_lock<std::mutex> ol(g_objM); return pred(); }   // timed out
        // loop: re-check pred under the run-token (so an auto-reset consumed by another waiter re-waits)
    }
}
inline void FairSetSignal(uint32_t obj, int32_t state) {   // write a dispatch-object signal_state (fair mode)
    { std::lock_guard<std::mutex> ol(g_objM); GST32(obj + 0x04, static_cast<uint32_t>(state)); }
    g_objCv.notify_all();
}

std::map<uint32_t,uint32_t> g_evSignalCount, g_evWaitCount;   // obj -> count (guarded by g_evMutex)
std::map<uint32_t,uint32_t> g_evSignalLR, g_evWaitLR;         // obj -> first guest caller LR
std::mutex g_evMutex;
thread_local uint32_t g_tlSignalLR = 0;   // ctx.lr of the guest fn that called the current signal/wait
void SignalObject(uint32_t obj, int32_t state) {
    if (g_evtrace) {
        std::lock_guard<std::mutex> lk(g_evMutex);
        g_evSignalCount[obj]++;
        if (!g_evSignalLR.count(obj)) g_evSignalLR[obj] = g_tlSignalLR;
    }
    if (g_fair) { FairSetSignal(obj, state); return; }                 // fair: write under g_objM + notify g_objCv
    if (g_coop) { GST32(obj + 0x04, static_cast<uint32_t>(state)); }   // caller already holds the token
    else { std::lock_guard<std::mutex> lk(g_waitMutex); GST32(obj + 0x04, static_cast<uint32_t>(state)); }
    g_waitCv.notify_all();
}

// timeout: <0 = infinite, 0 = poll, >0 = milliseconds. Returns 0 (success) or 0x102 (STATUS_TIMEOUT).
// Releases the execution token while blocked (lets other guest threads run), re-holds it on resume.
uint32_t WaitObject(uint32_t obj, int64_t timeoutMs) {
    auto signaled = [&]{ return static_cast<int32_t>(GLD32(obj + 0x04)) > 0; };
    if (g_evtrace) {
        std::lock_guard<std::mutex> lk2(g_evMutex);
        g_evWaitCount[obj]++;
        if (!g_evWaitLR.count(obj)) g_evWaitLR[obj] = g_tlSignalLR;
    }
    if (g_fair) {                                  // fair: release the run-token, wait on g_objCv, re-acquire FIFO
        bool ok = FairWaitUntil(signaled, timeoutMs);
        if (ok) { std::lock_guard<std::mutex> ol(g_objM); uint8_t type = g_base[obj + 0x00];
            if (type == 1 || type == 2 || type == 5)
                GST32(obj + 0x04, static_cast<uint32_t>(static_cast<int32_t>(GLD32(obj + 0x04)) - 1)); }
        return ok ? 0 : 0x00000102u;
    }
    std::unique_lock<std::mutex> lk = g_coop ? std::unique_lock<std::mutex>(g_waitMutex, std::adopt_lock)
                                             : std::unique_lock<std::mutex>(g_waitMutex);
    bool ok = true;
    if (timeoutMs < 0) g_waitCv.wait(lk, signaled);
    else ok = g_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), signaled);
    if (ok) {                                     // consume for auto-reset event / semaphore / mutant
        uint8_t type = g_base[obj + 0x00];
        if (type == 1 || type == 2 || type == 5)
            GST32(obj + 0x04, static_cast<uint32_t>(static_cast<int32_t>(GLD32(obj + 0x04)) - 1));
    }
    if (g_coop) lk.release();                      // coop: keep the token held as we resume running
    return ok ? 0 : 0x00000102u;
}

int64_t TimeoutMs(uint32_t timeoutPtr) {
    if (!timeoutPtr) return -1;                  // NULL -> infinite
    int64_t t = static_cast<int64_t>(GLD64(timeoutPtr));
    if (t == 0) return 0;                        // poll
    if (t < 0) return (-t) / 10000;              // relative 100ns -> ms
    return 5000;                                 // absolute -> bound it (bring-up)
}

void GuestThreadRun(ThreadRec* rec) {
    if (g_fair && getenv("REX_INITDIAG")) fprintf(stderr, "[initdiag] GuestThreadRun start=0x%X WAITING (g_tok next=%llu serving=%llu held=%d)\n",
        rec->startAddr, (unsigned long long)g_tok.next_, (unsigned long long)g_tok.serving_, (int)g_tok.held_);
    if (g_fair) g_tok.lock(); else if (g_coop) g_waitMutex.lock();   // acquire the execution token (FIFO-fair if g_fair); no token under REX_NOTOKEN (preemptive)
    if (g_fair && getenv("REX_INITDIAG")) fprintf(stderr, "[initdiag] GuestThreadRun start=0x%X GOT token -> running\n", rec->startAddr);
    tl_guestStart = rec->startAddr;          // for the REX_CSLEAK orphaned-critical-section diagnostic
    PPCContext ctx{};
    ctx.fpscr.csr = 0x1F80;                  // default MXCSR: all FP exceptions masked
    ctx.r1.u64 = rec->stackTop - 0x200;
    ctx.r13.u64 = rec->kpcr;
    if (rec->xapiStartup) {                  // XAPI trampoline: startup(start_address, start_context)
        ctx.r3.u64 = rec->startAddr;
        ctx.r4.u64 = rec->startContext;
        CallGuest(rec->xapiStartup, ctx);
    } else {                                 // raw thread: start_address(start_context)
        ctx.r3.u64 = rec->startContext;
        CallGuest(rec->startAddr, ctx);
    }
    CsExitCleanup(rec->startAddr);           // preemptive: release CSes this thread still holds (orphan fix) / REX_CSLEAK warn
    SignalObject(rec->threadAddr, 1);        // wake anyone waiting on this thread object (header type 6)
    if (g_fair) g_tok.unlock(); else if (g_coop) g_waitMutex.unlock();
}
} // namespace

namespace kernel {
// The cooperative execution token (see kernel.h). g_waitMutex is held by the running guest thread.
// No-ops under REX_NOTOKEN (preemptive mode).
void LockGuestExecution()   { if (g_fair) g_tok.lock();   else if (g_coop) g_waitMutex.lock(); }
void UnlockGuestExecution() { if (g_fair) g_tok.unlock(); else if (g_coop) g_waitMutex.unlock(); }
} // namespace kernel

// NTSTATUS ExCreateThread(*handle r3, stack r4, *tid r5, xapi r6, start r7, ctx r8, flags r9)
PPC_FUNC(__imp__ExCreateThread)
{
    uint32_t pHandle = ctx.r3.u32, stackSize = ctx.r4.u32, pThreadId = ctx.r5.u32;
    uint32_t xapi = ctx.r6.u32, startAddr = ctx.r7.u32, startCtx = ctx.r8.u32, flags = ctx.r9.u32;
    uint32_t tid = g_nextThreadId.fetch_add(1);
    auto* rec = new ThreadRec{};
    rec->startAddr = startAddr; rec->startContext = startCtx; rec->xapiStartup = xapi;
    rec->kpcr = CreateGuestThreadContext(stackSize, startAddr, tid, &rec->threadAddr, &rec->stackTop);
    uint32_t handle;
    {
        std::lock_guard<std::mutex> lk(g_handleMutex);
        handle = g_nextHandle++;
        g_handles[handle] = rec;
    }
    if (pHandle) PPC_STORE_U32(pHandle, handle);
    if (pThreadId) PPC_STORE_U32(pThreadId, tid);
    bool suspended = (flags & 1) != 0;       // X_CREATE_SUSPENDED
    if (!suspended && !g_noSpawn) { rec->started = true; rec->th = std::thread(GuestThreadRun, rec); rec->th.detach(); }
    KTRACE("ExCreateThread(start=0x%X ctx=0x%X xapi=0x%X flags=0x%X) -> handle=0x%X tid=%u%s\n",
        startAddr, startCtx, xapi, flags, handle, tid, suspended ? " (suspended)" : "");
    ctx.r3.u64 = 0;  // STATUS_SUCCESS
}

// NTSTATUS NtResumeThread(handle r3, *suspend_count r4)
PPC_FUNC(__imp__NtResumeThread)
{
    uint32_t handle = ctx.r3.u32, pCount = ctx.r4.u32;
    ThreadRec* rec = nullptr;
    { std::lock_guard<std::mutex> lk(g_handleMutex); auto it = g_handles.find(handle);
      if (it != g_handles.end()) rec = it->second; }
    if (pCount) PPC_STORE_U32(pCount, 1);
    if (rec && !g_noSpawn && !rec->started.exchange(true)) {
        rec->th = std::thread(GuestThreadRun, rec); rec->th.detach();
        KTRACE("NtResumeThread(0x%X) -> started\n", handle);
    }
    ctx.r3.u64 = 0;
}

// NTSTATUS ObReferenceObjectByHandle(handle r3, type r4, *out_object r5) -> guest object ptr (KTHREAD)
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    uint32_t handle = ctx.r3.u32, pOut = ctx.r5.u32;
    uint32_t obj = (handle == 0xFFFFFFFE) ? PPC_LOAD_U32(ctx.r13.u32 + 0x100) : ResolveObject(handle);
    if (pOut) PPC_STORE_U32(pOut, obj);
    ctx.r3.u64 = obj ? 0 : 0xC0000008u;  // STATUS_INVALID_HANDLE
}

PPC_FUNC(__imp__ObDereferenceObject) { ctx.r3.u64 = 0; }

// LONG KeSetAffinityThread(thread, affinity, *prev) — no-op (host scheduler), report prev=0.
PPC_FUNC(__imp__KeSetAffinityThread) { if (ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, 0); ctx.r3.u64 = 0; }

PPC_FUNC(__imp__ExRegisterTitleTerminateNotification) { /* no-op */ }

// void KeInitializeDpc(dpc r3, routine r4, context r5)
PPC_FUNC(__imp__KeInitializeDpc)
{
    uint32_t dpc = ctx.r3.u32;
    if (!dpc) return;
    PPC_STORE_U16(dpc + 0x00, 19);          // type
    PPC_STORE_U8 (dpc + 0x02, 0);
    PPC_STORE_U8 (dpc + 0x03, 0);
    PPC_STORE_U32(dpc + 0x10, ctx.r4.u32);  // routine
    PPC_STORE_U32(dpc + 0x14, ctx.r5.u32);  // context
}

// ---- GPU / vblank pump (goal step: video) ----------------------------------------------------------
namespace {
uint32_t g_ringBufferBase = 0, g_ringBufferSize = 0, g_rptrWriteBack = 0;
uint32_t g_interruptCallback = 0, g_interruptData = 0;
std::atomic<bool> g_vblankRunning{false};
uint32_t g_pumpKpcr = 0, g_pumpStack = 0;
std::atomic<uint32_t> g_vblankCount{0};

// CP synchronization (REX_NOTOKEN). The VblankPump is a HOST thread that runs ExecuteRing + the graphics-
// interrupt callback as GUEST code. Under the cooperative token that is serialized with the guest; under
// NOTOKEN the pump holds NO lock and races the guest's GPU submission/state — the source of the menu's
// gfx-interrupt + completion-object crashes (sub_821C7170 deref of a half-set device field, etc.). g_gpuMutex
// serializes the pump's CP+interrupt work with the guest's GPU-boundary functions (the ring kick sub_821C6600
// and the completion-object setup sub_821C73D8, which writes device+10900) WITHOUT a global run-token, so the
// decoders / menu logic stay concurrent. Recursive: the pump's callback may re-enter a hooked GPU fn.
std::recursive_mutex g_gpuMutex;
struct GpuLock { bool on; GpuLock() : on(g_preempt) { if (on) g_gpuMutex.lock(); } ~GpuLock() { if (on) g_gpuMutex.unlock(); } };

// Xbox 360 GPU register window is at guest 0x7FC80000 (reg index r -> 0x7FC80000 + r*4). The guest
// kicks the ring buffer by writing the write index to CP_RB_WPTR (0x1C5); the GPU reports progress in
// CP_RB_RPTR (0x1C4) and the title's RPtr write-back location. Ref: rexglue-sdk graphics_system.cpp.
constexpr uint32_t kReg_CP_RB_RPTR = 0x7FC80000 + 0x1C4 * 4;   // 0x7FC80710
constexpr uint32_t kReg_CP_RB_WPTR = 0x7FC80000 + 0x1C5 * 4;   // 0x7FC80714
constexpr uint32_t GpuRegAddr(uint32_t r) { return 0x7FC80000 + r * 4; }

// The Xbox 360 GPU register window (0x7FC80000) is plain guest memory in variant A — there is no MMIO
// read interception. But rexglue's GraphicsSystem::ReadRegister (graphics_system.cpp:241) returns SPECIFIC
// non-zero values for several registers that the title polls during graphics init; left at 0, the title's
// interrupt/vblank machinery never arms. The most important is reg 0x1951 (interrupt status): the guest
// graphics-interrupt callback sub_821C7170's vblank path reads it (mem 0x7FC86544) and SKIPS the whole
// vblank handler sub_821BF748 unless bit 0 is set — prod hardcodes ReadRegister(0x1951)==1. We replicate
// prod's reads by pre-seeding these locations in guest memory (and re-asserting the volatile 0x1951 on
// every vblank, since the guest may clear it after handling). Values 1:1 with ReadRegister's switch.
static void InitGpuRegisters() {
    GST32(GpuRegAddr(0x0F00), 0x08100748);   // RB_EDRAM_TIMING
    GST32(GpuRegAddr(0x0F01), 0x0000200E);   // RB_BC_CONTROL
    GST32(GpuRegAddr(0x194C), 720);          // R500_D1MODE_V_COUNTER = min(display_height,0xFFF)
    GST32(GpuRegAddr(0x1951), 1);            // interrupt status = vblank
    GST32(GpuRegAddr(0x1961), (1280u << 16) | 720u);  // AVIVO_D1MODE_VIEWPORT_SIZE = w<<16|h
}

// Fire the guest graphics-interrupt callback (source: 0=vblank, 1=command-buffer complete) on the pump's
// own context. Caller holds the cooperative token. Per rexglue, the TLS ptr must read 0 during interrupts.
// cpu = the logical processor to deliver on (prod's DispatchInterruptCallback sets the active CPU before
// invoking; vblank is delivered on cpu 2). The callback reads it from KPCR+0x10C (+268).
static void FireGfxInterrupt(uint32_t cb, uint32_t source, uint32_t cpu = 2) {
    // STOPGAP: the source=1 (command-buffer-complete) handler sub_821C7170 dereferences the device's active
    // completion object at *(interruptData+10900) and clears the firing CPU's ack bit in *(obj+0). The title
    // sets that field per submission (sub_821C73D8) and leaves it the sentinel 0xFFFFFFFF between submissions.
    // Variant A's stub CP re-runs the ring from the VblankPump, so it can fire this completion interrupt when
    // no completion is pending -> a wild deref of 0xFFFFFFFF -> SIGSEGV. Skip the spurious interrupt then
    // (there is nothing to acknowledge). [stopgap — like the fence-forward; a real CP would track completions.]
    static const bool g_intlog = getenv("REX_INTLOG") != nullptr;
    if (source == 1 && g_intlog) {
        uint32_t B = g_interruptData ? GLD32(g_interruptData + 10900) : 0;
        bool bv = (g_interruptData && B && B != 0xFFFFFFFFu);
        fprintf(stderr, "[int] FireGfx src=1 cpu=%u iData=0x%08X B=0x%08X B[0]=0x%08X *(B+0x10)=0x%08X *(B+0x14)=0x%08X -> %s\n",
                cpu, g_interruptData, B, bv ? GLD32(B) : 0, bv ? GLD32(B + 0x10) : 0, bv ? GLD32(B + 0x14) : 0,
                (g_interruptData && B == 0xFFFFFFFFu) ? "SKIP(sentinel)" : "FIRE");
    }
    // REX_BOOTSTRAP (cont.14): the render producer sub_821CC7A0 is the completion callback at *(B+0x10),
    // B=*(device+10900); in variant A that slot is null (the per-frame submit that registers it never runs),
    // so the source=1 interrupt's null-check skips it -> producer never fires -> consumer (tid=10) never
    // signalled -> kick-gate counter device+0x2b04 never decremented -> no kicks. Bootstrap by registering
    // the producer so the interrupt fires it; the producer then enqueues + KeSetEvents the consumer.
    static const bool g_bootstrap = getenv("REX_BOOTSTRAP") != nullptr;
    if (g_bootstrap && source == 1 && g_interruptData) {
        uint32_t B = GLD32(g_interruptData + 10900);
        if (B && B != 0xFFFFFFFFu && GLD32(B + 0x10) == 0)
            GST32(B + 0x10, 0x821CC7A0u);
    }
    if (source == 1 && g_interruptData && GLD32(g_interruptData + 10900) == 0xFFFFFFFFu) return;
    PPCContext ctx{};
    ctx.fpscr.csr = 0x1F80;              // default MXCSR: all FP exceptions masked
    ctx.r1.u64 = g_pumpStack - 0x200;
    ctx.r13.u64 = g_pumpKpcr;
    ctx.r3.u64 = source;
    ctx.r4.u64 = g_interruptData;
    uint32_t savedTls = GLD32(g_pumpKpcr + 0x00);
    GST32(g_pumpKpcr + 0x00, 0);
    GST8(g_pumpKpcr + 0x10C, static_cast<uint8_t>(cpu));   // active processor number
    CallGuest(cb, ctx);
    GST32(g_pumpKpcr + 0x00, savedTls);
}

// ---- PM4 command-processor interpreter -------------------------------------------------------------
// Executes the PM4 stream the title writes to the ring buffer (and the indirect buffers it references),
// producing the side effects the title's render loop waits on — EVENT_WRITE_SHD memory fences and
// INTERRUPT graphics-interrupt callbacks — so the GPU-completion events get satisfied. Structure 1:1
// with rexglue command_processor.cpp (ExecutePrimaryBuffer / ExecutePacket / ExecutePacketType*).
// ADDRESS MODEL: variant A's flat 4 GiB map places "physical" GPU memory (MmAllocatePhysicalMemoryEx,
// the ring @0xA0002000, the IBs) in the 0xA0000000 window. The PM4 carries physical addresses with their
// mirror bits stripped (e.g. IB addr 0x90040), so a physical/GPU address p resolves to guest
// 0xA0000000 | (p & 0x1FFFFFFF). VERIFIED: IB#1 phys 0x90040 -> 0xA0090040 holds real PM4 (WAIT_REG_MEM),
// while raw 0x90040 is zero. Ref: xenos.h GpuToCpu/CpuToGpu + xmemory.h TranslatePhysical.
namespace {
inline uint32_t TranslatePhys(uint32_t p) { return 0xA0000000u | (p & 0x1FFFFFFFu); }

enum { PM4_ME_INIT=0x48, PM4_NOP=0x10, PM4_INDIRECT_BUFFER=0x3F, PM4_INDIRECT_BUFFER_PFD=0x37,
       PM4_WAIT_REG_MEM=0x3C, PM4_EVENT_WRITE=0x46, PM4_EVENT_WRITE_SHD=0x58, PM4_EVENT_WRITE_EXT=0x5A,
       PM4_DRAW_INDX=0x22, PM4_DRAW_INDX_2=0x36, PM4_INTERRUPT=0x54, PM4_XE_SWAP=0x64,
       PM4_SET_CONSTANT=0x2D, PM4_SET_CONSTANT2=0x55 };
constexpr uint32_t XE_GPU_REG_VGT_EVENT_INITIATOR = 0x21F9;

std::atomic<uint32_t> g_gpuCounter{0};   // GPU swap counter — EVENT_WRITE_SHD is_counter fences read it
// T2b-step-1: set true while REX_EXECSEGS executes the deferred segments, so DRAW_INDX can capture EACH
// draw's LIVE fetch slot-0 (resolving the census-vs-execution open sub-Q + confirming per-draw verts).
thread_local bool tl_execsegs = false;
// T2b-step-2: per-draw carved geometry accumulated during a REX_EXECSEGS frame (clip-space XY, interleaved),
// submitted to the render bridge after the exec loop. Touched ONLY on the VdSwap thread under tl_execsegs.
thread_local std::vector<float> tl_esVerts;
// Task #8 (REX_MENUTEX): the backdrop quadrants carved as TEXTURED geometry — interleaved pos.xy (clip) +
// uv.xy (synthetic screen→[0,1]) — submitted to the textured pipeline (disk-loaded background .png) so the
// backdrop renders as real art instead of debug-colored rects. Same per-frame lifetime as tl_esVerts.
thread_local std::vector<float> tl_esTexVerts;
thread_local uint32_t tl_s1cursor = 0;   // REX_SPRITECARVE: byte cursor into the slot-1 vert pool, per frame
// T2b-step-5: the VS microcode loaded by the most-recent IM_LOAD_IMMEDIATE (op 0x2B) during execution — so a
// DRAW can decode the VS's OWN vfetch (which fetch slot + dword offset it reads). The sprite/text VS's slot
// 0 is a texture, so their verts come from a different slot/offset that only the vfetch instruction names.
thread_local uint32_t tl_vsUcode = 0, tl_vsSize = 0;
// REX_UITEXT (task #5 experiment): the UI text verts exist only at memcpy-fill time (local-space stride-16,
// recycled by exec time); the per-draw transform exists only at exec time (reg 0x4000, recorded in the
// segment). Bridge them: snapshot each text fill (guest thread, by vert COUNT) → at exec, pair the Nth
// prim-13 draw to the snapshot whose count == its numI (count is a strong discriminator), apply the live
// reg-0x4000 World+screen-ortho transform, render. Per-frame: the swap thread swaps out the accumulated
// snapshots, consumes by matching count, clears for the next frame. UNVERIFIED pairing (build-order vs
// draw-order) — gated, render-verified by eye.
struct TxtSnap { uint32_t count; std::vector<float> pos; bool used; };   // pos = count*2 (local x,y per vert)
std::mutex g_txtMtx; std::vector<TxtSnap> g_txtSnaps;                    // guest thread appends; swap drains
thread_local std::vector<TxtSnap>* tl_txtFrame = nullptr;               // swap-local snapshot view during exec
std::atomic<uint64_t> g_drawCount{0}, g_swapCount{0};
const bool g_cptrace = (getenv("REX_CPTRACE") != nullptr);

inline void WriteGpuReg(uint32_t index, uint32_t value) {
    GST32(0x7FC80000u + index * 4u, value);
    // R1 (REX_TEXBIND): does the title EVER bind a real TEXTURE base to a fetch constant anywhere (ring / init /
    // segment)? A Xenos texture fetch constant's d1 holds the base in bits[31:12]; vertex pools live at
    // 0x01xxxxxx (below the filter), so a write to the fetch region (0x4800..0x48FF) whose value masks to a
    // base in [0x04000000,0x20000000) is a TEXTURE bind. NONE ever = the bind/create path is fully stubbed;
    // SOME = bound but those draws aren't reached (the A↔B title-advance gate). Dedup by (reg,base). Gated.
    static const bool s_texbind = getenv("REX_TEXBIND") != nullptr;
    if (s_texbind && index >= 0x4800u && index < 0x4900u && ((index - 0x4800u) % 6u) == 1u) {
        uint32_t b = value & 0xFFFFF000u;   // d1 of a texture fetch slot holds the base in bits[31:12]
        // categorize to cut noise: UI/texture allocs (phys 0x04..0x06, incl. UI.xzp 0x048D + the 0xA494..0xA51C
        // texture blocks) and EDRAM (phys 0x10). Skip the rest (sizes/control words coincidentally in range).
        bool tex = (b >= 0x04000000u && b < 0x06000000u), edram = (b >= 0x10000000u && b < 0x10100000u);
        if (tex || edram) {
            static std::mutex m; static std::unordered_set<uint64_t> seen;
            std::lock_guard<std::mutex> lk(m);
            uint64_t key = (static_cast<uint64_t>(index) << 32) | b;
            if (seen.size() < 120 && seen.insert(key).second) {
                // R1 decisive: is the BOUND texture populated? sample 64 dwords for non-zero/varied content.
                uint32_t ga = 0xA0000000u | b, nz = 0, vr = 0, prev = 0;
                for (int i = 0; i < 64; i++) { uint32_t w = GLD32(ga + i*4); if (w) nz++; if (i && w != prev) vr++; prev = w; }
                // Texture-decode prep: the Xenos 2D texture fetch constant — d0 holds type(1:0)+endian+tiled(bit?),
                // d1 holds base[31:12]+data_format(5:0), d2 holds (width-1)[12:0]+(height-1)[25:13]. Capture the
                // FULL constant for POPULATED textures so I can decode them (format+dims+tiled → untile → RGBA).
                uint32_t d0 = GLD32(0x7FC80000u + (index-1u)*4u), d2 = GLD32(0x7FC80000u + (index+1u)*4u);
                uint32_t fmt = value & 0x3Fu, w_ = (d2 & 0x1FFFu)+1u, h_ = ((d2>>13)&0x1FFFu)+1u;
                // Documented xe_gpu_texture_fetch_t: tiled = dword0 bit31 (NOT bit2 — that's sign_x; the
                // old (d0>>2)&1 was wrong), endianness = dword1[7:6]. Matches the movie-frame parse below.
                uint32_t tiled = (d0>>31)&1u, endian = (value>>6)&3u;
                fprintf(stderr, "[texbind] %s slot %u d1=0x%08X -> texBase=0x%08X (%s) %ux%u fmt=0x%X tiled=%u endian=%u d0=%08X d2=%08X | DATA nz=%u/64 varied=%u/63 first=%08X %08X\n",
                        tl_execsegs ? "SEG " : "RING", (index - 0x4800u) / 6u, value, ga, edram ? "EDRAM-RT" : "texture",
                        w_, h_, fmt, tiled, endian, d0, d2, nz, vr, GLD32(ga), GLD32(ga+4));
                // REX_TEXDECODE: when a POPULATED texture binds, run the new decoder (untile + format-convert)
                // and dump a PPM — the real-data proof of GPU-RESOURCE-BUILD-PLAN piece 2. Capped + de-duped
                // (the outer `seen` set already dedups by reg+base). Default-off; no effect on the boot path.
                static const bool s_texdecode = getenv("REX_TEXDECODE") != nullptr;
                if (s_texdecode && !edram && nz >= 8 && rex_tex::BytesPerBlock(fmt) != 0) {
                    static std::atomic<int> s_n{0};
                    int n = s_n.fetch_add(1);
                    if (n < 16) {
                        rex_tex::Desc d{}; d.guestBase = ga; d.format = fmt; d.width = w_; d.height = h_;
                        d.tiled = tiled; d.endian = endian; d.pitchTexels = 0;
                        std::vector<uint8_t> rgba;
                        if (rex_tex::DecodeGuestToRGBA(d, rgba) && !rgba.empty()) {
                            char path[96]; snprintf(path, sizeof path, "/tmp/texdec_%02d_%08X_%ux%u_f%02X.ppm", n, ga, w_, h_, fmt);
                            rex_tex::WriteRGBAasPPM(path, rgba.data(), w_, h_);
                            fprintf(stderr, "[texdecode] wrote %s (tiled=%u endian=%u)\n", path, tiled, endian);
                        }
                    }
                }
            }
        }
    }
}

void ExecutePM4(uint32_t addr, uint32_t dwords, int depth);   // fwd

// Execute one type-3 packet body. `addr` is at the first data dword; `count` data dwords follow.
void ExecuteType3(uint32_t addr, uint32_t op, uint32_t count, int depth) {
    if (g_cptrace) fprintf(stderr, "[cp]%*s T3 op=0x%02X count=%u d0=0x%X d1=0x%X d2=0x%X\n", depth*2, "", op, count,
                           GLD32(addr), count>1?GLD32(addr+4):0, count>2?GLD32(addr+8):0);
    // GPU-build step (a): result-gate scan (REX_RESULTSCAN). The A<->B coupling gate is a "real GPU
    // result" the title observes before it kicks the content IBs. Faking the swap counter / completion
    // fence (which live in memory) is exhausted, so the gate must be something only real pixel work
    // produces: an occlusion query (ZPASS_DONE / VIZ_QUERY), a GPU->CPU memory poll (WAIT_REG_MEM), a
    // register readback (REG_TO_MEM), or an EDRAM resolve (RB_COPY). Log each as the CP executes the
    // kicked stream so we can pin which one. Event/op/reg constants per rexglue xenos.h. Gated, default off.
    static const bool s_resultscan = getenv("REX_RESULTSCAN") != nullptr;
    if (s_resultscan) {
        static std::atomic<int> rsn{0};
        if (rsn.load() < 400) switch (op) {
          case 0x3C: {                                   // WAIT_REG_MEM: poll mem/reg until (val&mask) <fn> ref
              uint32_t info=GLD32(addr), poll=GLD32(addr+4), ref=GLD32(addr+8), mask=GLD32(addr+12);
              static const char* fn[8]={"always","<","<=","==","!=",">=",">","never"};
              fprintf(stderr,"[result] WAIT_REG_MEM %s poll=%s:0x%X ref=0x%X mask=0x%X\n",
                      fn[info&7],(info&0x10)?"mem":"reg",poll,ref,mask); ++rsn; break; }
          case 0x52: case 0x53:                          // WAIT_REG_EQ / WAIT_REG_GTE
              fprintf(stderr,"[result] WAIT_REG_%s reg=0x%X ref=0x%X\n",op==0x52?"EQ":"GTE",GLD32(addr),GLD32(addr+4)); ++rsn; break;
          case 0x46: { uint32_t ev=GLD32(addr)&0x3F;     // EVENT_WRITE
              if (ev==21||ev==7||ev==8) { fprintf(stderr,"[result] EVENT_WRITE event=%u (%s)\n",ev,
                  ev==21?"ZPASS_DONE":ev==7?"VIZQUERY_START":"VIZQUERY_END"); ++rsn; } break; }
          case 0x5B:                                     // EVENT_WRITE_ZPD: occlusion (z-pass) sample count
              fprintf(stderr,"[result] EVENT_WRITE_ZPD (ZPASS_DONE) sampleCountAddr(reg0x2325)=0x%X\n",
                      GLD32(0x7FC80000u+0x2325*4u)); ++rsn; break;
          case 0x23:                                     // VIZ_QUERY: begin/end viz (occlusion) query
              fprintf(stderr,"[result] VIZ_QUERY d0=0x%X\n",GLD32(addr)); ++rsn; break;
          case 0x3E:                                     // REG_TO_MEM: read GPU reg -> system memory (readback)
              fprintf(stderr,"[result] REG_TO_MEM reg=0x%X -> mem 0x%X\n",GLD32(addr)&0xFFFF,GLD32(addr+4)); ++rsn; break;
          default: break;
        }
    }
    switch (op) {
      case PM4_INDIRECT_BUFFER:
      case PM4_INDIRECT_BUFFER_PFD: {
        uint32_t ibAddr = GLD32(addr), ibLen = GLD32(addr + 4) & 0xFFFFF;
        if (g_cptrace) fprintf(stderr, "[cp]%*s IB 0x%X (phys 0x%X) len=%u\n", depth*2, "", ibAddr, TranslatePhys(ibAddr), ibLen);
        ExecutePM4(TranslatePhys(ibAddr), ibLen, depth + 1);
        break;
      }
      case PM4_INTERRUPT: {
        uint32_t cpuMask = GLD32(addr);
        static const bool intlog2 = getenv("REX_INTLOG") != nullptr;
        if (g_cptrace || intlog2) fprintf(stderr, "[int] PM4_INTERRUPT mask=0x%X cb=0x%08X\n", cpuMask, g_interruptCallback);
        if (g_interruptCallback)
            for (int n = 0; n < 6; n++) if (cpuMask & (1u << n)) FireGfxInterrupt(g_interruptCallback, 1, n);
        break;
      }
      case PM4_EVENT_WRITE_SHD: {              // VS|PS-done event -> write a fence value to memory
        uint32_t initiator = GLD32(addr), address = GLD32(addr + 4), value = GLD32(addr + 8);
        WriteGpuReg(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);
        bool isCounter = (initiator >> 31) & 1;
        uint32_t dataValue = isCounter ? g_gpuCounter.load() : value;
        address &= ~0x3u;                       // low 2 bits = endianness; the standard fence is k8in32
        GST32(TranslatePhys(address), dataValue);   // BE store => guest's BE load reads back dataValue
        if (g_cptrace) fprintf(stderr, "[cp] EVENT_WRITE_SHD addr=0x%X val=0x%X cnt=%d\n", address, dataValue, isCounter);
        break;
      }
      case PM4_EVENT_WRITE:
      case PM4_EVENT_WRITE_EXT:
        WriteGpuReg(XE_GPU_REG_VGT_EVENT_INITIATOR, GLD32(addr) & 0x3F);
        break;
      case PM4_SET_CONSTANT: {                 // load constants/registers into the register file (rexglue
        uint32_t ot = GLD32(addr), index = ot & 0x7FF, type = (ot >> 16) & 0xFF;   // command_processor.cpp)
        uint32_t regBase;                       // type: 0=ALU(+0x4000) 1=FETCH(+0x4800, textures) 2=BOOL
        switch (type) { case 0: regBase=0x4000; break; case 1: regBase=0x4800; break;  // (+0x4900) 3=LOOP
                        case 2: regBase=0x4900; break; case 3: regBase=0x4908; break;  // (+0x4908) 4=REG
                        case 4: regBase=0x2000; break; default: regBase=0xFFFFFFFF; }  // (+0x2000)
        if (regBase != 0xFFFFFFFF)
            for (uint32_t i = 1; i < count; i++) WriteGpuReg(regBase + index + (i-1), GLD32(addr + i*4));
        // DEEP-BUILD trace (cont.23→24): does the segment EVER set the slot-0 vertex fetch constant (reg 0x4800,
        // index 0) to a REAL VB base? If [esset] shows index 0 written to 0x02xxxxxx (the stub pool) or never
        // written, the binding is missing/stubbed at the PM4 level (the resource-create never ran). If it's set to
        // a 0xA0xxxxxx data base, the binding exists and the bug is downstream (recycle/timing).
        if (tl_execsegs && type == 1) {
            static std::atomic<int> setn{0}; int k = setn.fetch_add(1);
            if (k < 40) fprintf(stderr, "[esset] FETCH write: index=%u count=%u d0=0x%08X d1=0x%08X (slot=%u)\n",
                                index, count, count>1?GLD32(addr+4):0, count>2?GLD32(addr+8):0, index/2);
        }
        break;
      }
      case PM4_SET_CONSTANT2: {                 // like SET_CONSTANT but 16-bit index, writes regs directly
        uint32_t index = GLD32(addr) & 0xFFFF;
        for (uint32_t i = 1; i < count; i++) WriteGpuReg(index + (i-1), GLD32(addr + i*4));
        // DEEP-BUILD trace: reg 0x4800 (slot-0 fetch) demonstrably changes between UI draws, but no type-1
        // SET_CONSTANT writes it — so SET_CONSTANT2 (or another path) does. Log writes landing in the 0x4800..
        // 0x48FF fetch window to find who binds the (texture) constants — the same channel that SHOULD bind verts.
        if (tl_execsegs && index >= 0x4800 && index < 0x4900) {
            static std::atomic<int> s2n{0}; int k = s2n.fetch_add(1);
            if (k < 40) fprintf(stderr, "[esset2] SET_CONSTANT2 reg=0x%X count=%u d0=0x%08X d1=0x%08X\n",
                                index, count, count>1?GLD32(addr+4):0, count>2?GLD32(addr+8):0);
        }
        break;
      }
      case PM4_DRAW_INDX: case PM4_DRAW_INDX_2: {
        // op 0x22 (DRAW_INDX): VGT_DRAW_INITIATOR is data[1] (data[0] is a control/viz word); op 0x36
        // (DRAW_INDX_2): it's data[0]. Reading data[0] for BOTH misdecoded every 0x22 draw (e.g. the bogus
        // prim=11 numI=33024 from a 0x8100_000B word). The census already reads data[1] for 0x22.
        uint32_t init = (op == PM4_DRAW_INDX) ? GLD32(addr + 4) : GLD32(addr);
        uint32_t numInd = init >> 16, prim = init & 0x3F;
        g_drawCount.fetch_add(1);
        // T2b-step-1: during REX_EXECSEGS, capture THIS draw's live fetch slot-0 (the foundation vkCmdDraw needs,
        // and resolves the open sub-Q: do ALL ~7-8 draws/frame point at real kVertex verts, or only the last?).
        if (tl_execsegs) {
            static std::atomic<int> esd{0}; int k = esd.fetch_add(1);
            if (k < 48) {
                uint32_t fc0 = GLD32(0x7FC80000u + 0x4800u*4u), base = fc0 & 0xFFFFFFFCu;
                uint32_t g = 0xA0000000u | (base & 0x1FFFFFFFu), d1 = GLD32(0x7FC80000u + 0x4801u*4u);
                float v[4]; for (int i=0;i<4;i++){ uint32_t u=GLD32(g+i*4); memcpy(&v[i],&u,4); }
                // Find the draw's VERTEX source: scan fetch slots (2dw each) for the first type-3 (kVertex)
                // entry — the backdrop uses slot 0, but the sprite/text draws' slot 0 is a texture (type 2),
                // so their verts must be in a different slot. vslot=-1 = no kVertex fetch (verts elsewhere/none).
                int vslot=-1; uint32_t vbase=0, vwords=0;
                for (int s=0;s<48;s++){ uint32_t f0=GLD32(0x7FC80000u+(0x4800u+s*2)*4u);
                    if((f0&3)==3){ uint32_t f1=GLD32(0x7FC80000u+(0x4800u+s*2+1)*4u);
                        vslot=s; vbase=0xA0000000u|((f0&0xFFFFFFFCu)&0x1FFFFFFFu); vwords=(f1>>2)&0xFFFFFFu; break; } }
                fprintf(stderr, "[esdraw] #%d op=0x%X numI=%u prim=%u | slot0 fc=%08X type=%u | vtxSlot=%d base=0x%X words=%u\n",
                        k, op, numInd, prim, fc0, fc0&3, vslot, vbase, vwords);
                // For the op-0x22 sprite/text draws: src_select (init[7:6]: 0=DMA/indexed, 2=auto-index),
                // index fmt (init[11]: 0=u16,1=u32), index buffer (data[2]=base, data[3]=size). If indexed,
                // read the first indices + the slot-1 verts they point at — tells us how to carve these draws
                // (and whether the verts are screen coords like the backdrop, or need $worldviewProj).
                if (op == 0x22 && vslot >= 0) {
                    uint32_t initw = GLD32(addr + 4);   // op-0x22 initiator = data[1]
                    uint32_t srcSel = (initw >> 6) & 3, idxFmt = (initw >> 11) & 1;
                    uint32_t ibRaw = GLD32(addr + 8), ibSz = GLD32(addr + 12);
                    uint32_t ig = 0xA0000000u | (ibRaw & 0x1FFFFFFFu);
                    uint32_t i01 = GLD32(ig), i23 = GLD32(ig + 4);                 // u16 BE indices: hi/lo per dword
                    uint32_t idx[4] = { i01 >> 16, i01 & 0xFFFF, i23 >> 16, i23 & 0xFFFF };
                    char vb[160]; size_t vo = 0; vb[0] = 0;
                    for (int j = 0; j < 3; j++) { uint32_t ix = idxFmt ? GLD32(ig + j*4) : idx[j];
                        uint32_t ux = GLD32(vbase + ix*8), uy = GLD32(vbase + ix*8 + 4); float fx, fy;
                        memcpy(&fx,&ux,4); memcpy(&fy,&uy,4);
                        vo += snprintf(vb+vo, sizeof(vb)-vo, " v[%u]=(%.1f,%.1f)", ix, fx, fy); }
                    fprintf(stderr, "[esidx] #%d prim=%u src=%u fmt=%s idxBase=0x%X sz=0x%X idx=[%u %u %u %u]%s\n",
                            k, prim, srcSel, idxFmt?"u32":"u16", ig, ibSz, idx[0],idx[1],idx[2],idx[3], vb);
                }
                // T2b-step-5: decode the ACTIVE VS's own vfetch (the authoritative per-draw vertex source —
                // which fetch slot + dword OFFSET it reads). The sprite/text verts sit deep in the slot-1 pool;
                // a non-zero vfetch offset (vs the backdrop VS's off=0) would explain where. Decode the verts
                // at base + off*stride to confirm they're real screen/authoring coords.
                if ((prim==5 || prim==13 || prim==4) && tl_vsUcode && tl_vsSize >= 3) {
                    for (uint32_t w=0; w+3 <= tl_vsSize && w < 256; w++) {
                        uint32_t a0=GLD32(tl_vsUcode+w*4), a1=GLD32(tl_vsUcode+w*4+4), a2=GLD32(tl_vsUcode+w*4+8);
                        if ((a0&0x1F)!=0 || !((a0>>19)&1)) continue;          // kVertexFetch + must_be_one
                        uint32_t fslot=((a0>>20)&0x1F)*3+((a0>>25)&3), ffmt=(a1>>16)&0x3F, fstr=a2&0xFF;
                        int foff=(int)((a2>>8)&0x7FFFFF); if(foff&0x400000) foff-=0x800000;   // signed 23-bit dwords
                        uint32_t fc=GLD32(0x7FC80000u+(0x4800u+fslot*2)*4u), fb=0xA0000000u|((fc&0xFFFFFFFCu)&0x1FFFFFFFu);
                        uint32_t va=fb + (uint32_t)foff*4u; float vx,vy; uint32_t ux=GLD32(va),uy=GLD32(va+4);
                        memcpy(&vx,&ux,4); memcpy(&vy,&uy,4);
                        fprintf(stderr,"[esvf] draw#%d prim=%u VS vfetch slot=%u type=%u fmt=0x%X stride=%udw off=%ddw | base=0x%X +off=0x%X v0=(%.1f,%.1f)\n",
                                k, prim, fslot, fc&3, ffmt, fstr, foff, fb, va, vx, vy);
                        break;
                    }
                }
            }
            // Task #4: which PRIMS does execsegs actually execute? Log each distinct prim once (across the whole
            // run, past the [esdraw] cap) — confirms whether the rich UI (prim 5 sprites / prim 13 text the census
            // found) is ever in the executed segments, or only the prim-8 backdrop + prim-0 setup.
            { static std::atomic<uint32_t> primSeen{0}; uint32_t bit = 1u << (prim & 31);
              if (!(primSeen.load() & bit)) { primSeen.fetch_or(bit);
                  fprintf(stderr, "[esprim] NEW prim=%u numI=%u fetchType=%u\n", prim, numInd, GLD32(0x7FC80000u+0x4800u*4u)&3); } }
            // REX_SCENE (survey): per draw, the live reg-0x4000 transform (World-translate Tx/Ty + Proj scale
            // Px/Py) + a bound texture, and whether the element lands ON-SCREEN. Maps the whole executed scene —
            // which draws are visible, which off-screen, what textures — to decide what's worth rendering.
            if (getenv("REX_SCENE")) { static std::atomic<int> sc{0}; int sk=sc.fetch_add(1);
                if (sk < 40) {
                    auto rg=[&](uint32_t c,int cc){ float f; uint32_t w=GLD32(0x7FC80000u+(0x4000u+c*4+cc)*4u); memcpy(&f,&w,4); return f; };
                    float Tx=rg(0,3),Ty=rg(1,3),Px=rg(4,0),Pxw=rg(4,3),Py=rg(5,1),Pyw=rg(5,3);
                    // transform a representative point: the World origin (0,0) → clip, to see where the element sits
                    float ox=(0+Tx)*Px+Pxw, oy=(0+Ty)*Py+Pyw;
                    // find a bound texture (physical-window base): scan 32 fetch slots (6dw) for a 0x05xxxxxx base.
                    // Task #8 cont. (cont.25): ALSO decode the texture DIMENSIONS — the Xenos 2D texture fetch
                    // constant stores width-1 in d2[12:0], height-1 in d2[25:13], data_format in d1[5:0]. The dims
                    // are the key to mapping a UI sprite draw to a disk .png (match by size) for disk-texturing.
                    // cont.25 R1: scan ALL fetch slots 0..42 (the fetch region is 0x4800..0x48FF; texture slots are
                    // 6dw → up to 42), and PREFER a POPULATED texture (real pixel data) over an empty one — the
                    // executed draw may bind its texture in a high slot (REX_SCENE used to scan only 0..31). Report
                    // the chosen texture + its populated status + which slot, to find a draw I can render textured.
                    uint32_t texBase=0, texW=0, texH=0, texFmt=0; int texSlot=-1; bool texPop=false;
                    for(uint32_t s=0;s<43 && !texPop;s++){ uint32_t d1=GLD32(0x7FC80000u+(0x4800u+s*6u+1u)*4u);
                        uint32_t b=d1&0xFFFFF000u; if(b>=0x04000000u && b<0x20000000u){
                            uint32_t ga=0xA0000000u|b, nz=0; for(int i=0;i<32;i++) if(GLD32(ga+i*4)) nz++;
                            // keep the first bound texture as a fallback, but prefer the first POPULATED one
                            if(texSlot<0 || nz){ uint32_t d2=GLD32(0x7FC80000u+(0x4800u+s*6u+2u)*4u);
                                texBase=ga; texW=(d2&0x1FFFu)+1u; texH=((d2>>13)&0x1FFFu)+1u; texFmt=d1&0x3Fu;
                                texSlot=(int)s; texPop=(nz>0); } } }
                    bool onScreen = ox>-1.2f && ox<1.2f && oy>-1.2f && oy<1.2f;
                    fprintf(stderr, "[scene] #%d prim=%u numI=%u World=(%.0f,%.0f) origin→clip=(%.2f,%.2f) %s tex=0x%X slot=%d %s %ux%u fmt=0x%X\n",
                            sk, prim, numInd, Tx, Ty, ox, oy, onScreen?"ON":"off", texBase, texSlot, texPop?"POPULATED":"empty", texW, texH, texFmt);
                    // one-shot: does the bound texture hold real pixel data (non-zero/varied)? decides whether
                    // texturing the backdrop/sprites shows real art, or the render-to-texture never ran.
                    if (texBase) { static std::atomic<bool> tdmp{false}; bool e=false;
                      if (tdmp.compare_exchange_strong(e,true)) {
                        uint32_t nz=0,prev=0,varied=0; for(int i=0;i<256;i++){ uint32_t w=GLD32(texBase+i*4); if(w)nz++; if(i&&w!=prev)varied++; prev=w; }
                        fprintf(stderr,"[scene-tex] base=0x%X first8dw=%08X %08X %08X %08X %08X %08X %08X %08X | nz=%u/256 varied=%u/255\n",
                                texBase, GLD32(texBase),GLD32(texBase+4),GLD32(texBase+8),GLD32(texBase+12),GLD32(texBase+16),GLD32(texBase+20),GLD32(texBase+24),GLD32(texBase+28), nz, varied);
                        // also probe the STATIC font/sprite atlas (0xA5004800, from the XEX — not render-to-EDRAM):
                        // if it holds real data, a textured pipeline + this atlas = readable glyphs (a tractable step).
                        for (uint32_t fa : {0xA5004800u, 0xA5004000u}) { uint32_t fnz=0,fp=0,fv=0;
                            for(int i=0;i<256;i++){ uint32_t w=GLD32(fa+i*4); if(w)fnz++; if(i&&w!=fp)fv++; fp=w; }
                            fprintf(stderr,"[atlas] 0x%X first6dw=%08X %08X %08X %08X %08X %08X | nz=%u/256 varied=%u/255\n",
                                    fa, GLD32(fa),GLD32(fa+4),GLD32(fa+8),GLD32(fa+12),GLD32(fa+16),GLD32(fa+20), fnz, fv); } }
                    }
                }
            }
            // T2b-step-6 (REX_UITX): the prim-13 text / prim-5 sprite UI draws read LOCAL-space verts from slot-1
            // (0xA01FE0FC, the memcpy-filled buffer, x~16..74) — the VS's slot-0 binding was lost in the stubbed
            // resource-create. To place them on screen they need the per-draw transform held in the ALU consts
            // (reg 0x4000). One-shot dump the const block + the actual slot-1 verts for the first text draw to
            // recover the local->clip matrix (the one missing piece for screen-correct UI).
            if (prim == 13) { static std::atomic<bool> ad{false}; bool e=false;
              if (ad.compare_exchange_strong(e,true)) {
                fprintf(stderr, "[esalu] prim-13 text draw numI=%u — ALU consts c0..c19 (reg 0x4000):\n", numInd);
                for (int c=0;c<20;c++){ float f[4]; for(int j=0;j<4;j++){ uint32_t u=GLD32(0x7FC80000u+(0x4000u+c*4+j)*4u); memcpy(&f[j],&u,4);}
                  fprintf(stderr, "  c%-2d = %11.4f %11.4f %11.4f %11.4f\n", c, f[0],f[1],f[2],f[3]); }
                // DECISIVE (task #5): does the REAL text VB (0xA022FFF0, the sub_821F8E60 Lock allocation that the
                // memcpy filled) still hold valid local-space verts at exec time? If YES, render structurally =
                // verts from this known VB + transform from reg0x4000 (both live at exec). If zeros, it's recycled.
                for (uint32_t vb : {0xA022FFF0u, 0xA01FE0FCu}) {
                    fprintf(stderr, "  text VB @0x%X (first 6, stride-16 pos.xy+uv.xy):\n", vb);
                    for (int i=0;i<6;i++){ float p[4]; for(int j=0;j<4;j++){uint32_t u=GLD32(vb+i*16+j*4); memcpy(&p[j],&u,4);}
                      fprintf(stderr, "    v%d pos=(%9.2f,%9.2f) uv=(%.3f,%.3f)\n", i, p[0],p[1],p[2],p[3]); }
                }
              }
            }
            // REX_UITEXT: pair THIS prim-13 text draw to the text snapshot whose vert count == numI (count is a
            // strong discriminator), then apply the LIVE reg-0x4000 transform (World-translate c0.w/c1.w +
            // screen-ortho Proj c4.x/c4.w, c5.y/c5.w) the segment just set → clip-space glyph quads. Bridges the
            // fill-time verts with the exec-time transform (the two never coexist otherwise). Pairing UNVERIFIED.
            if (tl_txtFrame && prim == 13 && numInd >= 4) {
                TxtSnap* snap = nullptr;
                for (auto& sn : *tl_txtFrame) if (!sn.used && sn.count == numInd) { snap = &sn; sn.used = true; break; }
                {   // DIAG: every prim-13 text draw's transform (Tx,Ty) — to see where each label wants to land
                    static std::atomic<int> td{0}; int dk=td.fetch_add(1);
                    if (dk < 20) { auto rg=[&](uint32_t c,int cc){ float f; uint32_t w=GLD32(0x7FC80000u+(0x4000u+c*4+cc)*4u); memcpy(&f,&w,4); return f; };
                        fprintf(stderr,"[uitxt] draw#%d numI=%u paired=%d World=(%.1f,%.1f) Proj=(%.4f,%.4f)\n",
                                dk, numInd, snap!=nullptr, rg(0,3), rg(1,3), rg(4,0), rg(5,1)); } }
                if (snap) {
                    auto reg = [&](uint32_t c,int comp){ float f; uint32_t w=GLD32(0x7FC80000u+(0x4000u+c*4+comp)*4u); memcpy(&f,&w,4); return f; };
                    float Tx=reg(0,3), Ty=reg(1,3), Px=reg(4,0), Pxw=reg(4,3), Py=reg(5,1), Pyw=reg(5,3);
                    // game-accurate placement: clip = Proj * (local + World-translate). For the captured label
                    // this lands off-screen (World.y=866 > the Proj's ~714 screen height) — a genuinely off-screen
                    // element in the executed segments. REX_UITEXT_FIT: render the glyph quads at their LOCAL
                    // coords mapped onto a visible band instead, to VALIDATE the glyph geometry/kerning (the text
                    // pipeline) even when the real placement is off-screen. Clearly a shape-validation, not placement.
                    static const bool s_fit = getenv("REX_UITEXT_FIT") != nullptr;
                    auto tfReal=[&](float lx,float ly,float&ox,float&oy){ float wx=lx+Tx, wy=ly+Ty; ox=wx*Px+Pxw; oy=wy*Py+Pyw; };
                    auto tfFit =[&](float lx,float ly,float&ox,float&oy){ ox=lx/640.0f-1.0f; oy=(ly+340.0f)/360.0f-1.0f; };
                    const float* p = snap->pos.data();
                    for (uint32_t g=0; g+4<=snap->count; g+=4) {       // glyph quad (TL,TR,BR,BL) → 2 tris
                        float q[8]; for(int j=0;j<4;j++){ if(s_fit) tfFit(p[(g+j)*2],p[(g+j)*2+1],q[j*2],q[j*2+1]); else tfReal(p[(g+j)*2],p[(g+j)*2+1],q[j*2],q[j*2+1]); }
                        float t[12]={q[0],q[1],q[2],q[3],q[4],q[5], q[0],q[1],q[4],q[5],q[6],q[7]};
                        if (tl_esVerts.size()<60000) tl_esVerts.insert(tl_esVerts.end(), t, t+12);
                    }
                }
            }
            // T2b-step-2: carve THIS draw's geometry into clip-space triangles for the render bridge.
            // Content draws = prim 8 (kRectangleList) numI 3, fetch type-3 kVertex, float2 (stride 2dw) screen
            // coords. kRectangleList: v0,v1,v2 given + v3 = v1+v2-v0 (the parallelogram's 4th corner) → 2 tris.
            // Screen→Vulkan-NDC: clip=(x/640-1, y/360-1) for the 1280×720 fb (NDC y-down matches screen y-down).
            uint32_t fc0 = GLD32(0x7FC80000u + 0x4800u*4u);
            static const bool s_skipBackdrop = getenv("REX_UITEXT_FIT") != nullptr;   // text-only validation view
            if (!s_skipBackdrop && (fc0 & 3u) == 3u && prim == 8 && numInd >= 3) {
                uint32_t base = fc0 & 0xFFFFFFFCu, gv = 0xA0000000u | (base & 0x1FFFFFFFu);
                float x[3], y[3];
                for (int i = 0; i < 3; i++) { uint32_t ux = GLD32(gv + i*8), uy = GLD32(gv + i*8 + 4);
                                              memcpy(&x[i], &ux, 4); memcpy(&y[i], &uy, 4); }
                // Xenos kRectangleList: v0,v1,v2 are 3 corners; the implied 4th is v3 = v0 + v2 - v1 (verified
                // from the RAW dump v0=(640,360) v1=(1280,360) v2=(1280,720) → v3=(640,720)=clean BR quadrant).
                // Two triangles: (v0,v1,v2) + (v0,v2,v3). (v3=v1+v2-v0 sheared the rects into parallelograms.)
                float x3 = x[0] + x[2] - x[1], y3 = y[0] + y[2] - y[1];
                auto cx = [](float v){ return v / 640.0f - 1.0f; };
                auto cy = [](float v){ return v / 360.0f - 1.0f; };
                static const bool s_menutex = getenv("REX_MENUTEX") != nullptr;   // Task #8: texture the backdrop
                if (s_menutex) {
                    // Emit pos.xy (clip) + uv.xy (synthetic screen→[0,1]: u=x/1280, v=y/720) per vertex. Each
                    // quadrant samples its own region of the 1280×720 background .png ⇒ the 4 quadrants
                    // reassemble the full background image. Drawn by PresentOnce via the textured pipeline.
                    auto u = [](float v){ return v / 1280.0f; };
                    auto w = [](float v){ return v / 720.0f; };
                    const float t[24] = {
                        cx(x[0]),cy(y[0]),u(x[0]),w(y[0]),  cx(x[1]),cy(y[1]),u(x[1]),w(y[1]),  cx(x[2]),cy(y[2]),u(x[2]),w(y[2]),
                        cx(x[0]),cy(y[0]),u(x[0]),w(y[0]),  cx(x[2]),cy(y[2]),u(x[2]),w(y[2]),  cx(x3),  cy(y3),  u(x3),  w(y3)  };
                    if (tl_esTexVerts.size() < 120000) tl_esTexVerts.insert(tl_esTexVerts.end(), t, t + 24);
                } else {
                    const float tri[12] = { cx(x[0]),cy(y[0]), cx(x[1]),cy(y[1]), cx(x[2]),cy(y[2]),
                                            cx(x[0]),cy(y[0]), cx(x[2]),cy(y[2]), cx(x3),  cy(y3)   };
                    if (tl_esVerts.size() < 60000) tl_esVerts.insert(tl_esVerts.end(), tri, tri + 12);
                }
            }
            // EXPERIMENT (REX_SPRITECARVE): the sprite(prim5)/text(prim13) draws fetch slot 0 (empty); their
            // verts were GENERATED at slot 1 (0xA01FE0FC, cont.22) in UI-authoring coords (~1768x1043). The VS
            // authoritatively fetches slot 0, so slot 1 is likely NOT the draws' real source (probably soup) —
            // but prior carves used the BROKEN init decode (garbage numI); the corrected numI makes one test
            // worthwhile. Sequential carve from the dense start, authoring→clip, triangulate per prim.
            static const bool s_spritecarve = getenv("REX_SPRITECARVE") != nullptr;
            if (s_spritecarve && (prim == 5 || prim == 13) && numInd >= 3) {
                uint32_t s1 = 0xA01FE0FCu;
                if (tl_s1cursor == 0)   // find the first dense authoring-coord run once per frame
                    for (uint32_t o = 8; o < 0x80000; o += 8) { uint32_t u = GLD32(s1 + o); float f; memcpy(&f, &u, 4);
                        if (f > 16.f && f < 4096.f) { tl_s1cursor = o; break; } }
                auto cx = [](float v){ return v / 884.0f - 1.0f; };
                auto cy = [](float v){ return v / 521.5f - 1.0f; };
                static thread_local float vb[512*2];
                uint32_t nr = numInd < 512 ? numInd : 512;
                for (uint32_t i = 0; i < nr; i++) { uint32_t ux = GLD32(s1 + tl_s1cursor + i*8), uy = GLD32(s1 + tl_s1cursor + i*8 + 4);
                    memcpy(&vb[i*2], &ux, 4); memcpy(&vb[i*2+1], &uy, 4); }
                tl_s1cursor += numInd * 8;
                if (prim == 5) { for (uint32_t i = 2; i < nr; i++) {   // tri-strip
                    float t[6] = { cx(vb[(i-2)*2]),cy(vb[(i-2)*2+1]), cx(vb[(i-1)*2]),cy(vb[(i-1)*2+1]), cx(vb[i*2]),cy(vb[i*2+1]) };
                    if (tl_esVerts.size() < 60000) tl_esVerts.insert(tl_esVerts.end(), t, t+6); } }
                else { for (uint32_t i = 0; i + 4 <= nr; i += 4) {     // quad-list -> 2 tris
                    float q[12] = { cx(vb[i*2]),cy(vb[i*2+1]), cx(vb[(i+1)*2]),cy(vb[(i+1)*2+1]), cx(vb[(i+2)*2]),cy(vb[(i+2)*2+1]),
                                    cx(vb[i*2]),cy(vb[i*2+1]), cx(vb[(i+2)*2]),cy(vb[(i+2)*2+1]), cx(vb[(i+3)*2]),cy(vb[(i+3)*2+1]) };
                    if (tl_esVerts.size() < 60000) tl_esVerts.insert(tl_esVerts.end(), q, q+12); } }
            }
        }
        // step (a) result-gate: a resolve (EDRAM->texture readback) is a draw with an active copy command —
        // RB_COPY_CONTROL (0x2318) non-zero AND RB_COPY_DEST_BASE (0x2319) set. (destBase alone is often a
        // stale register value, so require ctl != 0.) The title reads pixel results back to system memory.
        if (s_resultscan) {
            uint32_t copyCtl = GLD32(0x7FC80000u + 0x2318*4u), copyDest = GLD32(0x7FC80000u + 0x2319*4u);
            if (copyCtl && copyDest) fprintf(stderr,"[result] RESOLVE (EDRAM readback) ctl=0x%X destBase=0x%X destInfo=0x%X "
                                  "sampleCountAddr=0x%X\n", copyCtl, copyDest,
                                  GLD32(0x7FC80000u+0x231B*4u), GLD32(0x7FC80000u+0x2325*4u));
        }
        // REX_DRAWLOG: log non-degenerate draws + their bound texture fetch constants (reg 0x4800,
        // 6 dwords/slot) — to find the intro movie's full-screen quad and its decoded-frame texture.
        static const int drawlog = []{ const char* e = getenv("REX_DRAWLOG"); int v = e ? atoi(e) : 0; return v > 1 ? v : (e ? 24 : 0); }();
        static std::atomic<int> dn{0};
        if (drawlog && numInd > 2 && dn.load() < drawlog) {
            int k = ++dn;
            fprintf(stderr, "[draw] #%d init=0x%X numInd=%u prim=%u\n", k, init, numInd, prim);
            for (uint32_t slot = 0; slot < 32; slot++) {
                uint32_t fb = 0x7FC80000u + (0x4800u + slot*6u)*4u;
                uint32_t d0 = GLD32(fb), d1 = GLD32(fb+4), d2 = GLD32(fb+8);
                uint32_t baseAddr = ((d1 >> 12) & 0xFFFFF) << 12;
                if (baseAddr >= 0xA0000000u) {   // a bound texture (physical window)
                    fprintf(stderr, "[draw]   tex slot%u base=0x%X %ux%u fmt=%u tiled=%u type=%u @base=%08X\n",
                            slot, baseAddr, (d2 & 0x1FFF)+1, ((d2 >> 13) & 0x1FFF)+1, d1 & 0x3F,
                            (d0 >> 31) & 1, d0 & 3, GLD32(baseAddr));
                }
            }
        }
        // GPU-build piece 2 (decode): identify the draw's vertex inputs — the vertex-fetch constants in the
        // 0x4800 fetch space (2-dword entries; dword_0 = type:2|address:30 [dwords], dword_1 = endian:2|
        // size:24 [words]; type 3 = kVertex per rexglue xenos.h). These are the geometry the DRAW_INDX->Vulkan
        // translator must upload + vkCmdDraw. prim 8 = kRectangleList (the title's 2D rects). Gated REX_DRAWDECODE.
        static const int decn = []{ const char* e = getenv("REX_DRAWDECODE"); return e ? atoi(e) : 0; }();
        static std::atomic<int> ddn{0};
        if (decn && ddn.fetch_add(1) < decn) {
            const char* pn = prim==1?"point":prim==2?"line":prim==4?"tri-list":prim==5?"tri-strip":
                             prim==6?"tri-fan":prim==8?"RECT-list":prim==0xD?"quad":"?";
            int nvb = 0; char vbuf[256]; size_t off = 0; vbuf[0] = 0;
            for (uint32_t i = 0; i < 96; i++) {
                uint32_t d0 = GLD32(0x7FC80000u + (0x4800u + i*2u)*4u);
                if ((d0 & 3u) != 3u) continue;                         // 3 = kVertex
                uint32_t d1 = GLD32(0x7FC80000u + (0x4800u + i*2u + 1u)*4u);
                uint32_t vbAddr = d0 & 0xFFFFFFFCu, vbBytes = ((d1 >> 2) & 0xFFFFFFu) * 4u;
                if (!vbAddr || vbBytes == 0 || vbBytes > 0x800000u) continue;   // plausible vertex buffer
                if (nvb < 4 && off + 48 < sizeof(vbuf))
                    off += snprintf(vbuf+off, sizeof(vbuf)-off, " [vf%u addr=0x%X sz=%u]", i, vbAddr, vbBytes);
                nvb++;
            }
            fprintf(stderr, "[drawdec] #%d %s numInd=%u init=0x%X vertexbufs=%d%s\n",
                    (int)ddn.load(), pn, numInd, init, nvb, vbuf);
        }
        // GPU-build piece 3a (draw-state extraction): read the pipeline state a real draw binds — render
        // target (RB_SURFACE_INFO pitch/msaa, RB_COLOR_INFO base/format), viewport (PA_CL_VPORT scale/
        // offset -> width/height), window scissor, blend (RB_BLENDCONTROL0/RB_COLORCONTROL), cull
        // (PA_SU_SC_MODE_CNTL) and shader-program control (SQ_PROGRAM_CNTL). This is exactly the state the
        // DRAW_INDX->Vulkan translator turns into a VkPipeline + VkViewport + render pass. Reads the register
        // file the CP maintains from SET_CONSTANT (0x7FC80000 + reg*4). Reg numbers per rexglue
        // register_table.inc. Gated REX_DRAWSTATE=N (dump first N non-degenerate draws), default boot off.
        static const int dsn = []{ const char* e = getenv("REX_DRAWSTATE"); return e ? atoi(e) : 0; }();
        static std::atomic<int> dsc{0};
        if (dsn && numInd > 2 && dsc.fetch_add(1) < dsn) {
            auto rr = [&](uint32_t r){ return GLD32(0x7FC80000u + r*4u); };
            auto rf = [&](uint32_t r){ uint32_t u = rr(r); float f; memcpy(&f, &u, 4); return f; };
            const char* pn2 = prim==1?"point":prim==2?"line":prim==4?"tri-list":prim==5?"tri-strip":
                              prim==6?"tri-fan":prim==8?"RECT-list":prim==0xD?"quad":"?";
            uint32_t surf = rr(0x2000), col = rr(0x2001), mode = rr(0x2208);
            uint32_t blend0 = rr(0x2201), cctl = rr(0x2202), progc = rr(0x2180), sumode = rr(0x2205);
            uint32_t sciTL = rr(0x2081), sciBR = rr(0x2082);
            float vpw = 2.0f * rf(0x210F), vph = 2.0f * rf(0x2111);   // x/y scale -> viewport w/h (y may be -)
            fprintf(stderr, "[drawstate] #%d %s numInd=%u | RT pitch=%u msaa=%u colorFmt=%u colorBase=0x%X | "
                    "vp %.0fx%.0f off(%.1f,%.1f) | scissor (%u,%u)-(%u,%u) | blend0=0x%X colorCtl=0x%X "
                    "progCntl=0x%X suMode=0x%X mode=0x%X\n",
                    (int)dsc.load(), pn2, numInd,
                    (surf & 0x3FFF), (surf>>14)&3, (col>>16)&0xF, (col & 0xFFF),
                    vpw, vph, rf(0x2110), rf(0x2112),
                    sciTL & 0x7FFF, (sciTL>>16)&0x7FFF, sciBR & 0x7FFF, (sciBR>>16)&0x7FFF,
                    blend0, cctl, progc, sumode, mode);
        }
        break;
      }
      case PM4_XE_SWAP:
        g_gpuCounter.fetch_add(1); g_swapCount.fetch_add(1);
        if (g_cptrace) fprintf(stderr, "[cp] XE_SWAP #%llu\n", (unsigned long long)g_swapCount.load());
        break;
      case 0x2B:   // IM_LOAD_IMMEDIATE: embedded shader microcode. data[0] bit0 = type (0=VS,1=PS),
        if (tl_execsegs && (GLD32(addr) & 1) == 0) {   // VS: remember the microcode for the next draw's vfetch decode
            tl_vsUcode = addr + 8; tl_vsSize = GLD32(addr + 4) & 0xFFFF;   // data[1]&0xFFFF = size in dwords
        }
        break;
      default: break;   // ME_INIT/NOP/SET_CONSTANT/WAIT_REG_MEM/state: not modeled yet (no Vulkan) — skip
    }
}

// Walk a PM4 buffer [addr, addr+dwords) dispatching on packet>>30 (rexglue ExecutePacket).
void ExecutePM4(uint32_t addr, uint32_t dwords, int depth) {
    if (depth > 8) return;
    uint32_t end = addr + dwords * 4;
    while (addr + 4 <= end) {
        uint32_t packet = GLD32(addr); addr += 4;
        if (packet == 0) continue;
        // REX_CPDUMP=N: trace the first N top-level packets of the deferred buffer to find the draws and
        // where the parse desyncs. N=1 -> 400 (back-compat); larger N walks a whole frame (~tens of K pkts).
        static const int cpcap = []{ const char* e = getenv("REX_CPDUMP"); int v = e ? atoi(e) : 0; return v > 1 ? v : (e ? 400 : 0); }();
        static std::atomic<int> pn{0};
        if (cpcap && depth == 0 && pn.load() < cpcap) {
            int k = ++pn; uint32_t t = packet >> 30, op = (packet >> 8) & 0x7F;
            // flag draws + the suspicious low-byte!=0 "headers" (likely chain/jump packets or misaligned data)
            const char* note = (t == 3 && (op == 0x36 || op == 0x22)) ? " <<DRAW" : ((packet & 0xFF) && t == 3 ? " <<lowbyte" : "");
            fprintf(stderr, "[pm4] #%d @0x%X type%u op=0x%X cnt=%u raw=%08X%s\n",
                    k, addr - 4, t, t == 3 ? op : 0, ((packet >> 16) & 0x3FFF) + 1, packet, note);
            // decode the title's custom segment packets (0x38/0x79) + draws: dump their leading data dwords
            if (t == 3 && (op == 0x38 || op == 0x79 || op == 0x36 || op == 0x22))
                fprintf(stderr, "[pm4]   data: %08X %08X %08X %08X %08X %08X\n",
                        GLD32(addr), GLD32(addr+4), GLD32(addr+8), GLD32(addr+12), GLD32(addr+16), GLD32(addr+20));
        }
        switch (packet >> 30) {
          case 0: {                              // type-0: write `count` regs starting at base_index
            uint32_t count = ((packet >> 16) & 0x3FFF) + 1, baseIdx = packet & 0x7FFF;
            bool writeOne = (packet >> 15) & 1;
            for (uint32_t m = 0; m < count && addr + 4 <= end; m++, addr += 4)
                WriteGpuReg(writeOne ? baseIdx : baseIdx + m, GLD32(addr));
            break;
          }
          case 1: {                              // type-1: two reg writes
            WriteGpuReg(packet & 0x7FF, GLD32(addr));
            WriteGpuReg((packet >> 11) & 0x7FF, GLD32(addr + 4));
            addr += 8;
            break;
          }
          case 2: break;                          // type-2: no-op
          case 3: {                               // type-3: opcode packet
            uint32_t op = (packet >> 8) & 0x7F, count = ((packet >> 16) & 0x3FFF) + 1;
            ExecuteType3(addr, op, count, depth);
            addr += count * 4;                    // advance past the data dwords
            break;
          }
        }
    }
}

// Execute the primary ring buffer from rptr to wptr (dword indices); handles wraparound.
void ExecuteRing(uint32_t rptr, uint32_t wptr) {
    uint32_t ringDwords = g_ringBufferSize / 4;
    if (g_cptrace) fprintf(stderr, "[cp] === ExecuteRing rptr=%u wptr=%u (ringDwords=%u) ===\n", rptr, wptr, ringDwords);
    if (wptr >= rptr) {
        ExecutePM4(g_ringBufferBase + rptr * 4, wptr - rptr, 0);
    } else if (ringDwords) {                       // wrapped
        ExecutePM4(g_ringBufferBase + rptr * 4, ringDwords - rptr, 0);
        ExecutePM4(g_ringBufferBase, wptr, 0);
    }
}
} // namespace

std::thread::id g_pumpThreadId;   // set by VblankPump; lets sub_821B9270's token-yield skip the pump

void VblankPump() {
    g_pumpThreadId = std::this_thread::get_id();
    uint32_t lastWptr = 0;
    while (g_vblankRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));   // ~60 Hz
        uint32_t vbc = g_vblankCount.fetch_add(1);
        // REX_LOADERPROBE time-based survey: the loader does an early burst of completions then goes quiet,
        // so a completion-gated survey misses the steady state. Sample on the pump thread every ~300 frames
        // (~5 s) — the 20-children states + the UI texture targets — to see whether the loader EVER drains
        // (children[1..19] start) / the atlas/EDRAM get populated, vs. stays parked at child[0] forever.
        static const bool s_ldframe = getenv("REX_LOADERPROBE") != nullptr;
        if (s_ldframe && (vbc % 300) == 30) {
            auto rd = [&](uint32_t a){ uint32_t b; memcpy(&b, g_base + a, 4); return __builtin_bswap32(b); };
            char buf[760]; size_t o = 0; int started = 0;
            for (int i = 0; i < 20; i++) { uint32_t c = 0x82657578u + i*216u, cs = rd(c+136), cr = rd(c+208);
                if (cs || cr) started++;
                o += snprintf(buf+o, sizeof(buf)-o, " [%d]s%u/r%u", i, cs, cr); }
            // R0: is the flagged big-texture block EVER populated? sample 3 offsets (0 / 64KB / 900KB) — if all
            // stay 0 across the run, the decode/upload never writes it (the populate is stubbed).
            uint32_t tw = g_texWatchAddr, t0 = tw?rd(tw):0, t1 = tw?rd(tw+0x10000):0, t2 = tw?rd(tw+0xE0000):0;
            fprintf(stderr, "[ldframe] vbc=%u childrenStarted=%d/20 ch0(+136)=%u ch0(+152)=0x%X ch0(+160)=0x%X | atlasA5004800=%08X edramB0000000=%08X | tex0x%X[0/64k/900k]=%08X %08X %08X |%s\n",
                    vbc, started, rd(0x82657578u+136), rd(0x82657578u+152), rd(0x82657578u+160),
                    rd(0xA5004800u), rd(0xB0000000u), tw, t0, t1, t2, buf);
        }
        // REX_TEXSCAN (cont.38): the COMPLETE bind-independent texture sweep — (1) every tracked texture-sized
        // physical alloc sampled for population, (2) the reg-file fetch region (0x4800..0x48BF, 32 slots × 6 dw)
        // for set texture fetch constants. If a fetch const has populated data, DECODE it (cont.36 decoder,
        // tiled!) -> PPM = the live-tiled-texture render + the tiler's hardware confirmation. Once, at steady state.
        static const bool s_texscan = getenv("REX_TEXSCAN") != nullptr;
        static int s_scanDone = 0;
        if (s_texscan && (vbc % 300) == 60 && s_scanDone < 3) {
            s_scanDone++;
            auto rd = [&](uint32_t a){ uint32_t b; memcpy(&b, g_base + a, 4); return __builtin_bswap32(b); };
            std::lock_guard<std::mutex> lk(g_texAllocsMtx);
            int populated = 0;
            for (size_t i = 0; i < g_texAllocs.size(); i++) {
                uint32_t a = g_texAllocs[i].addr, sz = g_texAllocs[i].size, nz = 0, samples = 0;
                for (uint32_t off = 0; off + 4 <= sz; off += 4096) { if (rd(a + off)) nz++; samples++; }
                bool pop = samples && nz > samples / 20;   // >5% of sampled dwords nonzero
                if (pop) populated++;
                if (i < 28)
                    fprintf(stderr, "[texscan] block#%zu 0x%08X sz=0x%X nz=%u/%u %s first=%08X %08X\n",
                            i, a, sz, nz, samples, pop ? "POPULATED" : "zero", rd(a), rd(a + 4));
                // decode-attempt: if the block looks like an 8888 image (size <=8MB, pixel count = a clean
                // common width), decode it as linear 8888 -> PPM, to SEE the title's in-memory texture data.
                if (pop && sz <= 0x800000u) {
                    uint32_t px = sz / 4, w = 0;
                    if (px % 1280 == 0 && px / 1280 <= 1200) w = 1280;
                    else if (px % 1024 == 0 && px / 1024 <= 1200) w = 1024;
                    else if (px % 512 == 0 && px / 512 <= 1200) w = 512;
                    else if (px % 256 == 0 && px / 256 <= 1200) w = 256;
                    if (w) { uint32_t hh = px / w;
                        rex_tex::Desc dd{}; dd.guestBase = a; dd.format = rex_tex::FMT_8_8_8_8; dd.width = w; dd.height = hh; dd.tiled = 0; dd.endian = rex_tex::END_NONE;
                        std::vector<uint8_t> rgba;
                        if (rex_tex::DecodeGuestToRGBA(dd, rgba) && !rgba.empty()) {
                            char p[96]; snprintf(p, sizeof p, "/tmp/texblk_%08X_%ux%u.ppm", a, w, hh);
                            rex_tex::WriteRGBAasPPM(p, rgba.data(), w, hh);
                            fprintf(stderr, "[texscan] block 0x%08X decoded as %ux%u 8888 -> %s\n", a, w, hh, p);
                        }
                    }
                }
            }
            int texConsts = 0, decoded = 0;
            for (int slot = 0; slot < 32; slot++) {
                uint32_t base = 0x4800u + slot * 6;
                uint32_t d0 = rd(0x7FC80000u + base * 4), d1 = rd(0x7FC80000u + (base + 1) * 4), d2 = rd(0x7FC80000u + (base + 2) * 4);
                uint32_t b = d1 & 0xFFFFF000u;
                if (b < 0x04000000u || b >= 0x20000000u) continue;
                uint32_t ga = 0xA0000000u | b, fmt = d1 & 0x3Fu, w = (d2 & 0x1FFFu) + 1, h = ((d2 >> 13) & 0x1FFFu) + 1, tiled = (d0 >> 31) & 1u;
                uint32_t s0 = rd(ga), s1 = rd(ga + 4); texConsts++;
                fprintf(stderr, "[texscan] fetch slot %d base=0x%08X %ux%u fmt=0x%X tiled=%u data=%08X %08X\n", slot, ga, w, h, fmt, tiled, s0, s1);
                if ((s0 || s1) && rex_tex::BytesPerBlock(fmt)) {     // populated + decodable -> decode it
                    rex_tex::Desc dd{}; dd.guestBase = ga; dd.format = fmt; dd.width = w; dd.height = h; dd.tiled = tiled; dd.endian = (d1 >> 6) & 3u;
                    std::vector<uint8_t> rgba;
                    if (rex_tex::DecodeGuestToRGBA(dd, rgba) && !rgba.empty()) {
                        char p[96]; snprintf(p, sizeof p, "/tmp/texscan_s%d_%08X_%ux%u_f%02X_t%u.ppm", slot, ga, w, h, fmt, tiled);
                        rex_tex::WriteRGBAasPPM(p, rgba.data(), w, h);
                        fprintf(stderr, "[texscan] DECODED fetch slot %d -> %s\n", slot, p); decoded++;
                    }
                }
            }
            fprintf(stderr, "[texscan] SUMMARY vbc=%u: %zu tex-allocs (%d POPULATED); %d fetch-consts set, %d decoded\n",
                    vbc, g_texAllocs.size(), populated, texConsts, decoded);
        }
        uint32_t cb = g_interruptCallback;
        if (!cb) continue;
        if (g_fair && getenv("REX_INITDIAG")) { static int d=0; if ((d++ % 30)==0)
            fprintf(stderr, "[initdiag] g_tok next=%llu serving=%llu held=%d (churn=serving)\n",
                    (unsigned long long)g_tok.next_, (unsigned long long)g_tok.serving_, (int)g_tok.held_); }
        if (g_fair) g_tok.lock(); else if (g_coop) g_waitMutex.lock();
        GpuLock _gl;   // NOTOKEN: serialize this CP+interrupt batch with the guest's GPU-boundary functions
        // Real command processor: the guest kicks by writing the ring write index to CP_RB_WPTR. Execute
        // the newly-submitted PM4 (lastWptr..wptr) — which recurses into the indirect buffers and applies
        // the EVENT_WRITE_SHD fences / INTERRUPT callbacks the render loop waits on — then advance RPtr to
        // wptr (CP_RB_RPTR register + the guest's RPtr write-back) so the title sees the batch consumed.
        uint32_t wptr = GLD32(kReg_CP_RB_WPTR);
        bool consumed = (g_ringBufferBase && wptr != lastWptr);
        if (consumed) {
            ExecuteRing(lastWptr, wptr);
            GST32(kReg_CP_RB_RPTR, wptr);
            if (g_rptrWriteBack) GST32(g_rptrWriteBack, wptr);
            lastWptr = wptr;
        }
        GST32(GpuRegAddr(0x1951), 1);             // re-assert interrupt-status=vblank (prod ReadRegister)
        FireGfxInterrupt(cb, 0, /*cpu=*/2);       // vblank (prod delivers on cpu 2)
        if (consumed) FireGfxInterrupt(cb, 1, /*cpu=*/2);    // command-buffer complete
        // REX_CPCOMPLETE (cont.21 pillar A): break the kick-gate DEADLOCK by modeling GPU completion.
        // The title increments the pending-submission counter device+0x2b04 per submission; the ring kick
        // sub_821C6C80 fires only when it's 0. With no real GPU to complete submissions it climbs 0->0xA and
        // never resets -> 6 init kicks then every frame DEFERs (measured cont.21: 6 KICK / 74 DEFER). Drain
        // it one per vblank (a faithful per-completion decrement — NOT cont.15's blanket force-to-0 in the
        // kick hook) so the title keeps kicking its draw IBs. Success metric: kicks/WPTR climb past 6/37,
        // the title submits real frames, and its draw IBs (physical window) reach the CP. g_interruptData ==
        // the device base (== g_device captured by the kick; both = 0x26F80). [experiment, gated, default-off]
        static const bool s_cpcomplete = getenv("REX_CPCOMPLETE") != nullptr;
        if (s_cpcomplete && g_interruptData) {
            uint32_t c = GLD32(g_interruptData + 0x2b04);
            // REX_CPDRAIN (cont.25): drain the pending-GPU-work counter FULLY (to 0) each vblank instead of −1.
            // The counter climbs ~7-8/vblank but CPCOMPLETE drains 1/vblank → it never reaches 0, so the loader
            // work-loop (sub_82247E70) keeps seeing "GPU work pending" and re-queues forever (the advance gate).
            // Test: does modeling "GPU caught up" (counter=0) drain the loop + advance the title CLEANLY?
            static const bool s_cpdrain = getenv("REX_CPDRAIN") != nullptr;
            if (c > 0) {
                uint32_t nc = s_cpdrain ? 0 : c - 1;
                GST32(g_interruptData + 0x2b04, nc);
                if (g_ktrace) { static int dn = 0; if (dn++ < 40)
                    fprintf(stderr, "[cpcomplete] device+0x2b04 %u->%u (modeled GPU completion)\n", c, nc); }
            }
        }
        // REX_PUMPCB (cont.17 fix attempt): drive the render producer/consumer from the PUMP context (its
        // procType matches the consumer's — cont.15 showed a pump-fired interrupt DOES wake the consumer).
        // Scan the device cmd-buffer chunk for the callback records {0001057C,821CC7A0,ctx}; for each, populate
        // the completion object B{+0x10=producer,+0x14=ctx} + fire source=1 -> sub_821C7170 -> producer(ctx)
        // -> KeSetEvent -> consumer sub_821CC310 (tid=10) drains -> draw via *(item+16).
        static const bool s_pumpcb = getenv("REX_PUMPCB") != nullptr;
        if (s_pumpcb && cb && g_interruptData) {
            uint32_t dev = g_interruptData;   // == device (g_device); declared earlier than g_device
            uint32_t B = GLD32(g_interruptData + 10900);
            uint32_t chunk = GLD32(dev + 13568);
            if (B && B != 0xFFFFFFFFu && chunk >= 0xA0000000u) {
                uint32_t lo = chunk > 0x180000u ? chunk - 0x180000u : 0xA0000000u;
                int fired = 0;
                // cont.20: call the producer DIRECTLY with arg = the record's ctx (so the enqueued work item
                // IS ctx, whose +16 may be the real draw handler) but ON THE PUMP CONTEXT (g_pumpKpcr — its
                // procType matches the consumer's, so the producer's KeSetEvent wakes it). This combines
                // INVOKECB's arg=ctx with PUMPCB's pump context. The consumer drains the batch.
                (void)B;
                for (uint32_t a = lo; a + 12 <= chunk + 0x20000u && fired < 4; a += 4) {
                    if (GLD32(a) != 0x0001057Cu || GLD32(a + 4) != 0x821CC7A0u) continue;
                    PPCContext c{};
                    c.fpscr.csr = 0x1F80;
                    c.r1.u64 = g_pumpStack - 0x400;   // pump's guest stack (interrupts already returned)
                    c.r13.u64 = g_pumpKpcr;
                    c.r3.u64 = GLD32(a + 8);          // producer arg = ctx (the real work item)
                    CallGuest(0x821CC7A0u, c);
                    fired++;
                }
                if (g_ktrace && fired) fprintf(stderr, "[pumpcb] fired %d completion interrupts\n", fired);
            }
        }
        if (g_fair) g_tok.unlock(); else if (g_coop) g_waitMutex.unlock();
    }
}

void StartVblankPump() {
    if (g_vblankRunning.exchange(true)) return;
    uint32_t t = 0;
    g_pumpKpcr = CreateGuestThreadContext(0x20000, 0, /*threadId=*/99, &t, &g_pumpStack);
    InitGpuRegisters();   // seed the read-only GPU registers prod's ReadRegister returns (esp. 0x1951)
    std::thread(VblankPump).detach();
    fprintf(stderr, "[video] vblank pump started (kpcr=0x%X)\n", g_pumpKpcr);
}
} // namespace

// ---- GPU ring/vblank spin must yield the cooperative token (sub_821B9270) -------------------------
// sub_821B9270 is the backoff primitive of the title's GPU ring/vblank spin loops (6 call sites in the
// 0x821Cxxxx render code; e.g. the post-logo GPU-resource cleanup sub_821C6E58 waits here until a GPU
// fence reaches its target). It is a pure busy-spin (db16cyc no-ops) with no kernel yield, so under the
// cooperative token a guest worker holds the token the whole time it spins. But the value it spins on is
// advanced by the vblank pump, which must TAKE the token to run ExecutePM4 — so the pump blocks forever
// on g_waitMutex while the spinner waits for a fence only the pump can move: a deadlock (confirmed: with
// tid 4 spinning here the pump sits in std::mutex::lock). Wrap the recompiled spin so it releases the
// token across the backoff (like KeDelayExecutionThread), letting the pump run and advance the fence,
// then defer to the original logic. Skip the yield on the pump's own thread, which can re-enter this via
// the graphics-interrupt callback while legitimately holding the token. Strong def overrides the weak
// guest alias; __imp__sub_821B9270 is the recompiled body.
// REX_TEXSCAN (cont.39): sub_824495D8 is the generic physical-memory allocator (its 0x82449634 call site is the
// sole immediate caller of MmAllocatePhysicalMemoryEx). To find the texture-CREATE (the cont.25 R0 keystone that
// allocates the GPU-window texture blocks but never populates them), hook the allocator and log ITS immediate
// caller (ctx.lr, reliable — no fragile back-chain walk) whenever it returns a GPU-window (0xA4-0xA5) block.
// cont.40: correlate the texture-create with its GPU-block alloc (thread-local set by the allocator hook) so the
// descriptor structs can be dumped ONLY for the sub_821B0F18 calls that actually allocate a GPU texture block.
static thread_local uint32_t g_tl_lastGpuAlloc = 0;
// Dump 28 dwords of a descriptor object, flagging fields that look like a GPU block / working buffer / a dim.
static void DumpDesc(const char* tag, uint32_t obj) {
    if (obj < 0x1000u || obj >= 0xC0000000u) { fprintf(stderr, "    %s=0x%08X (not a ptr)\n", tag, obj); return; }
    fprintf(stderr, "    %s=0x%08X:", tag, obj);
    for (int i = 0; i < 28; i++) { uint32_t v = GLD32(obj + i*4); const char* f = "";
        if (v >= 0xA4000000u && v < 0xA6000000u) f = "<GPU";
        else if (v >= 0xA0000000u && v < 0xA4000000u) f = "<WBUF";
        else if (v >= 0x04000000u && v < 0x07000000u) f = "<src?";
        else if (v >= 8 && v <= 2048) f = "<n";
        fprintf(stderr, " [%02d]=%08X%s", i, v, f); }
    fprintf(stderr, "\n");
}
// Read a guest string (chars are stored in normal byte order; read base[addr+i] directly).
static std::string GuestStr(uint32_t addr, int maxLen = 80) {
    std::string s; if (addr < 0x1000u || addr >= 0xC0000000u) return "(null)";
    for (int i = 0; i < maxLen; i++) { uint8_t c = g_base[addr + i]; if (!c) break; s += (c >= 32 && c < 127) ? (char)c : '.'; }
    return s;
}

extern "C" PPC_FUNC(__imp__sub_824495D8);
PPC_FUNC(sub_824495D8)
{
    static const bool s_texscan = getenv("REX_TEXSCAN") != nullptr;
    uint32_t caller = static_cast<uint32_t>(ctx.lr);
    __imp__sub_824495D8(ctx, base);
    if (s_texscan) {
        uint32_t result = ctx.r3.u32;
        if (result >= 0xA4000000u && result < 0xA6000000u) {
            g_tl_lastGpuAlloc = result;   // cont.40: let the sub_821B0F18 hook know a GPU block was just allocated
            static std::mutex m; static std::unordered_map<uint32_t,int> seen;
            std::lock_guard<std::mutex> lk(m);
            if (seen[caller]++ == 0)
                fprintf(stderr, "[texcreate] GPU-window alloc via sub_824495D8 <- caller=0x%08X -> 0x%08X\n", caller, result);
        }
    }
}

// REX_TEXSCAN (cont.39/40): sub_821B0F18 is the render-range texture-create. cont.40: dump its descriptor structs
// (r3/r4/r5 = 0x9825xxxx) ONLY when it allocates a GPU block — to find which field = source ptr / dims / format /
// dest GPU block, the basis for implementing the missing tile+upload (cont.25 R0 keystone).
extern "C" PPC_FUNC(__imp__sub_821B0F18);
PPC_FUNC(sub_821B0F18)
{
    static const bool s_texscan = getenv("REX_TEXSCAN") != nullptr;
    uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32, lr = (uint32_t)ctx.lr;
    if (s_texscan) g_tl_lastGpuAlloc = 0;       // cleared; the original sets it if it allocs a GPU block
    __imp__sub_821B0F18(ctx, base);
    if (s_texscan && g_tl_lastGpuAlloc) {
        static std::atomic<int> n{0}; int i = n.fetch_add(1);
        if (i < 60) {
            std::string path = GuestStr(r4);   // cont.40: r4 = the resource PATH ("media\Assets\...")
            bool tex = path.find("extur") != std::string::npos || path.find("Hud") != std::string::npos ||
                       path.find("Graphics") != std::string::npos || path.find("Frontend") != std::string::npos ||
                       path.find(".bin") != std::string::npos;
            fprintf(stderr, "[texdesc] #%d -> GPU 0x%08X path='%s'%s\n", i, g_tl_lastGpuAlloc, path.c_str(), tex ? "  <-- TEXTURE?" : "");
            if (tex) { DumpDesc("r3", r3); DumpDesc("r5", r5); }   // full-dump the texture creates' descriptors
        }
    }
}

// cont.40: sub_82448090 (loader range) is the OTHER creator of GPU-window blocks (it allocates the 924KB/272KB
// TEXTURE blocks at 0xA49xxxxx — sub_821B0F18 turned out to be AUDIO). Dump its args/path/descriptors when it
// allocates a GPU block, to find the texture source/dims/format/dest (the cont.25 R0 upload keystone).
extern "C" PPC_FUNC(__imp__sub_82448090);
PPC_FUNC(sub_82448090)
{
    static const bool s_texscan = getenv("REX_TEXSCAN") != nullptr;
    uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32, lr = (uint32_t)ctx.lr;
    if (s_texscan) g_tl_lastGpuAlloc = 0;
    __imp__sub_82448090(ctx, base);
    if (s_texscan && g_tl_lastGpuAlloc) {
        static std::atomic<int> n{0}; int i = n.fetch_add(1);
        if (i < 40) {
            std::string p3 = GuestStr(r3), p4 = GuestStr(r4), p5 = GuestStr(r5);
            fprintf(stderr, "[texload] #%d -> GPU 0x%08X lr=%08X | r3=%08X('%s') r4=%08X('%s') r5=%08X('%s') r6=%08X r7=%08X ret=%08X\n",
                    i, g_tl_lastGpuAlloc, lr, r3, p3.c_str(), r4, p4.c_str(), r5, p5.c_str(), r6, r7, ctx.r3.u32);
            DumpDesc("r3", r3); DumpDesc("r5", r5);
        }
    }
}

// cont.40: sub_821BE840 is the TEXTURE-CREATE proper — it calls the GPU allocator sub_82448090 (at 0x821BE8F0)
// for the big 924KB/276KB texture blocks. Dump its args + the objects they point to: this level should carry the
// source pixel pointer + dims + format (the inputs for the missing tile+upload, cont.25 R0 keystone).
extern "C" PPC_FUNC(__imp__sub_821BE840);
PPC_FUNC(sub_821BE840)
{
    static const bool s_texscan = getenv("REX_TEXSCAN") != nullptr;
    uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32,
             r8 = ctx.r8.u32, r9 = ctx.r9.u32, r10 = ctx.r10.u32, lr = (uint32_t)ctx.lr;
    if (s_texscan) g_tl_lastGpuAlloc = 0;
    __imp__sub_821BE840(ctx, base);
    if (s_texscan && g_tl_lastGpuAlloc) {
        static std::atomic<int> n{0}; int i = n.fetch_add(1);
        if (i < 20) {
            fprintf(stderr, "[texcreate2] #%d -> GPU 0x%08X lr=%08X ret=%08X | r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X\n",
                    i, g_tl_lastGpuAlloc, lr, ctx.r3.u32, r3, r4, r5, r6, r7, r8, r9, r10);
            DumpDesc("r3", r3); DumpDesc("r4", r4);
        }
    }
}

extern "C" PPC_FUNC(__imp__sub_821B9270);
PPC_FUNC(sub_821B9270)
{
    if (g_coop && std::this_thread::get_id() != g_pumpThreadId) {
        kernel::UnlockGuestExecution();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        kernel::LockGuestExecution();
    }
    __imp__sub_821B9270(ctx, base);
}

// DEEP-BUILD (REX_LOADERPROBE): the stalled frontend (sub_82150970) calls sub_821BF298 every frame — it
// processes *(device+12924) GPU-completion work items at device+12928 (stride 16), calling each item's
// handler (vtable[9] of *(device+12900)), but ONLY if arg2 r4!=0 AND the count>0; else it no-ops. If the
// count (or r4) stays 0 at steady state, the frontend's completion drain is EMPTY — confirming the gate is
// "no real GPU completions enqueued" (the deep build), and giving the exact fields to populate to unblock.
extern "C" PPC_FUNC(__imp__sub_821BF298);
PPC_FUNC(sub_821BF298)
{
    static const bool s_lp = getenv("REX_LOADERPROBE") != nullptr;
    if (s_lp) {
        static std::atomic<int> n{0}; int k = n.fetch_add(1);
        if (k < 24 || (k % 4000) == 0) {
            uint32_t dev = ctx.r3.u32, r4 = ctx.r4.u32;
            fprintf(stderr, "[bf298] #%d dev=0x%08X arg2(r4)=0x%X *(dev+10896)=0x%08X count(dev+12924)=%u item0=[%08X %08X %08X %08X]\n",
                    k, dev, r4, GLD32(dev+10896), GLD32(dev+12924),
                    GLD32(dev+12928), GLD32(dev+12932), GLD32(dev+12936), GLD32(dev+12940));
        }
    }
    __imp__sub_821BF298(ctx, base);
}

// DEEP-BUILD experiment (REX_LOADERTRACE): trace the resource-loader per-child state machine sub_82248010.
// Each call processes ONE state transition (entry *(child+136) -> next). The done-check (loc_8224818C) sets
// state=DONE (1/12) iff vtable[9]() = *(child+208) is NOT in {2,3}; measured *(child+208)=1, so IF the child
// reaches the done-check it completes. Logging state-in->out + *(child+208) shows whether it advances to the
// done-check (state 11) or is reset (->2) by the external re-submitter first. Gated; default boot unchanged.
// FOCUSED BUILD step 1 (REX_VBFILL): the sprite/text dynamic VB is filled by the memcpy sub_8242BF10. Capture
// each fill's dest+source+size to recover the per-draw vertex REGIONS (discrete boundaries heuristic carving
// can't see). Filter to the slot-1 vert range. r3=dest, r4=src, r5=size (confirmed memcpy). Temporary diagnostic.
std::mutex g_vbMtx; std::vector<float> g_vbVerts;   // REX_VBFILL render accumulator
// cont.29 r31-corruption localization state (defined here, before the first user sub_8242BF10).
// thread_local: g_inPoll is set ONLY across the tid=10 transition poll; keeping it thread-local makes the
// default-on guard below safe against a concurrent large memcpy on another thread during the poll window.
thread_local bool g_inPoll = false;
static int  g_r31seq = 0;
extern "C" PPC_FUNC(__imp__sub_8242BF10);
PPC_FUNC(sub_8242BF10)
{
    // REX_R31TRACK (cont.29): inside the poll, log every memcpy dest+size — the one overrunning sub_82448158's
    // saved-r31 slot (~0x9825F540) is the corruptor. A garbage/huge size = the overflow.
    static const bool s_r31trk = getenv("REX_R31TRACK") != nullptr;
    if (g_inPoll && s_r31trk) { static std::atomic<int> mc{0}; if (mc.fetch_add(1) < 16)
        fprintf(stderr, "[r31]   memcpy dest=0x%08X src=0x%08X size=0x%X%s\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                (ctx.r3.u32 <= 0x9825F544u && ctx.r3.u32 + ctx.r5.u32 > 0x9825F544u) ? "  <== HITS saved-r31!" : ""); }
    // cont.30: PROMOTED TO DEFAULT-ON (was REX_CLAMPCPY). The menu->gameplay transition poll's lookup-and-copy
    // (sub_82448158 -> sub_8244FE80) copies a resource record whose blob-length field +60 = 0xFFFFFFFF (the -1
    // "size unknown / streaming-not-ready" sentinel of the still-loading Meshes\Global.bin record; same not-ready
    // class as cont.26's 0x60606060 deserialize count). sub_8244FF20 (NtOpenFile + read) returned success, so the
    // record is "found" but its length is the sentinel; the recompiled copy uses it as a memcpy size -> a ~4GB
    // stack-to-stack copy that shreds sub_82448158's saved-r31 slot and the whole frame, hanging/crashing the
    // title at the gate. Guard: zero any in-poll (tid=10 transition poll; g_inPoll thread-local) memcpy whose
    // size is impossibly large (>=256MB; the poll's legit copies are <=0x3C, the actual mesh DATA streams via a
    // separate loader, not this header copy). This advances 65 -> ~96 assets (Meshes\Global.bin +
    // Levels/Campaigns/Challenges.xmc + all gameplay geom). Two independent fixes (cont.28 REX_SKIPPOLL, cont.29
    // CLAMPCPY) confirm the root cause. REX_NOCLAMPCPY opts out (restores the old crash-prone 65-asset default).
    // The fully-upstream fix (populate +60 mid-stream) is the deep streaming-loader build; this is the correct
    // in-scope fix.
    static const bool noclampcpy = getenv("REX_NOCLAMPCPY") != nullptr;
    if (!noclampcpy && g_inPoll && ctx.r5.u32 >= 0x10000000u) {
        static std::atomic<int> cc{0}; if (cc.fetch_add(1) < 4)
            fprintf(stderr, "[clampcpy] in-poll garbage memcpy size 0x%X -> 0 (sentinel +60; was shredding the stack)\n", ctx.r5.u32);
        ctx.r5.u64 = 0;
    }
    // REX_UITEXT: snapshot each text-glyph fill (guest thread) keyed by vert count, for exec-time pairing.
    static const bool s_uitext = getenv("REX_UITEXT") != nullptr;
    if (s_uitext && rex_render::Enabled()) {
        uint32_t d = ctx.r3.u32, s = ctx.r4.u32, sz = ctx.r5.u32;
        if (d >= 0xA01F0000u && d < 0xA0300000u && sz >= 64 && sz < 0x20000 && (sz % 16) == 0) {
            uint32_t vc = sz / 16;                                // stride-16 (pos.xy + uv.xy) glyph verts
            auto rdf = [&](uint32_t off){ float f; uint32_t w=GLD32(s+off); memcpy(&f,&w,4); return f; };
            float u0=rdf(8), v0=rdf(12), px0=rdf(0);              // vert0: uv at +8/+12, pos.x at +0
            bool textLike = u0>=-0.3f && u0<=1.6f && v0>=-0.3f && v0<=1.6f && px0>=-4.f && px0<2200.f
                            && (vc % 4)==0 && vc>=4 && vc<=2048;   // 4 verts/glyph quad
            if (textLike) {
                std::lock_guard<std::mutex> lk(g_txtMtx);
                // dedup: the menu caches/refills the same labels every frame — keep each unique label once
                // (by count + first vert), so the persistent set stays the current screen's labels.
                float fx0=rdf(0), fy0=rdf(4); bool dup=false;
                for (auto& sn : g_txtSnaps) if (sn.count==vc && sn.pos.size()>=2
                        && fabsf(sn.pos[0]-fx0)<0.5f && fabsf(sn.pos[1]-fy0)<0.5f) { dup=true; break; }
                if (!dup && g_txtSnaps.size() < 256) {
                    TxtSnap snap; snap.count = vc; snap.used = false; snap.pos.reserve(vc*2);
                    for (uint32_t i=0;i<vc;i++){ snap.pos.push_back(rdf(i*16)); snap.pos.push_back(rdf(i*16+4)); }
                    g_txtSnaps.push_back(std::move(snap));
                }
            }
        }
    }
    static const bool s_vbfill = getenv("REX_VBFILL") != nullptr;
    if (s_vbfill && rex_render::Enabled()) {
        uint32_t d = ctx.r3.u32, s = ctx.r4.u32, sz = ctx.r5.u32;
        if (d >= 0xA01F0000u && d < 0xA0300000u && sz >= 16 && sz < 0x20000 && (sz % 8) == 0) {
            uint32_t nv = sz / 8;
            if (nv > 100) { static std::atomic<bool> td{false}; bool e=false;   // one-shot: dump a TEXT fill's verts
              if (td.compare_exchange_strong(e,true)) { fprintf(stderr,"[text] nv=%u DEST=0x%X src=0x%X caller_lr=0x%08X first 32 verts:\n ", nv, d, s, (uint32_t)ctx.lr);
                for(int i=0;i<32;i++){ float x,y; uint32_t a=GLD32(s+i*8),b=GLD32(s+i*8+4); memcpy(&x,&a,4);memcpy(&y,&b,4); fprintf(stderr,"(%.1f,%.1f) ",x,y); }
                fprintf(stderr,"\n");
                // Is the per-draw transform LIVE in the register file at memcpy (fill) time? If c0/c4 hold a real
                // World+Proj here, I can render text correctly RIGHT HERE (local verts + live transform, no
                // exec-time correlation). If identity/stale, the transform is only set later (in the segment).
                fprintf(stderr,"[text-xform@fill] reg0x4000 c0..c5:\n");
                for(int c=0;c<6;c++){ float f[4]; for(int j=0;j<4;j++){uint32_t w=GLD32(0x7FC80000u+(0x4000u+c*4+j)*4u); memcpy(&f[j],&w,4);}
                  fprintf(stderr,"  c%d = %11.4f %11.4f %11.4f %11.4f\n", c, f[0],f[1],f[2],f[3]); } } }
            float vx0,vy0,vx1; uint32_t u; u=GLD32(s);memcpy(&vx0,&u,4); u=GLD32(s+4);memcpy(&vy0,&u,4); u=GLD32(s+8);memcpy(&vx1,&u,4);
            bool frameStart = (vx0==0.f && vy0==0.f && nv>=4 && vx1>1700.f && vx1<1800.f);   // the full-screen quad
            auto cx=[](float v){return v/884.0f-1.0f;}; auto cy=[](float v){return v/521.5f-1.0f;};
            std::lock_guard<std::mutex> lk(g_vbMtx);
            auto ins=[&](const float* p,int n){ if(g_vbVerts.size()<100000u) g_vbVerts.insert(g_vbVerts.end(),p,p+n); };
            auto rd=[&](uint32_t i){ float f; uint32_t w=GLD32(s+i*4); memcpy(&f,&w,4); return f; };
            // skip the full-screen backdrop quad (frame marker) + non-screen-space fills (verts outside the
            // authoring range = not UI quads, e.g. the (0,0)(-512,0) local-space ones); triangulate the rest.
            bool sane = !frameStart && nv >= 4 && nv <= 16;   // panels/small UI; skip 504-vert local-space text + huge fills
            for (uint32_t i=0; i<nv*2 && sane; i++){ float f=rd(i); if (f<-32.f || f>2600.f) sane=false; }
            { float mn=1e9f,mx=-1e9f; for(uint32_t i=0;i<nv*2;i++){ float f=rd(i); if(f<mn)mn=f; if(f>mx)mx=f; } if (mx-mn < 40.f) sane=false; }  // skip tiny local-space spans
            // fill-level dedup: the menu's panels repeat every frame — keep each distinct panel (by its vert0) once.
            if (sane) { static std::vector<std::pair<float,float>> seen;
                bool dup=false; for (auto& pr:seen) if (pr.first==rd(0) && pr.second==rd(1)) { dup=true; break; }
                if (dup) sane=false; else if (seen.size()<512) seen.push_back({rd(0),rd(1)}); }
            if (sane) {
                { static std::atomic<int> sl{0}; int kk=sl.fetch_add(1);
                  if (kk < 60) fprintf(stderr, "[panel] #%d nv=%u v=(%.0f,%.0f)(%.0f,%.0f)(%.0f,%.0f) %s\n",
                        kk, nv, rd(0),rd(1),rd(2),rd(3),rd(4),rd(5), frameStart?"FS":""); }
                if (nv == 4) {   // prim-5 tri-strip quad -> 2 tris (v0,v1,v2)+(v0,v2,v3)
                    float q[12]={cx(rd(0)),cy(rd(1)),cx(rd(2)),cy(rd(3)),cx(rd(4)),cy(rd(5)), cx(rd(0)),cy(rd(1)),cx(rd(4)),cy(rd(5)),cx(rd(6)),cy(rd(7))}; ins(q,12);
                } else {         // tri-list (groups of 3) — handles the 6/13/... vert UI fills
                    for (uint32_t i=0;i+3<=nv;i+=3) { float t[6]={cx(rd(i*2)),cy(rd(i*2+1)),cx(rd(i*2+2)),cy(rd(i*2+3)),cx(rd(i*2+4)),cy(rd(i*2+5))}; ins(t,6); }
                }
                if (!g_vbVerts.empty()) rex_render::SubmitMenuGeometry(g_vbVerts.data(), (int)(g_vbVerts.size()/2));
            }
            // TEXT fills are stride-16 (pos.xy + uv.xy) glyph quads in LOCAL text-box space (x~16..74, y~0..38,
            // marching right). They CANNOT be placed at memcpy time: cont.23 measured reg0x4000 = all zeros here
            // (the per-draw World+screen-ortho transform is set later, when the segment executes — by which time
            // this VB is recycled to zeros). Rendering them needs the transform, which only coexists with the
            // verts if the stubbed vertex-stream binding is restored (the deep build). Left as the [text] dump
            // (measurement only); no submit — a local-space quad render lands off-screen in the top-left corner.
        }
    }
    __imp__sub_8242BF10(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82248010);
PPC_FUNC(sub_82248010)
{
    static const bool s_trace = getenv("REX_LOADERTRACE") != nullptr;
    static const bool s_latch = getenv("REX_LOADERLATCH") != nullptr;
    // REX_LOADERPROBE (deep build, STEP-0 prep): child[0] (0x82657578) is a STREAMING loader — state-8
    // sub_822485A0 reads a chunk [src=*(child+160), size=*(child+168)] (calls its vtable[3]) and sets
    // ready=*(child+208)=1; the state-10/11 finalize advances the DEST cursor *(child+152) += *(child+168).
    // It reaches state 1 per chunk then is re-driven for the next. DECISIVE: does the dest cursor +152
    // ADVANCE (real progress — the load will finish, then child[1..19] start) or STICK (re-reading one chunk
    // forever — the genuine wall)? Dump child[0]'s field window on each fresh completion. Gated, default-off.
    static const bool s_probe = getenv("REX_LOADERPROBE") != nullptr;
    uint32_t child = ctx.r3.u32;
    // REX_LOADERLATCH (decisive test): the child reaches DONE (state 1) but is re-init'd to 2 forever. HOLD it
    // at done (override the external re-init) — stronger than cont.22's one-shot force-past. If the worker's
    // poll then exits → loader drains / menu loads, the re-init was spurious; if it still fails, the resource
    // is genuinely null (deep build). Only the menu loader's child[0] (0x82657578).
    static std::atomic<uint32_t> g_latched{0};
    if (s_latch && g_latched.load() == child && child) {
        if (GLD32(child + 136) != 1) GST32(child + 136, 1);
    }
    if (!s_trace && !s_latch && !s_probe) { __imp__sub_82248010(ctx, base); return; }
    // One-shot: dump the loader object's vtable — the methods at +8/+20/+24/+32 are the resource sub-ops the
    // state handlers call (the ones that should CREATE the GPU resource but stub to null); +36 = sub_82105948.
    static std::atomic<bool> s_vtDumped{false}; bool ev = false;
    if (s_trace && child && s_vtDumped.compare_exchange_strong(ev, true)) {
        uint32_t vt = GLD32(child);
        fprintf(stderr, "[ldvt] child=0x%X vtable=0x%X methods:\n", child, vt);
        for (int i = 0; i < 20; i++) { uint32_t m = GLD32(vt + i*4);
            const char* tag = (i*4==8||i*4==20||i*4==24||i*4==32) ? " <-RESOURCE sub-op" : (i*4==36 ? " <-done-check(*(child+208))" : "");
            fprintf(stderr, "  vtable[%2d] (+%2d) = sub_%08X%s\n", i, i*4, m, tag); }
    }
    uint32_t stIn = GLD32(child + 136), c208 = GLD32(child + 208);
    __imp__sub_82248010(ctx, base);
    uint32_t stOut = GLD32(child + 136);
    if (s_probe && child == 0x82657578u && stOut == 1 && stIn != 1) {
        static std::atomic<int> cn{0}; int k = cn.fetch_add(1);
        static uint64_t prev152 = 0;
        auto rd64 = [&](uint32_t a){ return ((uint64_t)GLD32(a) << 32) | GLD32(a + 4); };
        uint64_t c152 = rd64(child + 152), c168 = rd64(child + 168);
        if (k < 64) fprintf(stderr, "[ldprobe] #%d cmpl via state%u: +144=0x%08X dst+152=0x%llX (Δ=0x%llX) src+160=0x%08X size+168=0x%llX +176=0x%08X +200=0x%08X rdy+208=%u\n",
            k, stIn, GLD32(child + 144), (unsigned long long)c152, (unsigned long long)(c152 - prev152),
            GLD32(child + 160), (unsigned long long)c168, GLD32(child + 176), GLD32(child + 200), GLD32(child + 208));
        prev152 = c152;
        // Periodic decisive summary: do children[1..19] ever START (loader progressing past child[0]), and do
        // the UI texture targets (font atlas 0xA5004800 / EDRAM backdrop 0xB0000000) ever get NON-ZERO data
        // while the loader runs? If children stay at 0 AND the atlas/EDRAM stay zero AND completions keep
        // climbing, the loader is LOOPING a fixed list whose uploads never reach the GPU targets (the wall).
        if ((k % 4000) == 50) {
            static std::mutex sm; std::lock_guard<std::mutex> lk(sm);
            char buf[760]; size_t o = 0; int started = 0;
            for (int i = 0; i < 20; i++) { uint32_t c = 0x82657578u + i*216u, cs = GLD32(c+136), cr = GLD32(c+208);
                if (cs || cr) started++;
                o += snprintf(buf+o, sizeof(buf)-o, " [%d]s%u/r%u", i, cs, cr); }
            // sample the texture targets at a few offsets (the upload would write here)
            uint32_t a0 = GLD32(0xA5004800u), a1 = GLD32(0xA5004804u), e0 = GLD32(0xB0000000u), e1 = GLD32(0xB0001000u);
            fprintf(stderr, "[ldsummary] cmpls=%d childrenStarted=%d/20 | atlas@A5004800=%08X %08X edram@B0000000=%08X %08X |%s\n",
                    k, started, a0, a1, e0, e1, buf);
        }
    }
    if (s_latch && stOut == 1 && c208 == 1 && child) g_latched.store(child);   // latch on first genuine completion
    static std::atomic<int> n{0};
    int k = n.fetch_add(1);
    if (k < 120 && stIn != stOut)
        fprintf(stderr, "[ldtrace] #%d child=0x%X state %u->%u c208=%u%s\n", k, child, stIn, stOut, c208,
                (s_latch && g_latched.load() == child) ? " [latched]" : "");
}

// ---- Ring-kick instrumentation (sub_821C6600): log the segment descriptor it consumes + the ring delta.
// The kick reads a 2-dword descriptor at r4 ({d0: low-24 used, d1: segment addr}) and emits an IB packet to
// the ring (advancing CP_RB_WPTR). Renderer route B needs the per-frame segment [addr,len] list; logging the
// 6 init kicks reveals the descriptor->IB encoding (which we then read from the segment table per-frame).
std::atomic<uint32_t> g_device{0};   // captured from the first ring-kick (device base, ~0x26F80)
// sub_821C73D8 sets up the per-submission GPU-completion object at *(device+10900) — the field the pump's
// source=1 gfx-interrupt handler (sub_821C7170) derefs. Serialize it with the pump under NOTOKEN so the pump
// never reads device+10900 while the guest is writing it (the other half of the gfx-interrupt race fix).
extern "C" PPC_FUNC(__imp__sub_821C73D8);
PPC_FUNC(sub_821C73D8) { GpuLock _gl; __imp__sub_821C73D8(ctx, base); }

// REX_KICKGATE (cont.13): the ring-kick gate. sub_821C6C80 kicks the ring (sub_821C6600) only when
// *(device+0x2b04)==0; non-zero runs a "process-pending" block and DEFERS the kick. prod: *(dev+0x2b04)
// is mostly 0 (kicks ~7062x/45s); variant A: kicks only 6x => this flag is stuck non-zero. Log it.
extern "C" PPC_FUNC(__imp__sub_821C6C80);
PPC_FUNC(sub_821C6C80) {
    static const bool kg = getenv("REX_KICKGATE") != nullptr;
    // REX_POOLCHK: the faithful-CP test. The content draws' vfetch source = slot-0 pool 0xA2000000 (RE'd),
    // which is 0x0BADF00D poison at the SWAP-time census. Sample it HERE (the kick gate fires ~1410x/frame =
    // dense MID-FRAME sampling), tracking whether it is EVER real float2 verts. If maxRealFloats stays low +
    // poison dominates => the title's vertex-gen never writes it (deeper gap); if it jumps high => the pool
    // IS written mid-frame (timing) => the faithful-CP approach (execute segments mid-frame) is viable.
    static const bool poolchk = getenv("REX_POOLCHK") != nullptr;
    if (poolchk) {
        static int samples=0;
        // sample BOTH candidate pools mid-frame: slot 0 (vfetch source, RE'd) + slot 1 (the type-3 kVertex
        // constant that HAD screen coords). Whichever is ever real float2 verts holds the menu geometry.
        static uint32_t pools[2]={0xA2000000u,0xA01FE0FCu}; static int realMax[2]={0,0};
        for(int p=0;p<2;p++){ int real=0; for(int i=0;i<64;i++){ uint32_t w=GLD32(pools[p]+i*4); if(!w||w==0xFFFFFFFFu||w==0x0BADF00Du)continue;
            float f; memcpy(&f,&w,4); if(f==f && f>-4096.0f && f<4096.0f && (f>0.0078125f||f<-0.0078125f)) real++; } if(real>realMax[p])realMax[p]=real; }
        samples++;
        if ((samples%4000)==1) fprintf(stderr,"[poolchk] samples=%d | slot0@0x%X maxRealFloats/64=%d head=%08X %08X | slot1@0x%X maxRealFloats/64=%d head=%08X %08X\n",
            samples, pools[0], realMax[0], GLD32(pools[0]), GLD32(pools[0]+4), pools[1], realMax[1], GLD32(pools[1]), GLD32(pools[1]+4));
    }
    // NOTE (cont.15): forcing the kick gate open here (GST32(r3+0x2b04,0)) DOES flow the ring (2725 kicks)
    // but yields ONLY init=0x30088 rects (no textured draws) and crashes (skips segment bookkeeping).
    // PROVEN non-viable — the textured draws are producer/consumer work items, not kicked segments.
    if (kg) {
        static int c = 0;
        if (c < 80) {
            uint32_t obj = ctx.r3.u32, dev = g_device.load();
            uint32_t fr3 = GLD32(obj + 0x2b04), fdev = dev ? GLD32(dev + 0x2b04) : 0;
            fprintf(stderr, "[kickgate] #%d r3=0x%08X dev=0x%08X *(r3+0x2b04)=0x%08X *(dev+0x2b04)=0x%08X %s\n",
                    c, obj, dev, fr3, fdev, fr3 ? "defer" : "KICK");
            c++;
        }
    }
    __imp__sub_821C6C80(ctx, base);
}

// REX_ENQLOG: characterize what the menu enqueues into its deferred render work-queue (cont.12 c11). sub_821CC7A0
// is the PRODUCER — it stores work item r3 into a per-process ring buffer (base+procType*108+11328, count
// +11412) and KeSetEvent's the consumer (sub_821CC310, which dequeues + calls *(item+16) to issue the draws).
// Log each DISTINCT work-item vtable (= render-command TYPE) + the running total. If the menu enqueues only a
// couple of rect types, the divergence is UPSTREAM — the menu render logic builds no textured-content items.
extern "C" PPC_FUNC(__imp__sub_821CC7A0);
PPC_FUNC(sub_821CC7A0) {
    static const bool enqlog = getenv("REX_ENQLOG") != nullptr;
    if (enqlog) {
        static std::atomic<uint64_t> cnt{0}; uint64_t c = ++cnt;
        uint32_t item = ctx.r3.u32, vt = item ? GLD32(item) : 0;
        static std::mutex em; static std::unordered_set<uint32_t> vts;
        std::lock_guard<std::mutex> lk(em);
        if (vts.insert(vt).second)
            fprintf(stderr, "[enq] #%llu NEW item-type: item=0x%X vtable=0x%X *(item+16)=0x%X (distinct=%zu)\n",
                    (unsigned long long)c, item, vt, item ? GLD32(item+16) : 0, vts.size());
        else if ((c % 4000) == 0)
            fprintf(stderr, "[enq] count=%llu distinct-types=%zu\n", (unsigned long long)c, vts.size());
    }
    __imp__sub_821CC7A0(ctx, base);
}

// REX_ENQLOG also hooks the CONSUMER sub_821CC310 (dequeues a work item r31=*(r3), calls *(r31+16) at
// lr=0x821CC4B0 to issue the draw). Disambiguates renderer path A vs B: if it RUNS, the segment-execution path
// is reached (path A — the handlers are just null/garbage = uninit objects, fix their init); if it's NEVER
// called, the deferred program is never reached at all (path B — drive it ourselves). Also logs the handler
// *(item+16) so we see whether the work items carry valid draw handlers or garbage.
extern "C" PPC_FUNC(__imp__sub_821CC310);
PPC_FUNC(sub_821CC310) {
    static const bool enqlog = getenv("REX_ENQLOG") != nullptr;
    if (enqlog) {
        static std::atomic<uint64_t> c310{0}; uint64_t c = ++c310;
        uint32_t item = ctx.r3.u32 ? GLD32(ctx.r3.u32) : 0, handler = item ? GLD32(item + 16) : 0;
        if (c <= 6 || (c % 4000) == 0)
            fprintf(stderr, "[consumer] #%llu sub_821CC310 r3=0x%X item=*(r3)=0x%X handler=*(item+16)=0x%X\n",
                    (unsigned long long)c, ctx.r3.u32, item, handler);
    }
    __imp__sub_821CC310(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_821C6600);
PPC_FUNC(sub_821C6600)
{
    GpuLock _gl;   // NOTOKEN: serialize the ring kick with the pump's CP+interrupt batch
    uint32_t dev = ctx.r3.u32, desc = ctx.r4.u32;
    if (!g_device.load()) g_device.store(dev);
    uint32_t d0 = desc ? GLD32(desc + 0) : 0, d1 = desc ? GLD32(desc + 4) : 0;
    uint32_t w0 = GLD32(0x7FC80714);
    __imp__sub_821C6600(ctx, base);
    if (g_ktrace) {
        uint32_t w1 = GLD32(0x7FC80714);
        fprintf(stderr, "[kick] dev=0x%X desc=0x%X d0=%08X d1=%08X | CP_RB_WPTR %u->%u (+%d dw)\n",
                dev, desc, d0, d1, w0, w1, (int)(w1 - w0));
    }
}

// ---- Force intro-movie end-of-stream (REX_MOVIE_EOF=N) ----------------------------------------------
// The intro plays Movies/en-en/sp_xbox_0_intro.wmv and advances to the menu ONLY on movie end-of-stream:
// each frame the movie widget's per-frame driver sub_82425BF8 calls its player's AdvanceFrame dispatch
// sub_8232AAE0 (= (*(*player+72))(player, ...)); when that returns 0x16660026 (the demuxer EOS code,
// produced by sub_8233A7D0) sub_82425BF8 POSTS the completion event on channel 0xAAC0CCDD (sub_8222A9F8),
// which the intro owner (an event/state machine: sub_82163118 <- ... <- sub_82150770) handles by tearing
// the movie down and switching to the menu (verified on the prod oracle: the movie runs ~534 frames /
// ~22 s, then this exact path fires). In variant A the VC-1 decoder is stuck, so AdvanceFrame never
// returns EOS and the intro hangs forever; REX_MOVIE_EOF=N synthesizes that EOS after N advance calls,
// driving the title's OWN intro->menu transition. Precise: only the movie widget's player (captured as
// *(widget+76) in sub_82425BF8) is forced, so the other AdvanceFrame caller (sub_8224EE20, a different
// media object) is untouched. Default-off (gate unset => boot unchanged).
namespace { std::atomic<uint32_t> g_moviePlayer{0}; }
extern "C" PPC_FUNC(__imp__sub_82425BF8);
PPC_FUNC(sub_82425BF8)
{
    g_moviePlayer.store(GLD32(ctx.r3.u32 + 76));   // movie widget 'this' -> player object
    __imp__sub_82425BF8(ctx, base);
}
extern "C" PPC_FUNC(__imp__sub_8232AAE0);
PPC_FUNC(sub_8232AAE0)
{
    static const char* eofEnv = getenv("REX_MOVIE_EOF");
    uint32_t player = ctx.r3.u32;                  // arg: the player object being advanced
    __imp__sub_8232AAE0(ctx, base);                // real AdvanceFrame; r3 = status on return
    // cont.35 (REX_MOVIEPROBE / REX_MOVIE_EOF_ALL): test whether the post-Level-1 stall is a cutscene
    // (Level1Intro.wmv) waiting via a player REX_MOVIE_EOF doesn't cover (it only forces player==g_moviePlayer).
    // PROBE logs every AdvanceFrame (which players advance at L1); EOF_ALL=N forces EOS for ANY player after N
    // total advances. If a non-boot-intro movie is the gate, EOF_ALL advances it.
    static const bool s_movieprobe = getenv("REX_MOVIEPROBE") != nullptr;
    if (s_movieprobe) { static std::atomic<int> mp{0}; int k = mp.fetch_add(1);
        if (k < 80) fprintf(stderr, "[movieprobe] advance #%d player=0x%X ret=0x%X (cur g_moviePlayer=0x%X)\n",
                            k, player, ctx.r3.u32, g_moviePlayer.load()); }
    static const char* eofAllEnv = getenv("REX_MOVIE_EOF_ALL");
    if (eofAllEnv && player) { static std::atomic<int> na{0}; int c = na.fetch_add(1), thr = atoi(eofAllEnv);
        if (c >= thr) { ctx.r3.u64 = 0x16660026u;
            static std::atomic<int> fe{0}; if (fe.fetch_add(1) < 6)
                fprintf(stderr, "[movie-eof-all] advance #%d player=0x%X -> FORCING EOS 0x16660026\n", c, player); } }
    if (eofEnv && player && player == g_moviePlayer.load()) {
        static std::atomic<int> n{0};
        int c = n.fetch_add(1), thr = atoi(eofEnv);
        if (g_ktrace && (c % 60 == 0 || c == thr))
            fprintf(stderr, "[movie-eof] advance #%d player=0x%X ret=0x%X%s\n", c, player, ctx.r3.u32,
                    (c >= thr) ? "  -> FORCING EOS 0x16660026" : "");
        if (c >= thr) ctx.r3.u64 = 0x16660026u;  // synthesize demuxer EOS -> title posts completion
    }
}
// REX_XFLAG: forcing the movie EOS alone is NOT enough to reach the menu. The intro->menu advance logic
// sub_82163118 is dispatched by the per-frame screen state machine sub_82161920 ONLY in its state==2 branch,
// behind a global "screen-transitions-enabled" byte at 0x828E82A6. prod's sub_8210AF90 sets that byte to 1;
// in variant A sub_8210AF90 is never reached, so the byte stays 0 and sub_82163118 never runs -> the title
// cannot leave the intro even once the movie has ended (gdb-verified: owner reaches state 2, owner+72=1,
// but g(0x828E82A6)=0). REX_XFLAG forces the byte so the state machine advances. Combined with REX_MOVIE_EOF
// (+ REX_SKIPINTRO START), this drives intro movie -> attract loop -> menu/frontend setup, hitting the next
// blocker (INDIRECT-NULL 0xFFFFFFFF at sub_8215DE84, a null screen vtable/jump-table). STOPGAP: the proper
// fix is to find why sub_8210AF90 isn't called in variant A. Default-off (gate unset => boot unchanged).
extern "C" PPC_FUNC(__imp__sub_82161920);
PPC_FUNC(sub_82161920)
{
    static const bool xflag = getenv("REX_XFLAG") != nullptr;
    if (xflag) g_base[0x828E82A6] = 1;     // screen-transitions-enabled flag (prod: set by sub_8210AF90)
    __imp__sub_82161920(ctx, base);
}
// Diag (REX_INITDIAG): does the transitions-enable worker run? sub_82250420 (work-loop) -> sub_8211B740
// (handler) -> sub_8210AF90 (sets 0x828E82A6). Used to verify REX_FAIRSCHED unstarves tid=10.
extern "C" PPC_FUNC(__imp__sub_82250420);
PPC_FUNC(sub_82250420) { if (getenv("REX_INITDIAG")) { static std::atomic<int> k{0};
    if (k.fetch_add(1)==0) fprintf(stderr, "[initdiag] sub_82250420 worker ENTERED (tid=10 runs)\n"); }
    __imp__sub_82250420(ctx, base); }
extern "C" PPC_FUNC(__imp__sub_8211B740);
PPC_FUNC(sub_8211B740) { if (getenv("REX_INITDIAG")) { static std::atomic<int> k{0};
    if (k.fetch_add(1)<3) fprintf(stderr, "[initdiag] sub_8211B740 work-handler ran\n"); }
    __imp__sub_8211B740(ctx, base); }
extern "C" PPC_FUNC(__imp__sub_8210AF90);
PPC_FUNC(sub_8210AF90) { if (getenv("REX_INITDIAG")) fprintf(stderr, "[initdiag] *** sub_8210AF90 RAN — 0x828E82A6 set, transitions enabled ***\n");
    __imp__sub_8210AF90(ctx, base); }
// Cooperative time-slice (REX_FAIRSCHED): the main thread's steady intro loop never blocks (the fence-forward
// satisfies its GPU waits instantly), so it holds the run-token forever and every other guest thread (the
// transitions worker tid=10, the VC-1 decoders) starves — regardless of token fairness. Yield the token once
// per frame at sub_82167248 (the per-frame update) so the FIFO serves the other ready threads, then re-acquire.
extern "C" PPC_FUNC(__imp__sub_82167248);
PPC_FUNC(sub_82167248)
{
    if (g_fair) { kernel::UnlockGuestExecution(); kernel::LockGuestExecution(); }   // per-frame cooperative yield
    __imp__sub_82167248(ctx, base);
}
// REX_ADVGATE (cont.25 R2 cont.): pin the exact divert in sub_8211B740 that skips the sub_8210AF90 dispatch
// (0x8211B91C). Its loc_8211B81C→dispatch path is a LINEAR run of these direct calls; log ENTER+RET for each
// (filtered to the sub_8211B740 call-site return-lr) — the LAST one that ENTERs but never RETs is the divert
// (a non-returning call / longjmp-SEH unwind to the tail 0x8211BE60). Gated, default-off.
namespace { std::atomic<int> g_advN{0};
inline void AdvLog(uint32_t lr, uint32_t site, const char* nm, const char* ph) {
    static const bool on = getenv("REX_ADVGATE") != nullptr;
    if (on && lr == site && g_advN.fetch_add(1) < 60)
        fprintf(stderr, "[adv] %-12s @0x%08X %s\n", nm, site, ph);
} }
#define ADV_HOOK(FN, SITE) extern "C" PPC_FUNC(__imp__##FN); PPC_FUNC(FN) { \
    uint32_t _lr = ctx.lr; AdvLog(_lr, SITE, #FN, "ENTER"); __imp__##FN(ctx, base); AdvLog(_lr, SITE, #FN, "RET"); }
ADV_HOOK(sub_82131758, 0x8211B830u)   // 1st middle call (loc_8211B81C)
ADV_HOOK(sub_8213E7E8, 0x8211B84Cu)
ADV_HOOK(sub_82132918, 0x8211B854u)
ADV_HOOK(sub_8212BE48, 0x8211B87Cu)
ADV_HOOK(sub_82110BE8, 0x8211B8D8u)
ADV_HOOK(sub_82267630, 0x8211B904u)   // last call before the 0x8211B91C dispatch — prime divert suspect
#undef ADV_HOOK
// sub_8211BD60 is called TWICE (0x8211B888 right after sub_8212BE48, and 0x8211B8C8) — the prime divert suspect
// (execution reached sub_8212BE48 RET then stopped). Hook both sites.
extern "C" PPC_FUNC(__imp__sub_8211BD60);
PPC_FUNC(sub_8211BD60) {
    uint32_t lr = ctx.lr; bool m = (lr == 0x8211B888u || lr == 0x8211B8C8u);
    if (m) AdvLog(lr, lr, "sub_8211BD60", "ENTER");
    __imp__sub_8211BD60(ctx, base);
    if (m) AdvLog(lr, lr, "sub_8211BD60", "RET");
}
// The 4 calls between sub_8211BD60 #1 (0x8211B888, RET) and #2 (0x8211B8C8, NOT reached) — the divert is here.
// All operate on the global *(0x828F2D34). The one that ENTERs but never RETs (longjmps to the tail) is it.
#define ADV_HOOK(FN, SITE) extern "C" PPC_FUNC(__imp__##FN); PPC_FUNC(FN) { \
    uint32_t _lr = ctx.lr; AdvLog(_lr, SITE, #FN, "ENTER"); __imp__##FN(ctx, base); AdvLog(_lr, SITE, #FN, "RET"); }
// sub_8211BE68 manual hook: it's the divert (ENTERs from sub_8211B740@0x8211B894 but never RETurns — stuck in
// a non-terminating resource-load loop). REX_FORCEBE68 = breakthrough test: skip it (return success) ONLY when
// called from sub_8211B740, so sub_8211B740 continues to the sub_8210AF90 dispatch → transitions enable. Then
// watch (REX_INITDIAG/REX_LOADERPROBE) whether the title ADVANCES (sub_8210AF90 runs, new menu assets requested)
// or hits the next blocker. Tests whether the advance machinery works downstream of this load.
// cont.28: r31 (the resource path) is corrupted across the vtable[11] poll (sub_822487C8) inside sub_8211BE68,
// so the subsequent sub_8224F890 gets 0xFFFFFFFF instead of the path -> resource not found -> sub_82110728
// runaway. Stash the real path here so REX_FIXPATH can restore it at the sub_8224F890 call (tractability test).
static std::atomic<uint32_t> g_be68Path{0};
extern "C" PPC_FUNC(__imp__sub_8211BE68);
PPC_FUNC(sub_8211BE68) {
    uint32_t _lr = ctx.lr;
    if (_lr == 0x8211B894u && ctx.r3.u32 >= 0x82000000u && ctx.r3.u32 < 0x82600000u)
        g_be68Path.store(ctx.r3.u32);   // the path arg, before the poll corrupts r31
    static const bool force = getenv("REX_FORCEBE68") != nullptr;
    if (force && _lr == 0x8211B894u) {
        static std::atomic<bool> once{false}; bool e=false;
        if (once.compare_exchange_strong(e,true)) fprintf(stderr, "[forcebe68] SKIPPING sub_8211BE68 @0x8211B894 — testing if the title then advances\n");
        ctx.r3.u64 = 0;
        return;
    }
    // REX_RESID (cont.27): identify the resource the stuck advance-gate fetch is for. Dump the job arg + the
    // two static descriptors the fetch uses (sub_8224F890's type key 0x820DA30C, sub_82247E70's comparator
    // 0x820D066C) as hex+ASCII (MSVC vtable/RTTI may carry a class name) to learn if it's a shader/render res.
    static const bool resid = getenv("REX_RESID") != nullptr;
    if (resid && _lr == 0x8211B894u) {
        static std::atomic<int> rc{0};
        if (rc.fetch_add(1) < 3) {
            auto dumpr = [&](const char* nm, uint32_t a){
                fprintf(stderr, "[resid] %s @0x%08X hex:", nm, a);
                for (int i=0;i<8;i++) fprintf(stderr, " %08X", PPC_LOAD_U32(a+i*4));
                fprintf(stderr, "  asc:");
                for (int i=0;i<48;i++){ uint8_t c=PPC_LOAD_U8(a+i); fprintf(stderr, "%c", (c>=32&&c<127)?(char)c:'.'); }
                fprintf(stderr, "\n");
            };
            uint32_t job = ctx.r3.u32;
            fprintf(stderr, "[resid] === sub_8211BE68 job(r3)=0x%08X ===\n", job);
            dumpr("typekey 820DA30C", 0x820DA30Cu);
            dumpr("compare 820D066C", 0x820D066Cu);
            // follow word0 of each one level (vtable -> code, or RTTI -> name)
            uint32_t k0 = PPC_LOAD_U32(0x820DA30Cu);
            if (k0 >= 0x82000000u && k0 < 0x83000000u) dumpr("  typekey[0]->", k0);
            if (job >= 0x82000000u && job < 0xA0000000u) dumpr("job->", job);
        }
    }
    // RIGOR (cont.28): log EVERY sub_8211BE68 call's (caller lr, r3 arg) at ENTER + a RET marker, for ALL
    // callers — so the call that ENTERs without a RET is the true hang, and its r3 tells whether it's the
    // Meshes path (0x820D2844) or a sentinel (0xFFFFFFFF). Resolves whether cont.27's resource ID is right.
    if (resid) { static std::atomic<int> ec{0}; int e = ec.fetch_add(1);
        if (e < 24) {
            uint32_t r3 = ctx.r3.u32; char pb[48]; pb[0]=0;
            if (r3 >= 0x82000000u && r3 < 0x82600000u) { int i=0; for(;i<47;i++){uint8_t c=PPC_LOAD_U8(r3+i); if(c<32||c>=127)break; pb[i]=(char)c;} pb[i]=0; }
            // The vtable[11] poll at 0x8211BE98 = *(*(*(0x828EEEC4))+44). Suspected of corrupting r31 (the path)
            // -> sub_8224F890 gets 0xFFFFFFFF. Capture the poll target so it can be read for r31 preservation.
            uint32_t pobj = PPC_LOAD_U32(0x828EEEC4u);
            uint32_t pvt  = (pobj >= 0x82000000u && pobj < 0xA0000000u) ? PPC_LOAD_U32(pobj) : 0;
            uint32_t poll = (pvt  >= 0x82000000u && pvt  < 0xA0000000u) ? PPC_LOAD_U32(pvt + 44) : 0;
            fprintf(stderr, "[be68call] #%d ENTER lr=0x%08X r3=0x%08X '%s' | pollobj=0x%08X vt=0x%08X poll[11]=0x%08X\n",
                    e, _lr, r3, pb, pobj, pvt, poll);
            __imp__sub_8211BE68(ctx, base);
            fprintf(stderr, "[be68call] #%d RET (lr=0x%08X)\n", e, _lr);
            return;
        }
    }
    AdvLog(_lr, 0x8211B894u, "sub_8211BE68", "ENTER"); __imp__sub_8211BE68(ctx, base); AdvLog(_lr, 0x8211B894u, "sub_8211BE68", "RET");
}
ADV_HOOK(sub_8211D160, 0x8211B8A4u)
ADV_HOOK(sub_8211DB78, 0x8211B8B0u)
ADV_HOOK(sub_8211DD18, 0x8211B8BCu)
#undef ADV_HOOK
// REX_ITEMSPIN (cont.25): the loader work-loop sub_82247E70 spins forever calling the item state-machine
// sub_82249338 (switch on item-state r5) while items stay pending (re-queued, never GPU-completed). Log the
// item+state sequence: if ONE item cycles a fixed state forever, that's the stuck resource + the completion
// state to model (the plan's STEP-0: signal what that state polls). Cap to keep it cheap. Gated, default-off.
extern "C" PPC_FUNC(__imp__sub_82249338);
PPC_FUNC(sub_82249338) {
    static const bool on = getenv("REX_ITEMSPIN") != nullptr;
    uint32_t r3 = ctx.r3.u32, r4 = ctx.r4.u32, r5 = ctx.r5.u32;
    // REX_LOOPCAP=N (cont.25): the loader work-loop sub_82247E70 spins forever calling this from lr=0x82247EFC
    // (loop continues while r3!=0). Cap it: after N calls from that site, return 0 so the loop EXITS — letting
    // sub_82247E70 process the real burst then return, so sub_8211BE68 returns having done MOST of its work
    // (cleaner than the blunt REX_FORCEBE68 skip-all). Test whether this advances the title with fewer nulls.
    static const char* s_cap = getenv("REX_LOOPCAP");
    if (s_cap && static_cast<uint32_t>(ctx.lr) == 0x82247EFCu) {
        static std::atomic<int> lc{0}; int n = lc.fetch_add(1);
        if (n >= atoi(s_cap)) {
            static std::atomic<bool> once{false}; bool e=false;
            if (once.compare_exchange_strong(e,true)) fprintf(stderr, "[loopcap] breaking sub_82247E70 loop after %d iters\n", n);
            ctx.r3.u64 = 0; return;
        }
    }
    __imp__sub_82249338(ctx, base);
    if (on) { static std::atomic<int> n{0}; int k = n.fetch_add(1);
        // VT1 PROBE (cont.26): the spin loop sub_82247E70 (lr=0x82247EFC) calls each item's vtable[1]
        // (*(*(item)+4)); that method returns a state code (r5 here = the PREVIOUS item's code). Observed
        // codes are only {0=advance/keep, 3=re-sift/re-queue}, NEVER 2 (=decrement count=remove/DONE). So
        // an item leaves the queue only when vtable[1] returns 2. To read that gate, log each spin-loop
        // returned item's vtable[1] guest target + its state field *(item+4). Windows: first 80 + a deep
        // steady-state slice (to catch the stuck item once the early burst settles).
        bool spin = (static_cast<uint32_t>(ctx.lr) == 0x82247EFCu);
        bool win  = (k < 80) || (k >= 300000 && k < 300120);
        if (win) {
            uint32_t reti = ctx.r3.u32;
            if (reti) {
                uint32_t vtbl = PPC_LOAD_U32(reti + 0);
                uint32_t vt1  = vtbl ? PPC_LOAD_U32(vtbl + 4) : 0;
                uint32_t s4   = PPC_LOAD_U32(reti + 4);
                fprintf(stderr, "[itemspin] #%d %s r5=%u r4=0x%08X ret=0x%08X vtbl=0x%08X vt1=0x%08X *+4=%u\n",
                        k, spin ? "SPIN" : "    ", r5, r4, reti, vtbl, vt1, s4);
            } else {
                fprintf(stderr, "[itemspin] #%d %s r5=%u r4=0x%08X ret=0 (loop-exit signal)\n",
                        k, spin ? "SPIN" : "    ", r5, r4);
            }
        }
    }
}
// REX_LOOPTRACE (cont.26): directly test whether the advance-gate spin loop sub_82247E70 RETURNS or hangs
// in this exact config. Log ENTER (seq#, args, lr) + RET. An ENTER with no matching RET (run times out) is
// the confirmed hang; every ENTER paired with a RET means the loop drains and is NOT the wall. Gated/off.
extern "C" PPC_FUNC(__imp__sub_82247E70);
PPC_FUNC(sub_82247E70) {
    static const bool on = getenv("REX_LOOPTRACE") != nullptr;
    if (!on) { __imp__sub_82247E70(ctx, base); return; }
    static std::atomic<int> seq{0}; int s = seq.fetch_add(1);
    uint32_t lr = ctx.lr, a3 = ctx.r3.u32, a4 = ctx.r4.u32;
    // PATH-LOAD probe (cont.28): for a path-based fetch (sub_8211BD60/sub_8211BE68 -> sub_8224F890 -> sub_8224F918
    // -> sub_82247E70 with r3 = the path string in .rdata), capture the path + the return. Textures\Global.bin
    // (works) should return FOUND; Meshes\Global.bin (hangs) should return NULL — pinpointing the divergence.
    bool isPath = (a3 >= 0x82000000u && a3 < 0x82600000u && (PPC_LOAD_U8(a3) >= 32 && PPC_LOAD_U8(a3) < 127));
    char pbuf[72]; pbuf[0]=0;
    if (isPath) { int i=0; for (; i<71; i++){ uint8_t c=PPC_LOAD_U8(a3+i); if(c<32||c>=127) break; pbuf[i]=(char)c; } pbuf[i]=0; }
    if (s < 50) fprintf(stderr, "[looptrace] #%d ENTER lr=0x%08X r3=0x%08X r4=0x%08X\n", s, lr, a3, a4);
    __imp__sub_82247E70(ctx, base);
    if (isPath) { static std::atomic<int> p{0}; if (p.fetch_add(1) < 400)
        fprintf(stderr, "[loadpath] sub_82247E70('%s') -> ret=0x%08X %s\n", pbuf, ctx.r3.u32, ctx.r3.u32 ? "FOUND" : "NULL=hang-root"); }
    if (s < 50) fprintf(stderr, "[looptrace] #%d RET (loop drained)\n", s);
}
// REX_BE68INNER (cont.26): sub_8211BE68 (from sub_8211B740) ENTERs but never RETs, yet its inner heap-walk
// sub_82247E70 always returns. Localize WHICH of sub_8211BE68's calls actually hangs by wrapping each with
// ENTER/RET + caller lr + r3. The one that shows ENTER without a matching RET is the true wall. Gated/off.
// Filter to sub_8211BE68's OWN call-sites so we see only its inner calls (the stuck invocation), not the
// hundreds of unrelated callers. The one with ENTER and no matching RET is the wall. If NONE fire for the
// final (stuck) invocation, the hang is the indirect vtable poll at 0x8211BE98 (before these named calls).
#define BE68_INNER_HOOK(FN, SITE) \
  extern "C" PPC_FUNC(__imp__##FN); \
  PPC_FUNC(FN) { static const bool on = getenv("REX_BE68INNER") != nullptr; \
    bool m = on && (static_cast<uint32_t>(ctx.lr) == SITE##u); \
    if (!m) { __imp__##FN(ctx, base); return; } \
    static std::atomic<int> k{0}; int n = k.fetch_add(1); uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32; \
    fprintf(stderr, "[be68in] %-14s #%d ENTER @0x%X r3=0x%08X r4=0x%08X\n", #FN, n, SITE, a3, a4); \
    __imp__##FN(ctx, base); \
    fprintf(stderr, "[be68in] %-14s #%d RET\n", #FN, n); }
BE68_INNER_HOOK(sub_82250110, 0x8211BEC4)
BE68_INNER_HOOK(sub_82110728, 0x8211BECC)
BE68_INNER_HOOK(sub_822501C8, 0x8211BED4)   // last named call
#undef BE68_INNER_HOOK
// sub_8224F890 standalone hook: REX_BE68INNER logging + REX_FIXPATH (cont.28). When called from sub_8211BE68
// @0x8211BEB0 with the corrupted r4=0xFFFFFFFF, restore r4 to the real path (stashed in g_be68Path). If this
// makes Meshes\Global.bin load and the title advance, the r31-corruption diagnosis is confirmed end-to-end.
extern "C" PPC_FUNC(__imp__sub_8224F890);
PPC_FUNC(sub_8224F890) {
    static const bool fixpath = getenv("REX_FIXPATH") != nullptr;
    static const bool be68in  = getenv("REX_BE68INNER") != nullptr;
    bool site = (static_cast<uint32_t>(ctx.lr) == 0x8211BEB0u);
    if (fixpath && site && ctx.r4.u32 == 0xFFFFFFFFu) {
        uint32_t p = g_be68Path.load();
        if (p) { static std::atomic<int> fc{0}; if (fc.fetch_add(1) < 4)
                     fprintf(stderr, "[fixpath] restoring sub_8224F890 r4 0xFFFFFFFF -> 0x%08X (the real path)\n", p);
                 ctx.r4.u64 = p; }
    }
    if (be68in && site) { static std::atomic<int> k{0}; int n = k.fetch_add(1);
        uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;
        fprintf(stderr, "[be68in] sub_8224F890     #%d ENTER @0x8211BEB0 r3=0x%08X r4=0x%08X\n", n, a3, a4);
        __imp__sub_8224F890(ctx, base);
        fprintf(stderr, "[be68in] sub_8224F890     #%d RET\n", n);
        return;
    }
    __imp__sub_8224F890(ctx, base);
}
// REX_728INNER (cont.26): sub_82110728 is the real hang (ENTER, never RET). It's a counted loop
//   r28 = sub_8224FDB0(obj); while(r28) { r28--; sub_82110B18; if(==0){sub_821B1310; sub_8212E830;} sub_82250090 }
// Either r28 is a runaway count (log it once) or one inner work call hangs (ENTER without a matching RET).
extern "C" PPC_FUNC(__imp__sub_8224FDB0);
PPC_FUNC(sub_8224FDB0) {
    static const bool on = getenv("REX_728INNER") != nullptr;
    static const bool cap = getenv("REX_728CAP") != nullptr;   // surgical advance test (cont.26)
    bool m = on && (static_cast<uint32_t>(ctx.lr) == 0x8211073Cu);   // the count-establishing call
    uint32_t obj = ctx.r3.u32;
    __imp__sub_8224FDB0(ctx, base);
    // REX_728CAP: sub_82110728 deserializes a loop-count from a null-source stream and gets 0x60606060
    // filler -> ~1.6B-iter runaway = the real advance-gate hang. Surgically clamp the absurd count to 0 so
    // the loop runs 0 times and sub_82110728 RETURNS (vs the blunt REX_FORCEBE68 skip of all of sub_8211BE68).
    // Test whether the title then advances CLEANLY past the gate. Only the count call (lr=0x8211073C).
    if (cap && static_cast<uint32_t>(ctx.lr) == 0x8211073Cu && ctx.r3.u32 > 0x10000u) {
        static std::atomic<int> c{0}; if (c.fetch_add(1) < 8)
            fprintf(stderr, "[728cap] clamping runaway count 0x%X -> 0 (stream=0x%08X)\n", ctx.r3.u32, obj);
        ctx.r3.u64 = 0;
    }
    if (m) { static std::atomic<int> k{0}; if (k.fetch_add(1) < 4) {
        uint32_t cnt = ctx.r3.u32;
        fprintf(stderr, "[728in] sub_8224FDB0 stream=0x%08X loop-count=0x%X\n", obj, cnt);
        // Dump the stream object + chase any guest pointers one level to find the 0x60 source buffer.
        for (int i = 0; i < 10; i++) {
            uint32_t w = PPC_LOAD_U32(obj + i*4);
            fprintf(stderr, "   stream[+%02X]=0x%08X", i*4, w);
            if (w >= 0x82000000u && w < 0xA0000000u) {   // looks like a guest data/heap pointer — peek
                fprintf(stderr, "  -> [%08X %08X %08X %08X]",
                        PPC_LOAD_U32(w), PPC_LOAD_U32(w+4), PPC_LOAD_U32(w+8), PPC_LOAD_U32(w+12));
            }
            fprintf(stderr, "\n");
        }
    } }
}
#define IH728(FN, SITE) \
  extern "C" PPC_FUNC(__imp__##FN); \
  PPC_FUNC(FN) { static const bool on = getenv("REX_728INNER") != nullptr; \
    bool m = on && (static_cast<uint32_t>(ctx.lr) == SITE##u); \
    if (!m) { __imp__##FN(ctx, base); return; } \
    static std::atomic<int> k{0}; int n = k.fetch_add(1); uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32; \
    if (n < 8) fprintf(stderr, "[728in] %-14s #%d ENTER r3=0x%08X r4=0x%08X\n", #FN, n, a3, a4); \
    __imp__##FN(ctx, base); \
    if (n < 8) fprintf(stderr, "[728in] %-14s #%d RET\n", #FN, n); }
IH728(sub_82110B18, 0x8211077C)
IH728(sub_821B1310, 0x821107B0)
IH728(sub_8212E830, 0x821107D8)
IH728(sub_82250090, 0x821107F0)
#undef IH728
// REX_SKIPPOLL (cont.28): the vtable[11] poll sub_822487C8 (called from sub_8211BE68@0x8211BE98) corrupts
// sub_8211BE68's frame (r31/path -> 0xFFFFFFFF, and broader — restoring r4 alone doesn't propagate). Test the
// corruption hypothesis: when called from that site, return success (1) WITHOUT running the corrupting body, so
// sub_8211BE68 proceeds with an intact frame. If the Meshes fetch then reaches sub_82247E70(path) / the title
// advances, the poll's corruption is the blocker. Filtered to the one site (sub_822487C8 is a hot vtable method).
// REX_R31TRACK (cont.29): localize WHICH callee of the poll sub_822487C8 corrupts r31 (path -> 0xFFFFFFFF). Set
// g_inPoll while inside the poll (from site 0x8211BE98); the callee hooks log r31 entry/exit only then. The call
// whose r31-out != r31-in is the corruptor (or its subtree). tid=10 single-thread here, so a plain flag is fine.
// (g_inPoll / g_r31seq defined earlier, before sub_8242BF10.)
extern "C" PPC_FUNC(__imp__sub_822487C8);
PPC_FUNC(sub_822487C8) {
    static const bool skip = getenv("REX_SKIPPOLL") != nullptr;
    static const bool trk  = getenv("REX_R31TRACK") != nullptr;
    bool atSite = (static_cast<uint32_t>(ctx.lr) == 0x8211BE98u);
    if (skip && atSite) {
        static std::atomic<int> sc{0}; if (sc.fetch_add(1) < 4)
            fprintf(stderr, "[skippoll] skipping sub_822487C8 @0x8211BE98 (returning 1, no body) — frame-corruption test\n");
        ctx.r3.u64 = 1;
        return;
    }
    if (atSite) {
        // Always mark in-poll here so REX_CLAMPCPY works standalone (the 0x8211BE98 poll is rare, not hot).
        uint32_t r31in = ctx.r31.u32; bool prev = g_inPoll; g_inPoll = true;
        bool logTrk = trk && g_r31seq < 2; int seq = logTrk ? g_r31seq++ : -1;
        if (logTrk) fprintf(stderr, "[r31] === poll #%d ENTER r31=0x%08X r1=0x%08X ===\n", seq, r31in, ctx.r1.u32);
        __imp__sub_822487C8(ctx, base);
        g_inPoll = prev;
        if (logTrk) fprintf(stderr, "[r31] === poll #%d EXIT  r31=0x%08X %s ===\n", seq, ctx.r31.u32,
                            ctx.r31.u32 == r31in ? "(preserved)" : "(CORRUPTED!)");
        return;
    }
    __imp__sub_822487C8(ctx, base);
}
// (cont.29) The callee r31-trackers R31_HOOK(sub_82448158/sub_82448B50/sub_8244FE80) confirmed sub_82448158 as the
// corruptor; removed (they wrapped hot loader fns, adding per-call overhead). The root cause is now pinned to the
// in-poll memcpy with size=*(obj+60)=0xFFFFFFFF; REX_CLAMPCPY neutralizes it (and REX_R31TRACK still logs the
// poll ENTER/EXIT + the memcpy dest/size for re-verification).

// cont.31 (REX_HANDLERGUARD, gated experiment): sub_824253C8 (ppc_recomp.59.cpp:21119) dispatches a GLOBAL
// handler fn-pointer at *(0x828183A0): it guards only `r10==0` (-> return error 0x80004001), then `bctr` to the
// pointer. When the handler is UNREGISTERED the global holds 0xFFFFFFFF (a -1 sentinel — same class as cont.30's
// +60), which passes the ==0 check, so it bctr's to 0xFFFFFFFF (INDIRECT-NULL at lr=0x8215DE84, the menu-setup
// caller). The generic INDIRECT-NULL guard then skips the call but leaves r3=1 (a FALSE success: r3=1 was set
// before the bctr), so the menu-setup proceeds on bad state and stalls (seen with REX_SKIPINTRO: loads 88
// gameplay assets then hangs here). Guard: treat the 0xFFFFFFFF sentinel like null -> return the proper error
// 0x80004001 (<0 as int32), so the caller (sub_8215DE84, `if (r3<0)` at ppc_recomp.7.cpp:16283) takes its
// "handler not ready" branch instead of a bogus success. Gated for testing; promote to default-on only if it
// cleanly advances. (Global addr derived exactly as the recomp does: r11=lis -32126 => 0x82820000, -31840.)
extern "C" PPC_FUNC(__imp__sub_824253C8);
PPC_FUNC(sub_824253C8) {
    static const bool guard = getenv("REX_HANDLERGUARD") != nullptr;
    if (guard) {
        uint32_t addr = (uint32_t)(-2105409536) + (uint32_t)(-31840);   // = 0x828183A0
        uint32_t h = GLD32(addr);
        if (h == 0xFFFFFFFFu) {                                          // unregistered-handler sentinel
            static std::atomic<int> g{0}; if (g.fetch_add(1) < 4)
                fprintf(stderr, "[handlerguard] handler *(0x%08X)=0xFFFFFFFF (unregistered) -> return error 0x80004001\n", addr);
            ctx.r3.u64 = 0x80004001u;
            return;
        }
    }
    __imp__sub_824253C8(ctx, base);
}

// cont.33 (REX_GATEDIAG): the gate AFTER Level 1 loads (reached with REX_SKIPINTRO+REX_HANDLERGUARD) =
// sub_82292CE0 (a frontend teardown/update at caller 0x8215079C). It reads a subsystem object *(0x827FD56C) and
// calls its vtable[1]; the INDIRECT-NULL shows that target is 0. Dump obj/vtable/vtable[1] to classify the gate:
// obj==0 => the subsystem was NEVER created (uninit); obj!=0,vt==0 => object exists but vtable null; the vtable
// addr (if non-zero) identifies the class (=> is it GPU/render = deep build, or a CPU subsystem = maybe tractable).
extern "C" PPC_FUNC(__imp__sub_82292CE0);
PPC_FUNC(sub_82292CE0) {
    static const bool diag = getenv("REX_GATEDIAG") != nullptr;
    if (diag) { static std::atomic<int> g{0}; if (g.fetch_add(1) < 3) {
        uint32_t obj = GLD32(0x827FD56Cu);
        uint32_t vt  = obj ? GLD32(obj) : 0;
        uint32_t m1  = vt  ? GLD32(vt + 4) : 0;
        fprintf(stderr, "[gatediag] sub_82292CE0: obj *(0x827FD56C)=0x%08X vtable=0x%08X vtable[1]=0x%08X struct+824=0x%08X\n",
                obj, vt, m1, GLD32(0x827FD568u + 824));
    }}
    __imp__sub_82292CE0(ctx, base);
}

// Diag (gated): confirm the title's own completion poster fires after a forced EOS (sub_82425BF8 EOF branch).
extern "C" PPC_FUNC(__imp__sub_8222A9F8);
PPC_FUNC(sub_8222A9F8)
{
    static const bool log = g_ktrace && getenv("REX_MOVIE_EOF") != nullptr;
    if (log) { static std::atomic<int> k{0};
        if (k.fetch_add(1) < 4) fprintf(stderr, "[movie-eof] completion-post (sub_8222A9F8) r3=0x%X r4=0x%X\n", ctx.r3.u32, ctx.r4.u32); }
    __imp__sub_8222A9F8(ctx, base);
}

// ---- GPU fence-wait completion: forward the completion fence past deferred segments (sub_821C6E58) --
// sub_821C6E58 spins until the GPU completion fence *(*(device+10896)) reaches `target` (r4), measured
// relative to the latest built fence head=*(device+10908). The title builds PM4 into command-buffer
// segments and only periodically kicks them to the ring (CP_RB_WPTR, via sub_821C6600); it relies on
// the real GPU auto-flushing partially-filled segments, so when it later waits on an OLDER fence
// (target != head) it assumes that work already reached the GPU. Variant A's CP only executes what the
// title explicitly kicks, so the deferred tail (built but un-kicked: e.g. fences 7..15 with head=17,
// fence=5) never runs and an old target is never reached — the post-logo resource teardown (on tid=4,
// whose exit the CRT-init join waits on) spins forever; later, the per-frame render loop hits the same
// wall. (Confirmed: the title's own flush sub_821C6D58 will NOT push this segment without the GPU↔CPU
// WAIT_REG_MEM handshake we don't model; calling it advances head but kicks nothing.) Variant A's CP is
// synchronous and has no renderer, so a deferred segment's only effect on this waiter is the fence value
// itself — its draws/state are no-ops here. So forward the completion fence to the requested target
// (forward-only) and let the wait succeed on its normal path: the synchronous CP has, by construction,
// already done everything that can affect the waiter.
// [STOPGAP. The clean fix is a continuous CP that follows the WAIT_REG_MEM-chained deferred IBs and
//  executes them, so the fence advances as a real result. This forwarding MUST be replaced before a real
//  renderer lands, or deferred draws would be skipped. See varianta/NIGHT-LOG.md + NEXT-SESSION-PROMPT.]
extern "C" PPC_FUNC(__imp__sub_821C6E58);
PPC_FUNC(sub_821C6E58)
{
    if (g_coop || g_preempt) {   // stopgap fires in BOTH scheduler modes — variant A has no real CP in either
        uint32_t device = ctx.r3.u32, target = ctx.r4.u32;
        uint32_t fenceptr = GLD32(device + 10896);
        if (fenceptr) {
            uint32_t head = GLD32(device + 10908), current = GLD32(fenceptr);
            // ROUTE A instrumentation: log the fence dynamics at each wait (target vs current vs head) +
            // the command-build cursor (device+13568 base / +13572 writeptr) — to design a real CP that
            // advances the counter by executing the built EVENT_WRITE_SHD fence-writes instead of forwarding.
            static std::atomic<int> wn{0};
            if (g_ktrace && wn.load() < 240) {
                int k = ++wn;
                fprintf(stderr, "[fencewait] #%d tgt=%u cur=%u head=%u gap=%d build=0x%X..0x%X\n",
                        k, target, current, head, (int)(target - current), GLD32(device + 13568), GLD32(device + 13572));
            }
            // Stuck on an OLDER fence (target != head) that hasn't been reached: the title built this
            // fence's command-buffer segment but deferred kicking it to the ring, relying on real-GPU
            // auto-flush as segments fill (its own target==head gate does not cover target<head, and its
            // flush sub_821C6D58 won't push this segment without the GPU↔CPU WAIT_REG_MEM handshake we
            // don't model). Variant A's CP is synchronous and has no renderer, so the deferred segment's
            // only effect on this waiter is the fence write itself — its draws/state are no-ops here.
            // Advance the completion fence to the target so the wait succeeds on the normal path (the
            // synchronous CP has, by construction, finished everything that can affect it). Forward-only.
            // [stopgap — the clean fix is a continuous CP that executes WAIT_REG_MEM-chained deferred IBs]
            if (target != head &&
                static_cast<uint32_t>(head - target) < static_cast<uint32_t>(head - current)) {
                GST32(fenceptr, target);
                if (g_ktrace) fprintf(stderr, "[fencefwd] fence@0x%X %u->%u (head=%u) — deferred-segment wait satisfied\n",
                                      fenceptr, current, target, head);
            }
        }
    }
    __imp__sub_821C6E58(ctx, base);
}

// sub_821C5DF0 — the post-frame GPU sync (sub_821BFF48 -> sub_821C6278 -> sub_821C5EA8 -> here): waits
// until the GPU's consumed command-buffer position *(fenceptr+4) (segment pointer; low 2 bits = wrap
// generation) reaches the caller's target (r5 = generation-tagged position, r4 = offset). Same deferred
// -segment problem as the counter wait above, on the +4 marker. Forward the segment pointer to the
// requested target when the wait would otherwise spin (the deferred PM4 never reaches variant A's CP).
// [stopgap — see sub_821C6E58]
extern "C" PPC_FUNC(__imp__sub_821C5DF0);
PPC_FUNC(sub_821C5DF0)
{
    if (g_coop || g_preempt) {   // stopgap fires in BOTH scheduler modes — variant A has no real CP in either
        uint32_t device = ctx.r3.u32, off = ctx.r4.u32, posTagged = ctx.r5.u32;
        uint32_t fenceptr = GLD32(device + 10896);
        if (fenceptr) {
            uint32_t segptr = GLD32(fenceptr + 4);
            uint32_t diff = (posTagged - segptr) & 3u;
            bool stuck = (diff != 0) && !(diff == 1 && off <= (segptr & ~3u));
            if (stuck) {
                GST32(fenceptr + 4, posTagged);
                if (g_ktrace) fprintf(stderr, "[fencefwd] segptr@0x%X 0x%X->0x%X (off=0x%X) — post-frame wait satisfied\n",
                                      fenceptr + 4, segptr, posTagged, off);
            }
        }
    }
    __imp__sub_821C5DF0(ctx, base);
}

// void VdInitializeRingBuffer(ptr r3, size_log2 r4)
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    g_ringBufferBase = ctx.r3.u32;
    g_ringBufferSize = 1u << (ctx.r4.u32 & 0x1F);
    KTRACE("VdInitializeRingBuffer(base=0x%X size=0x%X)\n", g_ringBufferBase, g_ringBufferSize);
}

// void VdEnableRingBufferRPtrWriteBack(ptr r3, block_log2 r4)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    g_rptrWriteBack = ctx.r3.u32;
    KTRACE("VdEnableRingBufferRPtrWriteBack(ptr=0x%X)\n", g_rptrWriteBack);
    StartVblankPump();   // GPU sync is now live — start firing vblank interrupts.
}

// void VdSetGraphicsInterruptCallback(callback r3, user_data r4)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    g_interruptCallback = ctx.r3.u32;
    g_interruptData = ctx.r4.u32;
    KTRACE("VdSetGraphicsInterruptCallback(cb=0x%X data=0x%X)\n", g_interruptCallback, g_interruptData);
}

// ---- Rtl helpers (stubs that don't do the work -> caller reads garbage) ----------------------------
// void RtlFillMemoryUlong(dst r3, length r4, pattern r5) — fill length/4 u32s with the BE pattern.
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    uint32_t dst = ctx.r3.u32, len = ctx.r4.u32, pat = ctx.r5.u32;
    for (uint32_t i = 0; i + 4 <= len; i += 4) PPC_STORE_U32(dst + i, pat);
}
// void RtlFillMemoryUchar / RtlFillMemory(dst r3, length r4, value r5)
PPC_FUNC(__imp__RtlFillMemoryUchar)
{
    uint32_t dst = ctx.r3.u32, len = ctx.r4.u32; uint8_t v = static_cast<uint8_t>(ctx.r5.u32);
    for (uint32_t i = 0; i < len; i++) PPC_STORE_U8(dst + i, v);
}
// void RtlTimeToTimeFields(*time r3, *fields r4) — TIME_FIELDS: y,mo,d,h,mi,s,ms,wday (8x u16). Fixed date.
PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    uint32_t f = ctx.r4.u32; if (!f) return;
    static const uint16_t tf[8] = { 2024, 1, 1, 0, 0, 0, 0, 1 };
    for (uint32_t i = 0; i < 8; i++) PPC_STORE_U16(f + i * 2, tf[i]);
}

// ---- XamInput (fill out-params: the input worker uses the returned state/caps) ---------------------
// No controller connected = the clean boot path; zero the out-struct so nothing reads garbage.
namespace { constexpr uint32_t kErrNotConnected = 0x0000048Fu; }   // ERROR_DEVICE_NOT_CONNECTED
PPC_FUNC(__imp__XamInputGetState)
{
    // REX_SKIPINTRO: present a CONNECTED pad with START (0x0010) held + an incrementing packet number, so
    // the title's intro/movie skip handler fires and advances to the menu/gameplay (where it builds real
    // textured PM4 draws). The intro is renderer-dead (VC-1 decoder idle); this exercises the real renderer.
    static const bool skip = getenv("REX_SKIPINTRO") != nullptr;
    uint32_t s = ctx.r5.u32;
    if (skip && s) {
        static std::atomic<uint32_t> pkt{0};
        uint32_t p = pkt.fetch_add(1);
        PPC_STORE_U32(s + 0, p + 1);                  // dwPacketNumber (changes each poll => new input)
        // PULSE A+START (down ~8 polls, up ~8) so an edge-detecting skip handler registers a press.
        uint16_t btn = ((p >> 3) & 1) ? 0x1010 : 0x0000;   // 0x1000=A | 0x0010=START
        PPC_STORE_U16(s + 4, btn);
        for (uint32_t i = 6; i < 16; i++) PPC_STORE_U8(s + i, 0);
        static std::atomic<bool> once{false}; bool e=false;
        if (g_ktrace && once.compare_exchange_strong(e,true)) fprintf(stderr, "[skipintro] injecting START press via XamInputGetState\n");
        ctx.r3.u64 = 0;                               // ERROR_SUCCESS (connected)
        return;
    }
    if (s) for (uint32_t i = 0; i < 16; i += 4) PPC_STORE_U32(s + i, 0);
    ctx.r3.u64 = kErrNotConnected;
}
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    uint32_t c = ctx.r5.u32; if (!c) { ctx.r3.u64 = 0x80070057u; return; }
    for (uint32_t i = 0; i < 0x20; i += 4) PPC_STORE_U32(c + i, 0);
    static const bool skip = getenv("REX_SKIPINTRO") != nullptr;
    if (skip) {                                   // report a connected standard gamepad (see XamInputGetState)
        PPC_STORE_U8(c + 0, 1); PPC_STORE_U8(c + 1, 1);   // Type=GAMEPAD, SubType=GAMEPAD
        ctx.r3.u64 = 0; return;
    }
    ctx.r3.u64 = kErrNotConnected;
}
PPC_FUNC(__imp__XamInputSetState) { ctx.r3.u64 = 0; }   // vibration: no-op success
// REX_SKIPINTRO: pulse a VK_PAD_START (0x5814) keystroke so an attract/title "press START" poll (which uses
// XamInputGetKeystrokeEx, per the boot-logo loop sub_8214F738) advances to the menu. X_INPUT_KEYSTROKE:
// VirtualKey@0(u16), Unicode@2, Flags@4 (KEYDOWN=1), UserIndex@6, HidCode@7. Return ERROR_EMPTY (0x10D5)
// between pulses so the title keeps polling instead of treating the pad as absent.
namespace { void InjectKeystroke(PPCContext& ctx) {
    uint32_t k = ctx.r5.u32; if (k) for (uint32_t i = 0; i < 16; i += 4) GST32(k + i, 0);
    static const bool skip = getenv("REX_SKIPINTRO") != nullptr;
    if (skip && k) {
        static std::atomic<uint32_t> p{0}; uint32_t c = p.fetch_add(1);
        if ((c % 24) < 2) {                            // periodic START keydown
            GST16(k + 0, 0x5814);                      // VirtualKey = VK_PAD_START
            GST16(k + 4, 0x0001);                      // Flags = XINPUT_KEYSTROKE_KEYDOWN
            static std::atomic<bool> once{false}; bool e=false;
            if (g_ktrace && once.compare_exchange_strong(e,true)) fprintf(stderr, "[skipintro] injecting VK_PAD_START keystroke\n");
            ctx.r3.u64 = 0; return;                    // ERROR_SUCCESS
        }
        ctx.r3.u64 = 0x000010D5u; return;             // ERROR_EMPTY
    }
    ctx.r3.u64 = kErrNotConnected;
} }
PPC_FUNC(__imp__XamInputGetKeystroke)   { InjectKeystroke(ctx); }
PPC_FUNC(__imp__XamInputGetKeystrokeEx) { InjectKeystroke(ctx); }

// ---- XAudio (fill out-params: the audio worker uses the returned driver/config/volume) -------------
// DWORD XAudioGetSpeakerConfig(*config r3) -> 0x00010001
PPC_FUNC(__imp__XAudioGetSpeakerConfig) { if (ctx.r3.u32) PPC_STORE_U32(ctx.r3.u32, 0x00010001u); ctx.r3.u64 = 0; }
// DWORD XAudioGetVoiceCategoryVolume(unk r3, *out r4) -> 1.0f
PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    if (ctx.r4.u32) { float v = 1.0f; uint32_t b; memcpy(&b, &v, 4); PPC_STORE_U32(ctx.r4.u32, b); }
    ctx.r3.u64 = 0;
}
// DWORD XAudioRegisterRenderDriverClient(*callback r3, *driver r4) -> driver handle 0x41550000|index
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    uint32_t cbPtr = ctx.r3.u32, drvPtr = ctx.r4.u32;
    if (!cbPtr || PPC_LOAD_U32(cbPtr) == 0) { ctx.r3.u64 = 0x80070057u; return; }   // E_INVALIDARG
    if (drvPtr) PPC_STORE_U32(drvPtr, 0x41550000u);   // valid driver handle (index 0)
    KTRACE("XAudioRegisterRenderDriverClient -> 0x41550000\n");
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient) { ctx.r3.u64 = 0; }

// DWORD VdInitializeEngines(...) -> 1 (success); the system command buffer id address is a no-op.
PPC_FUNC(__imp__VdInitializeEngines) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) { /* no-op */ }

// ---- the rest of the Vd* video surface (fill the out-params the D3D device init reads) -------------
// void VdGetSystemCommandBuffer(p0 r3, p1 r4) — zero 0x94 at p0, then the two GPU identifiers.
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    uint32_t p0 = ctx.r3.u32, p1 = ctx.r4.u32;
    if (p0) { for (uint32_t i = 0; i < 0x94; i += 4) PPC_STORE_U32(p0 + i, 0); PPC_STORE_U32(p0, 0xBEEF0000u); }
    if (p1) PPC_STORE_U32(p1, 0xBEEF0001u);
    KTRACE("VdGetSystemCommandBuffer -> 0xBEEF0000/0xBEEF0001\n");
}

// void VdGetCurrentDisplayInformation(X_DISPLAY_INFO* r3) — 0x58 bytes; fill 1280x720 COMPLETELY
// (incl. the scaler sub-struct — a zeroed scaled_output_w/h is a div-by-zero source in device build).
PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    uint32_t d = ctx.r3.u32; if (!d) return;
    for (uint32_t i = 0; i < 0x58; i += 4) PPC_STORE_U32(d + i, 0);
    PPC_STORE_U16(d + 0x00, 1280); PPC_STORE_U16(d + 0x02, 720);   // front_buffer width/height
    // scaler_parameters @ 0x8: source_rect.x2/y2 @ +0x10/+0x14, scaled_output_w/h @ +0x18/+0x1C,
    // horizontal/vertical_filter_type @ +0x30/+0x34.
    PPC_STORE_U32(d + 0x10, 1280); PPC_STORE_U32(d + 0x14, 720);   // scaler_source_rect.x2/y2
    PPC_STORE_U32(d + 0x18, 1280); PPC_STORE_U32(d + 0x1C, 720);   // scaled_output_width/height
    PPC_STORE_U32(d + 0x30, 1);    PPC_STORE_U32(d + 0x34, 1);     // h/v filter_type
    PPC_STORE_U16(d + 0x40, 320);  PPC_STORE_U16(d + 0x42, 180);   // overscan left/top (w/4, h/4)
    PPC_STORE_U16(d + 0x44, 320);  PPC_STORE_U16(d + 0x46, 180);   // overscan right/bottom
    PPC_STORE_U16(d + 0x48, 1280); PPC_STORE_U16(d + 0x4A, 720);   // display width/height
    { float hz = 60.0f; uint32_t b; memcpy(&b, &hz, 4); PPC_STORE_U32(d + 0x4C, b); } // refresh_rate
    PPC_STORE_U32(d + 0x50, 0);                                     // display_interlaced
    PPC_STORE_U16(d + 0x56, 1280);                                  // actual_display_width
}

// DWORD VdQueryVideoFlags() -> widescreen(1) | width>=1024(2)
PPC_FUNC(__imp__VdQueryVideoFlags) { ctx.r3.u64 = 1u | 2u; }

// void VdGetCurrentDisplayGamma(*type r3, *power r4) -> sRGB, 2.2
PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    if (ctx.r3.u32) PPC_STORE_U32(ctx.r3.u32, 1);
    if (ctx.r4.u32) { float p = 2.2f; uint32_t b; memcpy(&b, &p, 4); PPC_STORE_U32(ctx.r4.u32, b); }
}

PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded) { ctx.r3.u64 = 1; }           // BOOL TRUE
PPC_FUNC(__imp__VdSetDisplayMode)          { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdShutdownEngines)         { /* ignored */ }
PPC_FUNC(__imp__VdRetrainEDRAM)            { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdRetrainEDRAMWorker)      { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdPersistDisplay)          { if (ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, 0); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }
PPC_FUNC(__imp__VdEnableDisableClockGating)         { /* no-op */ }
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)    { ctx.r3.u64 = 0; }
// void VdSwap(...) — frame present. Minimal: acknowledge + advance the GPU swap counter so the title's
// frame-pacing / is_counter fences see presents happen (real present = renderer phase).
PPC_FUNC(__imp__VdSwap) {
    uint32_t n = g_gpuCounter.fetch_add(1) + 1;
    // r4 = D3D9 texture fetch constant (6 dwords, BE) describing the front buffer. Parse it (xenos
    // xe_gpu_texture_fetch_t): dword_0 bit31=tiled, bits22..30=pitch(>>5); dword_1 bits0..5=format,
    // bits6..7=endian, bits12..31=base_address(>>12, VIRTUAL); dword_2 = (w-1)|((h-1)<<13).
    uint32_t fetchPtr = ctx.r4.u32;
    uint32_t d0 = GLD32(fetchPtr), d1 = GLD32(fetchPtr + 4), d2 = GLD32(fetchPtr + 8);
    uint32_t tiled = (d0 >> 31) & 1, pitch = (d0 >> 22) & 0x1FF;
    uint32_t format = d1 & 0x3F, endian = (d1 >> 6) & 3;
    uint32_t baseVirt = ((d1 >> 12) & 0xFFFFF) << 12;
    uint32_t basePhysMirror = 0xA0000000u | (baseVirt & 0x1FFFFFFFu);
    uint32_t w = (d2 & 0x1FFF) + 1, h = ((d2 >> 13) & 0x1FFF) + 1;
    if (g_ktrace && n <= 6)
        fprintf(stderr, "[VdSwap] #%u fb_virt=0x%X %ux%u fmt=%u tiled=%u endian=%u pitch=%u | "
                "@virt=%08X %08X %08X | @0xA=%08X %08X %08X\n",
                n, baseVirt, w, h, format, tiled, endian, pitch,
                GLD32(baseVirt), GLD32(baseVirt + 4), GLD32(baseVirt + 8),
                GLD32(basePhysMirror), GLD32(basePhysMirror + 4), GLD32(basePhysMirror + 8));
    // One-time: dump the per-frame command-buffer region around r3 (the swap buffer ptr, "into the ring"
    // per rexglue but outside our main ring 0xA0002000 — the deferred per-frame command buffer). Reveals
    // where the frame's draws are so the CP can execute them (the prerequisite for rendering).
    if (g_ktrace) {
        static std::atomic<bool> dumped{false};
        bool e = false;
        if (n >= 3 && dumped.compare_exchange_strong(e, true)) {
            uint32_t r3 = ctx.r3.u32;
            fprintf(stderr, "[swapbuf] r3=0x%X r5=0x%X r6=0x%X r7=0x%X (ringBase=0x%X size=0x%X) — dwords [r3-0x80 .. r3+0x80):\n",
                    r3, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, GLD32(0x7FC80714), 0);
            for (uint32_t off = 0xFFFFFF80; off != 0x100; off += 0x20) {  // -0x80..+0x60 in 0x20 steps
                uint32_t a = r3 + off;
                char line[256]; int p = snprintf(line, sizeof line, "[swapbuf] %+5d:", (int)off);
                for (int k = 0; k < 8; k++) p += snprintf(line+p, sizeof line-p, " %08X", GLD32(a + k*4));
                fprintf(stderr, "%s\n", line);
            }
        }
        // One-time: walk the MAIN ring (small, 0x1000) as PM4 and flag INDIRECT_BUFFER packets. Model test:
        // the ring is far too small to hold a frame (~139K dwords), so per rexglue the per-frame draws must
        // reach the CP as IB packets in the ring pointing to the big staging segments at 0xA01xxxxx. If the
        // guest builds ring IBs past WPTR but never kicks (advances CP_RB_WPTR), they're the missing draws.
        static std::atomic<bool> rdumped{false};
        bool re = false;
        if (n >= 3 && rdumped.compare_exchange_strong(re, true) && g_ringBufferBase) {
            uint32_t wptr = GLD32(0x7FC80714), rptr = GLD32(0x7FC80710);  // CP_RB_WPTR(0x1C5)/RPTR(0x1C4)
            uint32_t nd = g_ringBufferSize / 4;
            fprintf(stderr, "[ringdump] base=0x%X dwords=%u CP_RB_WPTR=%u CP_RB_RPTR=%u — walking as PM4:\n",
                    g_ringBufferBase, nd, wptr, rptr);
            uint32_t a = g_ringBufferBase, end = g_ringBufferBase + nd * 4; int shown = 0;
            while (a + 4 <= end && shown < 90) {
                uint32_t pk = GLD32(a); uint32_t di = (a - g_ringBufferBase) / 4; a += 4;
                if (pk == 0) continue;
                uint32_t t = pk >> 30;
                const char* mark = (di == wptr) ? " <==WPTR" : (di == rptr ? " <==RPTR" : "");
                if (t == 3) {
                    uint32_t op = (pk >> 8) & 0x7F, cnt = ((pk >> 16) & 0x3FFF) + 1;
                    if (op == 0x3F || op == 0x37) {   // INDIRECT_BUFFER: print target + len
                        uint32_t ib = GLD32(a), len = GLD32(a + 4) & 0xFFFFF;
                        fprintf(stderr, "[ringdump] @+%u IB -> 0x%X (phys 0x%X) len=%u%s\n",
                                di, ib, 0xA0000000u | (ib & 0x1FFFFFFFu), len, mark);
                    } else {
                        fprintf(stderr, "[ringdump] @+%u T3 op=0x%X cnt=%u raw=%08X%s\n", di, op, cnt, pk, mark);
                    }
                    a += cnt * 4;
                } else {
                    fprintf(stderr, "[ringdump] @+%u type%u raw=%08X%s\n", di, t, pk, mark);
                    if (t == 1) a += 8; else if (t == 0) a += (((pk >> 16) & 0x3FFF) + 1) * 4;
                }
                shown++;
            }
        }
    }
    // Renderer part 1 (deferred-CP, EXPERIMENTAL, REX_DEFERCP=1): the per-frame draws live in a deferred
    // command buffer at 0xA01xxxxx (r3 = the swap write-point at its end), NOT the main ring — so our CP
    // never sees them. Execute the range built since the previous swap so DRAW_INDX etc. reach ExecutePM4
    // (verify via REX_CPTRACE: draw packets after the init ring). Bounded to a sane forward range.
    if (getenv("REX_DEFERCP")) {
        static uint32_t s_lastEnd = 0;
        uint32_t cur = ctx.r3.u32;
        // The per-frame command buffer is a single GROWING buffer; r3 advances ~0x88000/frame, so this
        // frame's commands are exactly [prev_r3, cur_r3]. Execute that range so the draws reach the CP.
        bool exec = (s_lastEnd && cur > s_lastEnd && (cur - s_lastEnd) < 0x200000u && cur >= 0xA0000000u);
        if (exec) {
            uint64_t before = g_drawCount.load();
            ExecutePM4(s_lastEnd, (cur - s_lastEnd) / 4, 0);
            if (g_ktrace && n <= 16) fprintf(stderr, "[defercp] swap#%u range=0x%X totaldraws=%llu (+%llu this frame)\n",
                n, cur - s_lastEnd, (unsigned long long)g_drawCount.load(),
                (unsigned long long)(g_drawCount.load() - before));
        }
        s_lastEnd = cur;
    }
    // Renderer part 1 (segment-CP, route B, REX_SEGCP=1): the per-frame draws are PM4 IBs ("segments")
    // referenced by 2-dword descriptors {d0 = 0x81000000 | len_dwords, d1 = phys_addr} that the title embeds
    // in the staging stream (and the kick sub_821C6600 turns into ring IBs on the working path — verified
    // from the 6 init kicks: d0=8100000B/d1=00090040 -> IB->0xA0090040 len=11, etc.). Rather than linearly
    // execute the staging stream (which desyncs on inline vertex data + markers), SCAN this frame's range for
    // descriptors and execute each referenced segment as a bounded IB, so the parse stays aligned (segments
    // are clean PM4, like the init IBs).
    if (getenv("REX_SEGCP")) {
        static uint32_t s_segLast = 0;
        uint32_t cur = ctx.r3.u32;
        if (s_segLast && cur > s_segLast && (cur - s_segLast) < 0x200000u && cur >= 0xA0000000u) {
            // One-time brute scan: how many REAL DRAW_INDX_2/INDX packets exist in the whole staging range,
            // regardless of segment alignment? Tells us whether the textured bulk is inline in this buffer
            // (need to reach it) or in separate segments referenced elsewhere (device+13568 array).
            static std::atomic<bool> scanned{false}; bool se = false;
            if (g_ktrace && scanned.compare_exchange_strong(se, true)) {
                int draws = 0, shown = 0;
                for (uint32_t a = s_segLast; a + 8 <= cur; a += 4) {
                    uint32_t pk = GLD32(a); if ((pk >> 30) != 3) continue;
                    uint32_t op = (pk >> 8) & 0x7F; if (op != 0x36 && op != 0x22) continue;
                    uint32_t init = GLD32(a + 4), prim = init & 0x3F, ni = init >> 16;
                    if (prim < 1 || prim > 0x15 || ni < 1 || ni > 0x4000) continue;   // sane draw
                    draws++;
                    if (shown < 16) { fprintf(stderr, "[drawscan] @0x%X init=0x%X numInd=%u prim=%u\n", a, init, ni, prim); shown++; }
                }
                fprintf(stderr, "[drawscan] total plausible draws in staging range 0x%X = %d\n", cur - s_segLast, draws);
            }
            uint64_t before = g_drawCount.load();
            int segs = 0;
            for (uint32_t a = s_segLast; a + 8 <= cur && segs < 4000; a += 4) {
                uint32_t d0 = GLD32(a);
                if ((d0 & 0xFFFF0000u) != 0x81000000u) continue;     // descriptor tag 0x8100_00LL
                uint32_t len = d0 & 0xFFFF, d1 = GLD32(a + 4);
                if (len == 0 || len > 0x4000u) continue;             // sane length (dwords)
                if (d1 < 0x10000u || d1 >= 0x20000000u) continue;    // sane physical addr (0xA-window)
                uint32_t seg = 0xA0000000u | (d1 & 0x1FFFFFFFu);
                uint32_t ht = GLD32(seg) >> 30;                      // first packet must look like PM4
                if (ht == 0 || ht == 1 || ht == 3) {
                    ExecutePM4(seg, len, 1);                         // depth 1 = an IB body (skips cpdump)
                    segs++; a += 4;                                  // consume the descriptor's 2nd dword
                }
            }
            if (g_ktrace && n <= 24) fprintf(stderr, "[segcp] swap#%u range=0x%X segs=%d totaldraws=%llu (+%llu)\n",
                n, cur - s_segLast, segs, (unsigned long long)g_drawCount.load(),
                (unsigned long long)(g_drawCount.load() - before));
        }
        s_segLast = cur;
    }
    // cont.16 (REX_FINDCB): locate the deferred render program's producer-callback records {0001057C, 0x821CC7A0,
    // ctx} (c11) — in the staging range vs the device+13568 chunk — so route-B direct-invoke knows where to scan.
    if (getenv("REX_FINDCB")) {
        static uint32_t s_fcbLast = 0;
        uint32_t cur = ctx.r3.u32;
        if (s_fcbLast && cur > s_fcbLast && (cur - s_fcbLast) < 0x200000u && cur >= 0xA0000000u) {
            static std::atomic<bool> done{false}; bool e = false;
            if (done.compare_exchange_strong(e, true)) {
                int fStg = 0, fChk = 0;
                for (uint32_t a = s_fcbLast; a + 12 <= cur; a += 4)
                    if (GLD32(a) == 0x0001057Cu && GLD32(a + 4) == 0x821CC7A0u) {
                        if (fStg < 8) fprintf(stderr, "[findcb] STAGING @0x%X ctx=0x%08X\n", a, GLD32(a + 8));
                        fStg++;
                    }
                uint32_t dev = g_device.load(), cb = dev ? GLD32(dev + 13568) : 0;
                if (cb >= 0xA0000000u) {
                    uint32_t lo = cb > 0x180000u ? cb - 0x180000u : 0xA0000000u;
                    for (uint32_t a = lo; a + 12 <= cb + 0x20000u; a += 4)
                        if (GLD32(a) == 0x0001057Cu && GLD32(a + 4) == 0x821CC7A0u) {
                            if (fChk < 8) fprintf(stderr, "[findcb] CHUNK @0x%X ctx=0x%08X\n", a, GLD32(a + 8));
                            fChk++;
                        }
                }
                fprintf(stderr, "[findcb] records: staging=%d chunk=%d (chunkbase=0x%08X)\n", fStg, fChk, cb);
            }
        }
        s_fcbLast = cur;
    }
    // cont.16 (REX_INVOKECB): route-B direct-invoke — call the producer sub_821CC7A0(ctx) for each callback
    // record {0001057C,821CC7A0,ctx} in this frame's staging range, bypassing the unpopulated/aliased completion
    // object B. The producer enqueues ctx -> KeSetEvents the consumer sub_821CC310 (tid=10) -> issues the draw.
    if (getenv("REX_INVOKECB")) {
        static uint32_t s_icbLast = 0;
        uint32_t cur = ctx.r3.u32;
        if (s_icbLast && cur > s_icbLast && (cur - s_icbLast) < 0x200000u && cur >= 0xA0000000u) {
            int inv = 0;
            for (uint32_t a = s_icbLast; a + 12 <= cur && inv < 64; a += 4) {
                if (GLD32(a) != 0x0001057Cu || GLD32(a + 4) != 0x821CC7A0u) continue;
                PPCContext c{};
                c.fpscr.csr = 0x1F80;
                c.r1.u64 = ctx.r1.u64 - 0x800;   // borrow current guest stack below the VdSwap frame
                c.r13.u64 = ctx.r13.u64;
                c.r3.u64 = GLD32(a + 8);          // ctx = the record's per-callback context
                CallGuest(0x821CC7A0u, c);
                inv++;
            }
            if (g_ktrace && inv) fprintf(stderr, "[invokecb] invoked %d producer callbacks (range 0x%X)\n", inv, cur - s_icbLast);
        }
        s_icbLast = cur;
    }
    // Task #2 / piece 3b (REX_EXECSEGS): EXECUTE the deferred device+13568 DIRECTORY segments through the CP.
    // The corrected census (REX_CHUNKDUMP) PROVED these segments hold the full menu content — textured DRAW_INDX,
    // ~2000 ALU constants, 186 fetch constants, viewport/blend/surface state, EDRAM resolves, VIZ queries — that
    // variant A's CP never executes (only the 3 setup clears are ever kicked). Unlike REX_CHUNKCP (which wrongly
    // runs the directory itself as inline PM4) and REX_SEGCP (which scans the r3 STAGING range), this follows the
    // device+13568 directory the census localized content to: scan it with the census's AUTHORITATIVE parse
    // (top byte 0x81 = descriptor, low 24 bits = segment length in dwords; next dword = phys addr), resolve
    // guest=0xA0000000|(phys&0x1FFFFFFF), and ExecutePM4 each segment. Executing them fires the REAL
    // EVENT_WRITE_SHD fences / PM4_INTERRUPT callbacks / resolves the content stream carries — the "real GPU
    // result" cont.22 found fence-forward FAKING could not reproduce. THE A<->B TEST: does running the real
    // content stream (vs faking its completion) let the resource-loader/transition proceed — populate the vfetch
    // pool at slot-0 (0xA2000000) and open the kick-gate on later frames? Observe slot0/draws/swaps across frames.
    // Gated REX_EXECSEGS=N (fire at swap>=N, default 3); default boot UNREGRESSED. DRAW_INDX is still a no-op
    // count (T2b wires vkCmdDraw next) — this increment proves execution + measures the downstream A<->B effect.
    if (getenv("REX_EXECSEGS") && g_device.load()) {
        static const uint32_t s_esAt = []{ const char* e=getenv("REX_EXECSEGS"); uint32_t v=e?(uint32_t)atoi(e):0; return v>1?v:3u; }();
        uint32_t dv = g_device.load(), base = GLD32(dv+13568), wptr = GLD32(dv+13572);
        bool valid = base >= 0xA0000000u && wptr > base && (wptr - base) < 0x100000u;
        if (valid && n >= s_esAt) {
            uint32_t slot0Before = GLD32(0xA2000000u);
            uint64_t drBefore = g_drawCount.load();
            int descsFound = 0, segs = 0;
            tl_execsegs = true;   // T2b-step-1: let DRAW_INDX capture each executed draw's live fetch slot-0
            tl_esVerts.clear();   // T2b-step-2: fresh per-frame geometry accumulator
            tl_esTexVerts.clear();// Task #8: fresh per-frame textured-backdrop accumulator
            tl_s1cursor = 0;      // REX_SPRITECARVE: rescan the slot-1 dense start each frame
            // REX_UITEXT: take this frame's accumulated text snapshots (drain the guest-thread buffer); the
            // prim-13 DRAW handler consumes them by matching count, applying the live reg-0x4000 transform.
            std::vector<TxtSnap> txtFrame;
            if (getenv("REX_UITEXT")) { std::lock_guard<std::mutex> lk(g_txtMtx); txtFrame = g_txtSnaps; }  // COPY (persist)
            for (auto& sn : txtFrame) sn.used = false;                                                       // re-match each frame
            tl_txtFrame = txtFrame.empty() ? nullptr : &txtFrame;
            for (uint32_t a = base; a + 8 <= wptr; a += 4) {
                uint32_t d = GLD32(a);
                if ((d >> 24) != 0x81u) continue;                       // census parse: 0x81LLLLLL descriptor tag
                uint32_t len = d & 0xFFFFFFu, addr = GLD32(a + 4);
                if (len == 0 || len >= 0x8000u || (addr & 3)) continue;  // sane length (dwords) + aligned addr
                descsFound++;
                uint32_t guest = 0xA0000000u | (addr & 0x1FFFFFFFu);
                if ((GLD32(guest) >> 30) == 2u) continue;               // first packet type-2 (nop) => not a segment
                ExecutePM4(guest, len, 1);                              // depth 1 = IB body (skips the cpdump path)
                segs++;
            }
            tl_execsegs = false;
            tl_txtFrame = nullptr;
            { static std::atomic<int> tf{0}; if (getenv("REX_UITEXT") && tf.fetch_add(1) < 4)
                fprintf(stderr, "[uitext] frame: %zu text snapshots available, esVerts now %zu\n", txtFrame.size(), tl_esVerts.size()/2); }
            // EXPERIMENT (REX_S1RENDER): render slot-1's LARGEST dense vert region as a quad-list — cont.22 found
            // it holds clean UI quads. Ignores the (missing) per-draw mapping; draws every quad in the main UI
            // vert buffer. authoring(~1768x1043)→clip (x/884-1, y/521.5-1). A shot at visible UI without the
            // resource-create build. Likely imperfect (the buffer may mix prims/strides) but decisive.
            if (getenv("REX_S1RENDER") && descsFound >= 10) {
                uint32_t s1=0xA01FE0FCu, bStart=0, bLen=0, rStart=0, rLen=0;
                for (uint32_t o=0;o<0x200000;o+=8){ uint32_t ux=GLD32(s1+o),uy=GLD32(s1+o+4); float fx,fy; memcpy(&fx,&ux,4);memcpy(&fy,&uy,4);
                    bool v=fx>-16.f&&fx<8192.f&&fy>-16.f&&fy<8192.f&&(ux||uy);
                    if(v){if(!rLen)rStart=o;rLen++;} else {if(rLen>bLen){bLen=rLen;bStart=rStart;}rLen=0;} }
                auto cx=[](float v){return v/884.0f-1.0f;}; auto cy=[](float v){return v/521.5f-1.0f;};
                for(uint32_t i=0;i+4<=bLen;i+=4){ float vv[8]; for(int j=0;j<8;j++){uint32_t u=GLD32(s1+bStart+(i*2+j)*4); memcpy(&vv[j],&u,4);}
                    float q[12]={cx(vv[0]),cy(vv[1]),cx(vv[2]),cy(vv[3]),cx(vv[4]),cy(vv[5]), cx(vv[0]),cy(vv[1]),cx(vv[4]),cy(vv[5]),cx(vv[6]),cy(vv[7])};
                    if(tl_esVerts.size()<60000) tl_esVerts.insert(tl_esVerts.end(),q,q+12); }
            }
            // T2b-step-2: hand this frame's carved content geometry to the render thread (drawn by PresentOnce
            // under REX_MENUTEST via the menu-quad float2 pipeline). Gated by rex_render::Enabled (REX_RENDER).
            if (rex_render::Enabled() && !tl_esVerts.empty())
                rex_render::SubmitMenuGeometry(tl_esVerts.data(), (int)(tl_esVerts.size() / 2));
            // Task #8: hand the textured backdrop (pos.xy+uv.xy quadrants) to the textured pipeline.
            if (rex_render::Enabled() && !tl_esTexVerts.empty())
                rex_render::SubmitTexturedGeometry(tl_esTexVerts.data(), (int)(tl_esTexVerts.size() / 4));
            uint32_t slot0After = GLD32(0xA2000000u);
            // BUILD step (REX_S1SCAN, one-shot): map the slot-1 (0xA01FE0FC) vert layout — the title generated the
            // sprite/text verts here, but the draws read the (empty) slot 0. Find every dense run of plausible
            // authoring-coord float2 verts (offset, count, first vert). The draw→region mapping can then be done
            // by vertex COUNT (a prim-13 numI=252 draw <-> a 252-vert region), the data the missing fetch constant
            // would carry. This is the path to rendering the sprite/text without the full resource-create build.
            if (getenv("REX_S1SCAN") && descsFound >= 10) { static std::atomic<bool> sd{false}; bool e=false;
              if (sd.compare_exchange_strong(e,true)) {
                uint32_t s1 = 0xA01FE0FCu; int nReg=0; uint32_t runStart=0, runLen=0;
                fprintf(stderr, "[s1scan] (rich frame swap#%u descs=%d) slot-1 dense vert regions (off: count v0):\n", n, descsFound);
                for (uint32_t o=0; o<0x200000 && nReg<60; o+=8) {
                    uint32_t ux=GLD32(s1+o), uy=GLD32(s1+o+4); float fx,fy; memcpy(&fx,&ux,4); memcpy(&fy,&uy,4);
                    bool valid = fx>-16.f && fx<8192.f && fy>-16.f && fy<8192.f && (ux||uy);
                    if (valid) { if(!runLen) runStart=o; runLen++; }
                    else { if (runLen>=4) { uint32_t a=GLD32(s1+runStart),b=GLD32(s1+runStart+4); float v0x,v0y;
                              memcpy(&v0x,&a,4); memcpy(&v0y,&b,4);
                              fprintf(stderr,"  +0x%05X: %u verts v0=(%.0f,%.0f)\n", runStart, runLen, v0x,v0y); nReg++; }
                           runLen=0; }
                }
                fprintf(stderr, "[s1scan] %d regions\n", nReg);
              }
            }
            // Read the LIVE fetch slot-0 constant the executed draws actually set (reg file 0x7FC80000+0x4800*4),
            // resolve it (NOT a hardcoded 0xA2000000), and gauge BOTH it and 0xA2000000 for real screen-coord
            // floats. Guards the "maybe the verts live where the live fetch points, not 0xA2000000" doubt: if the
            // live pool has real floats, the verts DO exist (placement bug); if both are empty, the verts truly
            // are never written. The loader should fill the pool with real verts once it proceeds (head stayed
            // 0xFFFFFFFF/0 filler in all prior runs).
            uint32_t fc0 = GLD32(0x7FC80000u + 0x4800u*4u), fcBase = fc0 & 0xFFFFFFFCu;
            uint32_t fcGuest = 0xA0000000u | (fcBase & 0x1FFFFFFFu);
            auto realFloats = [&](uint32_t g){ int r=0; for(int i=0;i<64;i++){ uint32_t u=GLD32(g+i*4); float f; memcpy(&f,&u,4);
                if(u && u!=0xFFFFFFFFu && f>-4096.f && f<4096.f) r++; } return r; };
            int s0real = realFloats(0xA2000000u), fcReal = realFloats(fcGuest);
            // One-shot RAW dump of the live-fetch pool: confirm the 46/64 "real floats" are STRUCTURED vertex
            // coords (screen 0..1280 / clip -1..1), not a coincidence of the weak [-4096,4096] filter.
            static std::atomic<int> rawDumped{0};
            if (fcReal > 8 && rawDumped.fetch_add(1) < 6) {
                fprintf(stderr, "[execsegs] RAW live-fetch pool @0x%X (fc0=%08X type=%u) first 24 floats:\n  ", fcGuest, fc0, fc0&3);
                for (int i=0;i<24;i++){ uint32_t u=GLD32(fcGuest+i*4); float f; memcpy(&f,&u,4); fprintf(stderr,"%.3g ", f); }
                fprintf(stderr, "\n");
            }
            // Task #4: track the RICHEST frame (most descriptors / most draws) across the whole run — catches
            // rich UI frames even past the per-frame log cap. The census found ~107 draws in a rich frame; if
            // execsegs never sees descs>3 / draws>8, the rich UI is NOT in device+13568 at VdSwap time.
            { static std::atomic<int> maxD{0}; int pm = maxD.load(); uint64_t dd = g_drawCount.load()-drBefore;
              if (descsFound > pm && maxD.compare_exchange_strong(pm, descsFound))
                  fprintf(stderr, "[execsegs] NEW-MAX descs=%d segs=%d draws+%llu (swap#%u)\n", descsFound, segs, (unsigned long long)dd, n); }
            static std::atomic<int> logged{0};
            if (g_ktrace && logged.fetch_add(1) < 60)
                fprintf(stderr, "[execsegs] swap#%u descs=%d segs=%d draws+%llu slot0(A2000000)Head %08X->%08X real=%d | liveFetch0=%08X->0x%X real=%d | swaps=%llu drawTotal=%llu\n",
                        n, descsFound, segs, (unsigned long long)(g_drawCount.load()-drBefore),
                        slot0Before, slot0After, s0real, fc0, fcGuest, fcReal,
                        (unsigned long long)g_swapCount.load(), (unsigned long long)g_drawCount.load());
        }
    }
    // Renderer part 1 (chunk-CP, route B v2, REX_CHUNKCP=1): the REAL command stream is NOT the r3 staging
    // buffer (0xA01xxxxx) but a pool of command-buffer chunks at 0xA04D-0xA056xxxx tracked in the device
    // struct (records {base, writeptr, 0x1080, base+4, 0, device}). device+13568=current-chunk base,
    // +13572=writeptr. Execute [base, writeptr) of the active chunk as an IB — this is where the textured
    // draws live (0xA0562200, with op 0x22 draws, sits inside the active chunk [A055BF00,A0564608)).
    if (getenv("REX_CHUNKCP")) {
        uint32_t dv = g_device.load();
        if (dv) {
            uint32_t base = GLD32(dv + 13568), wptr = GLD32(dv + 13572);
            if (base >= 0xA0000000u && wptr > base && (wptr - base) < 0x100000u) {
                uint64_t before = g_drawCount.load();
                ExecutePM4(base, (wptr - base) / 4, 1);
                if (g_ktrace && n <= 24)
                    fprintf(stderr, "[chunkcp] swap#%u base=0x%X len=0x%X totaldraws=%llu (+%llu)\n",
                        n, base, wptr - base, (unsigned long long)g_drawCount.load(),
                        (unsigned long long)(g_drawCount.load() - before));
            }
        }
    }
    // PIVOTAL EXPERIMENT (REX_CHUNKDUMP): does a RACE-FREE run build TEXTURED draws? The device+13568 chunk is
    // NOT inline PM4 — it's the title's SEGMENT DIRECTORY: records + segment descriptors {0x81LLLLLL, phys_addr}
    // (LLLLLL = segment length in dwords) that point at the actual PM4 segments scattered across the cmd-buffer
    // pool. (Brute-scanning the directory for op-0x22 headers gives FALSE POSITIVES — the 0xC134xxxx record
    // words decode as op 0x22.) So FOLLOW the descriptors: resolve guest = 0xA0000000 | (addr & 0x1FFFFFFF),
    // parse each segment as PM4, and count REAL draws (init != 0x30088 degenerate rect), rect draws,
    // SET_CONSTANT, and texture-fetch constants (type-1 FETCH payload with a 0xA-range base). Report
    // g_nonBenignInd (0 = race-free). Run several times: clean+realDraws/tex => the race is the whole gate;
    // clean+rects-only across all segments => the divergence is deeper than the race.
    if (getenv("REX_CHUNKDUMP") && g_device.load()) {
        uint32_t dv = g_device.load(), base = GLD32(dv+13568), wptr = GLD32(dv+13572);
        bool valid = base >= 0xA0000000u && wptr > base && (wptr - base) < 0x100000u;
        struct Desc { uint32_t off, len, addr; }; Desc descs[128]; int nDesc = 0;
        if (valid) for (uint32_t a = base; a + 8 <= wptr; a += 4) {
            uint32_t d = GLD32(a);
            if ((d >> 24) == 0x81u) { uint32_t len = d & 0xFFFFFF, addr = GLD32(a+4);
                if (len > 0 && len < 0x8000 && (addr & 3) == 0 && nDesc < 128) descs[nDesc++] = { a-base, len, addr }; }
        }
        static std::atomic<int> lastN{-1};
        if (valid && nDesc > 0 && lastN.exchange(n) != (int)n)
            fprintf(stderr, "[chunkscan] swap#%u segDescs=%d nonBenignInd=%d %s\n",
                    n, nDesc, g_nonBenignInd.load(), g_nonBenignInd.load()==0?"CLEAN":"RACED");
        // Trigger swap# is configurable via REX_CHUNKDUMP=N (N>1 => fire at n>=N; default 4000). This lets the
        // content census run at the most-advanced NOTOKEN+MOVIE_EOF full-menu state, where n (=swap count) stays
        // small (~6-13) — the cont.10/12 runs used n>=4000 which that state never reaches. Re-fire whenever a
        // RICHER frame (more segment descriptors) appears, so a run that race-crashes early still yields the
        // best census it reached, and a surviving run captures the full ~11-segment menu frame.
        static std::atomic<int> maxDumped{0};
        static const uint32_t s_cdAt = []{ const char* e = getenv("REX_CHUNKDUMP"); uint32_t v = e ? (uint32_t)atoi(e) : 0; return v > 1 ? v : 4000u; }();
        int prevMax = maxDumped.load();
        if (valid && nDesc >= 3 && n >= s_cdAt && nDesc > prevMax && maxDumped.compare_exchange_strong(prevMax, nDesc)) {   // richer frame
            int totReal=0, totTex=0, totRect=0;
            // step (a) result-gate totals: the "real GPU result" the A<->B gate may need. WAIT_REG_MEM
            // (GPU->CPU poll), ZPASS_DONE/VIZ_QUERY (occlusion), REG_TO_MEM/COND_WRITE (readback),
            // RB_COPY resolve (EDRAM->texture). Captures the first WAIT_REG_MEM poll target seen.
            int totWait=0, totZp=0, totViz=0, totRb=0, totRes=0; uint32_t fwPoll=0, fwRef=0, fwInfo=0;
            fprintf(stderr, "[chunkdump] swap#%u base=0x%X len=0x%X %s(ind=%d) segDescs=%d — FOLLOWING:\n",
                    n, base, wptr-base, g_nonBenignInd.load()==0?"CLEAN":"RACED", g_nonBenignInd.load(), nDesc);
            // Piece-3b increment 1: a frame-level register shadow (regs 0x2000-0x233F, the RB/PA/SQ state +
            // RB_COPY block) updated as we walk the segments IN DIRECTORY ORDER, so at each content DRAW we can
            // dump the pipeline state it binds (RT/viewport/scissor/blend/shader/mode) — exactly what the
            // DRAW_INDX->Vulkan translator must turn into a VkPipeline. State carries across segments within a
            // frame (the title sets state once, draws many times). Pure read/track — NO execution, NO side
            // effects (unlike running ExecutePM4 on the segments, which would fire interrupts/fences).
            uint32_t st[0x340] = {0}, fetch[0x180] = {0}; int drawDumps = 0;   // fetch[] = vertex/texture fetch consts 0x4800-0x497F
            uint32_t menuPoolBase = 0;   // Layer 2: the content draws' kVertex pool base, captured during the walk
            int opHist[128] = {0};       // type-3 opcode histogram (find constant-load packets the shadow misses)
            uint32_t loadOps[6] = {0};   // first few LOAD_ALU_CONSTANT (0x2F) raw d0/d1 to decode its dest reg
            auto stset = [&](uint32_t r, uint32_t v){ if (r >= 0x2000 && r < 0x2340) st[r-0x2000] = v;
                                                      else if (r >= 0x4800 && r < 0x4980) fetch[r-0x4800] = v; };
            auto stget = [&](uint32_t r){ return (r >= 0x2000 && r < 0x2340) ? st[r-0x2000] : (r >= 0x4800 && r < 0x4980 ? fetch[r-0x4800] : 0u); };
            for (int di = 0; di < nDesc && di < 24; di++) {
                uint32_t len = descs[di].len, addr = descs[di].addr, guest = 0xA0000000u | (addr & 0x1FFFFFFFu);
                int dReal=0, dRect=0, setc=0, tex=0, dWait=0, dZp=0, dViz=0, dRb=0, dRes=0;
                uint32_t firstReal=0; bool desync=false;   // firstReal = init of first non-clear DRAW; desync = not clean PM4
                // CORRECT PM4 walker (mirrors ExecutePM4): advance type-0 by its reg count, type-1 by 2,
                // type-2 by 0, type-3 by its data count. The old census only advanced type-3 by count and
                // every other packet by 1 dword, which MISALIGNED after any SET_CONSTANT/reg-write and both
                // HID real content past dword 1024 and FABRICATED false type-3 "draws" (cont.10's warning).
                uint32_t a = guest, end = guest + len*4;
                bool mapped = guest >= 0xA0000000u;
                for (int g = 0; mapped && a + 4 <= end && g < 100000; g++) {
                    uint32_t pkt = GLD32(a); a += 4; uint32_t t = pkt >> 30;
                    if (t == 0) {                                              // type-0: write cnt regs from baseIdx
                        uint32_t cnt=((pkt>>16)&0x3FFF)+1, bi=pkt&0x7FFF; bool one=(pkt>>15)&1;
                        if (!one && bi>=0x4800 && bi<0x4900 && cnt>=6) tex++;   // FETCH constants (textures/vertex streams) via type-0
                        for (uint32_t m=0;m<cnt&&a+4<=end;m++,a+=4){ uint32_t r=one?bi:bi+m; stset(r, GLD32(a));
                            if (r>=0x2318&&r<=0x2325) dRes++; else if (r==0x2293) dViz++; }
                    } else if (t == 1) { a += 8; }                             // type-1: 2 reg writes
                    else if (t == 2) { /* no-op */ }                          // type-2
                    else {                                                     // type-3: opcode packet
                        uint32_t op=(pkt>>8)&0x7F, cnt=((pkt>>16)&0x3FFF)+1;
                        if (a + cnt*4 > end) { desync=true; break; }           // count overruns segment => not clean PM4
                        if (op<128) opHist[op]++;
                        if (op==0x2F && !loadOps[0]) { loadOps[0]=GLD32(a); loadOps[1]=GLD32(a+4); loadOps[2]=GLD32(a+8); loadOps[3]=cnt; }   // LOAD_ALU_CONSTANT
                        if (op==0x22||op==0x36){ uint32_t init=(op==0x22)?GLD32(a+4):GLD32(a);   // 0x22: initiator is data[1]
                            bool clear=(init==0x30088u); if(clear)dRect++; else {dReal++; if(!firstReal)firstReal=init?init:0xFFFFFFFFu;}
                            // Piece-3b increment 1: dump the pipeline state each non-clear (content) draw binds.
                            if (!clear && drawDumps < 14) { ++drawDumps;
                                auto rf=[&](uint32_t r){ uint32_t u=stget(r); float f; memcpy(&f,&u,4); return f; };
                                uint32_t surf=stget(0x2000), col=stget(0x2001);
                                fprintf(stderr, "  [drawstate] seg#%d #%d op=0x%X prim=%u numI=%u | RT pitch=%u msaa=%u fmt=%u base=0x%X | "
                                        "vp %.0fx%.0f off(%.0f,%.0f) | scis(%u,%u)-(%u,%u) | blend0=0x%X colCtl=0x%X progCntl=0x%X depthCtl=0x%X mode=0x%X\n",
                                        di, drawDumps, op, init&0x3F, init>>16,
                                        (surf&0x3FFF),(surf>>14)&3,(col>>16)&0xF,(col&0xFFF),
                                        2.0f*rf(0x210F),2.0f*rf(0x2111), rf(0x2110), rf(0x2112),
                                        stget(0x2081)&0x7FFF,(stget(0x2081)>>16)&0x7FFF, stget(0x2082)&0x7FFF,(stget(0x2082)>>16)&0x7FFF,
                                        stget(0x2201), stget(0x2202), stget(0x2180), stget(0x2200), stget(0x2208));
                                // Piece-3b increment 2: decode the geometry this draw consumes. src_select (init[7:6]):
                                // 0=kDMA (indexed; data[2]=VGT_DMA_BASE index buffer, data[3]=VGT_DMA_SIZE.num_words),
                                // 2=kAutoIndex (sequential, no index buffer). idx fmt = init[11] (0=int16,1=int32).
                                uint32_t src=(init>>6)&3;
                                char g[256]; size_t go=0; g[0]=0;
                                if (op==0x22 && src==0){ uint32_t ib=GLD32(a+8), sz=GLD32(a+12)&0xFFFFFF;
                                    go+=snprintf(g+go,sizeof(g)-go,"idx %s base=0x%X count=%u",(init>>11)&1?"u32":"u16",ib,sz); }
                                else go+=snprintf(g+go,sizeof(g)-go,"%s",src==2?"auto-index":src==1?"immediate":"?");
                                // vertex streams: fetch constants (2-dword slots @0x4800), type[1:0]==3 = kVertex.
                                int nvb=0; uint32_t firstVb=0; for(uint32_t s=0;s<48&&nvb<3;s++){ uint32_t d0=stget(0x4800+s*2); if((d0&3)!=3)continue;
                                    uint32_t d1=stget(0x4800+s*2+1), vb=d0&0xFFFFFFFCu, vw=(d1>>2)&0xFFFFFF;
                                    if(!vb||!vw)continue; if(!firstVb)firstVb=vb; go+=snprintf(g+go,sizeof(g)-go," | vf%u@0x%X(%uB)",s,vb,vw*4); nvb++; }
                                if (firstVb && !menuPoolBase) menuPoolBase = firstVb;   // Layer 2: remember the pool base
                                // The VS vfetch reads fetch SLOT 0 (RE'd) — probe it per-draw: its base + first vertex.
                                { uint32_t s0=stget(0x4800), s0b=s0&0xFFFFFFFCu, sg=0xA0000000u|(s0b&0x1FFFFFFFu);
                                  float x,y; uint32_t wx=GLD32(sg),wy=GLD32(sg+4); memcpy(&x,&wx,4); memcpy(&y,&wy,4);
                                  go+=snprintf(g+go,sizeof(g)-go," | SLOT0=%08X ty=%u @0x%X v0=(%.1f,%.1f)",s0,s0&3,s0b,x,y); }
                                fprintf(stderr, "  [drawgeo] seg#%d #%d %s\n", di, drawDumps, g);
                                // Piece-3b increment 3: one-shot dump to RE the vertex format. Show ALL fetch slots (raw),
                                // then locate the actual vertex data (scan candidate base translations for non-zero).
                                static std::atomic<bool> vdumped{false}; bool vde=false;
                                if (firstVb && vdumped.compare_exchange_strong(vde,true)) {
                                    fprintf(stderr, "[vtxdump] draw op=0x%X prim=%u numI=%u — fetch slots (d0 d1 -> type/byteaddr/words):\n", op, init&0x3F, init>>16);
                                    for(uint32_t s=0;s<24;s++){ uint32_t d0=stget(0x4800+s*2),d1=stget(0x4800+s*2+1);
                                        if(!d0&&!d1)continue; fprintf(stderr,"  slot%u: %08X %08X  type=%u addr=0x%X words=%u\n",s,d0,d1,d0&3,d0&0xFFFFFFFCu,(d1>>2)&0xFFFFFF); }
                                    // The vfetch reads SLOT 0 (RE'd). Scan slot-0's region for data (head may be zero like slot 1).
                                    { uint32_t s0b=stget(0x4800)&0xFFFFFFFCu, sg=0xA0000000u|(s0b&0x1FFFFFFFu);
                                      uint32_t hit=0, nz=0; for(uint32_t o=0;o<0x800000;o+=4){ if(GLD32(sg+o)){ nz++; if(!hit)hit=o; } }
                                      fprintf(stderr,"[vtxdump] SLOT0 guest=0x%X: nonzero-dwords=%u firstNonZero=+0x%X — float dump:\n", sg, nz, hit);
                                      for(int r=0;r<6;r++){ char ln[256]; size_t lo=snprintf(ln,sizeof(ln),"  +0x%X:",hit+r*16);
                                        for(int c=0;c<4;c++){ uint32_t w=GLD32(sg+hit+r*16+c*4); float f; memcpy(&f,&w,4); lo+=snprintf(ln+lo,sizeof(ln)-lo," %08X(%g)",w,f); }
                                        fprintf(stderr,"%s\n",ln); } }
                                    // Crack the vertex FORMAT: scan the pool (physical window) for a DENSE region — a
                                    // 16-dword window where >=6 dwords are floats in screen-coord range [1,1280] — = a
                                    // packed vertex array. Dump 32 dwords there so the repeating stride is visible.
                                    uint32_t bvg = 0xA0000000u|(firstVb&0x1FFFFFFFu); uint32_t dense=0;
                                    auto isScreen=[&](uint32_t w){ float f; memcpy(&f,&w,4); return f>=1.0f && f<=1280.0f; };
                                    for(uint32_t o=0;o<0xC00000 && !dense;o+=4){ int hits=0; for(int k=0;k<16;k++) if(isScreen(GLD32(bvg+o+k*4))) hits++;
                                        if(hits>=6) dense=o; }
                                    fprintf(stderr,"[vtxdump] base 0x%X: dense screen-coord vertex region @+0x%X — 32 dwords (offset:hex|float):\n", bvg, dense);
                                    for(int r=0;r<8;r++){ char ln[300]; size_t lo=0; uint32_t b=bvg+dense+r*16;
                                        lo+=snprintf(ln+lo,sizeof(ln)-lo,"   +0x%X:",dense+r*16);
                                        for(int c=0;c<4;c++){uint32_t w=GLD32(b+c*4);lo+=snprintf(ln+lo,sizeof(ln)-lo," %08X",w);}
                                        lo+=snprintf(ln+lo,sizeof(ln)-lo,"  |");
                                        for(int c=0;c<4;c++){uint32_t w=GLD32(b+c*4);float f;memcpy(&f,&w,4);lo+=snprintf(ln+lo,sizeof(ln)-lo," %g",f);}
                                        fprintf(stderr,"%s\n",ln); }
                                } } }
                        else if (op==0x2D){ setc++; uint32_t ot=GLD32(a); uint32_t ty=(ot>>16)&0xFF, ix=ot&0x7FF;
                            if (ty==1){ bool t1=false; for(uint32_t j=1;j<cnt&&a+j*4+4<=end;j++){ stset(0x4800+ix+(j-1), GLD32(a+j*4));
                                            if(!t1 && (((GLD32(a+j*4)>>12)&0xFFFFF)<<12)>=0xA0000000u){tex++;t1=true;} } }
                            if (ty==4){ for(uint32_t j=1;j<cnt&&a+j*4+4<=end;j++) stset(0x2000+ix+(j-1), GLD32(a+j*4));
                                        if(ix>=0x318&&ix<=0x325) dRes++; if(ix==0x293) dViz++; } }
                        else if (op==0x55) setc++;
                        else if (op==0x3C){ dWait++; if(!fwPoll){fwInfo=GLD32(a);fwPoll=GLD32(a+4);fwRef=GLD32(a+8);} }
                        else if (op==0x52||op==0x53) dWait++;                  // WAIT_REG_EQ / GTE
                        else if (op==0x5B) dZp++;                              // EVENT_WRITE_ZPD
                        else if (op==0x46){ uint32_t ev=GLD32(a)&0x3F; if(ev==21)dZp++; else if(ev==7||ev==8)dViz++; }
                        else if (op==0x23) dViz++;                             // VIZ_QUERY
                        else if (op==0x3E||op==0x45) dRb++;                    // REG_TO_MEM / COND_WRITE
                        else if (op==0x2B) {                                   // IM_LOAD_IMMEDIATE: embedded shader microcode
                            // Decode the VS vertex format: scan the microcode for vfetch instructions (3 dwords;
                            // d0[4:0]=opcode kVertexFetch=0 + d0[19]=must_be_one; d1[21:16]=VertexFormat;
                            // d2[7:0]=dword stride, d2[30:8]=signed dword offset). Layout per rexglue ucode.h.
                            static std::atomic<int> vfDumped{0};
                            uint32_t shType=GLD32(a), szdw=GLD32(a+4)&0xFFFF;
                            if (shType==0 && szdw>3 && szdw<8192 && vfDumped.fetch_add(1)<40) {   // VS only
                                int kk=vfDumped.load(), found=0;
                                fprintf(stderr,"[vfetch] VS #%d size=%u dw:\n", kk, szdw);
                                for (uint32_t i=0;i+2<szdw && found<16;i++){
                                    uint32_t d0=GLD32(a+8+i*4), d1=GLD32(a+8+(i+1)*4), d2=GLD32(a+8+(i+2)*4);
                                    if ((d0&0x1F)!=0 || !((d0>>19)&1)) continue;       // kVertexFetch + must_be_one
                                    uint32_t fmt=(d1>>16)&0x3F, stride=d2&0xFF; int32_t off=(d2>>8)&0x7FFFFF; if(off&0x400000)off-=0x800000;
                                    if (fmt!=36&&fmt!=37&&fmt!=38&&fmt!=6&&fmt!=35) continue;   // float/float2/float4/ubyte4/uint4
                                    const char* fn=fmt==36?"float":fmt==37?"float2":fmt==38?"float4":fmt==6?"ubyte4":"uint4";
                                    fprintf(stderr,"  vfetch%s slot=%u(ci=%u sel=%u) fmt=%s stride=%udw off=%ddw dst=r%u | raw=%08X %08X %08X\n",
                                            (d1>>30)&1?"_mini":"", ((d0>>20)&0x1F)*3+((d0>>25)&3), (d0>>20)&0x1F,(d0>>25)&3, fn, stride, off, (d0>>12)&0x3F, d0,d1,d2);
                                    found++;
                                }
                                fprintf(stderr,"[vfetch] (%d vfetch in VS #%d)\n", found, kk);
                            }
                        }
                        a += cnt*4;
                    }
                }
                totReal+=dReal; totRect+=dRect; totTex+=tex;
                totWait+=dWait; totZp+=dZp; totViz+=dViz; totRb+=dRb; totRes+=dRes;
                fprintf(stderr, "  desc#%d @+0x%05X {len=0x%X addr=0x%08X->0x%08X}: realDraws=%d(init0=0x%X) rect=%d setConst=%d texFetch=%d | wait=%d zpass=%d viz=%d readback=%d resolve=%d %s  raw0=%08X %08X %08X\n",
                        di, descs[di].off, len, addr, guest, dReal, firstReal, dRect, setc, tex, dWait, dZp, dViz, dRb, dRes, desync?"DESYNC":"clean", GLD32(guest), GLD32(guest+4), GLD32(guest+8));
                // One-shot: dump the full packet sequence of the FIRST clean content segment, to verify it is
                // genuine PM4 (sensible opcodes/draw-inits/copy-regs) and not a lucky clean walk over data.
                static std::atomic<bool> segDumped{false}; bool sde=false;
                if ((dReal>0||dRes>0) && !desync && segDumped.compare_exchange_strong(sde,true)) {
                    fprintf(stderr, "[segdump] desc#%d @0x%08X len=0x%X — full packet walk:\n", di, guest, len);
                    uint32_t a2=guest, e2=guest+len*4; int pk=0;
                    for (int g=0; a2+4<=e2 && g<4000; g++) {
                        uint32_t pkt=GLD32(a2); a2+=4; uint32_t t=pkt>>30;
                        if (t==0){ uint32_t cnt=((pkt>>16)&0x3FFF)+1, bi=pkt&0x7FFF; bool one=(pkt>>15)&1;
                            if (pk++<120) fprintf(stderr,"  [%d] T0 reg=0x%X cnt=%u%s v0=0x%X\n",g,bi,cnt,one?" one":"",GLD32(a2));
                            a2+=cnt*4; }
                        else if (t==1){ if(pk++<120)fprintf(stderr,"  [%d] T1 r=0x%X,0x%X\n",g,pkt&0x7FF,(pkt>>11)&0x7FF); a2+=8; }
                        else if (t==2){ if(pk++<120)fprintf(stderr,"  [%d] T2 nop\n",g); }
                        else { uint32_t op=(pkt>>8)&0x7F, cnt=((pkt>>16)&0x3FFF)+1;
                            if (a2+cnt*4>e2){ fprintf(stderr,"  [%d] T3 op=0x%X cnt=%u OVERRUN\n",g,op,cnt); break; }
                            const char* on = op==0x22?"DRAW_INDX":op==0x36?"DRAW_INDX_2":op==0x2D?"SET_CONSTANT":op==0x55?"SET_CONST2":
                                op==0x3C?"WAIT_REG_MEM":op==0x23?"VIZ_QUERY":op==0x46?"EVENT_WRITE":op==0x5B?"EVENT_ZPD":op==0x10?"NOP":"?";
                            if (pk++<120){ if(op==0x22||op==0x36) fprintf(stderr,"  [%d] T3 %s cnt=%u init=0x%X\n",g,on,cnt,GLD32(a2));
                                else if(op==0x2D){ uint32_t ot=GLD32(a2); fprintf(stderr,"  [%d] T3 %s cnt=%u type=%u idx=0x%X (reg=0x%X)\n",g,on,cnt,(ot>>16)&0xFF,ot&0x7FF,((ot>>16)&0xFF)==4?0x2000+(ot&0x7FF):((ot>>16)&0xFF)==1?0x4800+(ot&0x7FF):(ot&0x7FF)); }
                                else fprintf(stderr,"  [%d] T3 %s cnt=%u d0=0x%X\n",g,on,cnt,GLD32(a2)); }
                            a2+=cnt*4; }
                    }
                    fprintf(stderr,"[segdump] (end; %d packets shown)\n", pk<120?pk:120);
                }
            }
            { char hb[512]; size_t ho=0; ho+=snprintf(hb+ho,sizeof(hb)-ho,"[ophist]");
              for (int op=0; op<128; op++) if (opHist[op]) {
                  const char* nm = op==0x22?"DRAW":op==0x36?"DRAW2":op==0x2D?"SETCONST":op==0x55?"SETCONST2":op==0x56?"SETSHCONST":
                      op==0x2F?"LOAD_ALU":op==0x2E?"LOAD_CTX":op==0x27?"IM_LOAD":op==0x2B?"IM_LOAD_IMM":op==0x3C?"WAITREG":
                      op==0x46?"EVTWRITE":op==0x5B?"EVT_ZPD":op==0x23?"VIZQ":op==0x10?"NOP":op==0x3F?"IB":op==0x54?"INT":"";
                  ho+=snprintf(hb+ho,sizeof(hb)-ho," 0x%02X%s%s=%d", op, *nm?":":"", nm, opHist[op]); }
              fprintf(stderr, "%s\n", hb);
              if (loadOps[0]||loadOps[1]) fprintf(stderr,"[ophist] first LOAD_ALU_CONSTANT(0x2F): d0=0x%X d1=0x%X d2=0x%X cnt=%u\n", loadOps[0],loadOps[1],loadOps[2],loadOps[3]); }
            // Piece-3b Layer 2: extract the menu geometry from the vertex pool + submit to the render thread
            // (auto-fit to clip). The vertex stream is fetch slot 1 (RE'd). Collect contiguous screen-coord
            // float2 verts from the pool's dense region, treat as quad-list (groups of 4 -> 2 tris), map the
            // bbox to clip [-0.9,0.9] (Y-flipped). One-shot, only when the renderer is active. APPROXIMATE
            // (per-draw pool offset still unpinned) — a "does the title's real geometry form a UI" test.
            if (rex_render::Enabled()) {
                static std::atomic<bool> submitted{false}; bool se=false;
                if (menuPoolBase && submitted.compare_exchange_strong(se,true)) {   // pool base from the content draws
                    uint32_t vg = 0xA0000000u | (menuPoolBase & 0x1FFFFFFFu);
                    auto scr=[&](uint32_t w){ float f; memcpy(&f,&w,4); return f>=1.0f && f<=2048.0f; };
                    uint32_t dense=0; for(uint32_t o=0;o<0xC00000 && !dense;o+=4){ int h=0; for(int k=0;k<16;k++) if(scr(GLD32(vg+o+k*4)))h++; if(h>=6)dense=o; }
                    std::vector<float> v; int gap=0;                       // contiguous screen-coord float2 verts
                    for(uint32_t o=dense; o<dense+0x80000 && v.size()<16000; o+=8){
                        uint32_t wx=GLD32(vg+o), wy=GLD32(vg+o+4); float x,y; memcpy(&x,&wx,4); memcpy(&y,&wy,4);
                        bool ok = x>=0&&x<=4096&&y>=0&&y<=4096 && !(x==0&&y==0);
                        if(ok){ v.push_back(x); v.push_back(y); gap=0; } else if(++gap>32 && !v.empty()) break;
                    }
                    if(v.size()>=8){ float minx=1e30f,miny=1e30f,maxx=-1e30f,maxy=-1e30f;
                        for(size_t i=0;i<v.size();i+=2){ if(v[i]<minx)minx=v[i]; if(v[i]>maxx)maxx=v[i]; if(v[i+1]<miny)miny=v[i+1]; if(v[i+1]>maxy)maxy=v[i+1]; }
                        float sx=(maxx>minx)?1.8f/(maxx-minx):0.0f, sy=(maxy>miny)?1.8f/(maxy-miny):0.0f;
                        std::vector<float> tri; size_t nq=(v.size()/2)/4;   // quad-list -> tri-list (0,1,2, 0,2,3)
                        for(size_t q=0;q<nq;q++){ float qx[4],qy[4];
                            for(int k=0;k<4;k++){ qx[k]=(v[(q*4+k)*2]-minx)*sx-0.9f; qy[k]=-((v[(q*4+k)*2+1]-miny)*sy-0.9f); }
                            static const int id6[6]={0,1,2,0,2,3}; for(int t=0;t<6;t++){ tri.push_back(qx[id6[t]]); tri.push_back(qy[id6[t]]); } }
                        fprintf(stderr,"[menugeo] pool=0x%X dense=+0x%X collected %zu verts -> %zu tris bbox=(%.0f,%.0f)-(%.0f,%.0f)\n",
                                vg, dense, v.size()/2, tri.size()/6, minx,miny,maxx,maxy);
                        rex_render::SubmitMenuGeometry(tri.data(), (int)(tri.size()/2));
                    }
                }
            }
            fprintf(stderr, "[chunkdump] TOTALS over %d segments: realDraws=%d rectDraws=%d texFetch=%d  => %s\n",
                    nDesc<24?nDesc:24, totReal, totRect, totTex,
                    (totReal>0||totTex>0) ? "*** TEXTURED/REAL CONTENT PRESENT ***" : "only rects / no textures");
            fprintf(stderr, "[chunkdump] RESULT-GATES: waitRegMem=%d zpass/occlusion=%d vizQuery=%d readback=%d resolve(RB_COPY)=%d  => %s%s\n",
                    totWait, totZp, totViz, totRb, totRes,
                    (totZp||totViz) ? "*** OCCLUSION QUERY PRESENT ***" : "no occlusion query",
                    totRes ? " *** EDRAM RESOLVE PRESENT ***" : (totWait ? " (waits present)" : ""));
            if (fwPoll) fprintf(stderr, "[chunkdump] first WAIT_REG_MEM: info=0x%X poll=%s:0x%X ref=0x%X\n",
                    fwInfo, (fwInfo&0x10)?"mem":"reg", fwPoll, fwRef);
        }
    }
    // One-time (settled frame): dump the device struct's segment-tracking region to find the flush queue /
    // descriptor array (sub_821C6C80 uses r8=device+13568; flush gate is device+13408). Look for content-
    // segment descriptors {0x8100_00LL, addr} that the inline-descriptor scan misses (textured draws).
    if (g_ktrace && g_device.load()) {
        static std::atomic<bool> ddumped{false}; bool de = false;
        if (n >= 10 && ddumped.compare_exchange_strong(de, true)) {
            uint32_t dv = g_device.load();
            fprintf(stderr, "[devdump] device=0x%X +10896=%08X +10908=%08X +13408=%08X +13568=%08X\n",
                    dv, GLD32(dv+10896), GLD32(dv+10908), GLD32(dv+13408), GLD32(dv+13568));
            for (uint32_t off = 13400; off <= 13720; off += 32) {
                char line[256]; int p = snprintf(line, sizeof line, "[devdump] +%u:", off);
                for (int k = 0; k < 8; k++) p += snprintf(line+p, sizeof line-p, " %08X", GLD32(dv + off + k*4));
                fprintf(stderr, "%s\n", line);
            }
        }
    }
    // Decoded-frame shortcut: sample the captured VC-1 frame-pool buffers to find the one holding the
    // decoded movie frame (high non-zero ratio + varied content vs an empty/uniform buffer).
    if (g_ktrace) {
        static std::atomic<bool> vsamp{false}; bool ve = false;
        // LATE sample (after a long run): did the VC-1 decoder ever write real content into its frame pool?
        // If buffers gain adjacent-byte variation -> it IS decoding (just slow/starved); if still uniform
        // black -> the decode is stuck/gated, not merely starved.
        if (n >= 220 && g_videoBufN.load() && vsamp.compare_exchange_strong(ve, true)) {
            int cnt = g_videoBufN.load();
            fprintf(stderr, "[video] LATE (swap#%u) %d frame-pool buffers (full 0x101440 scan):\n", n, cnt);
            for (int i = 0; i < cnt; i++) {
                uint32_t b = g_videoBufs[i], sz = g_videoBufSz[i]; uint32_t nz = 0, varied = 0;
                for (uint32_t o = 0; o + 4 <= sz; o += 4) {
                    uint32_t w = GLD32(b + o); if (w) nz++;
                    if ((w & 0xFF) != ((w >> 8) & 0xFF) || ((w >> 16) & 0xFF) != ((w >> 24) & 0xFF)) varied++;
                }
                fprintf(stderr, "[video]  buf%d @0x%X sz=0x%X nz=%u varied=%u\n", i, b, sz, nz, varied);
                // REX_VIDEODUMP: write the raw buffer (exact size) to disk for offline analysis of the
                // decoded-frame geometry. Only the varied (decoded) ones.
                if (getenv("REX_VIDEODUMP") && varied > 1000) {
                    char path[64]; snprintf(path, sizeof path, "/tmp/vbuf%d.raw", i);
                    FILE* f = fopen(path, "wb");
                    if (f) { fwrite(g_base + b, 1, sz, f); fclose(f);
                             fprintf(stderr, "[video]  -> dumped buf%d to %s (0x%X B)\n", i, path, sz); }
                }
            }
        }
    }
    if (rex_render::Enabled()) {
        // Publish the VC-1 frame-pool buffers (base+size) so the render thread can present the decoded intro
        // movie in color (picks the freshest Y triplet + does YUV->RGB). Cheap (a few atomic stores).
        rex_render::PublishVideo(g_videoBufs, g_videoBufSz, g_videoBufN.load());
        rex_render::Present(ctx.r4.u32);                         // non-blocking: publish fetch ptr to render thread
    }
    ctx.r3.u64 = 0;
}

// ====================================================================================================
// Dispatch-object synchronization: events, semaphores, single/multiple waits (goal: stable threading).
// Pointer-based Ke* operate on guest dispatch objects directly; the wait core lives above (WaitObject).
// ====================================================================================================

// void KeInitializeEvent(event r3, type r4, initial_state r5) — type 0=notification(manual), 1=sync(auto)
PPC_FUNC(__imp__KeInitializeEvent)
{
    uint32_t e = ctx.r3.u32;
    PPC_STORE_U8(e + 0x00, ctx.r4.u32 == 1 ? 1 : 0);   // dispatch type
    PPC_STORE_U8(e + 0x01, 0);
    PPC_STORE_U32(e + 0x04, ctx.r5.u32 ? 1 : 0);       // signal_state
    PPC_STORE_U32(e + 0x08, e + 0x08); PPC_STORE_U32(e + 0x0C, e + 0x08); // wait_list self-ref
}

// LONG KeSetEvent(event r3, increment r4, wait r5) -> previous signal state
PPC_FUNC(__imp__KeSetEvent)
{
    g_tlSignalLR = ctx.lr;
    uint32_t e = ctx.r3.u32;
    int32_t prev = static_cast<int32_t>(PPC_LOAD_U32(e + 0x04));
    SignalObject(e, 1);
    ctx.r3.u64 = static_cast<uint32_t>(prev);
}

// LONG KeResetEvent(event r3) -> previous signal state
PPC_FUNC(__imp__KeResetEvent)
{
    uint32_t e = ctx.r3.u32;
    int32_t prev = static_cast<int32_t>(PPC_LOAD_U32(e + 0x04));
    PPC_STORE_U32(e + 0x04, 0);
    ctx.r3.u64 = static_cast<uint32_t>(prev);
}

// LONG KePulseEvent(event r3, increment r4, wait r5) — signal then immediately clear
PPC_FUNC(__imp__KePulseEvent)
{
    g_tlSignalLR = ctx.lr;
    uint32_t e = ctx.r3.u32;
    int32_t prev = static_cast<int32_t>(PPC_LOAD_U32(e + 0x04));
    SignalObject(e, 1);
    SignalObject(e, 0);
    ctx.r3.u64 = static_cast<uint32_t>(prev);
}

// void KeInitializeSemaphore(sem r3, count r4, limit r5)
PPC_FUNC(__imp__KeInitializeSemaphore)
{
    uint32_t s = ctx.r3.u32;
    PPC_STORE_U8(s + 0x00, 5);                 // dispatch type = Semaphore
    PPC_STORE_U8(s + 0x01, 0);
    PPC_STORE_U32(s + 0x04, ctx.r4.u32);       // signal_state = count
    PPC_STORE_U32(s + 0x08, s + 0x08); PPC_STORE_U32(s + 0x0C, s + 0x08);
    PPC_STORE_U32(s + 0x10, ctx.r5.u32);       // limit
}

// LONG KeReleaseSemaphore(sem r3, increment r4, adjustment r5, wait r6) -> previous count
PPC_FUNC(__imp__KeReleaseSemaphore)
{
    g_tlSignalLR = ctx.lr;
    uint32_t s = ctx.r3.u32;
    int32_t prev = static_cast<int32_t>(PPC_LOAD_U32(s + 0x04));
    SignalObject(s, prev + static_cast<int32_t>(ctx.r5.u32));
    ctx.r3.u64 = static_cast<uint32_t>(prev);
}

// NTSTATUS KeWaitForSingleObject(object r3, reason r4, mode r5, alertable r6, *timeout r7)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    g_tlSignalLR = ctx.lr;
    ctx.r3.u64 = WaitObject(ctx.r3.u32, TimeoutMs(ctx.r7.u32));
}

// NTSTATUS NtWaitForSingleObjectEx(handle r3, mode r4, alertable r5, *timeout r6)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    uint32_t handle = ctx.r3.u32;
    uint32_t obj = (handle == 0xFFFFFFFE) ? PPC_LOAD_U32(ctx.r13.u32 + 0x100) : ResolveObject(handle);
    if (!obj) { ctx.r3.u64 = 0; return; }   // unknown handle: don't hang during bring-up
    ctx.r3.u64 = WaitObject(obj, TimeoutMs(ctx.r6.u32));
}

// ---- Xam: notifications / region / sign-in (return valid handles/values, not 0) -------------------
// HANDLE XamNotifyCreateListener(u64 mask r3, u32 max_version r4) -> non-zero listener handle.
// Bring-up: no real notification queue yet; a stable non-zero handle keeps the title from storing a
// null listener (which it later dereferences). XNotifyGetNext reports nothing pending.
PPC_FUNC(__imp__XamNotifyCreateListener) { ctx.r3.u64 = 0xCAFE0001u; }

// BOOL XNotifyGetNext(handle r3, match_id r4, *id r5, *param r6) -> FALSE (no notification)
PPC_FUNC(__imp__XNotifyGetNext)
{
    if (ctx.r5.u32) PPC_STORE_U32(ctx.r5.u32, 0);
    if (ctx.r6.u32) PPC_STORE_U32(ctx.r6.u32, 0);
    ctx.r3.u64 = 0;
}

// DWORD XGetGameRegion() -> region code (0x00FF = NTSC/U-ish, permissive for bring-up)
PPC_FUNC(__imp__XGetGameRegion) { ctx.r3.u64 = 0x00FFu; }

// DWORD XamUserGetSigninState(user_index r3) -> 1 (signed in, local) for user 0, else 0
PPC_FUNC(__imp__XamUserGetSigninState) { ctx.r3.u64 = (ctx.r3.u32 == 0) ? 1u : 0u; }

// HRESULT XamUserGetSigninInfo(user_index r3, flags r4, X_USER_SIGNIN_INFO* info r5)
// X_USER_SIGNIN_INFO (40 B): xuid@0x0 (u64), unk08@0x8, signin_state@0xC, unk10@0x10, unk14@0x14,
// name[16]@0x18. The stub returned success WITHOUT filling info (garbage) — implement for real.
PPC_FUNC(__imp__XamUserGetSigninInfo)
{
    uint32_t user = ctx.r3.u32, info = ctx.r5.u32;
    if (!info) { ctx.r3.u64 = 0x80070057u; return; }          // E_INVALIDARG
    for (uint32_t i = 0; i < 40; i++) PPC_STORE_U8(info + i, 0);
    if (user != 0) { ctx.r3.u64 = 0x80070525u; return; }      // ERROR_NO_SUCH_USER
    PPC_STORE_U64(info + 0x00, 0xE000000000000001ull);        // offline XUID
    PPC_STORE_U32(info + 0x0C, 1);                            // signin_state = signed in (local)
    const char* nm = "Player";
    for (uint32_t i = 0; nm[i] && i < 15; i++) PPC_STORE_U8(info + 0x18 + i, static_cast<uint8_t>(nm[i]));
    ctx.r3.u64 = 0;                                           // S_OK
}

// DWORD XamUserReadProfileSettings(title r3, user r4, xuid_count r5, xuids r6, setting_count r7,
//   setting_ids r8, *buffer_size r9, buffer r10, overlapped r11). First run = no saved settings:
// report a 0-setting result so the title falls back to defaults.
// XUSER_READ_PROFILE_SETTING_RESULT: setting_count@0x0, settings_ptr@0x4.
PPC_FUNC(__imp__XamUserReadProfileSettings)
{
    uint32_t bufSizePtr = ctx.r9.u32, bufPtr = ctx.r10.u32, overlapped = ctx.r11.u32;
    if (!bufPtr) {                                  // size query → need the 8-byte header
        if (bufSizePtr) PPC_STORE_U32(bufSizePtr, 8);
        ctx.r3.u64 = overlapped ? 0u : 0x8007007Au; // ERROR_INSUFFICIENT_BUFFER (sync)
        return;
    }
    PPC_STORE_U32(bufPtr + 0x00, 0);                // setting_count = 0 (none saved)
    PPC_STORE_U32(bufPtr + 0x04, 0);                // settings_ptr = null
    if (bufSizePtr) PPC_STORE_U32(bufSizePtr, 8);
    ctx.r3.u64 = 0;                                 // X_ERROR_SUCCESS
}

// ====================================================================================================
// File VFS (read-only) — backs Nt{CreateFile,ReadFile,QueryInformationFile,SetInformationFile,Close}.
// Guest paths (X_OBJECT_ATTRIBUTES.name_ptr -> X_ANSI_STRING) like "game:\media\foo" resolve under
// kernel::g_gameDir (the extracted game dir). Ref: rexglue-sdk src/filesystem + xboxkrnl_io.cpp.
// ====================================================================================================
namespace kernel { std::string g_gameDir; }
namespace {
std::unordered_map<uint32_t, FILE*> g_files;
std::unordered_map<uint32_t, std::string> g_fileNames;   // R0 (REX_TEXWATCH): handle->path, to label big reads
uint32_t g_nextFileHandle = 0xF2000000;   // file handle space (distinct from threads at 0xF1xxxxxx)
std::mutex g_fileMutex;

std::string GuestAnsiString(uint32_t strPtr) {   // X_ANSI_STRING: len@0(u16), max@2(u16), buf@4(u32)
    if (!strPtr) return {};
    uint16_t len = GLD16(strPtr + 0x00);
    uint32_t buf = GLD32(strPtr + 0x04);
    std::string s;
    for (uint16_t i = 0; i < len && buf; i++) s += static_cast<char>(g_base[buf + i]);
    return s;
}

// Resolve a relative path case-insensitively under base (Xbox FS is case-insensitive; Linux is not —
// the title opens "UI\UI.xzp"/"Assets\Audio" but the extracted dir is lowercase "ui"/"audio").
std::string ResolveCaseInsensitive(const std::string& base, const std::string& rel) {
    std::string cur = base;
    size_t i = 0;
    while (i < rel.size()) {
        size_t slash = rel.find('/', i);
        std::string comp = rel.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        i = (slash == std::string::npos) ? rel.size() : slash + 1;
        if (comp.empty() || comp == ".") continue;
        struct stat st;
        std::string exact = cur + "/" + comp;
        if (stat(exact.c_str(), &st) == 0) { cur = exact; continue; }   // exact case wins
        bool found = false;
        if (DIR* d = opendir(cur.c_str())) {
            while (struct dirent* e = readdir(d))
                if (strcasecmp(e->d_name, comp.c_str()) == 0) { cur += "/"; cur += e->d_name; found = true; break; }
            closedir(d);
        }
        if (!found) { cur += "/" + comp; }   // leave as-is → fopen will MISS
    }
    return cur;
}

// "game:\media\foo" / "\Device\...\foo" / "d:\foo" -> <g_gameDir>/media/foo (case-insensitive).
std::string TranslatePath(std::string p) {
    size_t colon = p.find(':');
    std::string rel = (colon != std::string::npos) ? p.substr(colon + 1) : p;
    for (char& c : rel) if (c == '\\') c = '/';
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    return ResolveCaseInsensitive(kernel::g_gameDir, rel);
}
FILE* FileForHandle(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_fileMutex);
    auto it = g_files.find(h);
    return it != g_files.end() ? it->second : nullptr;
}
} // namespace

// NTSTATUS NtCreateFile(*handle r3, access r4, obj_attr r5, io_status r6, alloc r7, attrs r8, share r9,
//                       disposition r10, options r11)
PPC_FUNC(__imp__NtCreateFile)
{
    uint32_t pHandle = ctx.r3.u32, objAttr = ctx.r5.u32, ioStatus = ctx.r6.u32;
    uint32_t namePtr = objAttr ? GLD32(objAttr + 0x04) : 0;
    std::string path = GuestAnsiString(namePtr);
    std::string host = TranslatePath(path);
    FILE* f = path.empty() ? nullptr : fopen(host.c_str(), "rb");
    KTRACE("NtCreateFile('%s' -> '%s') %s\n", path.c_str(), host.c_str(), f ? "OK" : "MISS");
    // REX_LOADERPROBE: timestamped asset-open trace — reveals the title's STATE PROGRESSION over time
    // (frontend assets early vs gameplay assets later) to distinguish "looping a fixed set" from "advancing".
    if (getenv("REX_LOADERPROBE") && !path.empty()) {
        static std::atomic<int> fn{0}; int k = fn.fetch_add(1);
        if (k < 400) fprintf(stderr, "[asset] #%d vbc=%u %s '%s'\n", k, g_vblankCount.load(), f ? "OK" : "MISS", path.c_str());
    }
    // REX_OPENER (cont.28): identify the global-asset LOADER. Log the guest caller (ctx.lr = the return addr in
    // the function that called NtCreateFile) for the global .bin assets around the variant-A stop point
    // (Textures/Global.bin is the last opened; Meshes/Global.bin never opens). Walk a couple guest stack frames
    // via the recomp linkage (mflr r12; stw r12,-8(r1); stwu r1,-N) -> a frame's saved LR is at *(backchain)-8.
    if (getenv("REX_OPENER") && (path.find("Global.bin") != std::string::npos ||
                                 path.find("Meshes") != std::string::npos ||
                                 path.find("global.bin") != std::string::npos)) {
        uint32_t lr = ctx.lr, sp = ctx.r1.u32;
        fprintf(stderr, "[opener] '%s' caller-lr=0x%08X", path.c_str(), lr);
        for (int i = 0; i < 6 && sp >= 0x1000u && sp < 0xC0000000u; i++) {
            uint32_t bc = PPC_LOAD_U32(sp);                       // back-chain -> caller frame
            if (bc <= sp || bc >= 0xC0000000u) break;
            uint32_t slr = PPC_LOAD_U32(bc - 8);                  // that frame's saved LR
            fprintf(stderr, " <-0x%08X", slr);
            sp = bc;
        }
        fprintf(stderr, "\n");
    }
    if (!f) {
        if (pHandle) PPC_STORE_U32(pHandle, 0);
        if (ioStatus) { PPC_STORE_U32(ioStatus + 0, 0xC0000034u); PPC_STORE_U32(ioStatus + 4, 0); }
        ctx.r3.u64 = 0xC0000034u;   // STATUS_OBJECT_NAME_NOT_FOUND
        return;
    }
    uint32_t h;
    { std::lock_guard<std::mutex> lk(g_fileMutex); h = g_nextFileHandle++; g_files[h] = f;
      if (getenv("REX_TEXWATCH")) g_fileNames[h] = path; }
    if (pHandle) PPC_STORE_U32(pHandle, h);
    if (ioStatus) { PPC_STORE_U32(ioStatus + 0, 0); PPC_STORE_U32(ioStatus + 4, 1); } // SUCCESS, FILE_OPENED
    ctx.r3.u64 = 0;
}

// NtOpenFile(*handle r3, access r4, obj_attr r5, io_status r6, share r7, options r8) — open-existing.
PPC_FUNC(__imp__NtOpenFile)
{
    uint32_t pHandle = ctx.r3.u32, objAttr = ctx.r5.u32, ioStatus = ctx.r6.u32;
    uint32_t namePtr = objAttr ? GLD32(objAttr + 0x04) : 0;
    std::string host = TranslatePath(GuestAnsiString(namePtr));
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) { if (pHandle) PPC_STORE_U32(pHandle, 0); ctx.r3.u64 = 0xC0000034u; return; }
    uint32_t h;
    { std::lock_guard<std::mutex> lk(g_fileMutex); h = g_nextFileHandle++; g_files[h] = f; }
    if (pHandle) PPC_STORE_U32(pHandle, h);
    if (ioStatus) { PPC_STORE_U32(ioStatus + 0, 0); PPC_STORE_U32(ioStatus + 4, 1); }
    ctx.r3.u64 = 0;
}

// NTSTATUS NtReadFile(handle r3, event r4, apc r5, apc_ctx r6, io_status r7, buffer r8, len r9, *offset r10)
PPC_FUNC(__imp__NtReadFile)
{
    uint32_t handle = ctx.r3.u32, ioStatus = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32,
             pOffset = ctx.r10.u32;
    FILE* f = FileForHandle(handle);
    if (!f) { ctx.r3.u64 = 0xC0000008u; return; }      // STATUS_INVALID_HANDLE
    if (pOffset) fseek(f, static_cast<long>(GLD64(pOffset)), SEEK_SET);
    size_t n = length ? fread(g_base + buffer, 1, length, f) : 0;
    if (ioStatus) { PPC_STORE_U32(ioStatus + 0, n ? 0 : 0xC0000011u); PPC_STORE_U32(ioStatus + 4, static_cast<uint32_t>(n)); }
    KTRACE("NtReadFile(h=0x%X len=%u off=%s) -> %zu\n", handle, length, pOffset ? "set" : "cur", n);
    // R0: where does asset data land? Big reads (textures/meshes) into the GPU window (0xA0000000+) mean the
    // title reads directly into GPU mem; reads into low/system memory mean a separate copy-to-GPU must follow
    // (which REX_TEXWATCH shows never happens). Log dest+len for big reads to map the texture data path.
    if (getenv("REX_TEXWATCH") && length >= 0x10000) {
        static std::atomic<int> rn{0}; int k = rn.fetch_add(1);
        std::string nm; { std::lock_guard<std::mutex> lk(g_fileMutex); auto it=g_fileNames.find(handle); if(it!=g_fileNames.end()) nm=it->second; }
        if (k < 40) fprintf(stderr, "[texread] dest=0x%08X len=0x%X %s '%s'\n", buffer, length,
                            buffer >= 0xA0000000u ? "GPU-WINDOW" : "system-mem", nm.c_str());
    }
    ctx.r3.u64 = (n || length == 0) ? 0 : 0xC0000011u; // STATUS_END_OF_FILE
}

// NTSTATUS NtQueryInformationFile(handle r3, io_status r4, info r5, len r6, class r7)
PPC_FUNC(__imp__NtQueryInformationFile)
{
    uint32_t handle = ctx.r3.u32, ioStatus = ctx.r4.u32, info = ctx.r5.u32, infoClass = ctx.r7.u32;
    FILE* f = FileForHandle(handle);
    if (!f) { ctx.r3.u64 = 0xC0000008u; return; }
    long cur = ftell(f); fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, cur, SEEK_SET);
    uint64_t sz = static_cast<uint64_t>(size);
    if (info) {
        // Field offsets MUST match the requested FILE_INFORMATION_CLASS exactly — the title reads the
        // file size at the class-specific EndOfFile offset to compute its read length. Getting this wrong
        // (e.g. writing EndOfFile at +8 for class 34, whose EndOfFile is at +40) yields a 0-byte read and
        // the asset never loads (root-caused: the XGS/.ptc loads here via class-34 size queries).
        switch (infoClass) {
        case 14:                                        // FilePositionInformation
            PPC_STORE_U64(info + 0, static_cast<uint64_t>(cur));   // CurrentByteOffset
            break;
        case 34:                                        // FileNetworkOpenInformation (56 bytes)
            PPC_STORE_U64(info + 0,  0);                 // CreationTime
            PPC_STORE_U64(info + 8,  0);                 // LastAccessTime
            PPC_STORE_U64(info + 16, 0);                 // LastWriteTime
            PPC_STORE_U64(info + 24, 0);                 // ChangeTime
            PPC_STORE_U64(info + 32, sz);                // AllocationSize
            PPC_STORE_U64(info + 40, sz);                // EndOfFile
            PPC_STORE_U32(info + 48, 0x80);              // FileAttributes = FILE_ATTRIBUTE_NORMAL
            break;
        case 5:                                         // FileStandardInformation (24 bytes)
        default:                                         // (AllocationSize @0, EndOfFile @8)
            PPC_STORE_U64(info + 0, sz);                 // AllocationSize
            PPC_STORE_U64(info + 8, sz);                 // EndOfFile
            PPC_STORE_U32(info + 16, 1);                 // NumberOfLinks
            break;
        }
    }
    KTRACE("NtQueryInformationFile(h=0x%X class=%u) -> size=%lu\n", handle, infoClass, sz);
    if (ioStatus) { PPC_STORE_U32(ioStatus + 0, 0); PPC_STORE_U32(ioStatus + 4, ctx.r6.u32); }
    ctx.r3.u64 = 0;
}

// NTSTATUS NtSetInformationFile(handle r3, io_status r4, info r5, len r6, class r7) — handle seek (14).
PPC_FUNC(__imp__NtSetInformationFile)
{
    uint32_t handle = ctx.r3.u32, info = ctx.r5.u32, infoClass = ctx.r7.u32;
    FILE* f = FileForHandle(handle);
    if (f && infoClass == 14 && info) fseek(f, static_cast<long>(GLD64(info + 0)), SEEK_SET);
    ctx.r3.u64 = 0;
}

// NTSTATUS NtClose(handle r3) — close file handles (others are no-op: thread/event handles persist).
PPC_FUNC(__imp__NtClose)
{
    uint32_t h = ctx.r3.u32;
    { std::lock_guard<std::mutex> lk(g_fileMutex); auto it = g_files.find(h);
      if (it != g_files.end()) { fclose(it->second); g_files.erase(it); } }
    ctx.r3.u64 = 0;
}

// NTSTATUS KeWaitForMultipleObjects(count r3, objects r4, waitType r5, reason r6, mode r7,
//                                   alertable r8, *timeout r9, waitblocks r10)
PPC_FUNC(__imp__KeWaitForMultipleObjects)
{
    g_tlSignalLR = ctx.lr;
    uint32_t count = ctx.r3.u32, objects = ctx.r4.u32, waitType = ctx.r5.u32; // 0=WaitAll, 1=WaitAny
    int64_t timeoutMs = TimeoutMs(ctx.r9.u32);
    auto firstReady = [&]() -> int {
        if (waitType == 1) {                       // WaitAny -> index of first signaled, else -1
            for (uint32_t i = 0; i < count; i++) {
                uint32_t o = GLD32(objects + i * 4);
                if (static_cast<int32_t>(GLD32(o + 0x04)) > 0) return static_cast<int>(i);
            }
            return -1;
        }
        for (uint32_t i = 0; i < count; i++) {     // WaitAll -> 0 if all signaled, else -1
            uint32_t o = GLD32(objects + i * 4);
            if (static_cast<int32_t>(GLD32(o + 0x04)) <= 0) return -1;
        }
        return 0;
    };
    auto pred = [&]{ return firstReady() >= 0; };
    // Consume auto-reset/semaphore/mutant objects we're satisfied on.
    auto consume = [&](uint32_t o){ uint8_t t = g_base[o + 0x00];
        if (t == 1 || t == 2 || t == 5) GST32(o + 0x04, static_cast<int32_t>(GLD32(o + 0x04)) - 1); };
    if (g_fair) {
        bool ok = FairWaitUntil(pred, timeoutMs);
        if (!ok) { ctx.r3.u64 = 0x102; return; }
        int idx = firstReady();
        { std::lock_guard<std::mutex> ol(g_objM);
          if (waitType == 1) consume(GLD32(objects + idx * 4));
          else for (uint32_t i = 0; i < count; i++) consume(GLD32(objects + i * 4)); }
        ctx.r3.u64 = (waitType == 1) ? static_cast<uint32_t>(idx) : 0;
        return;
    }
    std::unique_lock<std::mutex> lk = g_coop ? std::unique_lock<std::mutex>(g_waitMutex, std::adopt_lock)
                                             : std::unique_lock<std::mutex>(g_waitMutex);
    bool ok = true;
    if (timeoutMs < 0) g_waitCv.wait(lk, pred);
    else ok = g_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), pred);
    if (!ok) { if (g_coop) lk.release(); ctx.r3.u64 = 0x102; return; }   // STATUS_TIMEOUT
    int idx = firstReady();
    if (waitType == 1) consume(GLD32(objects + idx * 4));
    else for (uint32_t i = 0; i < count; i++) consume(GLD32(objects + i * 4));
    if (g_coop) lk.release();                                // coop: keep the token held as we resume
    ctx.r3.u64 = (waitType == 1) ? static_cast<uint32_t>(idx) : 0;   // WAIT_OBJECT_0 + idx
}

// NTSTATUS NtWaitForMultipleObjectsEx(count r3, handles r4, wait_type r5, wait_mode r6, alertable r7,
//                                     *timeout r8) — handle-based sibling of KeWaitForMultipleObjects.
// Resolves each HANDLE to its dispatch object, then runs the identical cooperative WaitAll/WaitAny core.
// Without this (as a soft stub returning success) a worker's wait loop busy-spins forever (tid=6 here
// waits WaitAny on its two render/sync events). Reference: rexglue-sdk xeNtWaitForMultipleObjectsEx.
PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    g_tlSignalLR = ctx.lr;
    uint32_t count = ctx.r3.u32, handles = ctx.r4.u32, waitType = ctx.r5.u32;
    int64_t timeoutMs = TimeoutMs(ctx.r8.u32);
    uint32_t objs[64];
    if (count > 64) count = 64;
    for (uint32_t i = 0; i < count; i++) objs[i] = ResolveObject(GLD32(handles + i * 4));
    auto signaled = [&](uint32_t o){ return o && static_cast<int32_t>(GLD32(o + 0x04)) > 0; };
    auto firstReady = [&]() -> int {
        if (waitType == 1) {                       // WaitAny -> index of first signaled, else -1
            for (uint32_t i = 0; i < count; i++) if (signaled(objs[i])) return static_cast<int>(i);
            return -1;
        }
        for (uint32_t i = 0; i < count; i++) if (!signaled(objs[i])) return -1;   // WaitAll
        return 0;
    };
    auto pred = [&]{ return firstReady() >= 0; };
    auto consume = [&](uint32_t o){ if (!o) return; uint8_t t = g_base[o + 0x00];
        if (t == 1 || t == 2 || t == 5) GST32(o + 0x04, static_cast<int32_t>(GLD32(o + 0x04)) - 1); };
    if (g_fair) {
        bool ok = FairWaitUntil(pred, timeoutMs);
        if (!ok) { ctx.r3.u64 = 0x102; return; }
        int idx = firstReady();
        { std::lock_guard<std::mutex> ol(g_objM);
          if (waitType == 1) consume(objs[idx]); else for (uint32_t i = 0; i < count; i++) consume(objs[i]); }
        ctx.r3.u64 = (waitType == 1) ? static_cast<uint32_t>(idx) : 0;
        return;
    }
    std::unique_lock<std::mutex> lk = g_coop ? std::unique_lock<std::mutex>(g_waitMutex, std::adopt_lock)
                                             : std::unique_lock<std::mutex>(g_waitMutex);
    bool ok = true;
    if (timeoutMs < 0) g_waitCv.wait(lk, pred);
    else ok = g_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), pred);
    if (!ok) { if (g_coop) lk.release(); ctx.r3.u64 = 0x102; return; }            // STATUS_TIMEOUT
    int idx = firstReady();
    if (waitType == 1) consume(objs[idx]);
    else for (uint32_t i = 0; i < count; i++) consume(objs[i]);
    if (g_coop) lk.release();
    ctx.r3.u64 = (waitType == 1) ? static_cast<uint32_t>(idx) : 0;
}

// ---- handle-based events (workers create these to synchronize) -------------------------------------
// DWORD NtCreateEvent(*handle r3, obj_attr r4, type r5 (0=manual,1=auto), initial_state r6)
PPC_FUNC(__imp__NtCreateEvent)
{
    uint32_t pHandle = ctx.r3.u32, type = ctx.r5.u32, initState = ctx.r6.u32, obj, handle;
    { std::lock_guard<std::mutex> lk(g_handleMutex); obj = AllocObject(0x20); handle = g_nextHandle++;
      g_objHandles[handle] = obj; }
    PPC_STORE_U8 (obj + 0x00, type == 0 ? 0 : 1);   // dispatch type: notification(0)/synchronization(1)
    PPC_STORE_U8 (obj + 0x01, 0);
    PPC_STORE_U32(obj + 0x04, initState ? 1 : 0);   // signal_state
    PPC_STORE_U32(obj + 0x08, obj + 0x08); PPC_STORE_U32(obj + 0x0C, obj + 0x08);  // wait_list self-ref
    if (pHandle) PPC_STORE_U32(pHandle, handle);
    KTRACE("NtCreateEvent(type=%u init=%u) -> handle=0x%X\n", type, initState, handle);
    ctx.r3.u64 = 0;
}
// LONG NtSetEvent(handle r3, *prev r4)
PPC_FUNC(__imp__NtSetEvent)
{
    g_tlSignalLR = ctx.lr;
    uint32_t obj = ResolveObject(ctx.r3.u32);
    if (obj) { int32_t prev = static_cast<int32_t>(PPC_LOAD_U32(obj + 0x04)); SignalObject(obj, 1);
               if (ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, static_cast<uint32_t>(prev)); }
    ctx.r3.u64 = 0;
}
PPC_FUNC(__imp__NtClearEvent) { uint32_t o = ResolveObject(ctx.r3.u32); if (o) PPC_STORE_U32(o + 0x04, 0); ctx.r3.u64 = 0; }
PPC_FUNC(__imp__NtPulseEvent)
{
    g_tlSignalLR = ctx.lr;
    uint32_t obj = ResolveObject(ctx.r3.u32);
    if (obj) { int32_t prev = static_cast<int32_t>(PPC_LOAD_U32(obj + 0x04)); SignalObject(obj, 1);
               SignalObject(obj, 0); if (ctx.r4.u32) PPC_STORE_U32(ctx.r4.u32, static_cast<uint32_t>(prev)); }
    ctx.r3.u64 = 0;
}

// ---- spinlocks / critical regions / IRQL (kernel-level serialization) ------------------------------
namespace { void SpinAcquire(uint32_t lock) {   // per-lock host mutex; release the token if contended
    auto* m = CsLock(lock);
    if (!m->try_lock()) { kernel::UnlockGuestExecution(); m->lock(); kernel::LockGuestExecution(); }
} }
PPC_FUNC(__imp__KfAcquireSpinLock)               { SpinAcquire(ctx.r3.u32); ctx.r3.u64 = 0; }  // -> old IRQL
PPC_FUNC(__imp__KfReleaseSpinLock)               { CsLock(ctx.r3.u32)->unlock(); }
PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)   { SpinAcquire(ctx.r3.u32); }
PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql) { CsLock(ctx.r3.u32)->unlock(); }
PPC_FUNC(__imp__KeTryToAcquireSpinLockAtRaisedIrql) { ctx.r3.u64 = CsLock(ctx.r3.u32)->try_lock() ? 1 : 0; }
PPC_FUNC(__imp__KeEnterCriticalRegion)           { /* APC-disable: no-op (no APC delivery yet) */ }
PPC_FUNC(__imp__KeLeaveCriticalRegion)           { /* no-op */ }
PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel)           { ctx.r3.u64 = 0; }   // old IRQL
PPC_FUNC(__imp__KfLowerIrql)                     { /* no-op */ }

// NTSTATUS KeDelayExecutionThread(mode r3, alertable r4, *interval r5) — sleep/yield. THE yield point:
// release the execution token so other guest threads run (breaks busy-wait token-starvation), then
// re-acquire. interval: <0 = relative 100ns, 0 = yield, >0 = absolute (treat as short).
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    int64_t t = ctx.r5.u32 ? static_cast<int64_t>(GLD64(ctx.r5.u32)) : 0;
    int64_t ms = (t < 0) ? (-t) / 10000 : (t == 0 ? 0 : 1);
    if (ms > 25) ms = 25;                                 // cap so a long guest delay can't stall boot
    kernel::UnlockGuestExecution();                       // yield the token (no-op under REX_NOTOKEN)
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    else        std::this_thread::yield();
    kernel::LockGuestExecution();                         // re-acquire (blocks until another yields)
    ctx.r3.u64 = 0;
}
// NTSTATUS NtDelayExecution(alertable r3, *interval r4) — same, args shifted.
PPC_FUNC(__imp__NtDelayExecution)
{
    int64_t t = ctx.r4.u32 ? static_cast<int64_t>(GLD64(ctx.r4.u32)) : 0;
    int64_t ms = (t < 0) ? (-t) / 10000 : (t == 0 ? 0 : 1);
    if (ms > 25) ms = 25;
    kernel::UnlockGuestExecution();
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms)); else std::this_thread::yield();
    kernel::LockGuestExecution();
    ctx.r3.u64 = 0;
}
