#pragma once
// Shared host-runtime state between the loader (runtime.cpp) and the kernel imports (kernel.cpp).
#include <cstdint>

extern uint8_t* g_base;   // guest memory base (4 GiB), defined in runtime.cpp

namespace kernel {

// Guest addresses of the boot environment the host provides (set by SetupEnvironment).
extern uint32_t g_kpcrAddr;        // X_KPCR    (loaded into r13)
extern uint32_t g_mainThreadAddr;  // X_KTHREAD (main thread)
extern uint32_t g_processAddr;     // X_KPROCESS (title process)

// Build the title process, the main-thread KTHREAD, the KPCR, and the static TLS block (the latter
// initialized from the XEX TLS directory — XEX_HEADER_TLS_INFO). Returns the KPCR guest address.
// xexFile = raw XEX bytes (for reading optional headers via getOptHeaderPtr).
// stackBase = high address of the guest stack, stackLimit = low address. startAddress = guest entry.
uint32_t SetupEnvironment(const uint8_t* xexFile, uint32_t stackBase, uint32_t stackLimit,
                          uint32_t startAddress);

} // namespace kernel
