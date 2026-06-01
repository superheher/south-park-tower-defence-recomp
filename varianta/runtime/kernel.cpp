// Variant A — kernel/xam import implementations (strong symbols; override the weak trap-stubs in
// import_stubs.gen.cpp). Behaviour reference: third_party/rexglue-sdk/src/ + Xenia.
// ABI: PPC_FUNC(__imp__Name)(PPCContext& ctx, uint8_t* base); args in ctx.r3,r4,...; NTSTATUS/ret in ctx.r3.
// Guest pointers are guest addresses — dereference via PPC_LOAD_*/PPC_STORE_* (base + addr, byte-swapped).
#include "ppc_recomp_shared.h"
#include "kernel.h"
#include <xex.h>     // getOptHeaderPtr, XEX_HEADER_*
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <utility>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>

// Lightweight import trace (set REX_KTRACE=0 to silence).
static const bool g_ktrace = []{ const char* e = getenv("REX_KTRACE"); return !e || e[0] != '0'; }();
#define KTRACE(...) do { if (g_ktrace) { fprintf(stderr, "[kernel] " __VA_ARGS__); } } while (0)

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
    KTRACE("NtAllocateVirtualMemory(req=0x%X sz=0x%X type=0x%X prot=0x%X) -> base=0x%X size=0x%X\n",
        reqBase, reqSize, allocType, protect, gbase, size);
    ctx.r3.u64 = kStatusSuccess;
}

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
    auto [rb, r] = FindRegion(gbase);
    if (!r) { ctx.r3.u64 = kStatusMemoryNotAllocated; return; }
    if (freeType & kMemRelease)        { r->reserved = false; r->committed = false; }
    else if (freeType & kMemDecommit)  { r->committed = false; }
    // Underlying host pages persist (lazy mmap); only bookkeeping changes.
    PPC_STORE_U32(pBase, rb);
    if (pSize) PPC_STORE_U32(pSize, r->size);
    KTRACE("NtFreeVirtualMemory(0x%X type=0x%X)\n", gbase, freeType);
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
} // namespace

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    auto* m = CsLock(ctx.r3.u32);
    if (!m->try_lock()) {                 // held by a thread that yielded while owning it: release the
        kernel::UnlockGuestExecution();   // execution token while we block, then re-acquire it.
        m->lock();
        kernel::LockGuestExecution();
    }
}
PPC_FUNC(__imp__RtlLeaveCriticalSection) { CsLock(ctx.r3.u32)->unlock(); }

// BOOLEAN RtlTryEnterCriticalSection(cs)
PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = CsLock(ctx.r3.u32)->try_lock() ? 1 : 0;
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

// ---- dispatch-object wait/signal (events, semaphores, mutants, thread-exit) ------------------------
// Xbox dispatch objects carry an X_DISPATCH_HEADER (type@0x00 u8, signal_state@0x04 s32). We back
// waits with one global condition variable; the signal_state lives in guest memory. Coarse but
// correct. Reference: rexglue-sdk xboxkrnl_threading.cpp.
// g_waitMutex IS the cooperative guest-execution token: the running guest thread holds it; waits
// release it (cv.wait) so another thread can run. So SignalObject (called by the running thread)
// must NOT re-lock it, and WaitObject adopts the already-held lock.
std::mutex g_waitMutex;
std::condition_variable g_waitCv;

void SignalObject(uint32_t obj, int32_t state) {
    GST32(obj + 0x04, static_cast<uint32_t>(state));   // caller already holds the token
    g_waitCv.notify_all();
}

// timeout: <0 = infinite, 0 = poll, >0 = milliseconds. Returns 0 (success) or 0x102 (STATUS_TIMEOUT).
// Releases the execution token while blocked (lets other guest threads run), re-holds it on resume.
uint32_t WaitObject(uint32_t obj, int64_t timeoutMs) {
    std::unique_lock<std::mutex> lk(g_waitMutex, std::adopt_lock);   // token already held by caller
    auto signaled = [&]{ return static_cast<int32_t>(GLD32(obj + 0x04)) > 0; };
    bool ok = true;
    if (timeoutMs < 0) g_waitCv.wait(lk, signaled);
    else ok = g_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), signaled);
    if (ok) {                                     // consume for auto-reset event / semaphore / mutant
        uint8_t type = g_base[obj + 0x00];
        if (type == 1 || type == 2 || type == 5)
            GST32(obj + 0x04, static_cast<uint32_t>(static_cast<int32_t>(GLD32(obj + 0x04)) - 1));
    }
    lk.release();                                 // keep the token held as we resume running
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
    g_waitMutex.lock();                      // acquire the execution token (blocks until the creator yields)
    PPCContext ctx{};
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
    SignalObject(rec->threadAddr, 1);        // wake anyone waiting on this thread object (header type 6)
    g_waitMutex.unlock();
}
} // namespace

