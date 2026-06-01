// Variant A host runtime — first bring-up (TASK 4 → boot).
// Reserves the 4 GiB guest address space, loads + parses the XEX, maps its sections, populates the
// indirect-call dispatch table from PPCFuncMappings[], sets up a guest stack, and calls the guest
// entry point. Kernel/xam imports are still trap-stubs (import_stubs.gen.cpp): the first one the boot
// sequence reaches prints its name and traps, revealing what to implement next.
#include "ppc_recomp_shared.h"
#include "kernel.h"  // kernel::SetupEnvironment
#include <image.h>   // XenonUtils: Image::ParseImage (decrypts/decompresses the XEX)
#include <file.h>    // XenonUtils: LoadFile
#include <sys/mman.h>
#include <cstdio>
#include <cstdint>
#include <cstring>

uint8_t* g_base = nullptr;

static PPCFunc* LookupHost(uint32_t guestAddr)
{
    for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
        if (static_cast<uint32_t>(PPCFuncMappings[i].guest) == guestAddr)
            return PPCFuncMappings[i].host;
    return nullptr;
}

int main(int argc, char** argv)
{
    const char* xexPath = argc > 1 ? argv[1] : "../private/extracted/default.xex";

    // The guest filesystem maps to the directory containing the XEX (the extracted game dir).
    { std::string p = xexPath; size_t s = p.find_last_of('/');
      kernel::g_gameDir = (s == std::string::npos) ? "." : p.substr(0, s); }

    // 1. Reserve the full 4 GiB guest address space (lazy-committed via MAP_NORESERVE).
    g_base = static_cast<uint8_t*>(mmap(nullptr, PPC_MEMORY_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    if (g_base == MAP_FAILED) { perror("mmap guest memory"); return 1; }

    // 2. Load + parse (decrypt/decompress) the XEX.
    auto file = LoadFile(xexPath);
    if (file.empty()) { fprintf(stderr, "cannot read %s\n", xexPath); return 1; }
    Image image = Image::ParseImage(file.data(), file.size());
    fprintf(stderr, "[loader] base=0x%zX size=0x%X entry=0x%zX sections=%zu\n",
        image.base, image.size, image.entry_point, image.sections.size());

    // 3. Map every section into guest memory at its absolute address. The parsed image buffer only
    //    spans [base, base+size); sections that straddle/exceed it (e.g. .reloc — load-time
    //    relocation data not needed at runtime) are clamped so we never read past the source buffer.
    //    Sections with no file data (e.g. .bss) are skipped — guest memory is already zero.
    // The XEX image buffer (xex.cpp) is sized from the decompressed blocks, which can be smaller than
    // image.size; .reloc straddles that tail and would read past the source. Skip PE metadata sections
    // that aren't guest runtime data (.reloc relocations, .pdata exception tables — XenonRecomp models
    // SEH via setjmp/longjmp, not .pdata).
    auto isMetadata = [](const std::string& n) { return n == ".reloc" || n == ".pdata"; };
    for (const auto& s : image.sections)
    {
        bool skip = isMetadata(s.name) || s.data == nullptr || s.size == 0;
        fprintf(stderr, "[loader]   %-12s 0x%zX..0x%zX (%7u B)%s\n",
            s.name.c_str(), s.base, s.base + s.size, s.size, skip ? "  [skipped]" : "");
        if (!skip)
            memcpy(g_base + s.base, s.data, s.size);
    }

    // 4. Populate the indirect-call dispatch table (PPC_LOOKUP_FUNC layout) from PPCFuncMappings[].
    fprintf(stderr, "[loader] sections mapped; populating dispatch table at guest 0x%llX...\n",
        (unsigned long long)(PPC_IMAGE_BASE + PPC_IMAGE_SIZE));
    size_t nfuncs = 0;
    for (size_t i = 0; PPCFuncMappings[i].host != nullptr; i++)
    {
        uint32_t addr = static_cast<uint32_t>(PPCFuncMappings[i].guest);
        *reinterpret_cast<PPCFunc**>(g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE
            + (uint64_t(addr - PPC_CODE_BASE) * 2)) = PPCFuncMappings[i].host;
        ++nfuncs;
    }
    fprintf(stderr, "[loader] mapped %zu sections, %zu functions into the dispatch table\n",
        image.sections.size(), nfuncs);

    // 5. Resolve the entry host function.
    PPCFunc* entry = LookupHost(static_cast<uint32_t>(image.entry_point));
    if (entry == nullptr)
    {
        fprintf(stderr, "[loader] ERROR: entry 0x%zX has no recompiled function\n", image.entry_point);
        return 1;
    }

    // 6. Build the guest boot environment: title X_KPROCESS + main-thread X_KTHREAD + X_KPCR + the
    //    static TLS block (initialized from the XEX TLS directory). r13 = KPCR; the CRT walks
    //    KPCR -> prcb_data.current_thread (KTHREAD) -> process (KPROCESS) and reads the per-thread TLS
    //    block, the stack bounds, and the process TLS slot bitmap. See kernel.cpp SetupEnvironment.
    constexpr uint32_t STACK_TOP = 0x70100000, STACK_BOTTOM = 0x70000000;
    uint32_t kpcr = kernel::SetupEnvironment(file.data(), STACK_TOP, STACK_BOTTOM,
        static_cast<uint32_t>(image.entry_point));

    static PPCContext ctx{};                     // msr defaults; everything else zero
    ctx.fpscr.csr = 0x1F80;                      // default MXCSR: ALL FP exceptions masked (else the
                                                 // guest's setcsr clears the mask bits -> fctidz traps)
    ctx.r1.u64 = STACK_TOP - 0x200;              // SP with a little headroom; back-chain is zeroed
    ctx.r13.u64 = kpcr;                          // KPCR pointer (Xbox 360 thread-base convention)

    fprintf(stderr, "[boot] calling guest entry 0x%zX (SP=0x%llX r13=0x%X)...\n",
        image.entry_point, (unsigned long long)ctx.r1.u64, kpcr);
    kernel::LockGuestExecution();   // main thread holds the cooperative execution token while running
    entry(ctx, g_base);
    kernel::UnlockGuestExecution();
    fprintf(stderr, "[boot] guest entry returned (r3=0x%llX).\n", (unsigned long long)ctx.r3.u64);
    return 0;
}
