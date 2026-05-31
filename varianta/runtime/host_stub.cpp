// Variant A — minimal host bring-up scaffold. NOT a working runtime yet (see README.md).
// Its only job right now is to give the link a `main` and the `base` pointer the recompiled
// code expects, so the project can LINK with the generated import trap-stubs.
#include "ppc_recomp_shared.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

// The recompiled code accesses guest memory as `base + guest_address` (see PPC_LOAD_*/PPC_STORE_*
// in ppc_context.h). PPC_MEMORY_SIZE is 4 GiB. A real runtime reserves this with mmap/VirtualAlloc.
uint8_t* g_base = nullptr;

// PPCFuncMappings[] (address -> host function) is defined in the generated ppc_func_mapping.cpp.
// PPC_LOOKUP_FUNC expects a pointer table laid out in guest memory at
//   base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (guest_addr - PPC_CODE_BASE) * 2.
// A real runtime populates that table from PPCFuncMappings during startup.

int main(int /*argc*/, char** /*argv*/)
{
    fprintf(stderr,
        "South Park: Let's Go Tower Defense Play! — variant A (XenonRecomp) host scaffold.\n"
        "Runtime not yet implemented. TODO (see runtime/README.md):\n"
        "  1. Reserve %lluB guest memory -> g_base.\n"
        "  2. Load private/extracted/default.xex sections at g_base+PPC_IMAGE_BASE.\n"
        "  3. Build the func-pointer lookup table from PPCFuncMappings[] (PPC_LOOKUP_FUNC layout).\n"
        "  4. Implement the %d kernel/xam imports (see IMPORTS-TODO.md; ref rexglue-sdk/src).\n"
        "  5. Init PPCContext, set up stack/TLS, call the guest entry point.\n",
        (unsigned long long)PPC_MEMORY_SIZE, 474);
    return 0;
}