namespace kernel {
// The cooperative execution token (see kernel.h). g_waitMutex is held by the running guest thread.
void LockGuestExecution()   { g_waitMutex.lock(); }
void UnlockGuestExecution() { g_waitMutex.unlock(); }
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
    uint32_t handle = ctx.r3.u32, pOut = ctx.r5.u32, obj = 0;
    if (handle == 0xFFFFFFFE) {                          // NtCurrentThread pseudo-handle
        obj = PPC_LOAD_U32(ctx.r13.u32 + 0x100);
    } else {
        std::lock_guard<std::mutex> lk(g_handleMutex);
        auto it = g_handles.find(handle);
        if (it != g_handles.end()) obj = it->second->threadAddr;
    }
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

void VblankPump() {
    while (g_vblankRunning.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));   // ~60 Hz
        g_vblankCount.fetch_add(1);
        uint32_t cb = g_interruptCallback;
        if (!cb) continue;
        // Fire the guest graphics interrupt callback (source = 0 vblank) on the pump's own context,
        // holding the cooperative execution token (it runs guest code). Per rexglue, the TLS ptr must
        // read 0 during interrupts (some titles check it).
        g_waitMutex.lock();
        PPCContext ctx{};
        ctx.r1.u64 = g_pumpStack - 0x200;
        ctx.r13.u64 = g_pumpKpcr;
        ctx.r3.u64 = 0;                  // source = vblank
        ctx.r4.u64 = g_interruptData;
        uint32_t savedTls = GLD32(g_pumpKpcr + 0x00);
        GST32(g_pumpKpcr + 0x00, 0);
        CallGuest(cb, ctx);
        GST32(g_pumpKpcr + 0x00, savedTls);
        g_waitMutex.unlock();
    }
}

void StartVblankPump() {
    if (g_vblankRunning.exchange(true)) return;
    uint32_t t = 0;
    g_pumpKpcr = CreateGuestThreadContext(0x20000, 0, /*threadId=*/99, &t, &g_pumpStack);
    std::thread(VblankPump).detach();
    fprintf(stderr, "[video] vblank pump started (kpcr=0x%X)\n", g_pumpKpcr);
}
} // namespace

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

// DWORD VdInitializeEngines(...) -> 1 (success); the system command buffer id address is a no-op.
PPC_FUNC(__imp__VdInitializeEngines) { ctx.r3.u64 = 1; }
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress) { /* no-op */ }

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
    uint32_t s = ctx.r3.u32;
    int32_t prev = static_cast<int32_t>(PPC_LOAD_U32(s + 0x04));
    SignalObject(s, prev + static_cast<int32_t>(ctx.r5.u32));
    ctx.r3.u64 = static_cast<uint32_t>(prev);
}

// NTSTATUS KeWaitForSingleObject(object r3, reason r4, mode r5, alertable r6, *timeout r7)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    ctx.r3.u64 = WaitObject(ctx.r3.u32, TimeoutMs(ctx.r7.u32));
}

// NTSTATUS NtWaitForSingleObjectEx(handle r3, mode r4, alertable r5, *timeout r6)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    uint32_t handle = ctx.r3.u32, obj = 0;
    if (handle == 0xFFFFFFFE) obj = PPC_LOAD_U32(ctx.r13.u32 + 0x100);   // current thread
    else { std::lock_guard<std::mutex> lk(g_handleMutex); auto it = g_handles.find(handle);
           if (it != g_handles.end()) obj = it->second->threadAddr; }
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

// "game:\media\foo" / "\Device\...\foo" / "d:\foo" -> <g_gameDir>/media/foo (best-effort).
std::string TranslatePath(std::string p) {
    size_t colon = p.find(':');
    std::string rel = (colon != std::string::npos) ? p.substr(colon + 1) : p;
    for (char& c : rel) if (c == '\\') c = '/';
    while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    return kernel::g_gameDir + "/" + rel;
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
    if (info) {
        if (infoClass == 14) {                          // FILE_POSITION_INFORMATION
            PPC_STORE_U64(info + 0, static_cast<uint64_t>(cur));
        } else {                                        // FILE_STANDARD/NETWORK_OPEN: report sizes
            PPC_STORE_U64(info + 0, static_cast<uint64_t>(size));   // AllocationSize
            PPC_STORE_U64(info + 8, static_cast<uint64_t>(size));   // EndOfFile
        }
    }
    if (ioStatus) { PPC_STORE_U32(ioStatus + 0, 0); PPC_STORE_U32(ioStatus + 4, 0); }
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
    uint32_t count = ctx.r3.u32, objects = ctx.r4.u32, waitType = ctx.r5.u32; // 0=WaitAll, 1=WaitAny
    int64_t timeoutMs = TimeoutMs(ctx.r9.u32);
    std::unique_lock<std::mutex> lk(g_waitMutex, std::adopt_lock);   // token already held by caller
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
    bool ok = true;
    if (timeoutMs < 0) g_waitCv.wait(lk, pred);
    else ok = g_waitCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), pred);
    if (!ok) { lk.release(); ctx.r3.u64 = 0x102; return; }   // keep the token; report STATUS_TIMEOUT
    int idx = firstReady();
    // Consume auto-reset/semaphore/mutant objects we're satisfied on.
    auto consume = [&](uint32_t o){ uint8_t t = g_base[o + 0x00];
        if (t == 1 || t == 2 || t == 5) GST32(o + 0x04, static_cast<int32_t>(GLD32(o + 0x04)) - 1); };
    if (waitType == 1) consume(GLD32(objects + idx * 4));
    else for (uint32_t i = 0; i < count; i++) consume(GLD32(objects + i * 4));
    lk.release();                                            // keep the token held as we resume
    ctx.r3.u64 = (waitType == 1) ? static_cast<uint32_t>(idx) : 0;   // WAIT_OBJECT_0 + idx
}
