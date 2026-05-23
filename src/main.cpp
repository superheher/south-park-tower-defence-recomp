// south_park_td - ReXGlue Recompiled Project

#include "generated/default/south_park_td_init.h"

#include "south_park_td_app.h"

// --- Crash diagnostics: symbolized backtrace on an unhandled fault ----------------------
// The rexglue runtime installs no crash handler, so a guest access violation just ends the
// log silently and cdb hangs on the live D3D12 window. SetUnhandledExceptionFilter fires
// only for genuinely-unhandled exceptions (after any runtime SEH), writing
// crash_backtrace.txt with the faulting address + a dbghelp-symbolized host stack (walked
// from the exception ContextRecord). In RelWithDebInfo this names the guest sub_XXXXXXXX
// frames, turning a silent 0xC0000005 into a located bug. Diagnostic aid; see KB 95.
#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#include <cstdio>
#pragma comment(lib, "dbghelp.lib")

static void RexSym(FILE* f, HANDLE proc, DWORD64 addr, int idx) {
  char symbuf[sizeof(SYMBOL_INFO) + 512] = {};
  auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
  sym->SizeOfStruct = sizeof(SYMBOL_INFO);
  sym->MaxNameLen = 511;
  DWORD64 disp = 0;
  if (SymFromAddr(proc, addr, &disp, sym))
    fprintf(f, "  [%2d] %s +0x%llX  (0x%llX)\n", idx, sym->Name,
            (unsigned long long)disp, (unsigned long long)addr);
  else
    fprintf(f, "  [%2d] 0x%llX\n", idx, (unsigned long long)addr);
}

static LONG WINAPI RexCrashHandler(EXCEPTION_POINTERS* ep) {
  const auto* er = ep->ExceptionRecord;
  FILE* f = nullptr;
  fopen_s(&f, "crash_backtrace.txt", "w");
  if (!f) return EXCEPTION_CONTINUE_SEARCH;
  fprintf(f, "Unhandled exception 0x%08lX at 0x%llX\n", er->ExceptionCode,
          (unsigned long long)reinterpret_cast<DWORD64>(er->ExceptionAddress));
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    const ULONG_PTR kind = er->ExceptionInformation[0];
    fprintf(f, "  %s access to 0x%llX\n",
            kind == 1 ? "WRITE" : (kind == 8 ? "EXEC" : "READ"),
            (unsigned long long)er->ExceptionInformation[1]);
  }
  HANDLE proc = GetCurrentProcess();
  SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
  SymInitialize(proc, nullptr, TRUE);
  fprintf(f, "Faulting frame:\n");
  RexSym(f, proc, reinterpret_cast<DWORD64>(er->ExceptionAddress), 0);
  // Walk the actual faulting stack from the exception context.
  fprintf(f, "Stack:\n");
  CONTEXT ctx = *ep->ContextRecord;
  STACKFRAME64 sf = {};
  sf.AddrPC.Offset = ctx.Rip;       sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = ctx.Rbp;    sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = ctx.Rsp;    sf.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
      break;
    if (sf.AddrPC.Offset == 0) break;
    RexSym(f, proc, sf.AddrPC.Offset, i);
  }
  fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;  // let the process die so the exit code still reflects the fault
}

namespace {
struct RexCrashInit {
  RexCrashInit() { SetUnhandledExceptionFilter(RexCrashHandler); }
};
RexCrashInit g_rexCrashInit;
}  // namespace
#endif

REX_DEFINE_APP(south_park_td, SouthParkTdApp::Create)
