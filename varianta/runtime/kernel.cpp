// Variant A — kernel/xam import implementations (strong symbols; override the weak trap-stubs in
// import_stubs.gen.cpp). Behaviour reference: third_party/rexglue-sdk/src/ + Xenia.
// ABI: PPC_FUNC(__imp__Name)(PPCContext& ctx, uint8_t* base); args in ctx.r3,r4,...; NTSTATUS/ret in ctx.r3.
// Guest pointers are guest addresses — dereference via PPC_LOAD_*/PPC_STORE_* (base + addr, byte-swapped).
#include "ppc_recomp_shared.h"
#include <cstdint>
#include <cstdio>

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
    ctx.r3.u64 = 0;  // STATUS_SUCCESS
}

// DWORD KeGetCurrentProcessType(void) -> 1 (X_PROCTYPE_TITLE)
PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    ctx.r3.u64 = 1;
}
