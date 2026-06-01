// Variant A — kernel/xam import implementations (strong symbols; override the weak trap-stubs in
// import_stubs.gen.cpp). Behaviour reference: third_party/rexglue-sdk/src/ + Xenia.
// ABI: PPC_FUNC(__imp__Name)(PPCContext& ctx, uint8_t* base); args in ctx.r3,r4,...; NTSTATUS/ret in ctx.r3.
// Guest pointers are guest addresses — dereference via PPC_LOAD_*/PPC_STORE_* (base + addr, byte-swapped).
#include "ppc_recomp_shared.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Lightweight import trace (set REX_KTRACE=0 to silence).
static const bool g_ktrace = []{ const char* e = getenv("REX_KTRACE"); return !e || e[0] != '0'; }();
#define KTRACE(...) do { if (g_ktrace) { fprintf(stderr, "[kernel] " __VA_ARGS__); } } while (0)

// ---- minimal guest heap (bump allocator) ----------------------------------------------------------
// A region inside the 4 GiB guest space, below the image base (0x82000000). Enough for early boot;
// a real allocator (free lists, NtFreeVirtualMemory) comes later.
static uint32_t g_heapNext = 0x40000000;
static uint32_t GuestAlloc(uint32_t size, uint32_t align)
{
    if (align < 0x1000) align = 0x1000;
    g_heapNext = (g_heapNext + (align - 1)) & ~(align - 1);
    uint32_t addr = g_heapNext;
    g_heapNext += (size + 0xFFF) & ~0xFFFu;
    return addr;
}

// NTSTATUS NtAllocateVirtualMemory(ProcessHandle, *BaseAddress, ZeroBits, *RegionSize, AllocType, Protect)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    uint32_t pBase = ctx.r4.u32;
    uint32_t pSize = ctx.r6.u32;
    uint32_t reqBase = pBase ? PPC_LOAD_U32(pBase) : 0;
    uint32_t reqSize = pSize ? PPC_LOAD_U32(pSize) : 0;
    uint32_t size = (reqSize + 0xFFFF) & ~0xFFFFu;     // 64 KiB granularity
    if (size == 0) size = 0x10000;
    uint32_t addr = reqBase ? reqBase : GuestAlloc(size, 0x10000);
    if (pBase) PPC_STORE_U32(pBase, addr);
    if (pSize) PPC_STORE_U32(pSize, size);
    KTRACE("NtAllocateVirtualMemory -> 0x%X (%u B)\n", addr, size);
    ctx.r3.u64 = 0;  // STATUS_SUCCESS
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
