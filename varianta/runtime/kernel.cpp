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
#include <utility>

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
uint32_t g_virtNext = 0x40000000;        // bump cursor for kernel-chosen bases (user virtual range)

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

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    // single-thread: no contention, nothing to do.
}

PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    // single-thread: no-op.
}

// BOOLEAN RtlTryEnterCriticalSection(cs) -> always acquires (single-thread)
PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = 1;
}

// ====================================================================================================
// Boot environment: title X_KPROCESS + main-thread X_KTHREAD + X_KPCR + static TLS block.
// Layouts are 1:1 with rexglue-sdk (Xenia-derived): include/rex/system/{xthread.h,kernel_state.h};
// behaviour mirrors KernelState::{Setup,InitializeProcess,SetProcessTLSVars} +
// XThread::{Create,InitializeGuestObject}. The recompiled CRT reads r13=KPCR → current_thread (KTHREAD)
// → process (KPROCESS), the per-thread TLS block, the stack bounds, and the process TLS slot bitmap.
// ====================================================================================================
namespace {
// Dedicated kernel arena (far from the guest virtual bump at 0x40000000 and the XEX image at
// 0x82000000); the full 4 GiB is lazily mmap'd so these addresses are already backed.
constexpr uint32_t kProcessAddr = 0x60000000;
constexpr uint32_t kThreadAddr  = 0x60001000;
constexpr uint32_t kKpcrAddr    = 0x60002000;
constexpr uint32_t kTlsAddr     = 0x60003000;  // per-thread TLS block (main thread)
constexpr uint32_t kDefaultTlsSlots = 1024;
constexpr uint32_t kProctypeUser = 1;          // X_PROCTYPE_USER (KeGetCurrentProcessType == 1)

// Big-endian guest stores/loads via the raw base (used during setup, no PPCContext in scope).
inline void GST8 (uint32_t a, uint8_t  v) { g_base[a] = v; }
inline void GST16(uint32_t a, uint16_t v) { uint16_t b = __builtin_bswap16(v); memcpy(g_base + a, &b, 2); }
inline void GST32(uint32_t a, uint32_t v) { uint32_t b = __builtin_bswap32(v); memcpy(g_base + a, &b, 4); }
inline void GST64(uint32_t a, uint64_t v) { uint64_t b = __builtin_bswap64(v); memcpy(g_base + a, &b, 8); }
inline uint32_t GLD32(uint32_t a) { uint32_t b; memcpy(&b, g_base + a, 4); return __builtin_bswap32(b); }

// Read a XEX optional header that stores its value INLINE (key low byte 0) as a BE u32.
uint32_t XexOptU32(const uint8_t* xex, uint32_t key, uint32_t fallback) {
    const void* p = getOptHeaderPtr(xex, key);
    if (!p) return fallback;
    uint32_t b; memcpy(&b, p, 4); return __builtin_bswap32(b);
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

    // ---- X_KTHREAD (main thread) — InitializeGuestObject --------------------------------------------
    const uint32_t T = kThreadAddr;
    memset(g_base + T, 0, 0xAB0);
    GST8 (T + 0x00, 6);                                      // header.type = ThreadObject
    GST32(T + 0x10, T + 0x10); GST32(T + 0x14, T + 0x10);   // self-ref ptrs
    GST32(T + 0x40, T + 0x20); GST32(T + 0x44, T + 0x20);   // wait_timeout_block.list_entry
    GST32(T + 0x48, T);        GST32(T + 0x4C, T + 0x18);   // .thread, .object
    GST16(T + 0x54, 0x0100);   GST16(T + 0x56, 0x0201);     // .wait_result_xstatus, .wait_type
    GST32(T + 0x5C, stackBase);                              // stack_base (high)
    GST32(T + 0x60, stackLimit);                             // stack_limit (low)
    GST32(T + 0x64, stackBase - 240);                        // stack_kernel
    GST32(T + 0x68, tlsDynamic);                             // tls_address (dynamic TLS base)
    GST8 (T + 0x6C, 2);                                      // thread_state = RUNNING
    GST32(T + 0x74, T + 0x74); GST32(T + 0x78, T + 0x74);   // apc_lists[0]: self-ref
    GST32(T + 0x7C, T + 0x7C); GST32(T + 0x80, T + 0x7C);   // apc_lists[1]: self-ref
    GST8 (T + 0x72, kProctypeUser); GST8(T + 0x73, kProctypeUser); // process_type(_dup)
    GST32(T + 0x84, P);                                      // process
    GST8 (T + 0x8B, 1);                                      // may_queue_apcs
    GST32(T + 0x9C, 0xFDFFD7FFu);                            // msr_mask
    GST32(T + 0xC0, kKpcrAddr + 0x100); GST32(T + 0xC4, kKpcrAddr + 0x100); // a/another_prcb_ptr
    GST32(T + 0xD0, stackBase);                              // stack_alloc_base
    GST32(T + 0x110, T + 0x110); GST32(T + 0x114, T + 0x110); // process_threads (relinked below)
    GST32(T + 0x144, T + 0x144); GST32(T + 0x148, T + 0x144); // timer_list: self-ref
    GST32(T + 0x14C, 1);                                     // thread_id (main = 1)
    GST32(T + 0x150, startAddress);                          // start_address
    GST32(T + 0x154, T + 0x154); GST32(T + 0x158, T + 0x154); // unk_154: self-ref
    GST32(T + 0x17C, 1);                                     // unk_17C

    // Link the main thread into the process thread list (XeInsertTailList: list was self-ref).
    GST32(T + 0x110, P + 0x04);   // entry->flink = head
    GST32(T + 0x114, P + 0x04);   // entry->blink = head (head->blink was head itself)
    GST32(P + 0x04, T + 0x110);   // head->flink = entry
    GST32(P + 0x08, T + 0x110);   // head->blink = entry
    GST32(P + 0x14, 1);           // thread_count = 1

    // ---- X_KPCR -------------------------------------------------------------------------------------
    const uint32_t K = kKpcrAddr;
    memset(g_base + K, 0, 0x2D8);
    GST32(K + 0x00, kTlsAddr);            // tls_ptr (static TLS / block start; this is *(r13))
    GST8 (K + 0x18, 0);                   // current_irql = PASSIVE
    GST64(K + 0x30, K);                   // pcr_ptr
    GST32(K + 0x70, stackBase);           // stack_base_ptr
    GST32(K + 0x74, stackLimit);          // stack_end_ptr
    GST32(K + 0x100, T);                  // prcb_data.current_thread
    GST32(K + 0x2A8, K + 0x100);          // prcb -> &prcb_data

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
