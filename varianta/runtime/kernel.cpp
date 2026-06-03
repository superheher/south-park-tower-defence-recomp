// Variant A — kernel/xam import implementations (strong symbols; override the weak trap-stubs in
// import_stubs.gen.cpp). Behaviour reference: third_party/rexglue-sdk/src/ + Xenia.
// ABI: PPC_FUNC(__imp__Name)(PPCContext& ctx, uint8_t* base); args in ctx.r3,r4,...; NTSTATUS/ret in ctx.r3.
// Guest pointers are guest addresses — dereference via PPC_LOAD_*/PPC_STORE_* (base + addr, byte-swapped).
#include "ppc_recomp_shared.h"
#include "kernel.h"
#include "rex_render.h"   // minimal Vulkan renderer (VdSwap present), active under REX_RENDER=1
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
void PPCIndirectNull(uint32_t target, uint32_t lr)
{
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    std::lock_guard<std::mutex> lk(m);
    if (seen.insert(target).second)
        fprintf(stderr, "[INDIRECT-NULL] target=0x%08X (caller lr=0x%08X)\n", target, lr);
}

// Every guest indirect branch/call (bctr/bctrl) routes here via rex_indirect.h's PPC_CALL_INDIRECT_FUNC.
// CRITICAL: bounds-check the target against the recompiled code range BEFORE indexing the function table.
// A recovered switch-table's out-of-range fallback (or any corrupted code pointer) can hand us a wild
// address; PPC_LOOKUP_FUNC would then dereference base + IMAGE_SIZE + (target-CODE_BASE)*2, which for a
// target far below CODE_BASE wraps to a slot gigabytes past the 4 GiB guest mapping — faulting the lookup
// READ itself. So: dispatch only in-range, mapped targets; log+skip everything else (in-range-but-unmapped
// = a still-missing jump-table case; out-of-range = a data divergence the title would also fault on).
void PPCInvokeGuest(PPCContext& ctx, uint8_t* base, uint32_t target)
{
    // REX_TRACEB740: log every indirect (bctrl) call made from within sub_8211B740 (its call sites'
    // return-LRs fall in [0x8211B748, 0x8211C000)) WITH the call's return value (the post-bctrl branches
    // gate on it). Reveals how far the 718-line transitions-init handler gets and whether the indirect
    // dispatch to sub_8210AF90 (the 0x828E82A6 setter) ever fires.
    uint32_t lr = static_cast<uint32_t>(ctx.lr);
    bool trace = g_traceB740 && lr >= 0x8211B748u && lr < 0x8211C000u;
    if (target >= PPC_CODE_BASE && target < (PPC_IMAGE_BASE + PPC_IMAGE_SIZE))
    {
        PPCFunc* fn = PPC_LOOKUP_FUNC(base, target);
        if (fn) {
            fn(ctx, base);
            if (trace) fprintf(stderr, "[b740] bctrl lr=0x%08X -> 0x%08X returned r3=0x%08X%s\n",
                               lr, target, ctx.r3.u32, target == 0x8210AF90u ? "  <== sub_8210AF90!" : "");
            return;
        }
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

// Resolve a guest code address to its recompiled host fn via the in-memory dispatch table (the same
// layout runtime.cpp populates: base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (addr - PPC_CODE_BASE)*2).
PPCFunc* DispatchLookup(uint32_t addr) {
    return *reinterpret_cast<PPCFunc**>(g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE
        + (uint64_t(addr - PPC_CODE_BASE) * 2));
}
void CallGuest(uint32_t addr, PPCContext& ctx) {
    if (PPCFunc* fn = DispatchLookup(addr)) fn(ctx, g_base);
    else fprintf(stderr, "[thread] CallGuest: no host fn at 0x%X\n", addr);
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
std::atomic<uint64_t> g_drawCount{0}, g_swapCount{0};
const bool g_cptrace = (getenv("REX_CPTRACE") != nullptr);

inline void WriteGpuReg(uint32_t index, uint32_t value) { GST32(0x7FC80000u + index * 4u, value); }

void ExecutePM4(uint32_t addr, uint32_t dwords, int depth);   // fwd

// Execute one type-3 packet body. `addr` is at the first data dword; `count` data dwords follow.
void ExecuteType3(uint32_t addr, uint32_t op, uint32_t count, int depth) {
    if (g_cptrace) fprintf(stderr, "[cp]%*s T3 op=0x%02X count=%u d0=0x%X d1=0x%X d2=0x%X\n", depth*2, "", op, count,
                           GLD32(addr), count>1?GLD32(addr+4):0, count>2?GLD32(addr+8):0);
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
        if (g_cptrace) fprintf(stderr, "[cp] INTERRUPT mask=0x%X\n", cpuMask);
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
        break;
      }
      case PM4_SET_CONSTANT2: {                 // like SET_CONSTANT but 16-bit index, writes regs directly
        uint32_t index = GLD32(addr) & 0xFFFF;
        for (uint32_t i = 1; i < count; i++) WriteGpuReg(index + (i-1), GLD32(addr + i*4));
        break;
      }
      case PM4_DRAW_INDX: case PM4_DRAW_INDX_2: {
        uint32_t init = GLD32(addr), numInd = init >> 16, prim = init & 0x3F;
        g_drawCount.fetch_add(1);
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
        break;
      }
      case PM4_XE_SWAP:
        g_gpuCounter.fetch_add(1); g_swapCount.fetch_add(1);
        if (g_cptrace) fprintf(stderr, "[cp] XE_SWAP #%llu\n", (unsigned long long)g_swapCount.load());
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
        g_vblankCount.fetch_add(1);
        uint32_t cb = g_interruptCallback;
        if (!cb) continue;
        if (g_fair && getenv("REX_INITDIAG")) { static int d=0; if ((d++ % 30)==0)
            fprintf(stderr, "[initdiag] g_tok next=%llu serving=%llu held=%d (churn=serving)\n",
                    (unsigned long long)g_tok.next_, (unsigned long long)g_tok.serving_, (int)g_tok.held_); }
        if (g_fair) g_tok.lock(); else if (g_coop) g_waitMutex.lock();
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

// ---- Ring-kick instrumentation (sub_821C6600): log the segment descriptor it consumes + the ring delta.
// The kick reads a 2-dword descriptor at r4 ({d0: low-24 used, d1: segment addr}) and emits an IB packet to
// the ring (advancing CP_RB_WPTR). Renderer route B needs the per-frame segment [addr,len] list; logging the
// 6 init kicks reveals the descriptor->IB encoding (which we then read from the segment table per-frame).
std::atomic<uint32_t> g_device{0};   // captured from the first ring-kick (device base, ~0x26F80)
extern "C" PPC_FUNC(__imp__sub_821C6600);
PPC_FUNC(sub_821C6600)
{
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
    if (!f) {
        if (pHandle) PPC_STORE_U32(pHandle, 0);
        if (ioStatus) { PPC_STORE_U32(ioStatus + 0, 0xC0000034u); PPC_STORE_U32(ioStatus + 4, 0); }
        ctx.r3.u64 = 0xC0000034u;   // STATUS_OBJECT_NAME_NOT_FOUND
        return;
    }
    uint32_t h;
    { std::lock_guard<std::mutex> lk(g_fileMutex); h = g_nextFileHandle++; g_files[h] = f; }
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
