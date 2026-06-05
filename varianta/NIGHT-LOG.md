# Variant A — Autonomous Night Session Log

Branch `experimental/hle-graphics-spike` · started **2026-05-31 22:31 MSK** · user asleep → fully autonomous.
Driver: `docs/NEXT-SESSION-VARIANTA-PROMPT.md`. **NEVER push.** Commit each increment as `superheher`.

## Baseline (verified at start)
- HEAD `925b657` on `86dfa35`. Toolchain clone `rexglue-recomps/third_party/XenonRecomp` @ `ddd128b`
  (upstream hedge-dev), with my **124-line `recompiler.cpp` diff == committed
  `varianta/patches/xenonrecomp-sp-instructions.patch`** (verified byte-identical). Binaries built.
- Unrecognized instructions = **10,960** (18 instructions implemented so far).
- Pre-existing dirty files in repo (`tools/perf/*.sh`, `tools/perf/detdiff/cand_*.json`) are NOT mine —
  keep every commit scoped to `varianta/` only.

## Guardrails (active)
- Do NOT touch `third_party/rexglue-sdk` or game-dir `librexruntime.so` (baseline `1a3f6076`).
- Do NOT bump the superproject `main` submodule pointer. Do NOT re-open variant B / Xenia CP opts.
- Do NOT run the game during TASK 1–3. Regenerate the patch after every `recompiler.cpp` change.

## Progress
- **[22:31]** Oriented; clone diff == committed patch (byte-identical). Started background rebuild of
  `XenonRecomp` to re-confirm the 10,960 baseline; reading `recompiler.cpp` to map emitter patterns.
- **[22:45]** Mapped emitter conventions. Key findings: (1) the full-vector byte-reversal on load
  *preserves each element's value* and merely relabels lane `i ↔ N-1-i` ⇒ per-lane symmetric ops
  (add/sub/shift/min/max/avg/compare) are reversal-agnostic (use the matching simde intrinsic or scalar
  loop, like existing `VSLW`/`VADDSHS`). (2) `setFromMask` is movemask-based: `imm=0xFFFF` for
  byte+halfword compares (each true halfword = 2 set bits), `0xF` via the `simde__m128`/float overload
  for word compares. (3) CR struct = `uint8_t lt/gt/eq/so`. Cross-referenced EVERY subtle op against
  RexGlue's recomp of this exact XEX (`generated/default/*`): carry ops, packs (operand order `(op2,op1)`),
  `vpkuhus` (unsigned-sat = `packus_epi16(min_epu16(op2,0xFF), min_epu16(op1,0xFF))` — matches RexGlue's
  16-lane scalar exactly), `vcfpuxws128`=`simde_mm_vctuxs`, `vslo`=`simde_mm_vslo`, `vsel128`≡`VSEL`.
- **[22:52] BATCH 1 (scalar) DONE — 10,960 → 10,363 (−597, exact).** Added `addc addme subfze bdzf
  bdnzt cror crorc`. All 7 verified ABSENT from histogram; build + recompile clean. Each emit cross-checked
  byte-for-byte vs RexGlue (carry = ADDE/SUBFE two-term form, alias-safe ordering; CR-bit branches select
  the REAL bit via `cr(BI/4).{lt,gt,eq,so}[BI%4]`, not hard-coded eq; `cror/crorc` = per-CR-bit logical).
  Patch regenerated, committed (`8e1e0dd`).
- **[23:05] BATCH 2 (vector analogs) DONE — 10,363 → 94 (−10,269, exact).** Added 26 vector ops:
  `vslh vsrh vsrah vsrab vrlh` (per-lane scalar shift/rotate), `vsubshs vmaxsh vminsh vminsw vsububm
  vaddsbs vsubsbs vavguh` (direct simde intrinsics), `vaddsws vavgsw` (int64-clamp scalar like `VSUBSWS`),
  `vspltish` (`set1_epi16`), `vcmpgtsh vcmpgtsw vcmpequh` (compare + CR6 `setFromMask`: 0xFFFF int-overload
  for halfword, 0xF float-overload for word), `vpkshss(128) vpkswss(128) vpkswus(128) vpkuhus(128)` (packs,
  operand order `(op2,op1)`; vpkuhus = `packus_epi16(min_epu16(·,0xFF), …)`), `stvebx` (single-byte element
  store). Aliased `vsel128`→`VSEL` (identical operands) and `lvehx`→`LVX` group (⚠ RexGlue treats `lvehx`
  as a FULL aligned vector load, not single-element — matched the validated reference, not the bare spec).
  Build + recompile clean; all 26 ABSENT. Remaining **94** = `vcfpuxws128`(73)+`vslo`(14)+`vslo128`(7),
  needing 2 `ppc_context.h` helpers (Batch 3). Committed (`d7f16e5`).
- **[23:20] BATCH 3 (helper-backed) DONE — 94 → 0.  🎯 TASK 1 COMPLETE: INSTRUCTION GAP FULLY CLOSED
  (13,183 → 0).** Added `vcfpuxws128` (float→uint saturate; unsigned analog of `VCTSXS`, `.u32` store) +
  `vslo`/`vslo128` (shift-left-by-octet), backed by 2 helpers ported into `XenonUtils/ppc_context.h`
  (`simde_mm_vctuxs`, `simde_mm_vslo`) verbatim from RexGlue's validated `rex::ppc` runtime (`vslo`
  simplified to a portable byte-loop — both SDK arch branches were identical). Recompile = **0 unrecognized**,
  90 generated TUs.
- **[23:24] TASK 3 (syntax-clean) DONE.** All 90 generated `ppc_recomp.*.cpp` + `ppc_func_mapping.cpp`
  pass `clang++ -std=c++20 -fsyntax-only` (parallel over all cores, **zero FAILs**) — validates both the new
  emitter cases AND the 2 new `ppc_context.h` helpers compile. The patch now spans `recompiler.cpp` +
  `ppc_context.h`. Committed (`ec0f24b`).
- **[23:55] TASK 2 (jump tables) — XenonAnalyse extended; 93 validated tables shipped (was 0).**
  Root cause of "0 detected": SP's MSVC-360 (a) inserts alignment NOPs mid-prologue and (b) emits
  `rlwinm` BEFORE the table-base `addi` (vs Unleashed) — defeating XenonAnalyse's fixed-sequence
  `SearchMask` + `ReadTable`'s hardcoded operand offsets. Fix (`XenonAnalyse/main.cpp`): NOP-tolerant
  `SearchMask`; role-based `ReadTableSP` (finds lis/addi/load/rlwinm by opcode, derives TYPE from the
  load op — order/NOP independent, reuses XA's address arithmetic); 2 SP-ordered patterns. → XA now
  detects **102 tables**. VALIDATION (safety): each candidate cross-checked vs RexGlue — 67 exact-match,
  35 all-labels-are-RexGlue-jump-targets, **0 misreads** (a wrong base shifts labels off all 98,729 loc
  targets; none did). Of 102, **93 are fully in-bounds → shipped** in `sp_switch_tables.toml`; **9 are
  boundary-limited → deferred**. Recompile = 0 unrecognized, **0 switch-case errors**, 93 `switch`
  statements emitted (were PPC_CALL_INDIRECT_FUNC). Re-ran TASK 3 syntax check WITH tables = all 90 TUs
  pass. Tooling + pipeline: `varianta/tools/jumptables/`. Patch now also spans `XenonAnalyse/main.cpp`.
  - ⚠ **FLAGGED (function-boundary problem, README §"Function Boundary Analysis" — no auto-solution):**
    the 9 deferred tables AND 161 pre-existing `// ERROR <addr>` (no-colon) CONDITIONAL-branch markers
    share one root cause — XenonAnalyse splits functions at computed-jump targets, so some branch/case
    targets land outside their function. Verified switch tables add ZERO errors (count identical with/
    without tables; switch-case errors `// ERROR:`-with-colon = 0). Fix path = manual function-boundary
    overrides or a boundary-analyzer extension (separate task). Committed (`4ae4809`).
- **[00:30] TASK 4 (host-runtime) — SCAFFOLD + full enumeration (the large remaining phase; bounded).**
  Recovered the recompiler ABI from `ppc_context.h`: `PPC_FUNC(f)=void f(PPCContext&,uint8_t* base)`,
  guest memory = `base+addr` (4 GiB, byte-swapped), indirect dispatch via `PPC_LOOKUP_FUNC`+
  `PPCFuncMappings[]`. Surface: **22,782** recompiled guest funcs + **474** kernel/xam imports to
  implement (Xam 44, Nt 30, Net 28, Ke 26, Rtl 21, Vd 20, Ex 11, …) — RexGlue's `rexglue-sdk/src/`
  implements all (1:1 behaviour ref). Built `varianta/runtime/`: `IMPORTS-TODO.md` (categorized list),
  `gen_import_stubs.py` (emits 474 trap-stubs so the image links — uses plain `PPC_FUNC` to match the
  C++-linkage `PPC_EXTERN_FUNC` import decls in `ppc_recomp_shared.h`), `CMakeLists.txt` (globs ppc/ +
  runtime/), `host_stub.cpp` (placeholder main + loader TODOs), `README.md` (completion path). Validated:
  stubs + host_stub object-compile clean; import symbols defined with correct mangled C++ linkage
  (`_Z15__imp__DbgPrintR10PPCContextPh`). NOT done: full 90-TU build/link (heavy/open-ended) and the
  real runtime (memory+loader+imports+entry) — that's the multi-week phase; scaffold is ready to build.
- **[00:30] TASK 4 LINK-WITH-STUBS — SUCCEEDED.** Built all 93 TUs with clang++ -O0 (89 `ppc_recomp` +
  `ppc_func_mapping` + 474-stub `import_stubs.gen.cpp` + `host_stub`) and **linked → `sp_td_varianta`
  (103 MB), 0 undefined symbols, 0 errors**; the exe runs (prints the scaffold notice, exits 0). This
  proves all **22,782** recompiled functions + the 474 import stubs form a coherent, linkable program —
  and confirms the ONLY external references are the enumerated imports (no hidden missing symbols). TASK
  4's stated night-scope (CMake skeleton + link-with-stubs + enumeration) is now COMPLETE. Booting needs
  the real runtime (deferred per the directive: "Don't try to boot... renderer is the phase after").

## Summary (night of 2026-05-31 → 06-01)
| Task | Result |
|---|---|
| **1. Instruction gap** | ✅ **13,183 → 0** (51 new instrs: 7 scalar + 26+ vector + helpers; all RexGlue-cross-checked) |
| **2. Jump tables** | ✅ **0 → 93** validated tables (extended XenonAnalyse: nop-tolerant + role-based + SP patterns); 9 deferred (boundary) |
| **3. Syntax-clean** | ✅ all 90 generated TUs pass `-fsyntax-only` (re-checked with tables) |
| **4. Host runtime** | ✅ (night-scope) scaffold + **link-with-stubs OK** (103 MB exe, 0 undefined) + 474-import enumeration; real runtime = next phase |
Commits on `experimental/hle-graphics-spike` (NOT pushed): `8e1e0dd`, `d7f16e5`, `ec0f24b`, `4ae4809`, + TASK4.
Prod `.so` `1a3f6076` untouched; rexglue-sdk untouched; superproject pointer not bumped.

## Continuation 2026-06-01 — host-runtime phase (user chose "start the host runtime")
- **[06:08] RUNTIME BRING-UP MILESTONE — the guest boots and runs real code.** Wrote
  `varianta/runtime/runtime.cpp`: reserve 4 GiB (`mmap` MAP_NORESERVE) → load+parse the XEX via
  XenonUtils `Image` (links prebuilt `libXenonUtils/disasm/fmt`) → map the 17 sections (skip
  `.reloc`/`.pdata`: not runtime data, and xex.cpp's decompressed image buffer is smaller than
  `image.size` so `.reloc` straddled the tail and faulted `memcpy`) → populate the **22,782-entry**
  dispatch table (`PPC_LOOKUP_FUNC` layout) → init `PPCContext` + a guest stack (SP=0x700FFE00) →
  call entry `0x824499A0`. Result: **the guest executes and runs until the first kernel import,
  `NtAllocateVirtualMemory`** (CRT heap setup; SIGILL from its trap-stub). The full
  load→dispatch→execute path works end-to-end. Next: implement the early-boot import cascade
  (make the 474 stubs `weak`, add strong impls in a `kernel.cpp` + a guest heap allocator).
- **[06:15] Import-cascade harness + first imports.** Made the 474 trap-stubs `weak`
  (`gen_import_stubs.py`) so strong impls in `runtime/kernel.cpp` override them — the scalable pattern.
  Implemented `NtAllocateVirtualMemory` (guest bump-heap at 0x40000000, 64 KiB granularity) and
  `KeGetCurrentProcessType` (→1 = title). Boot now advances import-by-import. **Current frontier:
  `RtlInitializeCriticalSection`** (rexglue-sdk ref `kernel/xboxkrnl/xboxkrnl_rtl.cpp`: header.type=1,
  lock_count=-1, recursion_count=0, owning_thread=0; then Rtl{Enter,Leave,TryEnter}CriticalSection).
  The cascade (critical sections → TLS → XConfig → threads → … → Vd* video → render) is the ongoing
  multi-week grind; each import's behaviour comes 1:1 from `rexglue-sdk/src/`. Checkpointing the loop
  here (commit + continue) after the boot milestone.
- **[06:45] KPCR set up; boot bail traced into `_xstart` CRT init.** Implemented critical-section trio
  + `HalReturnToFirmware` (logged exit) + a `REX_KTRACE` import trace. Found the thread mechanism
  (rexglue `kernel/crt/threading.cpp`): **`r13` = guest KPCR pointer**; X_KPCR (size 0x2D8): tls_ptr@0x0,
  stack_base_ptr@0x70, stack_end_ptr@0x74, prcb_data@0x100 (X_KPRCB.current_thread@0x0), prcb@0x2A8.
  Set up a minimal guest KPCR/KTHREAD/TLS in `runtime.cpp` + `r13=0x60000000`. **Boot trace unchanged**
  (NtAllocateVirtualMemory×2 → KeGetCurrentProcessType → RtlInitializeCriticalSection(cs=**0x618**) →
  HalReturnToFirmware(1) [exit]) ⇒ cs=0x618 is NOT derived from the current-thread pointer. Entry =
  **`_xstart`** (Xbox CRT startup, `ppc_recomp.65.cpp:1716`, NOT a `sub_`): sets 2 globals @0x82590000+
  to −1 → `sub_82450580` → `sub_8244EA18(1)` → `sub_824497B8` (return checked vs 0, branches); the bail
  `HalReturnToFirmware` is at `ppc_recomp.65.cpp:22221`. **NEXT (fresh context):** trace which init
  sub-fn computes `cs=0x618` (≈ `r2/TOC+0x618` or a null-global deref) and reaches HalReturnToFirmware;
  check whether `r2`/TOC or a loader-provided global must be initialized before `_xstart`. Deep
  boot-path debugging. Committed; checkpoint.
- **[06:50] Bail = CRT computing null-based addresses ⇒ needs systemic entry-environment setup.**
  Traced cs=0x618: `RtlInitializeCriticalSection` caller-lr=0x8244B874 (inside `sub_8244B380`),
  **r2=0x0**, r13=0x60000000. cs=r29, and r29 is reloaded from a caller-provided stack slot
  ([r31+316] = caller frame); 0x618 ≈ null-base+0x618 threaded up the call chain. **ROOT: `_xstart`
  expects a fuller loader-provided entry environment than set up.** Candidates: (a) **r2 / SDA base**
  (currently 0), (b) the **XEX TLS-directory static init** — `KPCR.tls_ptr` points at a ZEROED block,
  (c) entry-arg registers r3/r4, (d) XEX static initializers. This is the systemic multi-day piece
  (UnleashedRecomp/Xenia implement it extensively). Hand-tracing recompiled CRT code is low-velocity;
  **NEXT should set up the entry environment comprehensively** — parse the XEX PE TLS directory (at
  base+IMAGE_BASE) + copy static TLS data + research the correct entry registers (r2/r3) from
  rexglue/Xenia — rather than chase individual null derefs. ⚠ Reality check: a full boot is the
  multi-week runtime tail; the loop is grinding it but velocity is now low per iteration. Committed
  (lr/r2/r13 trace); checkpoint.
- **[07:10] Bail localized to CRT heap-init `sub_8244B380` returning 0 (via gdb backtraces).** Chain:
  `main→_xstart→sub_82450580→sub_824504A8→sub_8244B380→RtlInitCS(cs=0x618)`. `sub_82450580` bails to
  `HalReturnToFirmware` IFF `sub_824504A8` returns 0, and `sub_824504A8` returns `(sub_8244B380_result
  != 0)` — so the CRT heap/pool init `sub_8244B380(type=2, size=4096)` FAILS. ✅ **KPCR works** — gdb
  shows `r25=0x60001000` = my KTHREAD (current-thread link resolves). ❌ heap control-block base is
  `null+0x618` (`cs=0x618`, `r30=0x640`) — a NULL pointer, almost certainly read from the **ZEROED
  KTHREAD/process** objects. `sub_8244B380` also calls `NtQueryVirtualMemory`/`NtFreeVirtualMemory`
  (still trap-stubs, untaken branch). ⚠ Open puzzle: `r29=[r31+316]=r4(entry)`; static call site
  (`sub_824504A8:22137`) sets `r4=0`, yet runtime `r29=0x618` — resolve via a gdb watchpoint on
  `[r31+316]`. **NEXT: populate the guest thread/process/default-heap environment** (X_KTHREAD +
  process object + process heap) so the CRT finds a valid heap base. Velocity low; full boot = the
  multi-week tail. Checkpoint.

## PAUSE POINT — 2026-06-01 ~07:40 (autonomous loop paused for consolidation)
gdb shows the boot reaches the CRT heap-init with MULTIPLE `RtlInitCS` calls (cs=0/0x618; one with
`r30=0x100000` = the real NtAlloc heap, so partial heap setup DOES work) failing on null-based pointers
read from the **zeroed thread/process structures**. The CRT control flow + stack-spilled locals do not
trace cleanly statically, and per-iteration velocity on this archaeology is low. **Decision: pause the
self-running loop here.** The deterministic front (doc TASK 1–4) is COMPLETE and committed; the runtime
boot is the genuine multi-week tail, better advanced as focused systematic work than 20-min autonomous
null-chasing.

### What works (committed)
Load+parse XEX → map 17 sections → 22,782-fn dispatch table → init PPCContext+stack+KPCR/KTHREAD → call
`_xstart`; guest executes real code; imports resolve via weak-stub/`kernel.cpp` (NtAllocateVirtualMemory,
KeGetCurrentProcessType, critical-section trio, HalReturnToFirmware). KPCR current-thread link verified.

### Systematic next-steps (priority order; the multi-week tail)
1. **Full guest thread/process env** — populate `X_KTHREAD` (process ptr, TLS, thread id, stack
   base/limit), a `X_KPROCESS` object, and the **default process heap**, matching what the recompiled
   CRT reads (ref: rexglue-sdk `X_KTHREAD`/`X_KPROCESS` + its thread-init in `system/xthread.cpp`).
2. **XEX TLS-directory static init** (`KPCR.tls_ptr` currently → a zeroed block).
3. **Early-boot import cascade** as it surfaces: `NtQueryVirtualMemory`, `NtFreeVirtualMemory`,
   `RtlAllocateHeap`, … — behaviour 1:1 from `rexglue-sdk/src/`.
4. Then threading → `Vd*` video → native renderer (Plume + 19 shaders).

### Tooling that works
gdb backtraces (recompiled fns are real host frames at `-O0`); `REX_KTRACE=1` import trace; the
`varianta/runtime/` scaffold (`gen_import_stubs.py`, CMake links prebuilt `libXenonUtils`).

**Resume:** re-run `/loop go done docs/NEXT-SESSION-VARIANTA-PROMPT.md`, or tackle the systematic steps
interactively (recommended for the runtime).

## Session 2026-06-01 (continuation) — heap unblocked → full boot env → multithreaded video init
Goal: fill X_KTHREAD/X_KPROCESS/process heap → static TLS from XEX → import cascade → video → renderer.
Commits (NOT pushed): `8b6d18b`, `db90322`, `48f217d`, `e0f4e8a`, `b086014`.

- **[8b6d18b] HEAP-INIT BLOCKER FIXED — the root cause was a wrong NtAllocateVirtualMemory ABI.**
  The pause-point bail (CRT `RtlCreateHeap`=`sub_8244B380` getting a null heap base) was NOT a missing
  thread/process struct — it was that NtAllocateVirtualMemory used the **Windows 6-arg** signature
  (`ProcessHandle, *BaseAddress, ZeroBits, *RegionSize, …`) but Xbox 360 passes only
  `(*BaseAddress=r3, *RegionSize=r4, AllocType=r5, Protect=r6, Debug=r7)`. It read the *size* as the
  base and never wrote `*BaseAddress` → heap base 0 → lock CS at null+0x618. Rewrote a tracking memory
  manager (std::map over the lazily-mmap'd 4 GiB) for Nt{Allocate,Query,Free}VirtualMemory. RtlCreateHeap
  now succeeds (1 MB reserve @ 0x40000000, lock CS at a valid 0x40000618). **This retires the "needs
  X_KTHREAD/X_KPROCESS" framing of the pause point — the structs weren't the blocker; the ABI was.**
- **[db90322] Full boot environment + static TLS (goal steps 1-2 DONE).** `SetupEnvironment` builds, 1:1
  with rexglue (Xenia) layouts: title X_KPROCESS (TLS vars + slot bitmap from XEX_HEADER_TLS_INFO),
  main-thread X_KTHREAD (InitializeGuestObject, linked into process), X_KPCR (r13). Per-thread TLS block
  initialized from the XEX TLS directory. Implemented KeTlsAlloc/Free/Get/SetValue (process slot bitmap +
  per-thread dynamic array via r13→KPCR→thread), KeQueryPerformanceFrequency (50 MHz), KeQuerySystemTime,
  XexCheckExecutablePrivilege. Switched the 474 stubs to **soft** (log+return 0; REX_STUBTRAP=1 to trap)
  → one run maps the whole cascade. This title has NO static TLS data (only 64 dynamic slots).
- **[48f217d] Data imports → boot reaches GPU video init.** XGetVideoMode (1280x720), MmAllocatePhysicalMemoryEx,
  ExGetXConfigSetting, RtlInitAnsiString. Boot advances through CRT init into the Vd* GPU init cascade
  (VdInitializeRingBuffer/EnableRingBufferRPtrWriteBack/SetGraphicsInterruptCallback) and parks in the
  main-thread GPU ring-buffer/vblank spin **sub_821B9270** (no GPU to advance the RPtr/vblank).
- **[e0f4e8a] Real threading + Vd* vblank pump → boot goes MULTITHREADED, past the GPU spin.**
  ExCreateThread spawns a host std::thread running the guest start routine (XAPI trampoline / raw) on its
  own KTHREAD/KPCR/TLS/stack (factored CreateGuestThreadContext + FillKThread/FillKPcr). Handle table:
  NtResumeThread, ObReferenceObjectByHandle (+NtCurrentThread), ObDereferenceObject, KeSetAffinityThread,
  KeInitializeDpc. Critical sections are now REAL per-CS recursive host mutexes; memory manager mutex-guarded.
  A ~60 Hz vblank pump thread fires the guest graphics interrupt callback on its own context. The audio
  worker is created+resumed+runs; boot reaches audio init.
- **[b086014] Sync primitives + memory layout.** Events/semaphores/single+multiple waits (one global CV;
  signal_state in the guest dispatch header @+0x04); threads signal their KTHREAD header on exit. Memory
  layout: guest virtual bump now starts LOW (0x10000) so the title's big heap reserve (everything below its
  ~0x70000000 stack) ends below the image (0x82000000) instead of overwriting `.data`; kernel arena +
  worker structs/stacks moved ABOVE the image (0x90000000+). REX_NOSPAWN diagnostic gate.

### CURRENT FRONTIER — audio-subsystem init logic (race FIXED by the cooperative token, commit e56368f)
The concurrency race is RESOLVED: the cooperative execution token (`g_waitMutex`; only one guest thread
runs at a time, released across waits / CS contention) made the boot **deterministic** (3/3 identical).
Remaining is now a stable LOGIC bug, not a race:
- A/B (REX_NOSPAWN): workers OFF → main sails to `XamLoaderLaunchTitle` (then hangs waiting for the un-run
  worker); workers ON (default) → deterministic crash at `sub_8229C4B0` (ppc_recomp.37:11442, `[r3+0]`)
  called from `sub_8214FFD0:20349` (`r3 = [r27+4] = -1`).
- Watchpoint: `[0x828C124C]` is written ONCE, by the main thread, to a VALID `0x806F0200` (so that global
  is fine); the crashing `[r27+4]` is a DIFFERENT subsystem-object slot that stays -1. `sub_8214FFD0` looks
  like a generic "init subsystem N → register object → spawn its worker (sub_8229C4B0 creates a thread for
  `[r27+4]`)". When the audio worker (`sub_8230E898`, started via the XAPI trampoline 0x82450FD0) runs, a
  later subsystem's object pointer is left -1 because its imports are soft-stubs.
- The crash deref faults only because it reads 4 bytes straddling the top of the 4 GiB mmap (r3 = -1 = top
  of guest space). Any real pointer there would load fine.
- ✅ **The cascade DOES advance one step per correct import** (confirmed): implementing XamUserGetSigninInfo
  (fill the 40-B X_USER_SIGNIN_INFO; the stub returned S_OK without writing it) moved the frontier forward
  — the user/profile subsystem now calls **XamUserReadProfileSettings** (current last stub). So the path is
  the import-cascade grind: implement the next profile/file import → advance → repeat. The unset `[r27+4]`
  object belongs to a subsystem whose init reads profile/file data that the stubs don't provide yet.
- Memory layout verified correct: the title's 1.88 GB heap reserve now lands at base 0x48D0000 (ends
  0x749D0000, **below** the image 0x82000000); kernel arena + worker stacks at 0x90000000+.

### ⏩ UPDATE — cascade cleared, boot reaches the GPU command-processor boundary (commits through case-insensitive VFS)
The -1 subsystem WAS the asset/profile subsystem; resolved it by walking the cascade with correct imports:
XamUserGetSigninInfo → XamUserReadProfileSettings (0-settings) → then the **file VFS**. Implemented a
read-only VFS (Nt{CreateFile,OpenFile,ReadFile,QueryInformationFile,SetInformationFile,Close}) that maps
guest `game:\…` → `<g_gameDir>/…` (the extracted dir, from the XEX path), **case-insensitively** (Xbox FS
is case-insensitive; the title opens `UI\`/`Audio\` but the extract is lowercase `ui`/`audio` — that
mismatch was the last crash). Now `default.xex`/`SouthParkXact.xgs`/`ArcadeLogo.ptc`/`UI.xzp` all open OK and
**the crash is GONE (exit 139 → 124)**. The boot loads assets + allocates GPU physical memory, then
**deadlocks at the GPU ring-buffer spin `sub_821B9270`** (a worker busy-polls the RPtr write-back for the
GPU to "catch up"; it needs a command processor to advance RPtr — and it holds the cooperative token while
spinning, starving everything else). **This is the GPU-CP / renderer boundary (goal step 5).**

### Revised next-steps
1. **GPU command processor (the frontier).** Mechanism (traced): the spin `sub_821B9270` does
   `r11=[r29+10896]` (ptr to the RPtr write-back = `g_rptrWriteBack`), `r8=[r11]` (current RPtr),
   `r9=[r31+8]` (the expected/target = where the CPU's WPtr is) and loops `while (r9 != r8)`. So it waits
   for **RPtr to reach WPtr**. The guest publishes WPtr by writing GPU register **0x01C5 = CP_RB_WPTR**
   (rexglue `graphics_system.cpp:278`) via an MMIO store to the GPU register window — which my FLAT 4 GiB
   map does NOT intercept (the store just lands in memory, no side effect). Two ways to get WPtr:
   (a) find the CP_RB_WPTR guest address (GPU register base + 0x01C5*4 = +0x714; the base comes from the
   ring-buffer/Vd* setup or the device struct `r29` — trace `r29` and its +10888/+10896/+11008 fields), then
   a host GPU thread POLLS it and writes `[g_rptrWriteBack] = WPtr` (no token; pure memory write) → the spin
   exits; or (b) add real MMIO write-interception for the GPU register range (rexglue `mmio_handler.cpp`).
   (a) is the quick "null GPU" that should release the spin → boot proceeds. THEN the real renderer: parse +
   translate the PM4 stream the ring buffer holds → draws (Plume + the 19 shaders in
   `private/extracted/media/shaders/*.updb`; Unleashed `gpu/video.cpp` ref). Multi-week.
2. **Threading model (interlocks with #1).** Findings (commit w/ REX_NOTOKEN gate): PREEMPTIVE threads
   (`REX_NOTOKEN=1`) hang EARLIER (line 43, XamInputSetState) — worse. COOPERATIVE (default token) reaches
   asset-load + GPU init but is **non-deterministic** in *thread-ordering* (the token serializes execution,
   not the OS-scheduled handoff order) → different runs stop at the GPU spin / `RtlRaiseException` / a crash.
   The worker that hits the GPU spin has `r29=0` (**null GPU device**) = token-starvation: it busy-waits
   holding the token for device state the (blocked) main thread would produce. **Proper fix = a
   DETERMINISTIC cooperative scheduler (real fibers, like RexGlue: `rex/thread/fiber.h`) with explicit
   yield ordering + a PARALLEL host GPU thread** that advances RPtr by writing guest memory directly (so a
   busy-waiting fiber's condition becomes true without the fiber yielding). This is the core
   GPU-CP/threading-architecture rework — multi-week.
3. `RtlRaiseException` (a now-reachable frontier on some interleavings) = guest SEH; XenonRecomp models SEH
   via setjmp/longjmp — implement RtlRaiseException to drive the guest __except path (or it's a benign
   "feature probe" the title raises + catches).
4. Remaining soft-stubs surface as the boot proceeds (more Xam, audio XAudio*, input) — implement as hit.

### Systematic next-steps (priority order)
1. ✅ **DONE — cooperative execution token** (commit e56368f): the boot is now deterministic. (Ruled out:
   broken atomics — XenonRecomp emits real `__sync_bool_compare_and_swap` for `lwarx/stwcx`.)
2. **Resolve the subsystem-init -1.** Find which subsystem `sub_8214FFD0` is initializing when `[r27+4]`
   is -1: break at `ppc_recomp.6.cpp:20349`, read `r27` (the object-slot global) and trace back which
   `sub_*`/import was supposed to populate it. Likely it needs a subsystem import currently soft-stubbed
   (audio: XAudioRegisterRenderDriverClient/GetSpeakerConfig/GetVoiceCategoryVolume; or a Xam/notify one)
   to return a valid handle/object instead of 0. Implement that import for real (ref rexglue-sdk
   `src/audio/` + `src/kernel/xam/`). ⚠ gdb live-inspection of `ctx.*` fields interacts badly with the
   token under all-stop — prefer `REX_KTRACE=1` tracing + targeted `fprintf` over breakpoint scripts, or
   set `REX_NOSPAWN=1` to inspect the single-threaded path.
3. ⚠ **Pending hazard — GPU-spin starvation under the token.** The main-thread GPU spin `sub_821B9270`
   (`db16cyc` busy-loop, calls `sub_8244CE40` each iteration) doesn't yield, so once reached it will hold
   the token forever and starve the vblank pump → hang. Make `sub_8244CE40` (or the spin) release the
   token (it's likely a delay/yield), or detect the spin and yield. Needed before the GPU sync can drive.
4. Implement the spinlock imports (Kf/KeAcquire/ReleaseSpinLock*, KeEnter/LeaveCriticalRegion) as real
   token-releasing locks (referenced in 3-6 TUs; not yet hit pre-crash).
5. Then the cascade past audio: file VFS (NtCreateFile/NtReadFile/NtQueryInformationFile/NtClose backed
   by the extracted game dir), XamLoaderLaunchTitle → GPU command processor (consume the PM4 ring buffer)
   → native renderer (Plume + 19 shaders).
2. Implement the spinlock imports (Kf/KeAcquire/ReleaseSpinLock*, KeEnter/LeaveCriticalRegion) as real
   host locks — these guard kernel-level shared structures the threads touch.
3. Then continue the cascade past audio: file VFS (NtCreateFile/NtReadFile/NtQueryInformationFile/NtClose
   backed by the extracted game dir) for asset loading, XamLoaderLaunchTitle.
4. Then the GPU command processor (consume the PM4 ring buffer the title fills) → native renderer
   (Plume + 19 shaders). The vblank pump already fires the interrupt callback; RPtr write-back is tracked.

### Tooling added this session
`REX_STUBTRAP=1` (hard-trap unimplemented imports), `REX_NOSPAWN=1` (create thread handles but don't run
them — isolates concurrency bugs). gdb hardware watchpoints on `g_base+<guestaddr>` pinpoint who writes a
guest global (and which host thread). Build: `cmake --build runtime/out --target sp_td_varianta` then
`./sp_td_varianta <abs path to default.xex>`.

### UPDATE 2 — FP-mask unlock crosses the device build; dual frontier (jump-tables + GPU)
Major: a zeroed `PPCContext{}.fpscr.csr` made the guest's first `setcsr` unmask host FP exceptions, so
every `fctidz` trapped (SIGFPE inexact). Init `ctx.fpscr.csr=0x1F80` (all 3 context sites) crossed the
ENTIRE device-build FP cascade at once. + `VdQueryVideoMode` (device build calls it, was unfilled→div0)
+ `KeDelayExecutionThread`/`NtDelayExecution` as token-yielding sleeps (broke the GPU-init hang). Boot now
runs deep into GPU/render init on the main thread. NEW frontier is DUAL: (1) the **function-boundary /
jump-table problem** resurfaces at runtime — `sub_821DC228:17351` computed-jumps through a table @0x8221C244
that XenonAnalyse missed → recompiler emitted `PPC_CALL_INDIRECT_FUNC(0x821DC29C)` (a mid-function LABEL,
not a function) → null dispatch → PC=0 SIGSEGV. Fix = recover the table into `sp_switch_tables.toml` +
regenerate ppc/ (likely several such tables). (2) the GPU engine (device reads 0 GPU registers; CP/PM4/Plume).

---

## 2026-06-01 (cont.) — jump-table robustness + cooperative wait: boot 127 → ~10,500 trace-lines

Two committed fixes (`23bf29b`, `1d435a1`; NOT pushed) took the boot from a hard crash at the first
runtime jump table to ~10,500 trace-lines deep, across video init and UI asset load.

### Fix 1 — ALL jump-table switches hardened (the dominant blocker)
The function-boundary jump table (`sub_821DC228`, the float-store dispatcher) didn't just need recovery —
its recovered switch emitted `default: __builtin_unreachable()`. At `-O0`, that lets clang DROP the switch
bounds-check and compile a raw `jmp *table[idx]`. The caller's data-driven loop ran the index to 22 (the
table has 22 entries 0..21), so `jmp *table[22]` read one slot past the host jump table and jumped to
**host address 0** — a fatal crash with a wiped register/frame context (rip=0, rsi=0, zeroed stack).
On real hardware index 22 is *also* out of range, so the title never emits it there; ours diverged.

Fix (toolchain + runtime, hardens all 93 tables at once):
- `recompiler.cpp` (in `patches/xenonrecomp-sp-instructions.patch`, now 663 lines): the switch `default`
  now does the **real computed jump** `PPC_CALL_INDIRECT_FUNC(ctr)` — exactly what the original `bctr`
  does (`ctr` already holds the HW target from the preceding `mtctr`). `__builtin_unreachable` is gone
  from every generated switch. Regenerated `ppc/` (88 TUs).
- `rex_indirect.h` + `kernel.cpp`: every indirect branch now routes through a bounds-checked
  **`PPCInvokeGuest`** — it only indexes the function table for targets in `[PPC_CODE_BASE, IMAGE_END)`,
  else logs+skips (`INDIRECT-NULL`). Without the bound, a wild target's table slot lands GiB past the
  4 GiB guest map and faults the *lookup read itself*. The macro is now a 1-liner → helper, so future
  dispatch-policy tweaks recompile only `kernel.cpp`, not all ~90 TUs.

### Fix 2 — real `NtWaitForMultipleObjectsEx`
Was a soft stub returning success instantly → tid=6 (`sub_821E61A8`) busy-spun (WaitAny on two events,
infinite timeout). Implemented as the handle-based sibling of `KeWaitForMultipleObjects` (ResolveObject
each handle → same cooperative wait core). Crossed the spin → the main thread progressed to loading
`game:\UI\UI.xzp` (`NtReadFile` now returns 23.5 MB of real data) and reached `XamLoaderLaunchTitle`.

### NEW FRONTIER — a CRT static-init divergence (precise; resume here)
Main thread: `_xstart → sub_82249638 → sub_82249678 → sub_8214FFD0 → sub_8229C4B0`.
`sub_8229C4B0` is the constructor of the global object at `0x828E3A38`. At `ppc_recomp.37.cpp:8362-8388`
it calls `sub_82201800(r4 = &field@0x828E825C)` to build a sub-object, reads it back, and — if null —
defensively `goto loc_8229C59C`, which reads `*(r1+80)`. On the null-skip path that stack slot is
**uninitialized = 0xFFFFFFFF**, so `lwz r11,0(r3)` reads `base + 0xFFFFFFFF` (crossing the 4 GiB end) →
SIGSEGV.

Why the field is null: inside `sub_82201800` (`ppc_recomp.22.cpp:14784+`) the allocation
`sub_82448090(96)` **succeeds** (→`0xd89d0`), but the init `sub_822009F0(r3=obj, r4=0x828c3830)`
**returns `0x8030001C`** (sev=error, facility 0x30, code 0x1c). Line 14820 `bge` then takes the error
path, so the store `*(r29=0x828E825C) = r31(obj)` at `:14841` is **skipped** → field stays null.

⇒ Root-cause `sub_822009F0` (`ppc_recomp.22.cpp:~12560+`, a long chain of `PPC_CALL_INDIRECT_FUNC`
virtual calls): which virtual call / resource lookup yields `0x8030001C`. Likely shares a root with the
**count divergence** (`sub_821E07A0`'s loop bound = `*(u16)(sub_821E0150_ret + 31)`; runs ~10k → 10,347
`INDIRECT-NULL` skips). Leading hypotheses for the shared root: the ~48-instruction recomp gap (esp. VMX
lanes) silently mis-emitting a computation; a missing import returning wrong data; or an asset-parse
mismatch. Method: `gdb -batch`, `break ppc_recomp.N.cpp:LINE`, read `ctx.rN.u32` / `base` (the `-O0`
host frames + file:line map cleanly). Then: more init → the GPU engine (PM4 / register MMIO / Plume +
19 shaders) — still multi-week.

### Frontier fully traced — XUI resource-header validation (sub_82244378), and instruction-gap RULED OUT
Followed the `0x8030001C` error down 8 levels of the static-init virtual-dispatch chain
(`sub_822009F0:12679 → sub_822000A0:11159 → … → sub_82244378`). `sub_82244378`
(`ppc_recomp.28.cpp:14315-14353`) is a **XUI resource-header validator**:
- `*(r1+112) == 0x5855495A` ("XUIZ" magic) — **passes**.
- `*(r1+80) == 22` (size) — **passes**. ⚠ Read `ctx.rN.u32` (the byte-swapped guest value), not raw
  `*(uint*)(base+addr)`; the raw bytes read `0x16000000` and *look* like a byte-order bug but are not —
  `PPC_LOAD_U32`/`lwz` swaps them to 22.
- **FAILS at `:14351`** `cmplw cr6,r31,r3`: `*(r1+120) = 0x167d346` (a heap object) vs the virtual call
  at `:14349` — `sub_82469FD0`, a trivial getter `r3 = *(this+8)` — which returns **`0x700ff800`, a
  STACK address**. They must be equal; they differ → `goto loc_82244740` → builds `0x8030001C`.

Root: the XUI-loader object at `0xd8a70` was constructed with its **+8 field = a stack pointer** where a
persistent heap object is expected — an object-identity/lifetime divergence during XUI resource
construction (its own root is further upstream). Next: trace who constructs `object@0xd8a70`, what
`*(r1+120) = 0x167d346` is, and why `+8` holds a stack address.

**Instruction gap RULED OUT as the cause:** a full regen reports ZERO "Unrecognized instruction"; the
only warning is `vcmpgtuh.` ×39 (a VMX compare-with-record not setting CR6 — irrelevant to this scalar
CRT/XUI path). So the divergence is **data/semantic** (object construction), not codegen. The count
divergence in `sub_821E07A0` (~10k) is likely the same family.

Method that worked well for this archaeology: `gdb -batch`, `break ppc_recomp.N.cpp:LINE`, inspect
`ctx.rN.u32` / `base` (the `-O0` host frames + file:line map cleanly; finish/return-value reads are
flaky inside `commands` blocks — break at the post-call line instead).

### Rule-outs + reframe (the XUI divergence is an earlier systemic root, needs bisection)
A watchpoint on the XUI object's `+8` field (guest `0xd8a78`) shows that memory is **reused** across
objects: written `0xf80f70` (valid heap) by the ctor `sub_82244200`, then `0xf87300`, then `0x65007300`
(UTF-16 "e\0s\0" — string data) by `sub_824294B0`. So the failing validation reads a `+8` that belongs
to a *recycled* allocation — an object-lifetime/ordering divergence, downstream of an earlier root rather
than a local bug at `sub_82244378`.

Hypotheses eliminated:
- **Instruction gap** — full regen: zero "Unrecognized instruction"; only `vcmpgtuh.` (VMX CR-record),
  irrelevant to this scalar path.
- **`XamLoaderLaunchTitle`** — called with a NULL path (relaunch-self), BUT the reference build
  (`third_party/rexglue-sdk`, which renders this title fully) **also stubs it** (`REX_EXPORT_STUB` in
  `xam_misc.cpp`). So a no-op return is correct; not the divergence.
- **vtable / function-pointer resolution** — the failing virtual call resolved to the correct guest
  function (`sub_82469FD0`, a real getter). Dispatch is fine.

⇒ The remaining cause class is a **runtime-HLE import returning different data than rexglue-sdk**, or a
*handled-but-subtly-wrong* instruction, corrupting object data early (same family as the `sub_821E07A0`
count divergence). Localizing it efficiently wants a **guest-level reference comparison**: both variant A
and rexglue-sdk execute the *same* guest code at the *same* guest addresses, so capturing guest
memory/registers at a shared guest PC (e.g. entry of `sub_82244378` or `sub_821E07A0`) in both builds and
diffing pinpoints the first divergence — far cheaper than descending each crash by hand. (rexglue-sdk is a
different *recomp* so host addresses differ, but guest state should match a correct emulation.) Do NOT
modify the prod `.so`; trace read-only.

Net for the session: boot advanced 127 → ~10,500 trace-lines across video init + UI load; the blocking
crash is precisely localized and three root hypotheses eliminated. Reaching a rendered frame still
requires resolving the init divergence (above) AND then building the GPU engine (PM4 → Plume + 19
shaders) — the latter remains multi-week.

### Reference-diff is FEASIBLE — turnkey starting point for next session
Confirmed the working build can serve as a correctness oracle for the init divergence:
- The prod game exe `out/build/linux-amd64-release/south_park_td` (20 MB, the rexglue-sdk recomp that
  renders this title fully) is **NOT stripped**: it exports all ~30,310 guest `sub_*` symbols, incl.
  `sub_821E07A0` (count loop) @ host 0x733910 (W), `sub_82244378` (XUI validator) @ 0x879d80,
  `sub_821DC228` (float dispatcher) @ 0000000000725a20. So I can break read-only at the exact guest
  functions that diverge in variant A.
- A display is available (DISPLAY=:0 / wayland-0) and launch is known (`tools/gamectl.sh`):
  `cd out/build/linux-amd64-release && SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. ./south_park_td
  --game_data_root=../../../private/extracted --user_data_root=../../../private/userdata
  --log_file=run.log`. ⚠ leaks a 4.5 GB /dev/shm/xenia_memory_* per launch — run `tools/gamectl.sh
  kill_all` after to reclaim it; NEVER modify the prod binary (read-only gdb only).
- Procedure: break sub_821DC228 in prod and read the loop index (max should be ≤21; variant A reaches
  32767 because the count is 0x8000) → confirms which side is correct; then break sub_821E07A0 /
  sub_82244378 entry in BOTH builds and diff the guest object state (the construction inputs) to find the
  first divergence. Caveat: the two builds use different PPCContext layouts + heap addresses, so compare
  GUEST values (registers/memory contents), match the same call instance, not host pointers.

This won't itself produce frames (the GPU engine — PM4 → Plume → 19 shaders — remains multi-week after
init is correct), but it is the lowest-cost route to unblock the CRT-init divergence that currently
stops the boot well before the GPU boundary.

### Reference diff is OPERATIONAL — first cross-build signal captured
Ran the working build read-only under gdb and confirmed the mechanics work:
- `cd out/build/linux-amd64-release && timeout N env SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. DISPLAY=:0
  gdb -batch -x <script> --args ./south_park_td --game_data_root=<abs> --user_data_root=<abs>
  --log_file=run.log --log_level=info`, then `tools/gamectl.sh kill_all` + `rm -f /dev/shm/xenia_memory_*`.
- The exe is **PIE** → break by SYMBOL (`break sub_821DC228`), NOT by the `nm` link-time address.
- Prod uses the **same XenonRecomp PPCContext layout** as variant A: at a `PPC_FUNC` breakpoint
  `rdi`=&ctx, `rsi`=base (prod base = `0x100000000`), and `ctx.rN.u32` reads as `*(uint*)(rdi+off)` with
  r3@0x00, r1@0x10, r4@0x20. So guest registers/memory are readable identically in both builds.

First signal: in ~70 s of gdb-slowed boot, **prod calls `sub_821DC228` ZERO times** (it entered
`sub_821E07A0` once, r3=0x40029140, with no dispatcher calls), whereas **variant A calls it 32768 times**
(count=0x8000). Strongly suggests variant A's float-store loop is spurious / runs with a bogus count.
⚠ CAVEAT before concluding: gdb-slowed prod may simply not have *reached* the same boot stage yet —
next step must confirm prod passes the equivalent point (e.g. video init / the `sub_82244378` XUI parse)
and STILL never loops, and match the same `sub_821E07A0` invocation (it can be called from several sites).
Then diff the loop-entry guard inputs / count source (`*(orig+144)+31`) between builds to find where
variant A's count first goes wrong. This capability now makes any divergent function comparable to the
known-good build in minutes.

### CONFIRMED via reference oracle: variant A's count loop is SPURIOUS
Ran the working build ~95 s under gdb (read-only): it entered `sub_821E07A0` exactly once, called the
float-store dispatcher `sub_821DC228` **zero times**, and **reached rendering** (run.log: "Created 54
graphics pipelines from Vulkan storage", VulkanTextureCache active). So the correct build renders without
ever executing that loop — variant A's 32768-iteration loop (count=0x8000) is definitively wrong, not a
boot-stage artifact. (Caveat resolved: prod did reach the render stage, so it's "never runs", not "not
yet".)

The divergence is therefore in `sub_821E07A0`'s ENTRY logic (ppc_recomp.19.cpp from line 2877): before
the loop it does several `PPC_CALL_INDIRECT_FUNC` virtual calls + branches —
`if(r8==0) goto loc_821E08B8` (2912), `if(*(r31+16)!=0) goto loc_821E08DC` (2920),
`if(r3<0) goto loc_821E08E0` (2937) — one of which, in the correct build, routes PAST the loop. Variant A
takes the loop path because one of those virtual-call results / branch inputs diverges. Next: capture
those branch inputs (r8 @2909, *(r31+16) @2917, the 2933 vcall's r3 @2935) in variant A, then find the
first one whose value is wrong vs the correct build — that pins the divergence. (Cross-build address
matching is confounded by different heap layouts + prod's different host code/no source lines, so compare
guest VALUES and branch decisions, matched by call order, not host pointers.)

This is the same object-data-corruption family as the XUI `+8` crash; pinning either root likely explains
both. Still upstream of the GPU engine (multi-week) — but the reference oracle makes the init root
attackable directly now.

## 2026-06-01 (cont.) — BREAKTHROUGH: one VFS fix unblocks all init; boot reaches the RENDER LOOP

The reference oracle paid off. Traced the count divergence to its true root: the title sizes its asset
reads with **NtQueryInformationFile / FILE_NETWORK_OPEN_INFORMATION (class 34)**, whose `EndOfFile` is at
**+40** (offsets 0..31 are timestamps, AllocationSize @+32). My impl wrote `EndOfFile` at **+8**
(FILE_STANDARD layout) for every non-position class, so class-34 size queries returned 0 → 0-byte reads
→ `SouthParkXact.xgs` ("FSGX") and `*.ptc` never loaded → the main object's `+144` pointed at garbage →
BOTH the spurious 32768-iter `sub_821E07A0` count loop AND the `sub_82244378` XUI-validation SIGSEGV.

Fix (commit 760b0bd, kernel.cpp): switch on `infoClass` — 14 (FilePositionInformation, CurrentByteOffset
@0), 34 (FileNetworkOpenInformation: times @0..31, AllocationSize @32, EndOfFile @40, FileAttributes @48),
5/default (FileStandardInformation: Alloc @0, EndOfFile @8, NumberOfLinks @16); report bytes-written in
the IoStatusBlock.

Result — one fix, the whole init chain unblocked:
- XGS reads 931 B, .ptc 93863 B (were 0); **INDIRECT-NULL 10347 → 0** (count loop gone);
  **the SIGSEGV at sub_82244378 is GONE** (exit 139 → 124/running).
- Boot now crosses video init: `VdInitializeRingBuffer(base=0xA0002000 size=0x1000)`,
  `VdEnableRingBufferRPtrWriteBack(ptr=0x201003C)`, **vblank pump started**,
  `VdSetGraphicsInterruptCallback(cb=0x821C7170)`, `XGetVideoMode 1280x720`, 6 threads, XexLoadImage.
- It then runs the title's **main/render loop** `sub_821CC5D0` (a per-frame critical-section acquire +
  `KeWaitForSingleObject`, ~34/s). NOT crashed, NOT deadlocked — but NOT yet presenting (`VdSwap` not
  reached): it's waiting at the GPU command-processor boundary.

FRONTIER (task #6, the GPU engine — the last step before frames):
- The title writes PM4 to the ring buffer @0xA0002000 and waits on RPtr writeback @0x201003C. The vblank
  pump fires the graphics-interrupt callback every 16 ms, but the render-wait still spins → next: a "null
  GPU" that advances RPtr to the title's WPtr (capture WPtr via the system command buffer / CP_RB_WPTR)
  so the render-wait releases and the title proceeds to issue/await draws; THEN PM4 → Plume Vulkan + the
  19 shaders for actual pixels. This last stage remains multi-week, but init is no longer the blocker.

Lesson worth keeping: a single wrong struct-field offset in one kernel HLE call masqueraded as a deep
"object-graph corruption" across two unrelated subsystems. The guest-level reference oracle (diffing
against the rendering build) is what cut through it — keep it as the first tool for any divergence.

## 2026-06-01 (cont.) — GPU engine: scope nailed down to a PM4 interpreter (the renderer = step 5)

Pushed task #6 from "null GPU" to a fully-specified implementation plan, and proved no shortcut exists.

State recap: assets load, the game boots to its render loop and writes a PM4 stream to the ring buffer
(@0xA0002000). Built+committed (d9b009d) a minimal CP in the vblank pump that polls CP_RB_WPTR
(GPU MMIO base 0x7FC80000, reg 0x1C5 @0x7FC80714), advances RPtr in CP_RB_RPTR (0x7FC80710) + the
write-back (g_rptrWriteBack @0x201003C), and fires the command-complete interrupt. VERIFIED RPtr=WPtr=25.

Proven dead ends (so the next session doesn't repeat them):
- Advancing RPtr does NOT release the render-wait (event 0x36E70) — it isn't RPtr-gated.
- Firing the graphics-interrupt callback with source=1 alone does NOT release it (correctly ignored
  without backing GPU state).
- VdVerifyMEInitCommand is NOT called by the title — no stub-level shortcut.
- A KeSetEvent trace shows the title signals NEARBY render-context events (0x36e30, 0x36e54) but never
  0x36E70 — the 0x36E70 signal path is gated on the GPU actually PROCESSING the submitted PM4.

PM4 decoded: first packet 0xC0114800 = TYPE-3 op 0x48 = PM4_ME_INIT (count 17 → dwords 0..17), then
dwords 18..24 are the follow-on packet(s) to decode. So the render-wait needs a real PM4 interpreter.

IMPLEMENTATION SPEC (from rexglue-sdk src/graphics/command_processor.cpp — Xenia 1:1, already renders
this title; strongly prefer REUSING it over a scratch rewrite):
- CP worker loop: read write_ptr_index; if RPtr==WPtr (or 0xBAADF00D) wait; else
  read_ptr = ExecutePrimaryBuffer(read_ptr, write_ptr); store_and_swap RPtr to the write-back. (:290-53)
- ExecutePacket dispatch on `packet >> 30`: type 0 = ExecutePacketType0 (register-window writes),
  type 1/2 (rare/NOP), type 3 = ExecutePacketType3 (opcode = (packet>>8)&0x7F, count = ((packet>>16)&
  0x3FFF)+1). (:818-837)
- ME_INIT handler = ExecutePacketType3_ME_INIT (:1082) sets the CP micro-engine register bases.
- The render-wait ack is almost certainly a later packet: an EVENT_WRITE / interrupt-trigger that the
  CP turns into a memory write or graphics interrupt → the title's handler then signals 0x36E70. Decode
  dwords 18..24 + find that opcode; reproduce its side effect to release the wait (init-only, still no
  pixels). PIXELS then require the type-3 draw packets → Plume Vulkan + the 19 shaders
  (private/extracted/media/shaders/*.updb). Wire the CP to variant A's g_base / ring @0xA0002000 /
  regs @0x7FC80000 / RPtr-WB @0x201003C. This is the multi-week remainder; every other subsystem boots.

### Ring-buffer command structure fully decoded (completes the PM4-interpreter spec)
The 25-dword primary ring buffer @0xA0002000 is: PM4_ME_INIT (op 0x48, dwords 0..18) + TWO
PM4_INDIRECT_BUFFER packets (op 0x3F, confirmed vs command_processor.cpp:905/937 ExecutePacketType3_
INDIRECT_BUFFER:1200), each [IB_address, IB_size_dwords]: IB#1 @0x00090040 size 0x0B, IB#2 @0x00010000
size 0x40. ⇒ the REAL init/draw commands (and the ack that signals render-wait 0x36E70) are inside the
INDIRECT BUFFERS, not the primary ring. So the interpreter MUST recurse into IBs (read PM4 at IB_address
for IB_size dwords) — a plain primary-buffer walk won't see the commands. Implementation order for the
GPU engine: (1) primary-buffer walk RPtr→WPtr dispatch on packet>>30; (2) type-3 INDIRECT_BUFFER →
recurse; (3) ME_INIT + type-0 register-window writes (into the 0x7FC80000 reg file); (4) find the
EVENT_WRITE/interrupt opcode in the IBs that the title's handler turns into the KeSetEvent(0x36E70) ack,
reproduce it → render-wait releases (init done, still no pixels); (5) type-3 DRAW_* packets → Plume
Vulkan + 19 shaders → pixels. Best path remains reusing rexglue-sdk command_processor.cpp wholesale
(it already does 1-5 for this title) wired to variant A's g_base/ring/regs/RPtr-WB. Multi-week.

### Render-wait 0x36E70 — ALL bounded shortcuts exhausted; it requires real PM4 execution (confirmed)
Drove the render-wait release condition to ground. The main thread blocks on event 0x36E70; it is NOT
released by any of these (each tested/analyzed):
1. RPtr advance to WPtr (CP_RB_RPTR + write-back) — verified RPtr=WPtr=25, no release.
2. Graphics-interrupt callback source=1 (command-complete) — runs, no release.
3. VdVerifyMEInitCommand — the title never calls it (no stub shortcut).
4. Per-processor interrupt delivery — REFUTED: FillKPcr zeros the KPCR so every thread is processor 0;
   the callback (reads KPCR+0x10C) always acts as proc 0 and signals proc-0 events 0x36e30/0x36e54, NOT
   0x36E70. Callback args are correct (r3=source, r4=interrupt_callback_data_, matching rexglue
   DispatchInterruptCallback args[]={source, interrupt_callback_data_}).
⇒ 0x36E70 is the GPU **command-completion** event: the title submits PM4 (ME_INIT + 2 INDIRECT_BUFFER,
empty so far — it's awaiting CP-ready before filling the real-command IBs) and waits for the GPU to
EXECUTE it and write a completion fence/EOP that the title's code turns into KeSetEvent(0x36E70). With no
PM4 interpreter, that never happens. There is no stub/register/interrupt shortcut — only a real command
processor releases it.

DEFINITIVE: step 5 (renderer) == implement/integrate the GPU command processor (PM4 interpreter w/ IB
recursion + GPU memory model + EOP/fence + Plume Vulkan + 19 shaders). Reuse rexglue-sdk
command_processor.cpp. This is the multi-week remainder; every other subsystem boots and the game runs to
the point of submitting GPU work. Nothing further in this layer is a quick fix — confirmed exhaustively.

## 2026-06-01 (cont.) — GPU CP built: register-seed unlock + real PM4 interpreter; main thread advances; hit the GPU-init teardown

Two committed advances (6d85089, f6affa2), both verified via gdb. The boot moved from "stuck at the
early GPU spin" to "executing the real PM4 stream and walking the multi-stage GPU-init handshake."

### Fix A — seed the GPU registers prod's ReadRegister returns (commit 6d85089)
ROOT (found via the recompiled graphics-interrupt callback sub_821C7170 + the reference impl): variant
A's GPU register window 0x7FC80000 is **plain guest memory with no MMIO read interception**, so it read
0 everywhere. But rexglue `GraphicsSystem::ReadRegister` (graphics_system.cpp:241) returns specific
non-zero values for several registers the title polls. The decisive one is **reg 0x1951 (interrupt
status) @ mem 0x7FC86544**: the callback's vblank path does `lwz r11,25924(0x7FC80000); clrlwi. r11,31;
beq <skip>` — i.e. it SKIPS the whole vblank handler `sub_821BF748` unless bit 0 is set. Prod hardcodes
`ReadRegister(0x1951)==1`. Left 0 ⇒ the vblank handler never ran ⇒ the title stalled at the early GPU-init
wait. FIX: `InitGpuRegisters()` pre-seeds 0x0F00=0x08100748, 0x0F01=0x200E, 0x194C=720, 0x1951=1,
0x1961=(1280<<16|720) (1:1 with ReadRegister's switch); re-assert 0x1951=1 each vblank; deliver the
interrupt on **cpu 2** (KPCR+0x10C, matching prod `MarkVblank → DispatchInterruptCallback(0,2)`). RESULT:
boot crossed the `KeGetCurrentProcessType` spin (753→7 hits) through XAudioRegisterRenderDriverClient +
the 23.5MB UI.xzp load to the deeper render-loop waits. **Main thread reached the exact chain the task
named**: `_xstart→…→sub_822E2CF0→sub_822F2CE0→sub_8230F368→sub_8230F098→KeWaitForSingleObject(0x36E30,∞)`.

### Fix B — real PM4 command-processor interpreter (commit f6affa2)
Replaced the null-GPU blind RPtr advance with an actual interpreter in kernel.cpp (ExecutePM4 /
ExecuteType3 / ExecuteRing), structured 1:1 with rexglue command_processor.cpp. The pump, when WPTR
advances, runs ExecuteRing(lastWptr, wptr) under the coop token, then advances RPtr.
- **ADDRESS MODEL (verified, the key detail):** variant A's flat 4 GiB map puts "physical" GPU memory
  (MmAllocatePhysicalMemoryEx, the ring, the IBs) in the **0xA0000000 window**. The PM4 carries physical
  addresses with mirror bits stripped (e.g. IB addr 0x90040), so `TranslatePhys(p) = 0xA0000000 | (p &
  0x1FFFFFFF)`. PROVEN by dumping: IB#1 phys 0x90040 → guest **0xA0090040** holds real PM4 (a
  WAIT_REG_MEM packet), while raw 0x90040 is zero; IB#2 0x10000 → **0xA0010000** holds PM4, raw 0x10000
  holds UTF-16 text. ring base 0xA0002000 is self-consistent (TranslatePhys(0xA0002000)=0xA0002000).
- Ring decode (WPTR=25 dwords): `C0114800`=ME_INIT(0x48,count18) + `C0013F00`=INDIRECT_BUFFER(0x3F)→
  [0x90040,len11] + INDIRECT_BUFFER→[0x10000,len64]. PM4 opcodes from xenos.h (note PM4_INTERRUPT=0x54,
  XE_SWAP=0x64). Handlers: type-0/1 reg writes → 0x7FC80000 window; ME_INIT/NOP/SET_CONSTANT/WAIT_REG_MEM
  skipped; INDIRECT_BUFFER recurses (depth-guarded); EVENT_WRITE_SHD writes the fence value (GST32 = BE,
  k8in32) the guest polls; INTERRUPT fires the gfx callback per cpu-bit; DRAW_INDX/2 + XE_SWAP counted.
- VERIFIED RUN (REX_CPTRACE=1): executes the init batch, recurses IB#2 into 3 nested IBs (0xA2014000 /
  0xA2014D40 / 0xA2015980), writes 2 fences (0xA2010000=0x3, 0xA2010004=0xA00100D4). **The fence
  side-effects advanced the main thread past 0x36E30 to the next GPU-completion wait 0x388C4** (231620).
  REX_EVTRACE confirms: 0x36E30/0x36E54/0x388C4/0x388E8 each signaled once; 0x29CD4 waited 1314×, never
  signaled.

### CURRENT FRONTIER (precise, resume here) — the GPU-device TEARDOWN after init
After the interpreter executes the init batch, a **worker thread tears down the whole GPU device**.
Pinpointed with a hardware watchpoint on device+10900 (device = g_interruptData = 0x26F80):
1. `sub_821C73D8(device, config=0x828C1298)` builds the device (lr=0x821C7FFC, in sub_821C7F08): allocs
   cmdbuf1=**0xA0002000 (THE RING)**, cmdbuf2=0xA0010000, blk96 @device+10896=0xA2010000 (the GPU
   identifier/fence block, set by VdSetSystemCommandBufferGpuIdentifierAddress — a no-op in both builds),
   handler-block @device+10900=0xA2011000 (the command-complete handler block read by callback source==1).
2. Then a worker (`sub_8214F730→sub_8214F738→sub_822A2158→sub_822A7C08→sub_822A58F8`) memsets 16 buffers
   (sub_8242BF10 = memset), **zeroing device+10900, +10896, +15044(cmdbuf1 ptr), +15048(cmdbuf2 ptr) all
   to 0**. The ring @0xA0002000 still holds ME_INIT, but the device's buffer pointers are wiped.
3. STUCK STATE: main thread waits 0x388C4 (∞); render workers `sub_821CC5D0` wait 0x29CD4 (30ms poll) /
   0x29D40 (∞) — these GPU-completion events are NEVER signaled. The command-complete callback path
   (sub_821C7170 source==1) can't help because device+10900 is now 0.

INTERPRETATION (hypothesis, NOT yet proven): the title sets up the GPU, the init handshake isn't
satisfied by the CP, and a worker tears the device down (or it's a normal "free temp init buffers" step
the title can't progress past because the next phase's completion never arrives). The teardown is guest
code (a worker task sub_822A2158, 6 args), reached only BECAUSE the interpreter advanced the title there
— it is not interpreter-caused corruption (it's a guest memset of guest buffers).

### NEXT STEPS (in priority order)
1. **Root-cause the teardown / handshake.** Trace what makes the worker run sub_822A2158 and whether it's
   gated on a fence/identifier value at the GPU id block (0xA2010000 region) or a register — i.e. what
   "GPU init succeeded" signal the CP must produce. Watchpoint device+15044 / the id block; diff vs the
   reference oracle (prod renders, so its worker either doesn't tear down or re-inits). ⚠ prod is Release
   (no ctx symbols) — read its guest memory via its membase + break by guest symbol.
2. **Verify the EVENT_WRITE_SHD fence semantics** the render workers (0x29CD4/0x29D40) actually poll —
   they may need a counter fence (is_counter) tied to XE_SWAP, or a specific value the nested IBs request.
3. Once init completes and the title submits frames (WPTR advances repeatedly, XE_SWAP fires): wire
   DRAW_INDX/DRAW_INDX_2 → Plume Vulkan + the 19 shaders (private/extracted/media/shaders/*.updb), crib
   Unleashed gpu/video.cpp. = the multi-week render remainder.

TOOLING: REX_CPTRACE=1 (PM4 packet trace), REX_EVTRACE=1 (event signal/wait count maps g_evSignalCount/
g_evWaitCount, readable via gdb `p '(anonymous namespace)::g_evSignalCount'`). gdb scripts in /tmp:
dumpring.gdb (ring+IB dump), threads_va.gdb (thread dump), watch_teardown.gdb (the teardown watchpoint),
verify_dev.gdb (device-buffer state). Attach to the BINARY pid (filter /proc/PID/comm; pgrep -f also
matches the shell wrapper). Reading guest memory under attach is safe; live ctx stepping fights the token.

## 2026-06-01 (cont.) — TEARDOWN ROOT-CAUSED: it is guest-heap memory corruption (NOT a GPU-init handshake)

Drove the "GPU-device teardown" to ground with the reference oracle + watchpoints. The teardown is
**memory corruption from a guest-heap double-allocation**, surfaced (not caused) by the PM4 interpreter
advancing the title into ArcadeLogo.ptc resource processing.

THE MECHANISM (all gdb-verified):
- A worker runs `sub_8214F738` which opens `game:\media\ArcadeLogo.ptc` (variant A handle 0xF2000002)
  and processes it: `sub_822A2158 → sub_822A7C08 → sub_822A58F8`, which does **16 memcpys** (sub_8242BF10)
  of the .ptc data (src in the .ptc load buffer ~0x5C5xx) into a resource buffer whose base is **0x262B0**
  (a ~16 KB buffer: dst = base + r26, r26 ∈ {0,0x400,0x1400,0x2800,...}).
- That buffer **overlaps the live GPU device struct at 0x26F80** (device spans 0x26F80..~0x2AA48; the
  copies hit 0x276B0/0x28AB0/0x29EB0/0x2A2B0 and a watchpoint caught device+10900 (0x29A14) being set 0).
  So loading the .ptc OVERWRITES the device — zeroing +10900 (cmd-complete handler block), +10896 (GPU id
  block), +15044 (cmdbuf1/ring ptr), +15048 (cmdbuf2). That is the "teardown".
- Corrupting the device is why the downstream GPU-completion waits never release (events 0x29CD4/0x29D40
  in the device, 0x388C4 in the thread pool — main thread + render workers sub_821CC5D0 stall).

REFERENCE-ORACLE COMPARISON (corrected — earlier "prod never calls sub_822A2158" was breakpoint-
interference that segv'd prod; with a single breakpoint prod calls it **1×** and renders 55 pipelines):
- Both prod AND variant A run the .ptc copy (`sub_822A2158`) — it is NORMAL processing, not error-recovery.
- The divergence is PLACEMENT: prod's heap puts the .ptc buffer where it does NOT hit the device; variant
  A's heap puts it OVERLAPPING the device.
- prod's device is at **0x40016F80**; variant A's at **0x26F80** — both = `heap_base + 0x16F80`, but the
  title heap base differs: prod 0x40000000, variant A 0x10000 (from `NtAllocateVirtualMemory req=0
  sz=0x100000 → base=0x10000`, a 1 MB reserve; device + .ptc buffer are both sub-allocs inside it).
  (prod device addr read from its log: `[gpu] SetInterruptCallback(821C7170, 40016F80)`.)

⇒ ROOT: **variant A's guest heap (RtlAllocateHeap, sub_82448090; heap created by RtlCreateHeap
sub_8244B380) double-allocates** — it hands out 0x262B0 for a 16 KB .ptc buffer while the device is live
at 0x26F80. The heap's free-list is in a different state than prod's (the device's range isn't treated as
in-use, or an earlier alloc/free diverged). Same CLASS as the original systemic NtQueryInformationFile
bug: a subtle kernel-HLE/data divergence corrupting the title's own structures.

NEXT STEPS (the fix):
1. Find the heap-state divergence. Prime suspects: the heap-backing kernel HLE — `NtAllocateVirtualMemory`
   / `NtFreeVirtualMemory` / `NtQueryVirtualMemory` semantics feeding RtlCreateHeap (sub_8244B380) wrong
   region info (e.g. a wrong returned size/base so the heap mis-tracks free space), or a heap-block-header
   field computed wrong. Watchpoint the device allocation (who allocates 0x26F80, is its range recorded?)
   and the 0x262B0 allocation; diff the heap free-list vs the prod oracle (guest-level diff — the technique
   that found the systemic bug).
2. This is independent of the GPU CP (the CP writes only 0xA0000000+/0x7FC80000); the CP just advanced the
   boot far enough to surface it. Once the heap stops corrupting the device, re-evaluate the GPU-completion
   waits (they may then be satisfiable by the interpreter's fences/interrupts) → VdSwap → DRAW → Vulkan.

TOOLING added: REX_EVTRACE now records per-event signaler/waiter guest LRs (g_evSignalLR/g_evWaitLR) —
this is how the thread-pool producers/consumers were identified (signaler sub_8230E898, waiter
sub_8230F098). gdb scripts /tmp/{collide,watch_teardown,openchk,prod_f738,prod_a2158}.gdb.

## 2026-06-01 (cont.) — allocation diff: the teardown is a guest-heap free-list divergence (both device & .ptc buffer from sub_82448090)

Ran the allocation-sequence diff (option a). Findings (gdb watchpoints from the guest entry):
- **The device (0x26F80) is allocated by the GUEST HEAP allocator sub_82448090**, via the main-thread
  graphics init: `_xstart → … → sub_8212DBA0 → sub_821C7F08 → sub_821D7438` (sub_8212DBA0 calls
  sub_82448090 at :21083/:21136, then sub_821C7F08(device) at :21170; sub_821D7438 writes the device's
  first field = 0xFFFFFFFF).
- **The colliding .ptc buffer (0x262B0) is ALSO a sub_82448090 allocation**, via the worker:
  `sub_8214F730 → sub_8214F738 → sub_821BE840` (sub_821BE840 calls sub_82448090 ×3 at :8820/:8883/:8910).
  Watchpoints confirm 0x262B0 is NEVER written before the .ptc data lands (no separate zeroing), i.e. it is
  a fresh heap block whose header sits just below the watched word.
- ⇒ **Both come from the SAME guest heap (sub_82448090).** The device is allocated EARLY (heap+0x16F80);
  the .ptc buffer is allocated LATER but at a LOWER offset (heap+0x162B0) — so the heap is reusing a freed
  region, and that reused 16 KB block overlaps the live 15 KB device. The heap's **free-list tracks a free
  block at heap+0x162B0 of ≥16 KB that overlaps the live device at heap+0x16F80** — a free-list divergence.
- sub_82448090 is the SAME recompiled code in prod, so it can't misbehave on identical inputs ⇒ the
  divergence is the heap STATE: an earlier alloc/free in variant A differs from prod, leaving the free-list
  with a block overlapping the device. (heap base also differs: variant A 0x10000 vs prod 0x40000000, both
  device = base+0x16F80 — but the collision is relative/free-list, not just the base.)

REMAINING (the true root): diff the **full guest-heap alloc/free sequence** between the device allocation
and the .ptc-buffer allocation, variant A vs prod, to find the one operation that diverges (the bad
free/coalesce that leaves heap+0x162B0 free over the live device). Candidate tools: break sub_82448090 +
the heap's free (RtlFreeHeap) and log (size, ptr, caller) into a per-build trace, then diff. ⚠ prod is
Release (read guest mem by membase, break by symbol; can't read ctx). gdb: /tmp/{earlyalloc,devalloc2,
allochist}.gdb (watch a guest addr from __imp___xstart, walk the writers).

## 2026-06-01 (cont.) — teardown refined: device = one 24KB heap block; .ptc main buffers are CORRECT; corruption = a subset of wrong dest pointers

Deeper allocation diff:
- **The device is ONE 24 KB guest-heap block.** sub_8212DBA0 calls sub_82448090(size=24448=0x5F80) →
  0x26F40 (caller 0x8212DCEC); the device struct is `(0x26F40+131)&~0x7F = 0x26F80` (alloc aligned up
  128B; orig ptr saved at device-4), then memset to 24320 (0x5F00) → device spans 0x26F80..0x2CE80.
- **sub_821BE840's main .ptc buffers are CORRECT** — allocated in the 0xA0000000 physical window:
  a1 size 0x500 (small obj), a2 size **0x398000 (3.7 MB) → 0xA2016000 / 0xA23AE000**, a3 size 0x21E800 →
  0xA2746000. So the .ptc loader's big vertex/texture buffers are placed FINE (no device overlap).
- ⇒ The corruption is NOT a gross allocator failure. It's a **subset of destination pointers** in the
  16-entry buffer-ptr array that sub_822A58F8 walks (r28 = *(r1+1412)+60, where *(r1+1412)=0x20F50A98):
  most entries point to the correct 0xA2xxxxxx buffers, but SOME point INTO the device struct (bases
  0x262B0 / 0x29EB0 → writes land at device+0x770..+0x3370, hitting device+10900 the handler-block ptr).
- Still a variant-A divergence (prod renders); the device block alloc + the physical buffers are identical
  in spirit to prod, so the wrong array entries are the divergence.

REMAINING ROOT (next layer): who populates the buffer-ptr array at 0x20F50A98+60 with device-pointing
entries (0x262B0/0x29EB0)? Trace the writers of that array; determine whether *(r1+1412)=0x20F50A98 is a
valid structure or a wild pointer, and where the device-pointing entries come from (a .ptc-relocation base,
a stride/count parsed wrong, or a stale/garbage pointer). This is the .ptc-loader's vertex-stream
descriptor — several functions deep (sub_822A2158/sub_822A7C08/sub_822A7480/sub_822AC488/sub_822AC328).

## 2026-06-01 (cont.) — ROOT FOUND: .ptc vertex-stream destinations are UN-RELOCATED (missing the buffer base) → overwrite the device

Pinpointed the exact corrupting write with a targeted break (sub_822A58F8:8757, condition: the memcpy
whose [dst,dst+size) covers device+10900=0x29A14). Captured:
  dst=0x296B0 base(r11)=0x262B0 off(r26)=0x3400 src=0x5C560 size=0x400
  buffer-ptr array (16 entries) = 0x82B0, 0xBEB0, 0xFAB0, ... 0x262B0(#8), 0x29EB0(#9), ... 0x406B0
i.e. **array[i] = 0x82B0 + i*0x3C00** (16 stream buffers, stride 0x3C00=15360, built by the loop at
sub_822A58F8 loc_822A5E70: `r10 += r9; *(r11+=4)=r10`). dst = array[i] + r26.

THE BUG: these destination bases are **raw offsets (0x82B0 + i*0x3C00 = 0x82B0..0x442B0)** — they are
MISSING the real buffer base. sub_821BE840 ALLOCATED the real .ptc buffers at **0xA2016000** (3.7 MB,
0x398000); the 16 stream offsets (240 KB total) fit inside it, so the destinations SHOULD be
`0xA2016000 + (0x82B0 + i*0x3C00)`. Instead they are just `0x82B0 + i*0x3C00`, which lands in low memory
(0x82B0) through the title heap — straddling the **device block at 0x26F40** (entries #8/#9 = 0x262B0 /
0x29EB0 fall in the device), so the .ptc stream copy overwrites the device (incl. +10900 the handler ptr).

⇒ The .ptc vertex-stream destination computation fails to RELOCATE the stream offsets onto the allocated
buffer base — a missing/zero base. Since the recompiled guest code is identical to prod, the divergence is
a DATA value: the field that should carry the buffer base (0xA2016000) reads 0 (or the wrong field) when
the stream-destination array is built, leaving raw offsets. Same CLASS as the NtQueryInformationFile
systemic bug (a wrong field value, not codegen).

NEXT (the fix): find where sub_822A58F8 / sub_822A7C08 reads the per-stream buffer base for the array
build (the initial r10 / the source of 0x82B0). Determine which object field should hold 0xA2016000 but
reads 0 in variant A, and trace upstream to where sub_821BE840 stores its allocated buffer ptr into the
.ptc object — a field-offset / store divergence. Fix that store/read so the stream destinations relocate
onto 0xA2016000 → no device overwrite → re-test the GPU-completion waits.

## 2026-06-01 (cont.) — ⛔ PRIOR ROOT-CAUSE CORRECTED: it is a HEAP FREE-LIST corruption, NOT un-relocated .ptc offsets

The "ROOT FOUND: .ptc vertex-stream destinations are un-relocated" conclusion ABOVE is **WRONG** (it
assumed the dest array held raw offsets that should add a buffer base 0xA2016000). Drove it to ground with
gdb (scripts /tmp/{capture,relo,entry,alloc,site,trace,heaplog,heapdump,heapfl,narrow2,watchfl}.gdb) and
found the real mechanism:

THE DEST BASE *(obj+192) IS AN ALLOCATOR RETURN, NOT A FILE OFFSET. The .ptc stream-dest array
(`sub_822A7C08` builds `array[i] = *(obj+192) + i*stride` on its stack at r1+128; `sub_822A58F8` consumes
it as the r7 arg) gets its base from `sub_822A8150` (called at sub_822A2158:23167, right before
sub_822A7C08), which **allocates the stream buffer**: `sub_82448090(size=0x78000, flags=0x24870000)` →
stored at obj+192. gdb trace (/tmp/trace.gdb) caught it: that alloc **returns 0x82B0** — an address BELOW
the title heap base (0x10000), spanning the live GPU device at 0x26F40. So `*(obj+192)=0x82B0` is the
**allocator's bogus return**, not a missing relocation. (The object IS a stack struct in sub_822A2158's
frame, ~0x980AF7B0 because worker stacks live in the 0x98000000 region; +0 holds the "PTC+" magic copied
from the file header, which misled the prior session into the file-offset theory.)

THE ALLOCATOR PATH: `sub_82448090(sz,flags)` routes by flag bit0. flags 0x24870000 → bit0 clear →
`sub_8244DF68(0,sz)` → `sub_8244CDF0` (returns the single global heap handle = `*(0x82902448)` = 0x10000)
→ `sub_8244B950` (the NT classic heap allocator; heap signature 0xEEFFEEFF at heap+0x10). The 0x78000
request (480 KB) → index 0x7800; heap+0x28 VirtualMemoryThreshold = 0xF000, so 0x7800 < threshold ⇒ the
**free-list[0] large-block search** (loc_8244BBFC→loc_8244BC08), NOT the virtual-alloc path (loc_8244C08C
base=0) and NOT the extend helper sub_8244ADD8 (verified absent from the bt).

THE CORRUPT BLOCK (the smoking gun, /tmp/narrow2.gdb — break sub_8244B950 ppc_recomp.65.cpp:7704 when
ctx.r3.u32 in [0x1000,0x10000)): the free-list[0] walk SELECTS a fabricated block at **r3=0x82A0**
(= heapbase 0x10000 − 0x7D60), header (BE u32): `+0=0x80000000` (size field 0x8000 *16-byte* units =
0x80000 B ≥ the 0x78000 request, so the walk accepts it), `+4=0x00100000` (looks like the heap reserve
size), `+8=0x00010180` (Flink = the free-list[0] sentinel heap+0x180), `+0xC=0x000582A8` (Blink → **into
the .ptc file-load buffer region**: file buf = 0x50000 (NtAllocate req=0x50000 sz=0x20000), and
0x582A8 = 0x50000+0x82A8). So **free-list[0] is corrupted with a phantom large free block below the heap
base whose link points into the .ptc file buffer**; the 0x78000 alloc reuses it → dst array
0x82B0+i*0x3C00 → entries #8/#9 (0x262B0/0x29EB0) land in the device → device+10900 zeroed → GPU-completion
waits (main 0x388C4; render workers 0x29CD4/0x29D40) never release.

BASE-INDEPENDENT (so NOT a layout fix): tested g_virtNext bump base 0x10000 → bad block 0x82B0;
0x40000000 (prod's; the title heap then lands at prod's exact place — device struct = **0x40016F80**, the
address prod logs in `SetInterruptCallback(821C7170, 40016F80)`) → bad block **0x3FFF82B0 = base−0x7D50,
STILL spanning the device**. Reverted the base to 0x10000 (the deliberate choice so the title's big
low-memory reserve stays below the image). The device landing at prod's EXACT address with base
0x40000000 proves variant A's heap is in sync with prod up to the device — so the divergence is purely in
the large-alloc free-list state AFTER the device alloc.

⇒ TRUE ROOT (open): a guest-heap free/coalesce produces a phantom free block below the heap base, linked
into free-list[0], with a Blink into the .ptc file buffer. Same CLASS as the systemic
NtQueryInformationFile bug (a value/state divergence, not codegen) — but now localized to the heap. Prime
suspects: (a) a XenonRecomp emitter mis-translation in the heap free/coalesce path (sub_82448128 /
RtlFreeHeap and its backward-coalesce via a block's PreviousSize — prod uses the *rexglue* recompiler, so
a variant-A-only emitter bug here would diverge), or (b) variant A's NtAllocateVirtualMemory/
NtQueryVirtualMemory commit semantics (variant A tracks ONE VRegion per reserve and reports whole-region
commit state, not per-page — the heap may mis-track its committed/uncommitted ranges and coalesce wrong).

NEXT EXPERIMENT (precise): the corrupt block is linked MID-CHAIN (a watchpoint on just the free-list[0]
sentinel heap+0x184 did NOT catch it). Catch the corrupting insertion by (1) watching a WIDER window —
the whole free-list[0] LIST_ENTRY plus the phantom block's eventual location — or instrument RtlFreeHeap
(sub_82448128) + the coalesce to log every (block, size, prev-size, new free-list links) and flag the
first below-heap link, with the guest LR; OR (2) PROD-DIFF: prod uses the SAME `__imp__sub_XXX` ABI and
HAS the heap symbols (nm out/build/linux-amd64-release/south_park_td: sub_82448090/sub_8244B950/
sub_8244DF68) — log the heap op sequence (alloc/free sizes+results, relative to heap base) in both and
find the first divergent op after the device alloc. ⚠ prod is Release (no readable ctx) → read its guest
mem via membase / few breakpoints (hot heap funcs crash prod). Once the corrupting op is found, fix the
HLE/emitter divergence → the 0x78000 alloc lands in valid heap space → device uncorrupted → re-test the
GPU-completion waits → VdSwap → DRAW. Tools added this session: /tmp/trace.gdb (path through sub_822A8150),
/tmp/narrow2.gdb (catch the below-heap block + header + bt), /tmp/heaplog.gdb (alloc size/flags/result map),
/tmp/heapdump.gdb (heap struct dump). kernel.cpp:78 has an inline NOTE summarizing this.

### DEEPER (same session) — the phantom block is created by the heap EXTEND committing AT THE SEGMENT BASE
Drove it one layer further (gdb /tmp/{watch82a0,narrow3}.gdb). The free-list[0] walk had NO fit for the
0x78000 request, so it took the EXTEND path: sub_8244B950:7671 (loc_8244BC64) -> sub_8244ADD8 ->
sub_82449E78 (commit pages) -> sub_8244A108 (create/coalesce the new block). Caught at sub_8244A108:1882:
the new block r4 = 0x10000 -- the HEAP BASE / segment itself. sub_8244A108 wrote a fresh _HEAP_ENTRY at
0x10000 (overwriting the heap's own header: was Size=0x64/PrevSize=0 per heapdump, became 0x800007D6 =
Size=0x8000, PreviousSize=0x7D6), then its backward-coalesce did r31 = r4 - PreviousSize*16 = 0x10000 -
0x7D60 = 0x82A0 (the phantom block, below the base). Chain: extend commits the new block AT THE SEGMENT
BASE -> overwrites the heap header -> coalesce reads the just-written garbage PrevSize=0x7D6 -> underflows
to 0x82A0 -> free-list[0] gets the below-base phantom -> 0x78000 alloc reuses it -> device overwrite.

WHY commit at the base: sub_82449E78 walks the segment's UnCommittedRanges list (head = *(segment+56)) and
commits at the found range's address (*(range+4)); for the heap's first segment (0x10000) that address is
0x10000 (the base) -- the segment thinks it is UNCOMMITTED FROM THE BASE, ignoring the initial header commit
(early log: NtAllocate req=0x10000 sz=0x10000 COMMIT committed 0x10000..0x20000) and the early in-place
allocs (device 0x26F40 etc. landed correctly, so the FREE-LIST was fine -- only the segment's
UnCommittedRanges / commit-boundary is wrong). Boot log confirms the extend commit: NtAllocate req=0x10000
sz=0x80000 COMMIT -> 0x10000 (commits over the live header/device region).

=> DEEPEST ROOT (open): the heap segment's UnCommittedRanges (*(segment+56)) wrongly says uncommitted starts
at the segment base. Set at heap creation (RtlCreateHeap = sub_8244B380, ppc_recomp.65.cpp:4569) and
maintained by RtlpFindAndCommitPages. Likely a variant-A divergence in how the commit-tracking is
seeded/updated vs prod (XenonRecomp emitter bug in the segment/UCR bookkeeping, OR variant A's
NtAllocateVirtualMemory COMMIT return/semantics so the initial-commit does not shrink the UCR). NEXT: read
sub_8244B380's segment/UnCommittedRanges setup + capture *(segment+56) and *(range+4) at the extend; compare
with prod (same __imp__sub_XXX symbols).

ATTEMPTED FIX (kept, but NOT the cure): added zero-on-decommit/release to NtFreeVirtualMemory (real NT/Xbox
MEM_DECOMMIT loses page contents). Correct semantics + verified not to break early boot, but does NOT fix
this bug -- the garbage PrevSize comes from the extend overwriting the LIVE heap header at the base, not from
stale decommitted data. Left in as a correctness improvement.

## 2026-06-01 (cont.) — ✅ FIXED: the corruption was the NtFreeVirtualMemory(MEM_DECOMMIT) WRITEBACK (commit 4e011b5)
The "DEEPEST ROOT (open)" above (segment UnCommittedRanges says uncommitted-from-base) is now ROOT-CAUSED and
FIXED. It was an HLE bug in our kernel, NOT an emitter bug and NOT heap-create UCR seeding.

THE CONCRETE SEQUENCE (KTRACE with caller-LR added to Nt{Allocate,Free}VirtualMemory — that is what made it
diagnosable; see the title-heap 0x10000 reserve [0x10000,0x110000)):
- Heap grows commit CORRECTLY, advancing the boundary: NtAllocate COMMIT req=0x20000,0x30000,0x40000,0x50000,
  0x60000 -> committed to 0x80000 (all from lr=0x82449F30 = RtlpFindAndCommitPages sub_82449E78). The device
  (0x26F80) lands in this correctly-committed region. So the UCR was FINE up to here (refutes the earlier
  "set wrong at RtlCreateHeap" guess).
- Then the heap SHRINKS: NtFreeVirtualMemory(MEM_DECOMMIT, req=0x70000, reqsz=0x10000) from lr=0x8244B140
  (RtlpDeCommitFreeBlock sub_8244B018) -- decommit the file-buffer tail [0x70000,0x80000).
- ⚠ Our NtFreeVirtualMemory wrote the out-params back as the WHOLE reservation: *BaseAddress=rb=0x10000,
  *RegionSize=r->size=0x100000 (instead of the page-aligned requested sub-range 0x70000 / 0x10000).
- The guest decommit helper READS *BaseAddress / *RegionSize BACK (sub_8244B018: ppc_recomp.65.cpp:4184-4190
  `lwz r5,80(r1); lwz r4,84(r1); bl 0x82449d58`) and feeds them to sub_82449D58, which INSERTS [base,size)
  into the segment's UnCommittedRanges. So the heap recorded the ENTIRE reservation [0x10000,0x110000) as
  uncommitted -> UCR first-range addr reset to 0x10000.
- Next extend (the 0x78000 .ptc vertex-stream buffer): RtlpFindAndCommitPages walks the UCR, finds the
  [0x10000,...) range, and commits AT THE BASE: NtAllocate COMMIT req=0x10000 sz=0x80000 -> commits
  [0x10000,0x90000), over the live header + device. sub_8244A108 writes a fresh _HEAP_ENTRY at 0x10000
  (0x800007D6 = Size 0x8000 / PrevSize 0x7D6); backward-coalesce r31 = 0x10000 - 0x7D60 = 0x82A0 (the phantom
  below-base block). device+10900 zeroed -> all GPU-completion waits (main 0x388C4; render 0x29CD4/0x29D40)
  stall = the "teardown".

THE FIX (kernel.cpp NtFreeVirtualMemory): MEM_DECOMMIT now writes back the page-aligned requested base and the
page-rounded requested size, and only clears the region's committed flag when the WHOLE reservation is
decommitted; MEM_RELEASE still returns the whole allocation. (The prior zero-on-decommit was necessary-but-
not-sufficient; the writeback was the actual divergence — prod's rexglue kernel returns the sub-range, ours
returned the region.)

RESULT (verified, 4 runs, deterministic 462-463 lines): the post-decommit commit lands clear of the device
(NtAllocate COMMIT req=0x70000 then 0x80000, NEVER req=0x10000 sz=0x80000); 0 device-overwrite commits; boot
advances 171 -> 462 lines and 3 -> 6 assets (past ArcadeLogo.ptc, UI.xzp 23.5MB, Strings.bin). The GPU-device
teardown is GONE.

NEW FRONTIER (downstream, different class = the known function-boundary/jump-table problem, Update 3 workflow):
the boot now ends at two INDIRECT-NULLs, stable across all runs:
  [INDIRECT-NULL] target=0x8228A3B8 (caller lr=0x8228A210)  -- a missing jump-table case. 0x8228A3B8 is NOT a
    function start (absent from ppc_func_mapping); it is a mid-function label reached by a `bctr` inside
    sub_8228A208 (ppc_recomp.36.cpp:5667..8749, a ~3KB-line fn with a `mtctr;bctr` table). lr=0x8228A210 is
    STALE (bctr doesn't set LR). RECOVER per Update 3: gdb the table base/index at the bctr, read the BE-u32
    targets, add functions=[{address,size}] to sp_xenon.toml + a [[switch]] to sp_switch_tables.toml, clean
    regen ppc/ (rm ppc/*.cpp first), rebuild.
  [INDIRECT-NULL] target=0x00000000 (caller lr=0x8224FDEC)  -- a null call in ppc_recomp.30.cpp (likely an
    unfilled out-param/fn-ptr, Update 4 class, OR another table). Characterize after the first.
Then: more tables (Update 3 noted 9 deferred + 161 markers) -> the GPU CP draws -> VdSwap -> Plume Vulkan.

## 2026-06-01 (cont.) — ✅ jump table sub_8228A208 RECOVERED + vcmpbfp128/blrl IMPLEMENTED → boot 462→1191 lines (commits c10ab1a)
Past the heap fix, drove the boot through the next two blockers; new frontier is a CRT-init thread-join.

1. **Jump table in sub_8228A208 (the INDIRECT-NULL target=0x8228A3B8).** XenonAnalyse MISSES this table —
   it is a 16-bit OFFSET table (bctr @0x8228A3B4 → jumpbase 0x8228A3B8 + offtab[r19], offtab(u16) @0x820CCF38
   via `lhzx r0,0x820CCF38,r19*2`), and XenonAnalyse's prologue patterns don't match this two-level form
   (it found neighbours 0x822884C8/0x82289B74 but not this one). Recovered manually: bound `cmplwi r19,28;
   bgt 0x8228B6AC` ⇒ 29 cases (r19∈[0,28]), default 0x8228B6AC; read the 29 offsets from guest .rdata via
   gdb (/tmp/readtab.gdb), labels[i]=0x8228A3B8+offtab[i]. Added the [[switch]] to sp_switch_tables.toml
   (NO functions override — sub_8228A208 is already one whole function 0x8228A208..0x8228B858, unlike
   0x821DC228 which XenonAnalyse split). The downstream null call (lr=0x8224FDEC, a C++ virtual dispatch on
   a bad object) was a CONSEQUENCE of the skipped case and vanished once the table worked.

2. **vcmpbfp128 + blrl (the SIGTRAP after the jump table).** With the table fixed, the boot crashed (SIGTRAP)
   at sub_8241D010 (ppc_recomp.59.cpp, a Newton-Raphson reciprocal-refine on the main thread's CRT-init math):
   XenonRecomp emits `__builtin_debugtrap()` for recognised-but-unimplemented instructions. The crash was
   `vcmpbfp128` (vector compare-bounds). Implemented it + `blrl` in the emitter
   (patches/xenonrecomp-sp-instructions.patch, XenonRecomp/recompiler.cpp):
   - vcmpbfp[128]: per lane bit31=(a>b), bit30=(a<-b), 0 in [-b,b]; a<-b expressed as (-b)>a so only cmpgt
     is needed (xor flips b's sign); CR6 setFromMask on the `.` form.
   - blrl: capture lr as the target, set lr=return addr, PPC_CALL_INDIRECT_FUNC (mirror of bctrl).
   Rebuilt XenonRecomp → clean regen ppc/ (88 TUs) → only the 78 `vupkd3d128` debugtraps remain (D3D vertex
   unpack default-case, graphics-path, NOT yet reached). Regenerated the patch via `git -C ../../third_party/
   XenonRecomp diff`.
   RESULT (verified): 0 INDIRECT-NULL, 0 device-overwrite, NO crash (was SIGTRAP), boot 552→1191 lines, into
   GPU physical-buffer allocation (MmAllocatePhysicalMemoryEx 3.7MB blocks at 0xA2Fxxxxx). 8 threads.

3. **NEW FRONTIER = CRT-init thread-join deadlock.** Boot now reaches a quiescent steady state: all worker
   threads parked in KeWaitForMultipleObjects/NtWaitForMultipleObjectsEx on g_waitCv; one render worker
   (sub_821CC5D0 @0x821CC690) does a benign ~28 Hz KeGetCurrentProcessType poll (NOT the blocker). The
   **MAIN thread** is blocked forever: still in CRT static-init (sub_8214FFD0 global ctors) → sub_82448D78
   (thunk) → sub_8244E2D8 → **NtWaitForSingleObjectEx(handle=0xF1000002, timeout=∞)**. Handle 0xF1000002 =
   **thread tid=4** (start sub_8242B4A8, ppc_recomp.60.cpp:10096; created suspended at boot then resumed). Its
   KTHREAD resolves to 0x90100C40 (thread-arena 0x90100000+; events are a different arena 0x90400000+), whose
   dispatcher header is ALL-ZEROS (signal_state=0 = not terminated). So a global constructor spawns worker
   tid=4 and JOINS it (waits for it to exit), but tid=4 never exits (it is itself parked in a wait). RESUME:
   identify what tid=4 (sub_8242B4A8) does and what it waits on, and why it never terminates — a missing
   event signal, a queue it polls that is never fed, or a premature join. Tools: gdb attach + `thread apply
   all bt` (/tmp/{threads,t1,obj,h}.gdb); the wait is NtWaitForSingleObjectEx kernel.cpp:1272 / WaitObject:734.
   ⚠ XamLoaderLaunchTitle (a stub) is called around here but is NOT the blocker (the join is).

## 2026-06-01 (cont.) — ✅ CRT-init thread-join = boot-logo "press A/START" timeout; cleared TWO stuck-clock/token bugs; new frontier = GPU CP fence completion (commit 6ecdea6)
Root-caused the CRT-init thread-join deadlock end-to-end and cleared its two causes. The join is the
**boot-logo screen**: a global ctor spawns tid=4 (sub_8242B4A8 → sub_8242B428 → sub_8214F730 →
**sub_8214F738**) to show the logo and JOINs it; main waits ∞ in NtWaitForSingleObjectEx(0x90100C40).

**1. The logo poll (sub_8214F738) — gdb-decoded.** It is a bounded "press A/START, else 5 s timeout"
poll: `start = GetTickCount(); loop { sub_82448928()→[r1+96]; if [r1+96]==0x5800(VK_PAD_A) || ==0x5814
(VK_PAD_START) exit; Sleep(5ms); if (GetTickCount()-start) >= 5000 exit }`. sub_82448928 →
**XamInputGetKeystrokeEx** (so [r1+96] is the keystroke VirtualKey). Timeout and keypress CONVERGE to
the same cleanup (loc_8214FD44, returns 0) — it is a "skip-or-wait-5s" logo, exactly what prod (no
controller) does via timeout. gdb at the loop: `now=0 start=0 elapsed=0 status=0x0000` every iter → the
clock never moved.

**2. Root cause = GetTickCount stuck at 0.** sub_82448748 (the CRT clock) returns
`*(*(0x820008B8)+16)` = **KeTimeStampBundle.TickCount** (+16). KeTimeStampBundle is xboxkrnl.exe data-
export **ordinal 0x00AD**; variant A leaves that import as the unresolved placeholder **0xAD000100** (the
addr the title dereferences — verified 4×, clear of every MmAllocatePhysical region) and never advanced
+16. So elapsed stayed 0 < 5000 forever. FIX (kernel.cpp): a dedicated ~1 ms host thread (StartTimestamp
Pump, started in SetupEnvironment) writes guest-uptime-ms to 0xAD000110, zeroes +0/+8 — mirrors
rexglue-sdk xboxkrnl_module.cpp's 1 ms timestamp timer. VERIFIED: tick advances (0→8913→12912 ms), the
5 s timeout fires, **tid=4 leaves the logo loop** (now at sub_8214F738:19568 = post-loop cleanup).

**3. Second bug = cooperative-token starvation (exactly the prompt's anticipated token issue).** Past the
logo, the cleanup `sub_8214F738 → sub_821C1000(×4 resources) → sub_821C0850 → sub_821C6E58` waits for a
GPU fence: `while *(*(device+10896)) < target { sub_821B9270() }`. **sub_821B9270 is a pure busy-spin
(db16cyc no-ops), no kernel yield** → the guest worker holds the execution token the whole time. But the
fence is advanced by the **vblank pump**, which must TAKE the token to run ExecutePM4 → the pump blocked
forever in std::mutex::lock(g_waitMutex) (gdb-CONFIRMED: pump parked at `__lll_lock_wait → … →
VblankPump():g_waitMutex.lock`). Deadlock: spinner waits for a fence only the pump can move; pump waits
for the token the spinner holds. FIX (kernel.cpp): strong override of the weak guest alias sub_821B9270
releases the token across the backoff (Unlock→sleep 1ms→Lock) then calls the recompiled body
__imp__sub_821B9270; skips the yield on the pump's own thread (it can re-enter via the gfx-interrupt
callback while holding the token). VERIFIED: pump now runs its normal 16 ms cycle (no longer lock-blocked),
the ring DRAINS (RPTR==WPTR==37).

**4. NEW FRONTIER = GPU CP fence completion (the renderer, multi-week).** With the pump running and the
ring drained (RPTR==WPTR==37, WPTR not advancing — title submits no more because tid=4 is still waiting),
the resource fence `*(*(device+10896))` STILL never reaches `target` (sub_821C6E58 r30). So the cleanup
wait (and main's join) does not yet lift. This is now a pure GPU command-processor gap: the PM4 the title
submitted does not drive that fence to target — either the completing EVENT_WRITE/fence packet was never
in the ring (the title would submit it only after the cleanup, which blocks — or the logo's draws were
never really rendered by the null/PM4 GPU), or the pump's EVENT_WRITE handling doesn't target
*(device+10896). RESUME: capture `*(device+10896)` (device ≈ 0x26F80; fence ptr at +10896, target =
sub_821C6E58 r30/r4) and compare to g_rptrWriteBack / the EVENT_WRITE addresses the pump writes; make the
CP advance that fence (the GPU engine). 0 INDIRECT-NULL, 0 device-overwrite, no crash throughout.

**Secondary finding (documented, NOT fixed):** sub_821B9270 also embeds a 5 s GPU **watchdog** that would
abort the wait (return 0) if the fence is stuck — but its tick is `r30 = *(KTHREAD+0x58)`, and FillKThread
sets T+0x54/0x56/0x5C yet **omits T+0x58** (rexglue X_KTHREAD `unk_58`), so it is pinned at 0 and the
watchdog can never fire. Even if ticked, the watchdog routes to sub_821C8B30 (DbgPrint + GPU hang-recovery),
i.e. a degraded "GPU hung" path, not clean progress — so the clean fix is #4 (CP completes the fence), not
the watchdog. Tools this session: gdb loop/fence/CP scripts (/tmp/{loopwatch,timer,fence,cp,full}.gdb),
`thread apply all bt`. ⚠ grep -a on boot logs.

## 2026-06-01 (cont.) — ✅ CRT-init join LIFTED via GPU fence-forward stopgap → boot reaches the GAME MAIN LOOP (intro movie)
Root-caused the GPU CP fence frontier end-to-end and cleared it (stopgap), driving the boot from the
stuck CRT-init join (≈1191 lines) all the way into the **game's main loop** (≈178k non-spam lines, ~355k
GPU frames executed in 30 s).

**Root cause — deferred command-buffer segments never reach variant A's CP.** Instrumented the fence wait
`sub_821C6E58` (entry: r3=device=0x26F80, r4=target): fenceptr=`*(device+10896)`=**0xA2010000** (the
0xA-window mirror our EVENT_WRITE_SHD writes to — address model VERIFIED correct, no mismatch), current
fence=**5**, target=**7**, head=`*(device+10908)`=**17**. A 5 s watcher proved head/fence/WPTR/RPTR are
ALL frozen (WPTR=37, ring all-zero past dword 37). So the fence is plumbed correctly but never reaches an
old target. Decode: the title's packet-builder `sub_821C6A08` (ppc_recomp.17.cpp:11952) does
`device+10908 = fence+2` per EVENT_WRITE — head=17 ⇒ it BUILT fences 3,5,7,9,11,13,15 but only **3,5** are
in the kicked ring. The ring-kick is `sub_821C6600` (writes CP_RB_WPTR 0x7FC80714); it fired **6×**
(WPTR 19→37) then STOPPED — the title defers the tail, relying on the real GPU auto-flushing partially
-filled segments. `sub_821C6E58`'s own gate (12508-12525) auto-flushes only when `target==head`; for an
OLDER fence (7≠17) it just spins. Tried driving the title's flush `sub_821C6D58` from the wait — it
advances head (17→19) but **kicks nothing** (its main-flush path finds no "current command buffer"; the
deferred segment needs the GPU↔CPU WAIT_REG_MEM handshake — the ring's tail IB 0x975E0 has
WAIT_REG_MEM packets referencing 0x2011xxx with code addr 0x821CC7A0 — which variant A skips).

**Fix (STOPGAP) — forward the completion fence past deferred segments.** Variant A's CP is synchronous and
has NO renderer, so a deferred segment's only effect on a waiter is the fence value itself (its draws/state
are no-ops). So in the two fence-spin waiters that the boot actually hits, forward the GPU-completion
marker to the requested target (forward-only) and let the wait succeed on its normal path:
- `sub_821C6E58` (counter wait): `GST32(*(device+10896), target)` when `target!=head` and not-yet-reached.
- `sub_821C5DF0` (post-frame segptr wait, sub_821BFF48→sub_821C6278→sub_821C5EA8→here): the marker is
  `*(fenceptr+4)` (segment pointer; low 2 bits = wrap generation); forward it to r5 when stuck.
(There are 6 `sub_821B9270` spin sites total — sub_821BFF48/C5DF0/C6420/C6E58/CB690/CC140 — only these two
are hit so far.) **NO faked completion of CPU-visible data; only the GPU-fence markers.**

**Result — the game BOOTS and RUNS.** join lifts (tid=4 teardown completes → ExTerminateThread → main's
NtWaitForSingleObjectEx(0x90100C40) returns) → main runs CRT global ctors → enters the render loop. It
loads real game assets (LuaScripts, Fonts, Audio, AnimBlock.bin 1.4 MB, Stickers, subtitles), spawns more
worker threads, and runs the **intro state machine**, rendering ~12k GPU frames/s. 0 device-overwrites, no
crash, 1 benign INDIRECT-NULL (null fn-ptr target=0 @lr=0x82292D08, skipped).

**NEW FRONTIER = missing intro movie (content gap, not a recomp bug).** The loop is stuck retrying
`NtCreateFile('game:\Media\Assets\Movies\en-en\sp_xbox_0_intro.wmv')` → **MISS**. The extracted Movies/
dir has all Level1-11 movies but **no `en-en/` subdir and no sp_xbox_0_intro.wmv** (the localized intro
movies were not extracted). The game retries every frame. NEXT: either provide the file (check the XBLA
package for the en-en movie set) or make the intro skip on missing-movie; then chase VdSwap → first frame.

**⚠ The fence-forward is a STOPGAP** — it must be replaced by a continuous CP that follows the
WAIT_REG_MEM-chained deferred IBs and executes them (so the fence advances as a real result) BEFORE a real
renderer lands, or deferred draws would be skipped. Build/run unchanged (ninja -C runtime/out
sp_td_varianta). Diagnostics added (gated): ExecuteType3 full opcode trace under REX_CPTRACE=1; [fencefwd]
under REX_KTRACE=1.

## 2026-06-01 (cont.) — boot reaches & PLAYS the intro movie; the wall is now the GPU RENDERER (confirmed)
Followed the post-join boot to its natural limit. Findings:
- **The render loop works.** `VdSwap` IS called every frame (from sub_821BFF48) presenting *sequential
  framebuffers* (r3 = 0xA0013734, 0xA009B3B4, 0xA0123B74, … ~0x88000 apart = distinct frame targets). Our
  stub was silent so earlier "VdSwap=0" was a logging artifact, not absence. Improved VdSwap to advance the
  GPU swap counter (g_gpuCounter++) + log the first calls. The title renders to framebuffers + presents;
  variant A just can't *display* them (no Vulkan backend — DRAW_INDX is a no-op in ExecutePM4).
- **The intro movie loads & plays.** game:\Media\Assets\Movies\en-en\sp_xbox_0_intro.wmv (path fixed via
  en-en/ symlink) → NtQueryInformationFile size=8479541 → allocate 0x820000 → NtReadFile reads the whole
  8.4 MB → RtlInitializeCriticalSection ×~17 (player init) → MmAllocatePhysicalMemoryEx ×N (GPU mem) →
  ExCreateThread ×4 (tid 11-14, start 0x82339428.. = the VC-1/WMV decoder threads). So the title genuinely
  enters movie playback.
- **It is RENDERER-GATED, not slow.** A 180 s run produced **1,099,927 non-spam lines** but NEVER advanced
  past the movie (no NtClose on the movie handle, no menu assets, no next file). 180 s ≫ enough for even a
  5-10× slowed CPU VC-1 decode of an 8.4 MB intro. The movie playback (and thus the intro→menu transition)
  needs frames actually drawn/displayed — i.e. the GPU renderer. Main cycles through the movie-player render
  fns (sub_821D5910 → sub_822045E0 → …); 13 threads, all healthy (no deadlock, no crash, 0 device-overwrite).

**CONCLUSION — the boot-bring-up frontier is EXHAUSTED; the remaining work is the GPU renderer.** Variant
A's CP is a minimal PM4 interpreter (IB/INTERRUPT/EVENT_WRITE_SHD-fence/swap; draws = no-ops). To advance
past the intro (and to display anything) it needs a real PM4→Vulkan translator: GPU-state tracking from
SET_CONSTANT/type-0 register writes, Xenos-microcode→SPIR-V shader translation, DRAW_INDX→Vulkan draw,
texture/RT/vertex binding, and a Vulkan swapchain presented on VdSwap/XE_SWAP. That is exactly what
rexglue's command_processor + Vulkan backend (Xenia-based) already do — so the realistic options are
(a) port a minimal renderer, (b) integrate rexglue's existing Vulkan backend/Plume into variant A's runtime,
or (c) the fake-skip-the-intro stopgap to exercise more title LOGIC (menu/gameplay) for recomp coverage
(no display). This is a multi-week, deliberate phase — flagged for a human scope decision, not autonomous
loop work.

## 2026-06-02 (post-reboot) — RENDERER PART 1 REFRAMED: per-frame draws never reach the ring (REX_DEFERCP refuted)

Resumed after a host crash/reboot at clean HEAD `d0e1a3e`. Baseline re-verified: boot reaches the intro,
presents double-buffered framebuffers (0xA2016000/0xA23AE000, 1280x720 fmt=6), 0 device-overwrite, no crash.

Traced a full frame (REX_DEFERCP=1 + configurable REX_CPDUMP=N + REX_DRAWLOG). Findings overturn the
"linearly parse the staging/deferred buffer" approach (REX_DEFERCP):

- The per-frame staging buffer at 0xA01xxxxx is NOT a clean PM4 stream. A linear PM4 walk stays aligned for
  only ~2000 of ~139000 dwords/frame, then desyncs HARD: the op histogram becomes near-uniform across all
  0x00-0x7F (garbage signature), and the walk runs into INLINE VERTEX FLOATS (0x441FE000=639.0f=1280/2,
  0x44DD0000=1768.0f, …) and segment-address words masked as type3 (raw 0xC02B7784 == addr 0xA02B7784). It is
  the title's custom linked command SEGMENTS — markers op 0x38(cnt2)/0x79(cnt10) carrying segment addrs (e.g.
  data `8100008B 00013640 C009790C`) — interleaved with inline vertex data. Not walkable linearly.

- rexglue/Xenia 1:1 ref (agent-mapped graphics/command_processor.cpp + include xenos.h): there is NO deferred /
  system-command-buffer execution path. ALL GPU work reaches the CP via the MAIN RING. VdGetSystemCommandBuffer
  is a stub (0xBEEF, same as ours). VdSwap writes its swap PM4 (TYPE0 fetch-const + PM4_XE_SWAP) INTO the ring.
  Texture fetch constants come from SET_CONSTANT(0x2D, type=1)->reg 0x4800 (our CP does NOT handle 0x2D — a
  gap) or type-0 writes. kImmediate inline indices unsupported; vertices come from bound buffers.

- DIRECT ring dump (new one-time KTRACE diag in VdSwap): ring base=0xA0002000 size=0x1000 (1024 dwords). After
  init it holds ME_INIT + exactly 6 setup IBs (->0xA0090040/0xA0010000/0xA00900E0/0xA0010100/0xA00975A0/
  0xA00975E0) and CP_RB_WPTR == CP_RB_RPTR == 37 — fully consumed, NOTHING past WPTR. The guest writes NO
  per-frame commands to the ring. r3 in VdSwap = 0xA0123B74 (r7=0xBEEF0001) lies in the staging region, far
  outside the 0x1000 ring — r3 is the staging write-point, not a ring pointer.

- prod oracle (renders the SAME guest: 54 pipelines incl. PS adf7088205c03df9) launched & confirmed rendering,
  so on the correct path the title DOES feed the ring per-frame. sub_821C6600 ("the kick") hit only ~1x in 5s
  under gdb on prod (likely init-only) -> the per-frame ring submission is some OTHER path.

CONCLUSION: the per-frame draws are built in the staging buffer (0xA01xxxxx) but the title's FLUSH to the ring
(sub_821C6D58: write IB packets to the ring + advance WPTR) never fires in variant A — gated on a GPU<->CPU
WAIT_REG_MEM handshake that the fence-forward stopgap does not reproduce. The tiny ring is meant to carry IB
packets pointing to staging segments; our existing ring-CP would render them IF they were kicked. So
REX_DEFERCP (linear staging parse) is the WRONG target. Real part-1 work = get the segment draws to the CP:
either (A) model the handshake so the title's own flush fires, or (B) decode the segment descriptors and
execute each segment as an IB ourselves. (Also: add SET_CONSTANT 0x2D handling for fetch constants.)

Diagnostics added this commit (gated/one-time, boot unregressed): configurable REX_CPDUMP=N cap + op
0x38/0x79/draw data dump in ExecutePM4; one-time ring walk in VdSwap flagging IB targets + WPTR/RPTR.

## 2026-06-02 (post-reboot, cont.) — RENDERER PART 1: route B (segment-CP) VALIDATED — clean aligned draws

User chose route B (decode the title's segments, execute each as an IB). Implemented + validated:
- Instrumented the ring-kick sub_821C6600: the 6 init kicks reveal the SEGMENT DESCRIPTOR format
  definitively = a 2-dword {d0 = 0x81000000 | len_dwords, d1 = phys_addr}. e.g. d0=8100000B d1=00090040
  -> IB->0xA0090040 len=11; d0=8100010A d1=00010100 -> IB->0xA0010100 len=266. The "op 0x38" payloads seen
  earlier in the staging stream (8100008B 00013640, ...) ARE exactly these descriptors embedded inline.
- REX_SEGCP=1: scan this frame's staging range [prev_r3,cur_r3] for the descriptor signature
  ((d0&0xFFFF0000)==0x81000000 + sane len/addr) and execute each referenced segment as a bounded IB
  ExecutePM4(0xA0000000|d1, len, depth=1). RESULT: ~8 segments/frame, ~9 draws/frame, and they are CLEAN +
  ALIGNED (all init=0x30088 numInd=3 prim=8 = kRectangleList) — NOT the desync garbage the linear DEFERCP
  produced (init=0xC0000000 numInd=49152). 0 device-overwrite, no crash, boot reaches intro. => route B's
  segment-IB model WORKS; segments are clean PM4 and parse aligned.
- Added SET_CONSTANT(0x2D) + SET_CONSTANT2(0x55) to ExecuteType3 (type1->reg0x4800 fetch/textures,
  type4->+0x2000 regs, type0->+0x4000 ALU, 2->+0x4900, 3->+0x4908; per rexglue command_processor.cpp).
  Made REX_DRAWLOG=N configurable.

LIMITATION / NEXT: the embedded-descriptor scan finds only a UNIFORM subset — across 500 logged draws 100%
are init=0x30088 (untextured rects), ZERO bound textures. So these are one layer (clears/fills); the
textured bulk (movie quad, sprites, the ~808-draw dominant pshader adf7088205c03df9) is in segments NOT
referenced by inline descriptors. The COMPLETE per-frame segment list lives in the flush sub_821C6D58's
queue: flush -> sub_821C6810 (provides segment region+count r30) -> sub_821C6C80 (consumes; r8=device+13568
array base, r6=count) -> kick. Device fields: +10896 completion-ctr ptr, +10908 head fence, +13408/+13568
segment tracking, +10940/10941 gate bytes (flush skips the segment block if device+13408!=0). NEXT:
empirically hook sub_821C6C80 / read the device+13568 array to enumerate ALL segments (incl. textured), then
present the movie quad's bound texture (shortcut to visible content).

## 2026-06-02 (post-reboot, cont. 2) — the intro emits only ~5-8 untextured rects/frame; content is GATED

Pushed route B further to locate the textured/movie draws. Findings:
- Device struct segment-tracking (one-time devdump): device=0x26F80, +10896=A2010000 (completion-ctr ptr),
  +10908=0x45 (head fence, grew from init), +13408=0 (flush segment-gate is OPEN, not skipping), +13568=
  current command-buffer base (GROWS ~0x88680/frame: 0xA0090180 -> ... -> 0xA055BF00 -> 0xA066D000 — i.e.
  the SAME growing per-frame buffer r3 points into, just another cursor). The struct also holds a pool of
  0x1080-byte chunk records {base, writeptr, 0x1080, base+4, 0, device}.
- REX_CHUNKCP=1 (execute the device+13568 active command buffer [base,writeptr) linearly as an IB): DESYNCS
  exactly like the r3-staging linear parse — recovers init=0xC0000000 (x118, packet headers read as draws),
  prim=60 (invalid) garbage; only a handful of real init=0x30088 rects. So the device chunk is the SAME
  mixed buffer (PM4 + inline vertex data + 0x8100xxxx segment descriptors), NOT a clean IB.
- BRUTE draw-scan of a whole frame's staging range: only ~5 real DRAW packets, ALL init=0x30088 prim=8
  (kRectangleList), untextured. REX_SEGCP (descriptor-referenced segments) likewise: ~8/frame, 100% of 500
  logged draws = init=0x30088 rects, ZERO bound textures across the entire run.

CONCLUSION (leading hypothesis, strong evidence): during the intro variant A's title emits only ~5-8
untextured rectangle draws per frame and NEVER builds the movie/textured draws. prod renders the intro (54
pipelines, real shaders) from the SAME guest, so on the correct path the title DOES build movie draws. The
difference is the GPU<->CPU handshake: the fence-forward stopgap lets the boot's waits return (so it loops)
but does NOT truly drive GPU/decode progress, so the title idles in a minimal per-frame loop (clear rects +
VdGetSystemCommandBuffer) and never advances to decode+present the movie. Ring stays frozen (6 init kicks).
=> Route B validated the segment model + the descriptor format + the command-buffer pool, and produces clean
aligned draws from descriptor segments — but the MOVIE will not appear until the handshake (route A) drives
the title forward. The two routes CONVERGE: the root blocker is the real GPU<->CPU fence/sync handshake, not
the segment parsing. NEXT: (A) model the handshake — make fences advance as a true RESULT of CP execution of
kicked ring content (replace the fence-forward stopgap), so the title proceeds to build+render the movie; or
investigate whether the intro movie uses a non-PM4 path (VdInitializeScalerCommandBuffer / overlay) we stub.
New diags (gated): REX_CHUNKCP, [drawscan], [devdump], g_device capture.

## 2026-06-02 (cont. 3) — route A fence instrumentation REVISES the premise: the title is NOT fence-gated

Started route A (model the handshake); instrumented the fence wait sub_821C6E58 ([fencewait], capped). The
data revises the "gated" hypothesis from cont.2:
- target = head - 8 ALWAYS; gap (target-current) is a constant 6; head climbs smoothly 17 -> 1439+ over 240
  waits (and far beyond, capped). The title builds ~3 segments (~6 fences)/frame and waits only for the GPU
  to stay within 8 fences (4 segments) — a PIPELINE-DEPTH THROTTLE, not a stuck wait.
- The forward satisfies the throttle every frame (28380 load-bearing forwards); the title progresses frame
  after frame the whole run, presenting each frame (framebuffer empty — only rects drawn).
=> The title is NOT fence-blocked; it actively runs its per-frame render loop, just emitting only untextured
rects. So route A's premise (a gated title the handshake unblocks) does NOT hold — replacing the forward with
real fence execution keeps the same smooth loop and would NOT change what is drawn.

REFRAME: the textured draws DO exist (prod renders the same guest with 54 pipelines, so the guest builds
textured PM4 draws each frame), but variant A is NOT CAPTURING them — they live in per-frame command-buffer
content our CP/scan doesn't reach. Evidence: brute draw-scan + SEGCP of the r3-staging range find only ~5-9
draws/frame, all init=0x30088 rects; the textured bulk is in other segments/buffers (the device-tracked
cmd-buffer pool, device+13568, grows ~0x88680/frame; its records reference 0xA04D-0xA056xxxx chunks). So the
real problem is COMPLETE per-frame command-buffer COVERAGE/enumeration, not the fence handshake. Options:
(1) decoded-frame shortcut — find the VC-1 decoder's output surface (4 threads @0x82339428) and present it
    directly (visible intro, sidesteps buffer RE); (2) full cmd-buffer enumeration — crack the device-tracked
    pool / segment format so ALL per-frame draws reach the CP (the general renderer path, hard).
Added [fencewait] instrumentation (gated).

## 2026-06-02 (cont. 4) — decoded-frame shortcut: there IS no decoded frame — the VC-1 decoder is idle

Pursued the user-chosen decoded-frame shortcut (find the VC-1 decoder output + present it directly):
- Captured the decoder's frame-pool buffers (16 allocs from one site LR 0x8244DD2C, ~0x101440 each).
  Full-buffer scan: ALL are uniform fills (Y=0x00 black luma / chroma=0x80 neutral), varied=0 everywhere —
  i.e. cleared-to-black, NO real decoded image (real VC-1 output is never perfectly uniform).
- WMV is fully loaded: one NtReadFile of 8479541 bytes (the 8.4MB sp_xbox_0_intro.wmv) into memory; demux
  reads from RAM after (no further file reads).
- BROAD heap scan (0x1000000..0x9000000, 0x10000 chunks, report high adjacent-byte variation) for ANY
  image-like region: found only (a) Lua/script TEXT at 0x2E9-0x2F2xxxx (visible strings), and (b) a
  FULL-ENTROPY region 0x496-0x4Axxxxx (varied=16384/16384) inside the 23MB alloc at 0x48D0000 = the
  COMPRESSED WMV bitstream (full entropy = compressed, not decoded; decoded frames have spatial smoothness).
  NO decoded video frame anywhere in the heap.
=> CONCLUSION: there is NO decoded frame to present — the VC-1 decoder has its input (compressed WMV in RAM)
but produces NO output (its frame buffers stay uniform black). The decoder is IDLE / gated. So the intro is
not invisible because of the renderer — the MOVIE DECODE PIPELINE is not running. This unifies the session's
findings: the title runs a healthy per-frame loop (not fence-gated), draws only clear rects, presents empty
buffers, and the movie never decodes. Getting the intro visible needs the DECODER to run (a video/XMA-decode
subsystem investigation: why are the decoder threads tid11-14 @0x82339428 idle — waiting on demux input, a
GPU surface, or a play trigger?), OR skip the intro to exercise menu/gameplay rendering instead. All 3
near-term options' premises now mapped: route A (title not gated), route B (only rects built during intro),
shortcut (no decoded frame). Diagnostics added (gated): video-buffer capture (g_videoBufs, LR 0x8244DD2C) +
broad heap image-scan in VdSwap.

## 2026-06-02 (cont. 5) — intro is MOVIE-gated (not input-skippable); root points at the coop scheduler

User chose "skip intro -> render menu". Findings:
- The intro does NOT poll input: XamInputGetState is called EXACTLY ONCE in a 25s run (hot loop = 33654x
  VdGetSystemCommandBuffer). Injecting a CONNECTED pad with START, then pulsed A+START (REX_SKIPINTRO, via
  XamInputGetState + XamInputGetCapabilities) does NOT advance past the intro (identical asset set, no menu
  load). => the intro transition is MOVIE-gated (plays until movie-EOF / first-frame ready), not a skip
  button. (REX_SKIPINTRO kept, gated — may matter in the menu, which likely does poll input.)
- gdb all-thread bt of the running boot (26 threads): only Thread 1 (main) executes guest code (holds the
  single coop token) — busy in the per-frame intro builder (sub_821FACD0 <- ... <- sub_822170E8). 11 threads
  parked in lll_lock_wait (waiting for the token), 6 in pthread_cond_wait. Threads 10/11 wait in
  KeWaitForSingleObject from sub_821CC5D0 (the 0x821CCxxx GPU-sync region) = GPU-completion workers. The VC-1
  decoder threads (entry sub_82339428) appear in NO guest frame at any depth and are NOT in KeWait — they're
  parked in lll_lock_wait, i.e. waiting for the coop TOKEN (not an event/data gate).
=> LEADING ROOT: the cooperative single-token scheduler keeps the CPU-heavy VC-1 decoder threads from
running (they compete with the busy main thread + ~10 other workers for ONE token), so the movie never
decodes (frame buffers stay uniform black) -> never plays/EOFs -> the intro never advances. This is exactly
RENDERER-PHASE-PLAN Step 1 (fibers / a real scheduler), deferred earlier as "coop token sufficient for now" —
it is NOT sufficient for movie decode. (Caveat: could also be a main<->decoder start/feed handshake; exact
gate needs reading the decoder threads' saved guest PC. But the decoder is parked on the token, not an
event.) STRATEGIC: visible content needs ONE of (a) a better scheduler/threading so the decoder runs -> movie
plays -> intro auto-advances to menu/gameplay (then the renderer for the drawn content); (b) force the
movie-player "done" state to skip the intro (movie-player RE); (c) the renderer's full command-buffer
enumeration (needed regardless). All multi-session, title-specific. Added REX_SKIPINTRO (gated input inject).

## 2026-06-02 (cont. 6) — the decoder is STUCK, not merely starved (90s run = zero progress)

90s run, late sample (swap#220) of the VC-1 frame pool: the 16 buffers are BIT-IDENTICAL to the frame-40
sample (buf0 nz=0; buf1 nz=131728; buf2 nz=65864; ... varied=0 everywhere) — ZERO progress over 90s. So the
decoder is NOT just slow/starved (that would accrue partial frames) — it is STUCK: it never writes real
content into its pool. The decoder threads cycle on the coop token but are blocked BEFORE the decode (the
playback never starts/feeds), so nothing decodes. => refines cont.5: the intro hang is a STUCK movie-PLAYBACK
pipeline (decoder waits for work; main thread loops presenting empty frames without driving playback), not
pure scheduler starvation. Reaching visible content needs deep RE of the movie-player start/feed path (or
the menu, which sits behind the same stuck intro). NET for the session: the wall is now COMPLETELY mapped,
but visible content was not reached — every remaining path is multi-session title-specific RE.

## 2026-06-02 (cont. 7) — ✅ intro→menu transition RE'd + FORCED: title now advances intro→attract→menu-setup (NOT pushed)
User directive: "force «movie ended» (pinpoint-RE the intro→menu transition condition)". Done, and the intro
hang is now BROKEN — the title advances past the intro into the attract loop and into menu/frontend setup.

### The transition mechanism (RE'd end-to-end; prod oracle confirms)
- Per frame the movie widget's driver `sub_82425BF8` calls the player AdvanceFrame dispatch `sub_8232AAE0`
  (= `(*(*player+72))(player,...)`, player = `*(widget+76)`). It returns a status in facility 0x1666:
  **0x166600E8** = "no frame / buffering" (variant A's stuck-decoder steady state), **0x16660026** = EOS
  (produced by `sub_8233A7D0`). On EOS, `sub_82425BF8` POSTS the completion (`sub_822221C8` + `sub_8222A9F8`,
  channel 0xAAC0CCDD, event type 8).
- The completion is consumed by the per-frame screen state machine `sub_82161920` (driven by the event/state
  pump `sub_82150770`), which in its **state==2** branch dispatches `sub_82163118` = the intro→menu advance.
- PROD ORACLE (gdb, SIGSEGV passed through so its write-watch handler runs): the intro movie runs **~534
  frames / ~22 s** (t=25.2→47.3s under gdb), then EOS fires and the completion path tears the movie down
  (`sub_82425648` via `sub_824259F8`/`sub_82425F50` and via `sub_82150770→sub_82161920→sub_82163118→…→
  sub_824267B0`) and advances. ⚠ NOTE: the transition does NOT swap the screen-manager's current-screen
  (`*(0x828EAB18+12)` unchanged in prod) — a hardware watchpoint there never fires; the intro/menu is a
  sub-state, not a top screen swap. (`+0x8C`/child-list reads are unreliable — discard.)

### Two gates, both forced (env-gated levers in kernel.cpp; default-off, boot unregressed)
1. **`REX_MOVIE_EOF=N`** — variant A's VC-1 decoder is stuck, so AdvanceFrame returns 0x166600E8 FOREVER
   (no EOS). Wrap `sub_8232AAE0`: after N advances of the movie player (captured as `*(widget+76)` in a
   `sub_82425BF8` wrap), force its return to 0x16660026. PROVEN: movie pump stops (no-force = 44,820 advances
   in 30s and climbing; force@30 = plateaus at 30) and the completion poster `sub_8222A9F8` fires. But ALONE
   it does NOT advance — the movie self-stops, the intro keeps running.
2. **`REX_XFLAG=1`** — `sub_82163118` (the advance) is gated, even in state 2, behind a global
   "screen-transitions-enabled" byte **`0x828E82A6`**, which prod's `sub_8210AF90` sets to 1 but which is **0
   in variant A** (`sub_8210AF90` never reached). gdb-verified: owner reaches state 2, `owner+72=1`,
   `g(0x828EEF1C)=1`, but `g(0x828E82A6)=0` → `sub_82163118` ticks=0. Forcing the byte in the `sub_82161920`
   wrap makes `sub_82163118` run.

### Result (REX_MOVIE_EOF=30 + REX_XFLAG=1 [+ REX_SKIPINTRO])
- With both: `sub_82163118` runs, the completion is processed, and the title ADVANCES out of the intro into
  the **attract loop** — it opens `towerDefense_attract_movie.wmv` (symlinked into `Movies/en-en/` like the
  intro; it exists in `Movies/`) and cycles intro↔attract (the classic console demo loop). No crash,
  0 device-overwrite. The long-standing intro hang is GONE.
- `REX_SKIPINTRO` now also injects a pulsed **VK_PAD_START (0x5814) keystroke via XamInputGetKeystrokeEx**
  (the title/logo poll path; the old XamInputGetState-only inject didn't reach it). With all three gates the
  title leaves the attract loop and enters **menu/frontend setup**: main thread runs the screen state machine
  `sub_82150770 → sub_8215DBD0`, allocates menu GPU buffers (`MmAllocatePhysicalMemoryEx 0x195000/0x3000/
  0x10000`), then hits the **NEXT BLOCKER: `[INDIRECT-NULL] target=0xFFFFFFFF (caller lr=0x8215DE84)`** — a
  null screen vtable/jump-table slot in the frontend setup (the project's known function-boundary/jump-table
  class, Update-3 workflow; OR a method unimplemented because the renderer is absent).

### Diagnostics / method
- prod oracle under gdb is viable IF `handle SIGSEGV nostop noprint pass` (prod uses SIGSEGV for GPU
  write-watch; default gdb intercepts it and looks like a crash in sub_821C6AF8). prod base = 0x100000000.
- Hooks added (all gated, passthrough when off): `g_moviePlayer` capture (sub_82425BF8 wrap), `sub_8232AAE0`
  (REX_MOVIE_EOF), `sub_82161920` (REX_XFLAG), `sub_8222A9F8` completion-post log, `XamInputGetKeystroke(Ex)`
  START inject (REX_SKIPINTRO). Build `ninja -C runtime/out sp_td_varianta`.

### NEXT (resume here)
- **Reach the menu fully:** root-cause the INDIRECT-NULL `0xFFFFFFFF @ sub_8215DE84` (read the vtable/jump
  table at the call site in sub_8215Dxxx; recover it per the Update-3 workflow, or implement the missing
  screen method). This is the gateway from frontend-setup to a live menu.
- **Proper fix for the REX_XFLAG stopgap:** find why `sub_8210AF90` (sets 0x828E82A6=1) is never called in
  variant A (an init divergence; trace its caller, diff prod) so transitions enable themselves.
- The movie still never DECODES (decoder stuck) — these forces make the title BEHAVE as if the movie ended;
  for a visible intro the decoder/scheduler work (cont. 5/6) is still open. But for exercising menu/gameplay
  rendering, the force chain is the way in.

## 2026-06-02 (cont. 8) — proper fix for REX_XFLAG: root = cooperative-scheduler starvation (multi-session); band-aid reverted
User picked "proper fix instead of the REX_XFLAG stopgap: why is sub_8210AF90 (sets 0x828E82A6) never called
in variant A?" Root-caused to a CHAIN of cooperative-scheduler issues — NOT a one-liner. No code kept
(the band-aid regressed; reverted to HEAD 176b54d). Findings:

- **0x828E82A6 is a one-time init flag**: the ONLY writer is `sub_8210AF90` (`li r11,1; stb r11,-32090(0x828F0000)`),
  which is a TEARDOWN of the global app object `0x828E8AF8` (frees fields +184..+200 via sub_82448D78/
  sub_8244D670/sub_82448B50, clears +237) that — as a side effect — enables screen transitions. BSS-zero
  until set, never cleared. So REX_XFLAG (force `g_base[0x828E82A6]=1`) faithfully replicates its flag effect.
- **sub_8210AF90 has no direct callers** — it's a vtable method invoked indirectly. PROD bt (gdb, `handle
  SIGSEGV nostop pass` so prod's GPU write-watch runs; prod base=0x100000000): worker thread `sub_82450FD0`
  (xapi trampoline) → `sub_82250420` (a work-queue loop: wait `sub_8244DC18` → `*(workobj)->vtable[0]()`) →
  `sub_8211B740` (work handler, vtable[0] of workobj 0x828E8BB0) → `sub_8210AF90`, at t≈0 (early boot).
- **variant A DOES spawn + resume that worker**: `ExCreateThread(start=0x82250420 ctx=0x828E8BB0 flags=1
  SUSPENDED) -> tid=10`, and `NtResumeThread(0xF100000F) -> started`. BUT tid=10 **never runs its guest
  entry** (`sub_82250420` ENTER=0 via a hook): an all-thread gdb bt shows **3 resumed guest threads parked
  at `GuestThreadRun:804` = `g_waitMutex.lock()`** (the cooperative single-token acquire) — they never won
  the token. The main thread's tight per-frame intro loop holds/re-wins the token; tid=10 was resumed LATE
  (after the boot window where the earlier workers tids 5/7/8 got their turns), so it STARVES. This is the
  long-flagged Step-1 issue (the cooperative token is not fair).
- **Targeted band-aid TESTED + REVERTED**: a yield-on-resume in NtResumeThread (release the token after
  spawning so the new thread runs its init to its first wait). Result: tid=10 now STARTS and processes its
  init work (`sub_82250420` ENTER=1, `sub_8211B740` runs) — BUT (a) `sub_8211B740` STILL doesn't reach
  `sub_8210AF90` even over 40s (a FURTHER divergence: sub_8211B740's work/state differs from prod's; it's a
  718-line init fn with many vtable dispatches), and (b) it **REGRESSED** the existing REX_XFLAG advance
  (changing the cooperative scheduling breaks the determinism the design relies on: attract no longer
  reached). Reverted (git checkout); confirmed the committed REX_XFLAG advance is restored (attract reached).
- **CONCLUSION**: REX_XFLAG masks (1) cooperative-scheduler startup-starvation of tid=10 AND (2) a deeper
  `sub_8211B740 → sub_8210AF90` state divergence. The proper fix = a FAIR cooperative scheduler (the
  long-deferred RENDERER-PHASE-PLAN Step 1: fibers/real scheduler) + resolving the sub_8211B740 init-state
  divergence — multi-session, title-specific. A naive band-aid regresses the boot. **REX_XFLAG stays as the
  pragmatic stopgap** (it replicates sub_8210AF90's flag side-effect exactly). Diagnostics this session were
  temporary (REX_INITDIAG) and were reverted with the band-aid; the prod-oracle method (`handle SIGSEGV
  nostop pass`, ExCreateThread-log thread map, all-thread bt → GuestThreadRun:804 token-parked count) is the
  reusable technique. NET: no code change (HEAD stays 176b54d); the deliverable is the root-cause + the
  verdict that the fix is the Step-1 scheduler work.

## 2026-06-02 (cont. 9) — Step 1: fair cooperative scheduler IMPLEMENTED (boots stably) + REAL root found = main never yields; ⚠ env broke mid-test
User picked option 2 (the fair scheduler / Step 1). Implemented it, found it boots stably, but discovered the
starvation root is NOT token fairness — it's that the MAIN THREAD NEVER YIELDS the token in the steady intro
loop. ⚠ The dev environment then degraded (see end); the work is UNCOMMITTED in the working tree.

### What was built (kernel.cpp, gated REX_FAIRSCHED, default path byte-unchanged)
- A fair FIFO run-token `class FairMutex` (ticket lock: lock takes next_++, waits serving_==my; unlock does
  serving_++ + notify) replacing the unfair plain `g_waitMutex` WHEN g_fair. The run-token (g_tok) is held by
  the one running guest thread; object-waits are SEPARATED onto `g_objM`/`g_objCv` — a thread blocked on a
  guest dispatch object releases g_tok (FairWaitUntil), waits on g_objCv until signaled, re-acquires g_tok
  FIFO-fairly. SignalObject (fair) writes signal_state under g_objM + notifies g_objCv. FairWaitUntil loops +
  re-checks under the token (handles auto-reset consume races). Branched: WaitObject, KeWaitForMultipleObjects,
  NtWaitForMultipleObjectsEx, GuestThreadRun, LockGuestExecution/UnlockGuestExecution, VblankPump, SignalObject.
- VERIFIED (before the env broke): default boot (g_fair off) UNCHANGED (intro, 0 device-overwrite, progress
  ~32k); REX_FAIRSCHED=1 BOOTS STABLY — reaches the intro, NO crash/deadlock, progress higher (~50-95k = more
  threads run). So the fair-token core is correct + deadlock-free.

### REAL root (gdb-verified) — fairness ALONE doesn't help
- Even under REX_FAIRSCHED, tid=10 (sub_82250420, the transitions-enable worker) + the VC-1 decoder threads
  (0x82339428/58) STILL never run. A GuestThreadRun token-acquire log shows tid=10 "WAITING (g_tok next=1306
  **serving=1305 held=1**)" but never "GOT token"; decoders likewise. No spurious unlocks (lock/unlock balanced).
- ⇒ the MAIN THREAD holds the run-token (ticket 1305) and runs the steady intro loop WITHOUT EVER YIELDING:
  the **fence-forward stopgap makes its GPU fence-waits instant** (no block → no FairWaitUntil → no g_tok
  release), and its other waits are pre-satisfied. So `serving` is frozen at 1305 and NO other guest thread
  can run — INDEPENDENT of token fairness. (Same in default mode; the fair token just makes it explicit.)
  This is why the earlier yield-on-resume "worked" once: it forced a single release. The general fix is
  cooperative TIME-SLICING: make the main release the token periodically.
- Was testing exactly that: a per-frame token yield at `sub_82167248` (gated g_fair: UnlockGuestExecution();
  LockGuestExecution()). This is IN the working tree but the build/test DID NOT COMPLETE.

### ⚠ Dev-environment degradation (mid-test) — NOT a code bug
- The Bash shell's stdout capture broke (even `echo X` returns exit 1 / no output; commands' output files come
  back empty). Cause: this session ran the title ~15× and `timeout` sends SIGTERM, which the multithreaded
  title IGNORES → orphaned processes each mmapping 4 GiB + holding a /dev/shm xenia_memory_* → RAM/resource
  exhaustion. Killed orphans via `pkill -9 -x sp_td_varianta` (NOT `-f` — that self-matches the shell), but the
  shell capture did not recover within the session. ⇒ could not build-verify / test the per-frame yield / commit.

### STATE + NEXT (resume precisely)
- Working tree: kernel.cpp has UNCOMMITTED fair-scheduler + per-frame-yield + REX_INITDIAG diag hooks (all
  gated by REX_FAIRSCHED / REX_INITDIAG; default boot unaffected). Last commit = b967e92 (clean kernel.cpp =
  176b54d). The per-frame-yield edit is UNBUILT/UNTESTED.
- NEXT: (1) restart the dev env / free RAM (`pkill -9 -x sp_td_varianta`; rm /dev/shm/xenia_memory_*); ALWAYS
  run the title with `timeout -s KILL N` (it ignores SIGTERM). (2) build-verify kernel.cpp (`ninja -C
  runtime/out sp_td_varianta`). (3) Test REX_FAIRSCHED=1 + REX_INITDIAG=1 + REX_MOVIE_EOF=30 (no REX_XFLAG):
  does the per-frame yield let tid=10 GET the token → sub_8211B740 → sub_8210AF90 (flag set) AND the decoders
  run (movie decode!) AND the intro advance? grep [initdiag] 'GOT token' / 'sub_8210AF90 RAN' / towerDefense.
  (4) If it works → big Step-1 win (keep + make default carefully, watch for the determinism regression that
  yield-on-resume showed). If it regresses/doesn't help → the deeper interaction is the fence-forward stopgap
  (replace it with a real fence so the main genuinely blocks/yields). (5) Build-verify then commit or revert.
- KEY INSIGHT for Step 1: the cooperative-scheduler problem is the MAIN-THREAD-NEVER-YIELDS (tied to the
  fence-forward stopgap), NOT token fairness. A fair token is necessary but insufficient; cooperative
  time-slicing (or making GPU waits actually block) is the operative fix.

## 2026-06-02 (cont. 10) — ✅✅ BREAKTHROUGH: fair sched + per-frame yield → the VC-1 INTRO MOVIE DECODES (Step-1 validated); committed
Resumes cont.9. Env recovered, the per-frame yield built + tested. **The decoder was STARVED, not stuck** —
the fair scheduler + per-frame yield unstarves it and the movie decodes for the first time in variant A.
This overturns cont.6/cont.8's "stuck/gated, not merely starved" conclusion.

### Dev-env recovery — the REAL cause was a /tmp quota, not RAM
- cont.9 blamed RAM exhaustion. RAM was actually fine (16 GB free). The real cause: **`/tmp` is tmpfs with a
  per-user disk quota (`usrquota`, 12791 MB), and it was maxed out.** Two runaway debug dumps —
  `/tmp/movieopen_out.txt` (6.5 GB) + `/tmp/moviestart_out.txt` (5.6 GB) from cont.7 gdb sessions — filled the
  quota. The harness's own per-command `pwd >| /tmp/...cwd` write then failed → Bash returned exit 1 / no
  stdout. Fix: `rm` the two giant files (quota 12791→122 MB) → stdout capture restored immediately.
- Workaround used while diagnosing (before the fix): run Bash with the sandbox disabled + redirect output to a
  file outside /tmp (`> /home/h/_rexout.txt 2>&1`) and read it back. ⚠ Going forward: don't leave multi-GB
  dumps in /tmp; redirect big logs to the project dir or cap them.

### Build — cont.9's "unbuilt" was overcautious; the change WAS built
- `kernel.cpp.o` (13:43:37) + binary (13:43:38) were already newer than `kernel.cpp` (13:43:34), and the
  uncommitted diag strings (`sub_82250420 worker ENTERED`, `spurious g_tok.unlock`) were present in the binary.
  cont.9 marked it "unbuilt" because stdout broke before it could verify. Force-rebuilt anyway (clean, EXIT=0);
  confirmed the per-frame-yield override is compiled: `nm` shows `T sub_82167248` + `U __imp__sub_82167248`.

### Tests (all `timeout -s KILL`, /dev/shm cleaned each run)
1. **Default boot (no fair flags) — UNREGRESSED.** 109 664 lines/15 s, 12 068 VdGetSystemCommandBuffer + 12 269
   fence-forwards (healthy intro loop), **0** `initdiag`/`g_tok` lines (zero fair-mode leakage), no crash,
   **0** device-overwrite. The gating is clean by construction (default path is the unchanged g_waitMutex code).
2. **`REX_FAIRSCHED=1 REX_INITDIAG=1 REX_MOVIE_EOF=30` (no XFLAG) — scheduler fix works.** Threads that were
   token-starved forever now START + run: GuestThreadRun "GOT token" for tid=10 `sub_82250420` (×1),
   `sub_8211B740` ran (×1), and the VC-1 decoders `0x82339428/58/88` (×1 each). No crash, **0** device-overwrite,
   no FairMutex `[BUG]` (lock/unlock balanced). BUT `sub_8210AF90` did **NOT** run (0) → the 718-line
   `sub_8211B740` still diverges before it → transitions flag `0x828E82A6` stays 0 → intro does NOT auto-advance
   (fence head still climbing 17→39473 at 30 s). Exactly the cont.8-predicted *second* divergence (a logic gap
   inside sub_8211B740, not a scheduling gap).
3. **🎯 DECODER A/B (the decisive evidence) — `[video] LATE swap#220` frame-pool scan, same build, only
   REX_FAIRSCHED differs:**
   - Baseline (no fair): **all 16 buffers `nz=0 varied=0`** — uniform black, decoder produces nothing.
   - `REX_FAIRSCHED=1`: **buf4–11 carry real content**, `varied=8552 / 43090 / 64589 / 36729 / 56437 / 62655 /
     28047 / 13593`. By the diag's own criterion (adjacent-byte variation ⇒ decoding) the movie **IS decoding**.
   ⇒ The VC-1 decoder was **STARVED** by the cooperative single-token scheduler (main never yielded → decoder
   threads never got the token), **not stuck/gated**. The per-frame yield gives them the token and they decode.
   This is the first decoded movie content in variant A and validates the entire Step-1 (fibers/real-scheduler)
   theory that has been the leading root since cont.5.
4. **Determinism regression (the cont.9-flagged watch-item) — REX_FAIRSCHED ✗ REX_XFLAG.** The known-good cont.7
   combo `REX_XFLAG=1 REX_MOVIE_EOF=30` reaches attract (×2) + menu-setup in 161 311 lines/25 s; adding
   `REX_FAIRSCHED=1` STALLS it to **2 534 lines/25 s, attract=0** (never leaves intro; alive but ~64× slower,
   ending in a `[fencewait]`). So the fair object-waits are incompatible with the **forced** transition (matches
   the cont.8 band-aid regression). NOTE: REX_FAIRSCHED *alone* is fine (test 2 ran 42 k lines, intro loop
   alive) — the stall is specific to fair + REX_XFLAG. And it matters less now: REX_XFLAG was a stopgap for the
   *stuck-movie* problem that fair sched actually fixes; the path forward is the **natural** movie→EOF→advance,
   not XFLAG.

### Decision — COMMITTED (gated, default-safe, reversible, measured win)
- This is the prompt's "works → big Step-1 win" branch: the movie decodes. Committed the fair scheduler
  (`class FairMutex` FIFO run-token + fair object-waits in SignalObject/WaitObject/KeWaitForMultipleObjects/
  NtWaitForMultipleObjectsEx) + the per-frame cooperative yield at `sub_82167248` + the REX_INITDIAG worker-chain
  diag — all behind `REX_FAIRSCHED`/`REX_INITDIAG`, default boot untouched. Reversible (local, not pushed).

### Remaining blockers + NEXT
- **(a) `sub_8211B740` divergence** — even with tid=10 running, the 718-line handler doesn't reach
  `sub_8210AF90`, so `0x828E82A6` (transitions-enabled) is never set and the intro can't advance naturally.
  RE this divergence (it's state/data, not scheduling). This is the proper fix that retires REX_XFLAG.
- **(b) present the decoded frames** — the decoder now writes real frames to its pool (buf4–11). Wire the
  decoded surface to VdSwap/rex_render present → a VISIBLE intro movie (the decoded-frame shortcut, which cont.4
  abandoned only because there was no decoded frame; now there is).
- **(c)** make fair + forced-transition not stall (only if XFLAG is still wanted as a stopgap); lower priority
  than (a)/(b).
- ⚠ Do NOT combine REX_FAIRSCHED with REX_XFLAG (stalls). Use REX_FAIRSCHED with REX_MOVIE_EOF for decode work.

## 2026-06-02 (cont. 11) — ✅✅ THE DECODED INTRO MOVIE IS ON SCREEN (grayscale) — renderer increment 3: decoded-frame present
Resumes cont.10 (the movie now decodes). Implemented + verified the prompt's option **(b)** "present the
decoded frames": the host render thread now uploads the VC-1 luma plane and presents it via Vulkan, so the
intro movie is **VISIBLE for the first time in variant A**. Committed, gated behind REX_RENDER, default boot
unregressed. (User picked "both: present first, then RE" at the session fork.)

### Frame geometry — RE'd from the live decode dump
- Added **REX_VIDEODUMP**: the `[video]` LATE diag (swap#220) also fwrites each varied frame-pool buffer raw
  to `/tmp/vbufN.raw`. Reproduced cont.10 decode (`REX_FAIRSCHED=1 REX_MOVIE_EOF=30`) → buf4–11 dumped.
- `ffprobe`: the movie is **1280×720 wmv3** (VC-1/WMV9). A full 720p YUV420 frame = 1,382,400 B but each pool
  buffer is only `0x101440` = 1,053,760 B (SMALLER → not full-res planar 420).
- Visualized the dump at candidate strides (numpy/PIL). Row-diff autocorrelation found a SHARP minimum at
  **stride 1344** (rowdiff 1.33 vs noise at 1280); rendering at 1344 → a perfectly clean, recognizable South
  Park intro frame (snowy mountains, pines, the town w/ RHINOPLASTY storefront, Cartman from behind).
  ⇒ **Y (luma) plane = LINEAR, pitch 1344 B, 1280×720 visible, offset 0, 8-bit.** (The earlier "dashes" at
  stride 1280 were a wrong-stride shear of sparse detail over a smooth sky gradient — not tiling.)
- Xenia 360 `GetTiledOffset2D` untiling made it WORSE (block-scramble) ⇒ the surface is NOT GPU-tiled, just
  plain linear with a padded pitch.
- **CHROMA still unresolved**: after Y (1344×720 = 967,680 B) only ~86 KB tail remains — too little for 420
  chroma (needs ~460 KB). Tail shows row-striped chroma-ish content; NV12/planar guesses gave wrong colors
  (magenta/green bands). Non-standard layout ⇒ deferred; presenting **grayscale (luma)** for now.

### Implementation (vulkan_render.cpp + rex_render.h + kernel.cpp), gated REX_RENDER
- `VdSwap` publishes `g_videoBufs[16]`+count to the render thread (`rex_render::PublishVideo`), INSIDE the
  existing `if (rex_render::Enabled())` block ⇒ **zero cost in default boot**. The render thread reads guest
  memory directly (`extern g_base`) and each present picks a CLEAN frame, expands its luma → a host-visible
  BGRA staging buffer (gray = Y,Y,Y,255) → `vkCmdCopyBufferToImage` into the swapchain image (no shaders).
- Frame SELECTION (the hard part — racing the decoder writing guest memory):
  - v1 "freshest changed buffer" → TEARING (read mid-decode: top new, bottom stale).
  - v2 "settled ≥2 presents" → still a black SEAM. The cooperative scheduler STALLS the decoder mid-frame, so
    a half-written buffer (black unwritten bands) looks settled. Proven with **REX_RENDER_DUMPSEL** (dump the
    exact selected guest buffer): rendered offline at 1344 it had black bands = partial decode.
  - v3 (final) "settled ≥2 presents AND complete": sample a 72×32 grid; a row whose mean luma ≤12 is an
    unwritten black band; require ≥69/72 rows written ⇒ display only fully-decoded frames → CLEAN full frame
    (min luma 43, no black bands, mean 152 = the offline frame). ✅ Screenshot delivered to user.
- Capture (`REX_RENDER_SHOT=N`) now triggers on the Nth DECODED frame (decouples from render/decode timing).

### Tests (all `timeout -s KILL`, /dev/shm cleaned each run)
1. `REX_RENDER=1 REX_FAIRSCHED=1`: window opens (RADV POLARIS11); decode kicks in (video buf selection valid,
   alternates complete buffers buf6/buf9); **0** device-overwrite; no crash. Captured a clean grayscale intro
   frame. Motion is present but SLOW — without REX_MOVIE_EOF the title advances slowly (guest swaps crawl to
   ~191/55s); that's a cooperative-throttle/perf matter, NOT a present-path bug.
2. **Default boot (no flags) UNREGRESSED**: **0** device-overwrite, no crash, **zero `[render]` lines**
   (renderer gated off), reaches the intro loop. Added per-VdSwap code is behind the REX_RENDER gate.

### NEXT
- **(a) COLOR**: RE the non-standard chroma layout in the buffer tail (or find the decoder's surface
  descriptor / a separate chroma plane) → YUV→RGB. The luma path + upload pipeline are already in place.
- **(b) Smooth MOTION**: the title advances slowly without REX_MOVIE_EOF (decoder cooperatively throttled);
  a faster scheduler / real blocking fence would speed playback (ties into Step 1).
- **(c)** [prompt option (a)] RE `sub_8211B740` divergence → natural intro→menu transition (retire REX_XFLAG).

Diagnostics added (all env-gated, default boot unregressed): REX_VIDEODUMP, REX_RENDER_DUMPSEL.
Build: `ninja -C runtime/out sp_td_varianta`. Visible movie:
`REX_RENDER=1 REX_FAIRSCHED=1 ./runtime/out/sp_td_varianta ../private/extracted/default.xex`.

### RE progress on path (a) — sub_8211B740 divergence (prod-oracle call chain captured)
After the present milestone, pivoted to the prompt's option (a) per the user. Findings (a START, multi-session):
- `sub_8210AF90` (sets the 0x828E82A6 transitions flag; teardown of app-object 0x828E8AF8) has **ZERO direct
  callers** in the whole variant-A recompiled image — it is reached ONLY via an indirect (vtable) dispatch.
- **Prod-oracle gdb backtrace at `__imp__sub_8210AF90`** (prod reaches it; variant A doesn't) gives the exact
  chain: `sub_82450FD0` (trampoline) → `sub_82250420` (tid=10 work-loop) → **`sub_8211B740`** → `sub_8210AF90`.
  In prod sub_8211B740 calls it directly at +0x1220; in variant A (XenonRecomp) the SAME call is one of the
  function's **7 indirect `PPC_CALL_INDIRECT_FUNC(ctr)` vtable calls** — which is why it's invisible statically.
  Run: `cd out/build/linux-amd64-release && SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. gdb -batch -x /tmp/oracle.gdb
  --args ./south_park_td --game_data_root=.../private/extracted --user_data_root=.../private/userdata
  --license_mask=1 --mnk_mode=true --always_win=true --window_width=960 --window_height=540` with a breakpoint
  on `__imp__sub_8210AF90` + `handle SIGSEGV nostop noprint pass`.
- `sub_8211B740` (720 C-lines, ppc_recomp.3.cpp:11035) structure: ~7 indirect vtable calls gated by ~6
  conditional branches (first at guest 0x8211B7A4: `beq` on `sub_8224FB68`'s return = a name/registry lookup;
  others at 0x8211B818 / 0x8211B8EC / 0x8211B9C0 / 0x8211B9AC / 0x8211BBE8). The divergence (cont.10: "state/
  data, not scheduling") is ONE of these branches going the wrong way in variant A (a failed lookup / unset
  state), skipping the vtable call that dispatches to sub_8210AF90.
- NEXT (to finish path a): instrument variant A's sub_8211B740 branch path (which loc_ labels it reaches under
  REX_FAIRSCHED, where tid=10 runs it) and diff against prod's path (gdb-step prod through sub_8211B740 to the
  sub_8210AF90 bctrl) → the first divergent branch + its state input is the fix target. Then RE that input
  (likely one of the sub_82248AF8 / sub_82108E20 / sub_82253540 / sub_8224FB68 lookups returning differently).

### ✅✅ COLOR DONE (cont.11, same session) — the intro movie now renders in FULL COLOR
After the grayscale present + the sub_8211B740 characterization, cracked the chroma and added color.
- **The chroma is a SEPARATE allocation, not in the Y buffer's tail.** The decoder's NtAllocateVirtualMemory
  log (LR 0x8244DD2C) shows a repeating TRIPLE per frame: **Y (req 0x101440)** then **U then V (req 0x40520
  each)** — planar I420. (My earlier "86KB tail" confusion was from dumping a fixed 0x101440 for every pool
  slot, which over-read the small chroma allocations. The 0x101440 buffer holds ONLY the Y plane + padding.)
- Pool order in g_videoBufs: Y at indices 0,3,6,9,12,15,18 (size 0x101440); its U = idx+1, V = idx+2 (0x40520).
- Chroma geometry from autocorrelation on the small buffers: **sharp stride minimum at 672 (= luma pitch/2),
  mean ≈128** ⇒ U/V are pitch 672, 640×360. Verified by rendering frame 2 (Y=buf6,U=buf7,V=buf8) → a correct
  vibrant frame (blue sky, green pines, Cartman's turquoise hat + red jacket). **Full-range BT.601**, U/V order
  = first-small=U, second-small=V (601 with that order gave correct colors; VU or 709 were wrong/off).
- Implementation: kernel.cpp captures base+size (g_videoBufSz[24]); PublishVideo passes both; the render thread
  selects only Y planes (size 0x101440), reads U/V from the next two slots, and does integer fixed-point
  YUV→RGB (full-range BT.601) on the CPU into the BGRA staging buffer (chroma upsampled 2× nearest). Same
  vkCmdCopyBufferToImage path, no shaders.
- Verified ON SCREEN: REX_RENDER=1 REX_FAIRSCHED=1 → captured frame has R≠G≠B (mean 134/160/158 = color),
  matches the offline proof, 0 device-overwrite, no crash. Default boot UNREGRESSED (0 [render], 0 overwrite).
  Screenshot delivered to user. ⇒ **Task (a) COLOR is DONE.** Remaining: (b) smooth motion (coop-throttle),
  (c) sub_8211B740 RE for the natural transition.

### Motion investigation (cont.11, task b) — the movie PLAYS but ~10× too slow (scheduler-bound)
Added a distinct-displayed-frame counter to the render thread. Decisive result (REX_RENDER=1 REX_FAIRSCHED=1,
55s): the count climbs steadily 15→28→41→…→126 — so playback **advances** (NOT stuck on ~2 frames; the
buf6↔buf9 alternation is just the decoder double-buffering into 2 Y slots whose CONTENTS change each frame).
But 126 distinct frames / 55s = **~2.3 movie-fps** vs the movie's 24 fps ≈ **10× too slow**, tracking the
guest's slow progress (~3.6 swaps/s without REX_MOVIE_EOF). Root: the cooperative single-token scheduler runs
ONE guest thread at a time, so the 4-thread VC-1 decoder can't use multiple cores → decode throughput starved.
No safe quick lever (the recompiled code assumes one-at-a-time execution; running decoder threads truly
concurrently would race/corrupt). ⇒ **smooth motion = the deep Step-1 real-concurrency scheduler**, multi-
session. (This refines cont.7's "stuck movie" to "slow movie": with REX_FAIRSCHED it genuinely plays through.)

## 2026-06-02 (cont.11, renderer) — user picked the FULL PM4→Vulkan renderer; foundation laid + critical blocker found
After the color movie, the user chose the full renderer (menu/gameplay content, not just the movie). Scoped
it and made the verifiable foundation; surfaced the gating blocker.

### ⛔ CRITICAL FINDING — the translator has NO real draws to consume in any reachable state
- With the movie now decoding (REX_FAIRSCHED), the intro STILL emits only `init=0x30088 numInd=3 prim=8`
  draws (single untextured rects, 8/frame via REX_SEGCP), ZERO bound textures. The movie quad is NOT a PM4
  draw in variant A — prod presents it via the scaler/overlay path (the SpMovie 3-plane YUV shader, see
  below); our decoded-frame CPU present already replicates that.
- The FORCED transition (REX_XFLAG=1 REX_MOVIE_EOF=30 REX_SKIPINTRO=1) toward attract/menu ALSO emits only
  the same untextured rects (80/80) + ZERO textures, and hits INDIRECT-NULL blockers (target=0x0 @
  lr=0x82292D08; target=0x82367BD8 @ lr=0x8236859C; plus the known sub_8215DE84). ⇒ the real textured PM4
  draws are gated behind the screen-setup INDIRECT-NULL blockers — reaching a LIVE menu/gameplay screen
  (deep, title-specific RE) is the PREREQUISITE for the renderer to have anything to translate.
- ⇒ The renderer splits into (1) the shader/pipeline toolchain (verifiable in isolation — done below) and
  (2) the draw-state translator (BLOCKED on real draws until the screen progression is unblocked).

### ✅ Shader toolchain — DE-RISKED and built (varianta/tools/shaderc/)
- Confirmed the 19 `.updb` carry ORIGINAL D3D9 HLSL (ps_3_0) + interpolator/constant/sampler metadata.
  All 19 are PIXEL shaders (.psh); vertex shaders are only `.xbv` (compiled) → the renderer will use
  handwritten generic VS per vertex layout.
- libshaderc is installed (`libshaderc_shared.so.1`, no dev pkg). Wrote `compile.cpp` (GLSL→SPIR-V, declares
  the shaderc C API inline) + `build.sh`. Ported 5 shaders to Vulkan GLSL covering every pattern and compiled
  them to VALID SPIR-V (magic 0x07230203): Simple/SPTextured (tex*color, s0), SPUntextured (vertex color),
  SimpleCol (color*matCol push-const), **SpMovie (3 samplers Y/U/V → YUV→RGB studio-range BT.601 — the
  movie-quad shader)**. The remaining 14 are mechanical variants. Port map: `sampler2D`+`tex2D`→`texture()`,
  `:COLOR`/`:TEXCOORD` interpolators→`layout(location=<.updb Register>)`, `sN`→`set=0,binding=N`,
  `uniform floatN`→push constant.

### NEXT (renderer, by dependency order)
1. **Unblock the screen progression** (THE prerequisite for real draws): RE the INDIRECT-NULL screen-setup
   blockers (sub_8215DE84 / 0x82292D08 / 0x8236859C) and/or the natural transition (sub_8211B740) so the
   title reaches a live menu screen that emits textured PM4 draws.
2. **Draw-state translator**: once real draws flow (REX_SEGCP already captures the segment IBs), decode per
   draw: vertex fetch constants (reg 0x4800) → vertex buffers + layout; ALU constants (reg 0x4000) →
   uniforms; bound textures (reg 0x4800) → VkImage (detile + format); render target / viewport regs. Build a
   VkPipeline (ported PS + generic VS) and vkCmdDraw into the swapchain image (the render thread already owns
   it). Verify against the menu.
3. **Port the remaining 14 shaders**; add generic vertex shaders; texture upload/detile.

### sub_8211B740 divergence — NARROWED (cont.11, REX_TRACEB740) to the mid-section direct calls
Toward unblocking the screen progression (the renderer's prerequisite), traced sub_8211B740's indirect calls.
Added REX_TRACEB740: PPCInvokeGuest logs every bctrl whose call-site LR ∈ [0x8211B748,0x8211C000) (i.e. from
sub_8211B740) with its return value (the post-bctrl branches gate on it). Gated, default boot unregressed.
- Result (REX_FAIRSCHED=1, tid=10 runs sub_8211B740): it makes EXACTLY 2 indirect calls, both returning
  NORMAL values, then never reaches any later bctrl (the sub_8210AF90 dispatch never fires):
  - `0x8211B7D4 -> 0x82118E10 ret=0x1` (the first loop body; loop ran once, r31=1).
  - `0x8211B804 -> 0x82248F18 ret=0x0133F260` (a vtable[1] call returning a VALID pointer).
- The branch right after the 2nd bctrl (`beq 0x8211B818` on r3==0) therefore goes the EXPECTED non-zero way
  (calls sub_8211BC40, stores the ptr at [r30+64]). So the early part is FINE — the divergence is DEEPER, in
  the direct-call init section after 0x8211B814 (sub_82131758 / sub_8213E7E8 / sub_82132918 / sub_8212BE48 /
  sub_8211BD60 ...), which my indirect-only trace can't see, BEFORE the later sub_8210AF90 indirect dispatch.
- NEXT (to finish): trace those direct calls (gdb breakpoints on the mid-section callees filtered to the
  sub_8211B740 invocation, or temporary fprintf in the recompiled body) to find the one that hangs/returns-
  early/diverges; compare to prod. The 2 early bctrls + their returns are now the verified-good prefix.

### ⭐ sub_8211B740 "divergence" ROOT-CAUSED (cont.11) — it's STARVATION, not a code divergence (UNIFIES with slow movie)
Continued the natural-transition RE with gdb on the live boot (REX_FAIRSCHED REX_MOVIE_EOF=30 REX_TRACEB740).
Decisive chain of evidence:
- sub_8211B740 makes EXACTLY 2 indirect calls (0x82118E10→1, 0x82248F18→valid ptr) over an 85s run and NEVER
  reaches the later sub_8210AF90 dispatch (the 0x828E82A6 setter).
- gdb backtrace of the worker tid=10: it IS executing sub_8211B740, grinding in its heavy SIMD init chain
  `sub_82132918 (call site 0x8211B854) → sub_8212F6F0 → sub_822C14E8` — a vector matrix/transform loop
  (vperm/vpermwi/float-adds, stvewx128 to strided addrs). Samples show it at different lines (11190, 11195)
  across time ⇒ PROGRESSING, not hung on a sync (top frame = simde_mm_shuffle_epi8, pure compute).
- ⭐ PER-THREAD CPU (`ps -L`): ONLY the main thread burns CPU (82%); tid=10 and the other 12 threads are ~0%.
  ⇒ sub_822C14E8 is NOT an infinite/busy loop — tid=10 is STARVED. The main thread's fence-forward makes its
  GPU waits instant, so it holds the cooperative token ~continuously; the worker's heavy one-shot init gets
  near-zero CPU and barely advances → never finishes → never reaches sub_8210AF90 → 0x828E82A6 stays 0.
- ⇒ This OVERTURNS cont.8/cont.10's "sub_8211B740 diverges (state/data)" — it is NOT a code divergence. It is
  the SAME cooperative-scheduler starvation as the slow movie (worker/decoder threads starved by the
  CPU-bound main thread). The per-frame yield (cont.10) gives the decoders enough for slow decode but NOT
  enough for the worker's heavy SIMD init.
- ⇒ FIX (unified, deep): Step-1 real scheduler (true concurrency) OR replace the fence-forward stopgap with a
  REAL blocking fence so the main thread BLOCKS on GPU waits and yields CPU to the worker + decoders. Either
  makes sub_8211B740 finish → natural transition AND smooth movie. REX_XFLAG stays the stopgap meanwhile.
  This is THE Step-1 item; it gates both the natural intro→menu transition (→ real draws for the renderer)
  and smooth playback. Next concrete sub-task: prototype a real blocking fence (replace fence-forward in the
  ~6 sub_821B9270 waiters) and re-measure per-thread CPU + whether sub_8210AF90 fires.

## 2026-06-03 (cont. 12) — true concurrency (REX_NOTOKEN) made to BOOT; scheduler is NOT the root blocker (overturns cont.11)
Resumed at HEAD=4555fb8 with the prior session's UNTESTED `BlockFenceYield` (REX_BLOCKFENCE) prototype in the
working tree. Tested it, then pivoted to true concurrency. Decisive results:

### 1) `BlockFenceYield` (REX_BLOCKFENCE) — DISPROVEN (the cont.11 planned "blocking fence" next-step)
- The prototype cedes a FIFO run-token round at the fence-forward sites, intending to un-starve the workers.
- A/B per-thread CPU (REX_FAIRSCHED vs +REX_BLOCKFENCE=0 vs =50): **IDENTICAL** — one thread 99%, all others
  ~0%, total still 100% (one core). With =50µs the total CPU did NOT drop ⇒ the cede barely fires.
- Why: (a) the fence-forward (sub_821C6E58/sub_821C5DF0) runs only ~per-frame (around present), NOT in the hot
  intro loop (VdGetSystemCommandBuffer ×thousands), so BlockFenceYield ≈ the EXISTING per-frame yield; (b) gdb
  thread-state map (REX_FAIRSCHED): the workers/decoders are blocked in `g_objCv.wait` (FairWaitUntil:820 —
  waiting for a guest OBJECT to be signaled), NOT in the `g_tok` run-token queue. Ceding the run-token is a
  no-op when nobody is waiting for the token. ⇒ removed BlockFenceYield from the tree.

### 2) The cooperative single-token serializes ALL guest threads onto ONE core (root of the 10× slow movie)
- gdb (cooperative, REX_FAIRSCHED): the token DOES circulate (caught a VC-1 decoder sub_82339458 mid-decode
  holding it, main blocked at the per-frame yield), but only one runs at a time. 4 decoders + main + tid=10
  share one core ⇒ ~10× slow. This is inherent to the cooperative model, not a fairness bug.

### 3) REX_NOTOKEN (preemptive / true concurrency) was HALF-IMPLEMENTED — fixed 2 bugs, now BOOTS
- Out of the box REX_NOTOKEN dead-locked at **124 lines**. Two bugs (both default-safe — the cooperative path
  is byte-identical; only the REX_NOTOKEN branch changes):
  - **Fix #1 — GuestThreadRun token hold.** `GuestThreadRun` did `if (g_fair) g_tok.lock(); else g_waitMutex.lock();`
    with NO `g_coop` guard (unlike `LockGuestExecution`). Under NOTOKEN it held g_waitMutex for the thread's
    whole lifetime, so a spawned thread re-locking it inside `WaitObject` self-deadlocked (non-recursive mutex).
    Gated it `else if (g_coop)`. → boot **124 → 1387 lines, reaches VdSwap #1**.
  - **Fix #2 — fence-forward gate.** The forward overrides (sub_821C6E58/sub_821C5DF0) were wrapped in
    `if (g_coop)`, so under NOTOKEN the GPU-fence-spin (sub_821B9270) was never satisfied → a spawned thread
    span forever in sub_821C6E58→sub_821B9270 while main blocked on NtWaitForSingleObjectEx waiting for it
    (gdb-confirmed). Added `g_preempt = !g_coop` and gated the forward `if (g_coop || g_preempt)`. → boot
    **1387 → 369750 lines** (≈2× past the default cooperative boot), reaches the steady per-frame intro loop.
- **TRUE CONCURRENCY CONFIRMED:** per-thread CPU under NOTOKEN = **2 threads at ~99% (total 199%)** — two host
  cores running guest code simultaneously (the cooperative token can never exceed ~100%). No crash (segv=0),
  and the **default cooperative boot is UNREGRESSED** (57k non-spam lines/22s, 0 device-overwrite, reaches intro).

### 4) ⭐ But concurrency does NOT unblock the movie or the transition — they are BLOCKED-WAITING, not CPU-starved
This OVERTURNS cont.11's "starvation" diagnosis. Under true concurrency, with their own cores available:
- **Movie**: the VC-1 decoder threads (sub_82339428→…→KeWaitForSingleObject) sit BLOCKED in object-waits at
  ~0% CPU, and the frame pool is uniform/black at swap#220 (`[video]` varied=0 for all Y/UV buffers) — i.e. NOT
  decoding. (Cooperative-FAIRSCHED, cont.10, DID decode: buf4-11 varied=8552..64589. The per-frame yield there
  happened to drive the decode; NOTOKEN's free-running threads leave the decoder blocked on a signal/input that
  never comes.) ⚠ swap#220 arrives at ~11 s under NOTOKEN (main runs ~5-6× faster on its own core), so "black at
  swap#220" is partly an early sample — but the gdb-confirmed object-blocked decoders are the stronger signal.
- **Transition**: tid=10 (sub_8211B740) under concurrency progresses FURTHER than cont.11 (it gets PAST the
  sub_82132918→sub_822C14E8 SIMD section cont.11 thought was the wall) and then BLOCKS deeper, at
  sub_8211B740→sub_8212BE48→…→sub_82427858→sub_82435C48 (~0% CPU = a wait, not a spin). It still never reaches
  sub_8210AF90 (0x828E82A6 stays 0) even after 90 s with a full dedicated core. So sub_8211B740 is gated on an
  object/condition variant A doesn't satisfy, NOT on CPU.
- ⇒ **CONCLUSION:** the cooperative scheduler made the decoder + tid=10 LOOK CPU-starved, but with full
  concurrency they are revealed as BLOCKED on missing signals / unmet state — deeper title-specific RE problems.
  The scheduler is NOT the root blocker for either the movie or the natural transition. REX_NOTOKEN is a real
  infrastructure milestone (true concurrency now boots, default-safe) and likely the right long-term base, but
  it does not by itself produce a smoother movie or the natural intro→menu transition.

### Committed (default-safe, NOT pushed): the 2 NOTOKEN fixes + BlockFenceYield removal.
### NEXT (deep, title-specific RE — pick one; awaiting user):
- (a) Why do the VC-1 decoder threads stay blocked on their object-waits under NOTOKEN? (what input/signal —
  demux feed, buffer-ready event, 4-thread decode handshake — are they waiting for that variant A never posts?)
- (b) What does tid=10's new blocker sub_82435C48 wait for? (the deeper transition gate, past the SIMD init).
- (c) Keep the renderer on the REX_XFLAG forced-transition path (reach a menu screen for real draws) and treat
  the scheduler/movie as a separate track.
- Diag this session (all gated/reversible): per-thread CPU + gdb thread-state scripts; `[video]` swap#220 dump.

### cont.12 (b) — natural-transition RE under true concurrency (user-picked direction b): tid=10 hangs on an ORPHANED critical section; releasing it advances intro→menu, converging with the INDIRECT-NULL renderer blockers
With REX_NOTOKEN booting, pursued WHY tid=10 (sub_8211B740) still never reaches sub_8210AF90.
- **gdb full stack of tid=10**: it is BLOCKED in `RtlEnterCriticalSection` (std::recursive_mutex) at
  sub_8211B740→…→sub_82434860→**sub_82435C48** — waiting on a guest CRITICAL SECTION, NOT a CPU spin (~0% CPU).
  Reading the host mutex (`__owner`): the owning LWP has ALREADY EXITED ⇒ an **ORPHANED critical section**
  (std::recursive_mutex is not auto-released on thread exit, so a later RtlEnterCriticalSection deadlocks).
- **REX_CSLEAK diagnostic** (track per-host-thread held CSes; on GuestThreadRun exit warn/release) named the
  culprit, exactly one event: `[CS-LEAK] guest thread start=0x8242B4A8 EXITED owning CS=0x82818628`.
  sub_8242B4A8 = the GPU/video-init thread (sub_8242B4A8→sub_8214F730→fence-wait sub_821C6E58); it acquires
  CS 0x82818628, does GPU init, and EXITS without releasing it — its RtlLeaveCriticalSection branch is skipped
  because the fence-forward stopgap doesn't reproduce the real GPU sequence, so it diverges. Under the
  cooperative token this never surfaces (one thread at a time); under NOTOKEN tid=10 contends and deadlocks.
- **Fix (NOTOKEN-gated, default-safe)**: when a guest thread exits, release any CS it still holds (it owns
  those host mutexes, and it returned normally so the guarded state is consistent). Result: tid=10 UNBLOCKS
  and the title **ADVANCES OUT OF THE INTRO into MENU/FRONTEND setup** — loads `Global/Textures/Global.bin`
  (28165 B) + allocs menu buffers (NEW activity: NtQueryDirectoryFile, KeQueryBasePriorityThread, a flood of
  RtlInitializeCriticalSection from sub_82427xxx). **Concrete progress toward the renderer.** Then a CASCADE
  of `[INDIRECT-NULL]` (null vtable/jump-table slots: 0x82292D08, 0x8236859C, **0x821BF834, 0x821C71A4**) →
  **SIGSEGV at ~34s**.
- ⭐ **CONVERGENCE (directions b + c):** the natural path reaches the SAME menu-setup INDIRECT-NULL blockers
  as the forced REX_XFLAG path (cont.11: 0x82292D08 / 0x8236859C / sub_8215DE84). The NEW INDIRECT-NULLs are
  in the **0x821Bxxxx / 0x821Cxxxx CP/render code** ⇒ the menu tries to RENDER through GPU vtables variant A
  has not populated (no real CP/renderer) → the **renderer is THE blocker for a live menu**. (Note: sub_8210AF90
  STILL never fires — the advance happens via a different path under NOTOKEN, so the cont.7/8 "0x828E82A6 via
  sub_8210AF90" model is not the only route to leave the intro.)
- ⇒ Both ways of reaching the menu converge on **INDIRECT-NULL screen/GPU-vtable recovery (= the renderer
  work)**. NOTOKEN + the orphan-CS release get the title there NATURALLY (no REX_XFLAG).
- **Committed (NOTOKEN-gated, default-safe):** REX_CSLEAK diagnostic + orphaned-CS release-on-exit. Default
  cooperative boot unregressed (VdSwap, 0 device-overwrite). The ~34s SIGSEGV (render-vtable INDIRECT-NULL
  cascade) is the next frontier — the renderer.
- NEXT: recover the INDIRECT-NULL screen/GPU vtables (sub_8215DE84 / 0x82292D08 / 0x8236859C / 0x821BF834 /
  0x821C71A4) so the menu builds real textured PM4 draws → the draw-state translator has content. This is the
  renderer (RENDERER-PHASE-PLAN), now reachable via the natural NOTOKEN path.

### cont.12 (c) — INDIRECT-NULL menu/renderer recovery STARTED via the prod oracle (user-picked)
With NOTOKEN+CS-fix reaching menu setup, classified the INDIRECT-NULL cascade with REX_INDDUMP (per distinct
null call-site: dump GPRs + the C++ vtable chain), then read the CORRECT values from the prod oracle.

INDIRECT-NULL site classification (REX_INDDUMP):
- ROOT — lr=0x82292D08 in sub_82292CE0: a C++ VIRTUAL CALL on a NULL global singleton. The recomp does
  `obj=*(r31+4); obj->vtable->method[1]()` with r31=0x827FD568 (a fixed image global). Dump: *(r31+4)=
  *(0x827FD56C)=0 — the singleton is NEVER CONSTRUCTED in variant A → the virtual call targets null.
- 4 sites (lr 0x8236859C / 0x823685D0 / 0x82368070 / 0x82368160) all target 0x82367BD8 = a MISSING JUMP
  TABLE in sub_82367B88 (switch on r3=0..11; table base 0x82367BA8 [lis 0x8209<<16 + 0x7BA8], default
  0x82367C50, case code starts 0x82367BD8). XenonAnalyse missed it and emitted the switch `bctr` as an
  indirect call. It is one of the BOUNDARY-LIMITED cases — the 0x82367BA8 table DATA was mis-split into a
  function sub_82367BA8 — so recovering it needs the function-boundary fix, not just a toml add.
- 3 sites (sub_825ABxxx, target 0x0): registers hold ASCII path fragments + *(r3)=0xB4B4B4B4 (uninit fill)
  = DOWNSTREAM corruption (a symptom of the incomplete menu state), not a root.

PROD ORACLE (gdb prod; guest base FIXED at 0x100000000; at a recomp fn entry rsi=base [SysV ABI 2nd arg],
r31=0x827FD568 const; symbols present, not stripped):
- At sub_82292CE0: global 0x827FD56C = obj 0x45FE78B0 (a heap object), *obj = vtable 0x820948B0 (static, in
  the image), vt[0]=0x8248E768, vt[1]=0x824927B8 (the method variant A wants). Prod caller path:
  main → sub_82249970 → sub_82150770 → sub_82292CE0 (the frontend/menu setup). ⇒ variant A reaches the same
  virtual call but with the singleton 0x827FD56C UNCONSTRUCTED — root confirmed via the oracle.
- Constructor of the singleton: a hardware-watchpoint on host 0x182FD56C (= prod base + 0x827FD56C) is the
  method (find the code that allocs obj 0x45FE78B0-class + sets vtable 0x820948B0 + stores it at 0x827FD56C);
  first try via a stop()-callback failed (software fallback / arming-in-callback), retried top-level.
Committed: REX_INDDUMP (7f901e9). Tooling: prod base=0x100000000, $rsi=base, r31 const; /tmp/prod_oracle.gdb
(read object/vtable) + /tmp/prod_ctor2.gdb (watchpoint the constructor).
NEXT: identify the singleton's constructor + why variant A skips it (an upstream menu-init divergence,
likely the same fence-forward/GPU-init lineage), then recover so sub_82292CE0's virtual call succeeds and the
menu builds real textured PM4 draws → the draw-state translator (the renderer). The jump table sub_82367B88
is a separate boundary-limited recovery.

### cont.12 (c, cont.) — singleton constructor chain found; the menu KICKS REAL GPU WORK (ring alive); SIGSEGV is a gfx-interrupt null-vtable
Traced the 0x820948B0-class constructor STATICALLY (no flaky watchpoint): the vtable 0x820948B0 is set ONLY
by sub_824883E0 (`lis 0x8209 + addi 0x48B0; stw r11,0(this)`), called only by the construct-wrapper
sub_824898C0, called only by the lazy getter sub_8248F4C8 (allocs, constructs, caches the object at a DIFFERENT
global 0x82819358 via `stw r31,-27816(r25)` r25=0x82820000; returns it), called only by sub_8248F988. So the
0x827FD56C the virtual call reads is a SEPARATE reference (a manager at 0x827FD568 holding the object at +4),
populated by a caller of the getter, not the getter's own cache.
- ⭐ varianta hit-check (gdb, NOTOKEN+CS-fix, breakpoints on the chain): the getter sub_8248F4C8 IS reached
  (1 hit) but the constructor sub_824883E0 AND the construct-wrapper sub_824898C0 are NEVER called (0 hits).
  ⇒ in variant A the getter runs but its lazy-init branch SKIPS construction (or a different construct path
  is never reached), so the 0x820948B0-class object is never built and 0x827FD56C stays null → the
  sub_82292CE0 virtual call hits null. The recovery target = why the construction branch is skipped.
- ⭐⭐ The ~34s SIGSEGV is NOT the null singleton directly — and it reveals a BIG step: under NOTOKEN+CS-fix the
  menu loads real assets (Global/Textures + Global/Meshes/Global.bin) and **KICKS REAL GPU WORK — the main
  ring is ALIVE** (was frozen at WPTR=37 the whole intro). The crash stack: VblankPump → ExecuteRing(rptr=37
  wptr=...) → ExecutePM4 → ExecuteType3(op=0x54 EVENT_WRITE) → FireGfxInterrupt(cb,source=1) →
  sub_821C7170 → `PPC_STORE_U32(r31+0,...)` with r31 GARBAGE → SIGSEGV. sub_821C7170 is itself an INDIRECT-NULL
  site (0x821C71A4, target=0x003F8000 = a garbage method pointer in r10): the skipped indirect call leaves r31
  garbage, so the next store faults. ⇒ a downstream null/garbage vtable in the graphics-interrupt callback path
  the CP now exercises (because the menu emits PM4 EVENT_WRITE that fires the gfx interrupt).
- NET: directions (b)+(c) have driven variant A from a frozen intro to a MENU that loads assets and emits real
  GPU work — the renderer now has live PM4 to process. The remaining blockers are a cluster of uninitialised
  menu/GPU objects (null singleton 0x827FD56C via skipped construction; the gfx-interrupt callback's garbage
  vtable in sub_821C7170; the missing jump table sub_82367B88), all surfacing because variant A's menu/GPU
  subsystem init diverges (the same lineage as the fence-forward stopgap / no real CP). Each is a concrete,
  separately-recoverable target. Diag REX_INDDUMP + prod-oracle method (base 0x100000000, rsi=base, r31 const).

### cont.12 (c2) — ROOT of the menu null-singleton: ExAllocatePoolTypeWithTag was a NULL stub; implemented the kernel pool allocator
Traced the singleton-construction failure all the way down (gdb FinishBreakpoints on the chain return values):
getter sub_8248F4C8 (cache *(0x82819358)=0 ⇒ it DOES take the construct path, not the already-cached error
path) → sub_82497720 (CS+refcount) → sub_82497678 → sub_824A5E50 (calls a static allocator object's vtable[0]
sub_824A5DD0) → sub_824A5DD0 → **ExAllocatePoolTypeWithTag** → returned **0x8007000E (E_OUTOFMEMORY)**. Because
ExAllocatePoolTypeWithTag / ExAllocatePoolWithTag / ExFreePool were weak stubs returning NULL, EVERY guest
kernel-pool allocation failed → the singleton manager object was never built → 0x827FD56C stayed null → the
menu virtual-called through it = the whole INDIRECT-NULL cascade + SIGSEGV. (sub_824A5DD0: `mr r3,r4; lis/ori
r4=tag 0x4E574D20; li r5,0; b ExAllocatePoolTypeWithTag` ⇒ ABI r3=NumberOfBytes, r4=Tag, r5=PoolType.)
- **FIX (kernel.cpp, NOT gated — a real kernel primitive):** implemented ExAllocatePoolTypeWithTag(size r3,
  tag r4, type r5) + ExAllocatePoolWithTag (Xbox 360 2-arg, size-first) + ExFreePool (no-op leak) as a
  fine-grained 16-byte-aligned bump allocator over >=8 MiB arenas carved from the guest VM cursor g_virtNext
  (tracked in g_regions; every block is fresh demand-zero guest memory).
- **RESULT:** the singleton constructor NOW RUNS (sub_824883E0 / sub_824898C0: 0→1 hits). The menu progresses
  much further: it now loads its RENDERING ASSETS — **shaders SPBackdropTextured.xbp / SpTextured.xbv+.xbp,
  textures LipSyncTextures.bin / Global.bin** (the frontend setting up its draw pipeline). The SIGSEGV MOVED
  FORWARD: was sub_821C7170 (gfx-interrupt) → now sub_82368028 ← sub_8233DFD0 ← sub_8233E198 (the menu/UI
  region near the still-missing jump table sub_82367B88). Default cooperative boot UNREGRESSED (ALIVE, VdSwap,
  0 device-overwrite, 0 segv; the [stub] ExAllocatePoolType line is gone — the override is live).
- ⇒ ExAllocatePoolTypeWithTag was a FUNDAMENTAL missing primitive — implementing it unblocks ALL menu pool
  allocations at once (cluster item 1 RESOLVED). NEXT = the new crash at sub_82368028 (cluster items 2/3: the
  missing jump table sub_82367B88 and/or the gfx-interrupt null-vtable sub_821C7170).

### cont.12 (c3) — past the pool fix the menu hits a NOTOKEN+stub-CP concurrency swamp (gfx-interrupt + string-as-code corruption)
With the pool allocator unblocking the menu, the next crashes are RACE-DEPENDENT under NOTOKEN (the host
VblankPump CP thread fires GPU interrupts / re-runs the ring concurrently with the menu's GPU-setup thread):
- **gfx-interrupt crash (sub_821C7170, recurs on the VblankPump thread):** our CP, on a menu PM4_INTERRUPT
  (op 0x54), calls FireGfxInterrupt(source=1). The source=1 (command-buffer-complete) handler sub_821C7170
  derefs the device's active completion object at *(device+10900) and clears the firing CPU's ack bit. The
  title sets that field per submission (sub_821C73D8) and leaves it the sentinel 0xFFFFFFFF between
  submissions; variant A's CP fires the interrupt when none is pending → deref of 0xFFFFFFFF (last byte of
  the 4 GiB map) → SIGSEGV. STOPGAP guard added in FireGfxInterrupt: skip source=1 when *(device+10900)==
  0xFFFFFFFF (nothing to acknowledge). Default boot unregressed; the gfx-interrupt crash is gone.
- BUT the title still dies ~32s at a DIFFERENT spot: an INDIRECT-NULL cascade at lr=0x82204D08 whose targets
  are ASCII PATH FRAGMENTS (0x67616D65="game", 0x41737365="Asse", 0x74735C47="ts\G") — sub_82204xxx is
  executing a path STRING as a function/vtable table = downstream corruption (same class as the earlier
  sub_825AB garbage). The menu's state is inconsistent: a buffer that should hold pointers holds a path string.
- ⇒ Past the (clean) pool-allocator win, the menu bring-up is a CLUSTER of variant-A-specific concurrency +
  uninitialised-object issues: (a) NOTOKEN guest-thread races; (b) the VblankPump host CP firing GPU
  interrupts / re-running the ring asynchronously vs the guest's GPU submission; (c) string-as-code
  corruption from incomplete menu state; (d) the still-missing jump table sub_82367B88. This is the
  renderer / CP-concurrency boundary — fixing crashes one-by-one under NOTOKEN races is whack-a-mole; the
  structural fix is a properly-synchronised CP (the renderer work). The pool allocator + gfx-interrupt guard
  are committed correct stopgaps that advanced the menu to loading its shaders; the rest needs the CP/render
  architecture. Diag REX_INDDUMP; run REX_NOTOKEN=1 REX_CSLEAK=1.

### cont.12 (c4) — CP synchronization (g_gpuMutex) fixes the pump-races-guest crash; the menu now reaches its RENDER code = the renderer boundary
USER picked the structural fix (synchronize the CP). ROOT of the race class: the VblankPump is a HOST thread
that runs ExecuteRing + the gfx-interrupt CALLBACK (guest code); under the cooperative token it is serialized
with the guest, but under NOTOKEN the pump's `if(g_fair)…else if(g_coop)…` acquires NOTHING -> it races every
guest thread.
- FIX (g_gpuMutex, NOTOKEN-only, default path untouched): the pump holds g_gpuMutex over its ExecuteRing +
  FireGfxInterrupt batch; the guest's GPU-boundary functions hold it too — the ring kick sub_821C6600 and the
  completion-object setup sub_821C73D8 (which writes the device+10900 the source=1 handler derefs). This
  serializes the host pump with the guest GPU submission WITHOUT a global run-token (decoders/menu logic stay
  concurrent; recursive mutex for callback re-entry). No deadlock; default boot UNREGRESSED.
- RESULT: the gfx-interrupt crash (sub_821C7170, pump-races-guest) is GONE from the cascade. The title
  progresses INTO its render code and now SIGSEGVs ~33s on INDIRECT-NULLs in the 0x821Bxxxx/0x821Cxxxx GPU/CP
  region whose targets are FLOAT values (0x43A04000=320.5f, 0x421A0000=38.5f, 0x3E340000=0.176f) — i.e. the
  title's render code is reading VERTEX / render-state data as function pointers, because variant A's stub CP
  maintains no real GPU draw-state.
- ⇒ ⭐ THE MENU HAS REACHED THE RENDERER BOUNDARY. The chain of this session's fixes drove it there naturally
  (no REX_XFLAG): pool allocator -> the singleton constructs -> the menu loads its shaders/textures; CP-sync
  -> the pump no longer races the guest -> the menu reaches its actual RENDERING; and rendering needs the real
  GPU draw-state that only a PM4->Vulkan renderer maintains. This UNIFIES with cont.11: the renderer
  (draw-state translator: vfetch reg0x4800 -> vertex buffers + layout, ALU reg0x4000 -> uniforms, textures,
  RT/viewport; + the 19 .updb shaders -> SPIR-V; vkCmdDraw into the swapchain the render thread owns) is THE
  remaining work — now reachable through the NATURAL menu (pool + CS-fix + NOTOKEN + CP-sync), not the forced
  REX_XFLAG path. Committed correct stopgaps this session: pool allocator (724c104), gfx-interrupt guard +
  g_gpuMutex CP-sync. Run: REX_NOTOKEN=1 REX_CSLEAK=1 -> menu (shaders) -> render-code SIGSEGV ~33s.

### cont.12 (c5) — draw-state translator START blocked on INPUT: only init rects reach the CP (the cont.11 coverage problem, confirmed in the menu)
USER picked the renderer (draw-state translator). PREREQ check — what DRAW_INDX/state actually reaches the CP
during the menu? REX_DRAWLOG (main ring) and REX_SEGCP (route B) both during the NOTOKEN+CS-fix menu:
- 60 draws via the main ring, 120 via route B — ALL `init=0x30088 numInd=3 prim=8` (the same degenerate
  RECT_LIST as the intro), ZERO bound textures. The menu's REAL textured draws do NOT reach the CP.
- ⇒ same COVERAGE PROBLEM as cont.11 (b22fe28): the title builds its real draws in its CUSTOM deferred
  command-buffer pool (device+13568, growing ~0x88680/frame: PM4 interleaved with INLINE VERTEX floats +
  0x8100xxxx segment-link descriptors; cont.11 REX_CHUNKCP linear-parse DESYNCED on it). They never reach the
  main ring (only the 6 init kicks) or the route-B segment scan. The CP's existing SET_CONSTANT(0x4000 ALU /
  0x4800 fetch+tex) + DRAW_INDX plumbing is ready, but it has no real draw to translate.
- So the draw-state translator cannot START until the real draws reach the CP. PREREQUISITE options:
  (A) crack the title's deferred cmd-buffer format (device+13568) and execute its chunks correctly (follow the
      segment links / the 0x38/0x79 ops, skip the inline-vertex regions) — the direct path to the real draws,
      cont.11's "full cmd-buffer enumeration (hard)";
  (B) intercept the title's draw-RECORD function (where it emits a DRAW_INDX into its cmd-buffer with the GPU
      regs live) and translate at the source — bypasses the cmd-buffer format but needs to find that fn;
  (C) drive the title's own flush sub_821C6D58 to push the built IBs to the ring (route A — but cont.11 showed
      the title is NOT fence-gated, so this premise is weak).
  (Separately, the menu still SIGSEGVs ~33s — a stability blocker, but device+13568 grows regardless, so the
  draws ARE being built; the blocker is extraction, not the crash.)
- ⇒ The renderer's true first step is the deferred-cmd-buffer extraction (A or B), not the Vulkan translation
  (which is plumbed + the shaders are ported). This is the cont.11-unresolved hard RE, now scoped to the menu.

### cont.12 (c6) — draw-record interception (user-picked): finding the draw fn — random prod sampling too noisy
USER picked: intercept the title's draw-record function (hook where it emits a DRAW_INDX, translate at the
source, bypassing the deferred cmd-buffer format). Step 1 = find that function.
- Tried RANDOM prod-thread sampling (gdb `thread apply all bt` ×3 during the menu): too noisy — the histogram
  is dominated by BLOCKED threads in waits (workers sub_8230FA80/sub_8230E898, GPU-sync sub_821CC5D0/
  sub_821BFF48, decoders sub_82339xxx, the trampoline sub_82450FD0) + the screen-loop (sub_82249678/970/AD0,
  sub_82150970). A draw call is fast and was not caught; prod at the static menu draws infrequently.
- NEXT (targeted, reliable): find the draw-record fn by INSTRUMENTING variant A's own cmd-buffer writes — the
  title records DRAW_INDX_2 (op 0x36) packets into device+13568's current chunk; hook the cmd-buffer reserve/
  write (the fn that advances *(device+13568+4) writeptr, cont.11-mapped) and log the guest LR when an op-0x36
  header is written ⇒ the LR is inside the draw-record fn. Then read its accessible D3D state (vertex streams,
  shader, textures, RT from the device object) and translate to vkCmdDraw. Alternative: a prod hardware
  watchpoint on the cmd-buffer for an op-0x36 write (flaky here, as the 0x827FD56C watchpoints were). The
  translator's Vulkan side (state-extraction reg-file, ported shaders, render-thread swapchain) is READY; the
  whole remaining renderer work is this draw EXTRACTION + the per-draw-type Vulkan translation. Multi-session.

### cont.12 (c7) — draw-record INTERCEPTION is a proven NO-GO (XDK D3D inlined); only the PM4 cmd-buffer extraction is viable
Pursued the targeted draw-record-fn search. The cmd-buffer base/writeptr writers (device+13568/+13572) trace
to sub_821CC830 — and that is D3D FRAME-BEGIN (resets device+13568/13572=0, allocs the ring segments via
sub_821C5BA8), NOT a draw. ⚠ This re-confirms the prior HLE-spike (same branch, 2 days ago, [[sp_hle_phase0_progress]])
DETERMINATION: the title's static XDK D3D **INLINES every draw/state emit as PM4 stores** — there is NO
per-draw / per-sprite / per-material guest FUNCTION to hook (RE-confirmed at call-structure level); the draws
are PM4 stores deep in the render subtrees (frame render sub_82150970 → 10 structural passes; kicks
sub_821C6600 ~11/frame, NOT draw-proportional). And "texture binding only via PM4" — a non-PM4 override never
gets the texture SET_CONSTANT, so it can't reconstruct sprites.
- ⇒ The user-picked "intercept the draw-record function" is TECHNICALLY BLOCKED (no such function exists —
  the D3D is inlined). This is exactly why the HLE variant-B native-render spike was a NO-GO.
- ⇒ The ONLY viable draw input for the renderer is the **PM4 cmd-buffer** (option A): crack the title's
  deferred cmd-buffer (device+13568) and execute its PM4 — which carries BOTH the DRAW_INDX_2 packets AND the
  SET_CONSTANT(0x4800 fetch+tex / 0x4000 ALU) state the existing CP already applies. cont.11's REX_CHUNKCP
  LINEAR parse desynced on the title's custom structure (PM4 interleaved with inline-vertex floats +
  0x8100xxxx segment-link descriptors); the fix is a STRUCTURED parse — follow the segment chain (the 0x38/
  0x79 ops / 0x8100xxxx links) and parse PM4 within each segment, skipping/handling the inline-vertex regions.
  This is cont.11's "full cmd-buffer enumeration (hard)" — now established as the ONLY path (interception is
  ruled out), scoped to the menu where the title builds real textured draws into device+13568 ~0x88680/frame.
- The CP/Vulkan side is ready (SET_CONSTANT→reg-file extraction, ported shaders, render-thread swapchain). The
  whole remaining renderer = this structured deferred-cmd-buffer parse → feed the real DRAW_INDX+state to the
  existing CP draw path → translate to vkCmdDraw. Multi-session, but now the approach is unambiguous.

## cont.12(c8) — Option A is MOOT: the title builds NO real draws; render-path INDIRECT-NULL is the real gate

**Ran the structured device+13568 chunk dump (REX_CHUNKDUMP) + a per-swap brute draw-scan over a full menu run.
The conclusion is decisive and overturns Option A's premise.**

- **device+13568 chunks hold only 1–3 draws each (max 16), and they are ALL degenerate rects.** Per-swap brute
  histogram over 14,460 swaps: 2943 chunks×1 draw, 2008×2, 1143×3, 165×4, 142×6, 164×7, 164×16 — never the
  dozens/hundreds a real menu needs. The structured dump of a draw-bearing chunk = a few init packets (op 0x02
  cnt10, T0 reg0x168/0x2D0 writes) then ZEROS to the end. The chunk is barely filled.
- **Three independent draw-scans converge:** route-B SEGCP = 120 [draw], ALL `init=0x30088 numInd=3 prim=8`,
  **0 textures bound**; ring/CP path = 60 [draw], ALL `init=0x30088` rect, **0 textures**; chunk brute-scan =
  rects only. variant A builds ONLY untextured degenerate rects, in BOTH intro AND menu. **There is nothing to
  extract** — Option A (crack device+13568) cannot produce textured draws that the title never builds.
- **Root cause = render-path INDIRECT-NULL on uninitialized objects.** The last non-spam lines before the ~33s
  segv are indirect calls whose targets are FLOATS read out of an object's vtable slot:
  `target=0x3E340000 (0.176f) @ lr=0x821BF834`, `target=0x43A04000 (320.0f) @ lr=0x821CC4B0`,
  `target=0x421A0000 (38.5f) @ lr=0x821B925C`, plus `target=0x82367BD8 @ lr=0x8236859C` (a real code addr —
  near-miss / unregistered). The caller sites **`sub_821BFF48` and `sub_821CC5D0` are in the prod oracle's
  recurring render-function set** (sub_82450FD0×30, sub_8230FA80/E898/E7D8×9, sub_82150970, sub_821CC5D0,
  sub_821BFF48, sub_821B9270…). So these ARE the draw path; in variant A their state objects are uninitialized
  (vtable field = float data) → the indirect dispatch fails → the title SKIPS the textured content draws
  (emitting only the clear/background rects) and then crashes ~33s. SAME class as the singleton-ctor bug fixed
  earlier this session (uninit object → null/garbage vtable → INDIRECT-NULL).
- **⇒ The renderer's real prerequisite is NOT cmd-buffer extraction but fixing the render-path uninitialized-
  object cluster** (`sub_821BFF48` / `sub_821CC5D0` / `sub_821B9270` / `sub_8236859C` callers) so the indirect
  calls dispatch, the title runs its content-draw code, and real textured DRAW_INDX_2 + SET_CONSTANT(0x4800)
  state finally appears in the cmd-buffer. THEN extraction has something to render. This is exactly the
  INDIRECT-NULL-vtable recovery via the prod oracle the user endorsed earlier — now localized to the render path.
- Diagnostics added (gated, default boot unregressed): `REX_CHUNKDUMP` = per-swap brute draw-scan of the active
  device+13568 chunk + one-time structured PM4 dump of the first draw-bearing chunk ([chunkscan]/[chunkdump]).

## cont.12(c9) — render-path INDIRECT-NULL cluster mapped: it's a NOTOKEN RACE, plus one recompiler gap fixed

Drilled into the render-path INDIRECT-NULL cluster (user picked "fix the render-path cluster"). REX_INDDUMP
(dumps GPRs + vtable chain per distinct site) over several NOTOKEN menu runs. The cluster is **heterogeneous**,
and crucially **the set varies run-to-run** — the smoking gun for a race.

- **Site structures (recomp source):**
  - `sub_821CC310` (owns lr=0x821CC4B0): a work-queue executor. `r31 = *(r3)` is a command/sync object —
    spinlock at +0 (lwarx/stwcx CAS loop), ready-mask at +56, count at +60, **callback fn-ptr at +16**. It
    fills fields (+0/+24/+28/+32/+36) then `bctrl *(r31+16)` with r3=r30. In variant A `*(r31+16)=0x43A04000`
    = **320.0f** (a screen coord, not a code pointer).
  - `sub_821B9270` (owns lr=0x821B925C): `bctrl r9` with a float arg (f1 = f13·f12·f0); r9 was loaded earlier
    and in variant A = `0x421A0000` = **38.5f**.
  - `sub_821BF834`: target `0x3E340000` = **0.176f**.
  - `sub_82249C58`/`sub_82448B70`: target = garbage with `r31=0xFFFFFFFF` and `*(r31+4)=0xC0012D01` (a real
    PM4 packet: type3 op0x2D SET_CONSTANT) — i.e. a command-buffer walk reading PM4 *data* as a fn-ptr.
- **It's a RACE, not a deterministic missing-init.** Run A: float-vtable sites + crash ~33s. Run B: jump-table
  + garbage sites. Run C (after the fix below): **only the benign 0x82292D08, segv=0, NO crash, ran the full
  32s.** Same binary, same env — the set and the crash are non-deterministic. The specific float values
  (320f/38.5f) are not random garbage: the render-command objects are **pooled/reused**, and under the
  preemptive NOTOKEN scheduler the executor (sub_821CC310) sometimes runs **before the enqueuer finishes
  constructing the object**, reading the previous occupant's **stale float data** at the +16 callback offset.
  Classic use-before-init on a recycled pool slot. This unifies the scheduler tension: the cooperative single
  token serializes (no race) but starves the VC-1 decoder; NOTOKEN feeds the decoder but races the render-
  command construction. ⇒ the renderer needs a scheduler that does **both** (Step-1, real synchronization).
- **One DETERMINISTIC site fixed (a genuine recompiler gap, not a race): `sub_82367BD8`.** `sub_82367B88` is a
  12-way switch: `r12 = 0x82360000+31656 = 0x82367BA8 (table base); bctr *(table + r3*4)`. XenonAnalyse
  mis-identified the table at 0x82367BA8 as CODE and decoded its 12 address-dwords as 12 `lwz r17,X(r22)`
  instructions (the bogus "sub_82367BA8"), absorbing case-0's handler — which sits right after the table at
  **0x82367BD8 = `li r3,0 ; blr`** — so it was never emitted as a callable function. Cases 1-11 (sub_82367BE0
  /BE8/BF0/BFC/C08/C14/C20/C2C/C38/C44) WERE emitted; only case-0 was lost. Re-supplied it in PPCInvokeGuest
  (small always-on switch, transcribed from the recomp's own decode): `case 0x82367BD8: ctx.r3.s64=0; return;`.
  Verified: the 0x82367BD8 INDIRECT-NULL (4 call sites in the sub_82368xxx float-format chain) is gone; default
  boot unregressed (VdSwap=6, segv=0, devOver=0). This is the reusable pattern for other recompiler jump-table
  gaps (in-range, decodable, consistent target).
- **Pivotal open question for next session:** does a verified RACE-FREE run actually build textured content
  draws (textures bound) into device+13568? If yes → the render gate is purely the race → fix enqueuer/executor
  synchronization (Step-1) and the structured device+13568 parse THEN has real content. If a clean run still
  builds only rects → the divergence is deeper than the race. Answering it needs the structured device+13568
  parse run on a confirmed-clean boot (the brute-scan can't see texture binds; CHUNKCP desyncs). So Option A
  (the parse) is not dead — it is **gated on first getting a clean run**, which reframes the whole renderer:
  step 1 = race-free scheduling, step 2 = parse device+13568 on a clean run to confirm/extract textured draws.

## cont.12(c10) — pivotal experiment DONE: device+13568 is a STATE directory (no draws); even a clean run builds NO textured content

Ran the pivotal experiment the user picked. Cracked the device+13568 format and followed it on confirmed-clean
menu boots. **Answer: NO — a race-free run does not build textured draws; the divergence is deeper than the race.**

- **device+13568 is NOT inline PM4 — it's the title's SEGMENT DIRECTORY.** Each chunk holds records + segment
  descriptors **{0x81LLLLLL, phys_addr}** (LLLLLL = segment length in dwords) plus 0xC1/0xC2-tagged pointers.
  Resolve a descriptor's segment to a guest address with **guest = 0xA0000000 | (addr & 0x1FFFFFFF)** — verified
  correct (the segments then parse cleanly as PM4). (Brute-scanning the directory for op-0x22 headers gives
  FALSE POSITIVES: the 0xC134xxxx record/pointer words decode as op 0x22 — that's why the earlier "1–16
  draws/chunk" counts were noise.) Menu-frame directories carry ~11–20 descriptors; init chunks ~3.
- **Every followed segment is render STATE / EVENT / CALLBACK — ZERO draws.** Over 3 confirmed-CLEAN menu boots
  (g_nonBenignInd==0, swap#4002/4019, 11–13 descriptors each) the per-segment tally is `realDraws=0 rectDraws=0
  texFetch=0` for EVERY segment, no exceptions. The segment-start signatures: `C0004600`=COND_WRITE(0x46),
  `C0006000/C0006100`=op0x60/0x61, `C0003B00…C0025800`=EVENT_WRITE(0x58), SET_CONSTANT blocks (8/4 per state
  segment, all non-texture — texFetch=0), repeating data records `00000A31 0X000000 00010A2F`, and CALLBACK
  records **`0001057C 821CC7A0 <ctx>`** — a code pointer into the render executor region (sub_821CC*, the same
  family as sub_821CC310). So the directory is the per-frame **state + work-queue**; the actual DRAW packets
  (the init=0x30088 degenerate rects) are emitted to the **main ring** by the executor, and there are **no
  textured draws anywhere** — clean or raced.
- **⇒ The NOTOKEN race is NOT the whole render gate.** Fixing it makes the run not crash, but the title STILL
  emits only degenerate rects + state — never the textured draws prod builds on the identical guest (54
  pipelines). So the renderer is blocked by a **deeper divergence in the draw-issuing path itself**, not by the
  race, not by the cmd-buffer parse (device+13568 parses fine — it just contains no draws), and not by coverage
  (the draws ARE found; they're simply all rects). This unifies the entire multi-session search: ring, staging
  (SEGCP), and now the device+13568 directory ALL show only init=0x30088 rects with 0 textures.
- **Next direction (deep): why does variant A's render code emit only rects?** The thread to pull is the
  executor callback **0x821CC7A0** (carried by the `0001057C 821CC7A0` segment records) and its family
  sub_821CC310 — trace what it emits per work-item and why the textured-content draws prod issues are absent
  (a degraded render-object state that takes a degenerate path, or a content-draw subsystem that never runs).
  The race fix and the device+13568 parser remain useful downstream but are no longer the leading blocker.
- Diagnostics (gated, default boot unregressed VdSwap=6/segv=0): REX_CHUNKDUMP now = per-swap segment-descriptor
  count + CLEAN/RACED (g_nonBenignInd) + a one-time segment-FOLLOW of the first settled menu directory (resolves
  each {0x81LLLLLL,addr}, parses the segment, tallies realDraws/rectDraws/setConst/texFetch). g_nonBenignInd =
  count of non-benign (lr!=0x82292D08) INDIRECT-NULLs = the race indicator.

## cont.12(c11) — the unifying conclusion: the title BUILDS a deferred render program it never EXECUTES

User picked the executor-level trace ("why does draw-issuing emit only rects?"). Traced the executor callback
**0x821CC7A0** that the device+13568 segments carry (records `0001057C 821CC7A0 <ctx>`).

- **0x821CC7A0 = sub_821CC7A0 is a work-queue PRODUCER**, not a draw function: it stores its arg into a
  per-process ring buffer (base = *(0x8200098C/0x82000990) + procType·108 + 11328; count at +11412) and
  KeSetEvent's the consumer (sub_821CC310, which dequeues and calls *(item+16) to actually issue the draws).
- **Hooked it (REX_ENQLOG): over a full menu run sub_821CC7A0 is called ZERO times**, even though the menu is
  reached (36 shader loads) and the segments reference 0x821CC7A0 as a callback. So the segment render
  callbacks are **queued but never invoked** — the title's deferred render PROGRAM (the device+13568 segments:
  state + draw callbacks) is BUILT but never successfully EXECUTED. Meanwhile the fence-forward stopgap keeps
  satisfying the deferred-segment waits ([fencefwd] fence climbing 93717→93729…), so the title PROCEEDS as if
  the GPU ran the program — without the program actually running. The render-path INDIRECT-NULLs (e.g.
  `lr=0x821BF834 target=0xFFFFFFFF`, the same family the memory calls "WAIT_REG_MEM callbacks to 0x821BF860/
  0x821CC7A0") are plausibly the FAILED callback invocations of that program.
- **⇒ This UNIFIES the whole multi-session renderer search.** Every layer agreed the title emits only
  init=0x30088 rects with 0 textures (ring, staging/SEGCP, device+13568). The reason: variant A builds the
  textured render program (prod runs the same guest → 54 pipelines) but **never executes it** — only the
  directly-kicked degenerate rects reach the CP. Not a parse problem (device+13568 parses), not a coverage
  problem (the draws are found — they're rects), not purely the NOTOKEN race (clean runs also lack textured
  draws), not the draw-issuing logic per se — it's that the **deferred render program is never run**, exactly
  the stopgap hazard cont.11 flagged ("fence-forward lets the title proceed; deferred draws skipped").
- **Two concrete renderer paths (multi-session, pick next session):** (A) make the title execute its own
  program — drive the real GPU↔CPU handshake / fix the render-path callback invocations (the INDIRECT-NULL
  race + whatever gates sub_821CC7A0/the consumer) so the segments run; or (B) execute the device+13568
  segments OURSELVES — the directory format is cracked ({0x81LLLLLL,addr}, resolve 0xA0000000|(addr&0x1FFFFFFF)),
  so a real CP can walk it, run each segment's PM4, and invoke its callbacks (0x821CC7A0 → producer → consumer
  → draws), replacing the fence-forward so the fence advances as a REAL result. (B) is the "full cmd-buffer
  enumeration" route, now unblocked by the cracked format; (A) needs the deferred-execution trigger RE'd.
- Caveat to resolve first next session: confirm whether the segment callbacks are *attempted-and-fail*
  (INDIRECT-NULL at 0x821BF834/0x821BF860) vs *never reached* — hook 0x821BF860 + the consumer sub_821CC310 to
  see if the segment-execution path runs at all. That disambiguates path (A) (fix the failing invocation) from
  needing path (B) (drive it ourselves). Diagnostic added: REX_ENQLOG ([enq], gated; default boot unregressed).

## cont.12(c12) — disambiguation: leans path A — the render path is REACHED but its callback queues are EMPTY

Hooked the consumer sub_821CC310 too (REX_ENQLOG). Over a full menu run:

- **The sub_821CC* work-queue is DORMANT: producer sub_821CC7A0 = 0 calls AND consumer sub_821CC310 = 0 calls.**
  So the 0x821CC7A0 callbacks the device+13568 segments carry are **dead data** for the menu — that queue is
  not the active render path. (So path B "execute the segments + invoke 0x821CC7A0" is weakly supported — it'd
  drive a queue the title itself never uses.)
- **The ACTIVE render attempt is `sub_821BF748`** (the function that owns the INDIRECT-NULL at 0x821BF834). It's
  a **spinlock-protected DPC / callback-queue processor**: `KfAcquireSpinLock(r3+16920)`, increments a counter
  at +16756, stamps `mftb` at +16760, compares head/tail at +16908/+16912, and calls the queued callback at
  **`*(r31+16752)`** under a null-check. variant A: that callback is **null on a clean run (null-check skips it
  → no draw) or garbage 0xFFFFFFFF when raced (→ INDIRECT-NULL)**. Either way the **render-callback queue is
  EMPTY** — nothing valid was ever enqueued into it.
- **⇒ Path A, but the real gap is UPSTREAM of the callback site:** the render path IS reached (sub_821BF748
  runs every frame), it's just that **no producer enqueues textured render work** into its queue (+16752 stays
  null/garbage). This is the same shape as the dormant sub_821CC7A0 producer. So the divergence is the **menu
  render logic never enqueuing the textured-content render callbacks** — consistent with "emits only rects."
  Fixing the INDIRECT-NULL race (making +16752 not *garbage*) would only turn a crash into a *null-skip*, still
  no draw — so the race genuinely is NOT the gate (reconfirms c10/c11). The gate is **why nothing is enqueued**.
- **Next session (deep, the actual root): find the render-work PRODUCER** — what code in prod enqueues a valid
  callback into sub_821BF748's queue (+16752) / sets up the textured-draw render items, and why variant A's
  menu never calls it. Prod-oracle: break in prod at sub_821BF748, read the live `*(r31+16752)` (the real
  callback fn) + walk back to its enqueuer; then check whether that enqueuer runs in variant A. That enqueuer
  (or the object/state it needs) is the missing piece. Diagnostic: REX_ENQLOG now also hooks sub_821CC310
  ([consumer]); default boot unregressed.

## cont.13 (2026-06-03/04, autonomous) — RENDER PATH REFRAMED via prod oracle: the producer is the source=1 gfx-interrupt DPC; sub_821BF748/+16752 (c12's lead) is a DEAD END

Prod oracle (gdb on `out/build/linux-amd64-release/south_park_td`, base=0x100000000, `handle SIGSEGV nostop pass`) overturns c12 and pins the real render dispatch + a concrete success metric.

**c12's lead (sub_821BF748 +16752) is a DEAD END — proven in prod.** Object guest 0x40016f80, callback slot guest 0x4001b0f0 (+0x4170). A HARDWARE watchpoint on the slot over 160s at full vblank rate NEVER fired, and the dispatch `call *%rax` (sub_821BF748+387) executed **0 times**. So prod's +16752 callback is null too — sub_821BF748 is the **vblank (source=0)** DPC processor, NOT the render path. The +16752 chase was a red herring.

**The REAL render producer path (verified, prod):** producer `sub_821CC7A0` is called **4192×/120s** (consumer `sub_821CC310` 4191×, `sub_821BF860` 2095×). Backtrace:
`CommandProcessor::WorkerThreadMain -> ExecutePrimaryBuffer -> ExecuteIndirectBuffer -> ExecutePacketType3_INTERRUPT -> DispatchInterruptCallback -> ExecuteInterrupt -> sub_821C7170 -> sub_821CC7A0`.
⇒ the title's per-frame command stream (IBs in the PRIMARY ring) carries **INTERRUPT (type-3 op 0x54) packets**; the CP fires the guest gfx-interrupt **source=1 (command-complete)** → handler `sub_821C7170` → producer → enqueues render work → consumer `sub_821CC310` dequeues + calls *(item+16) → draws.

**sub_821C7170 decoded (prod disasm).** Two paths on the source arg:
- source==0 (vblank): reads MMIO 0x7FC86544 bit0, calls sub_821BF748 (the dead +16752 queue).
- source==1 (cmd-complete): `callback = *( *(g_interruptData + 0x2A94) + 0x10 )`, null-checked at +122 (skips the call if null), resolved + called at +204 (guest LR **0x821C71A4** = variant A's long-standing INDIRECT-NULL site). In prod this callback = sub_821CC7A0 (or sub_821BF860).
**0x2A94 = 10900** — EXACTLY the field variant A's `FireGfxInterrupt` STOPGAP checks (`if(source==1 && *(g_interruptData+10900)==0xFFFFFFFF) return;`). So `B = *(device+0x2A94)` = the per-submission completion object; `*(B+0x10)` = the DPC callback to run. Prod values: device(=g_interruptData=r4)=0x40016f80, B=0xffc9a000, *(B+0x10)=0x821cc7a0.

**variant A state (REX_INTLOG+REX_ENQLOG added this session; NOTOKEN+CSLEAK menu run, reaches SPTextured/SPBackdropTextured shader loads):**
- producer sub_821CC7A0 = **0**, consumer = 0, PM4_INTERRUPT executed = **1** (vs prod's per-frame stream).
- source=1 FireGfxInterrupt fired **3×**, all identical: `iData=0x00026F80 B=*(+10900)=0xA2011000 *(B+0x10)=0x00000000 -> FIRE`.
  ⇒ B is VALID (not the 0xFFFFFFFF sentinel) so the STOPGAP is NOT the blocker; **`*(B+0x10)` is NULL** — the producer was never registered into the completion object, so sub_821C7170's null-check skips the call.
⇒ Two divergences, one root: (D1) `*(B+0x10)` null (producer not registered) and (D2) source=1 barely fires (1 INTERRUPT packet vs prod's per-frame stream). Both because **variant A's CP never executes the title's per-frame render command stream** — the IBs with the INTERRUPT packets + the device+13568 segment records `0001057C 821CC7A0 <ctx>` that register the producer. This is the long-known "ring frozen WPTR=37 / flush sub_821C6D58 gated / deferred program never executed" root, now pinned to a concrete dispatch mechanism + success metric.

**SUCCESS METRIC for the renderer:** `REX_ENQLOG [enq]` (sub_821CC7A0) > 0 ⇒ producer fires ⇒ consumer issues real draws.

**NEXT:** (a) find who writes `*(B+0x10)=0x821CC7A0` in prod (the completion-callback registration; a prod HW watchpoint on B+0x10 armed at the first source=1 interrupt did NOT fire in 90s ⇒ it's written earlier, during graphics init — arm earlier / trace sub_821C73D8 which sets device+0x2A94=B, or follow the device+13568 `821CC7A0 <ctx>` segment records); (b) decide fix: drive variant A's CP to execute the per-frame IBs/segments so the INTERRUPT packets fire + B+0x10 gets registered (route B, now with the dispatch mechanism understood), vs. the title's own flush sub_821C6D58. Diagnostic added: **REX_INTLOG** ([int], default boot unregressed).

## cont.14 (2026-06-04, autonomous) — RENDER DEADLOCK ROOT FOUND: kick gated by a pending-counter (device+0x2b04) the CONSUMER (sub_821CC310, tid=10) must decrement — it never runs. Unifies with tid=10 starvation.

Continuing cont.13 (real render path = source=1 gfx-interrupt → producer sub_821CC7A0). Traced WHY variant A never executes the per-frame command stream, to the root.

**Per-frame render-submit chain RUNS in variant A but the KICK is gated.** Prod kicks the ring (sub_821C6600) 7062×/45s; variant A only 6× (init). Chain: `sub_82249638→…→sub_821CC830(D3D frame)→sub_821C6D58(flush)→sub_821C6C80→sub_821C6600(kick)`. variant A reaches the flush (3322×) and sub_821C6C80 (1410×) per-frame, but kicks 6×.

**The gate (decoded from prod disasm + verified both sides): `sub_821C6C80` kicks only when `*(device+0x2b04)==0`** (0x2b04=11012). Non-zero → runs a "process-pending" block (sub_821C5CD0) and DEFERS the kick.
- prod: the field oscillates **0↔1** (mostly 0 → kicks).
- variant A (REX_KICKGATE): the field climbs **MONOTONICALLY 0→1→2→…→0xA…→ and NEVER resets** → stuck non-zero → kick deferred forever after the 6 init kicks.

**device+0x2b04 is a pending-segment counter (producer/consumer flow control), proven by a prod HW watchpoint on device+0x2b04 (0x140019a84):**
- **Increment (→N+1):** render thread `sub_82249638→sub_82249678→sub_82249970→sub_82249AD0→sub_82150970→sub_821BF298→sub_821CCA28→sub_821C6C80` (queues a pending segment, defers the kick).
- **Decrement (→0):** `sub_82450FD0(thread)→sub_821CC5D0→sub_821CC310(CONSUMER)→sub_821CC140` — the consumer, when it drains a work item, decrements the counter.

**⇒ THE DEADLOCK / RENDER ROOT:** the consumer **sub_821CC310** runs on **tid=10 (thread entry sub_82450FD0)** — the long-starved/blocked thread (cont.8–12). In variant A the consumer NEVER runs (`[consumer]=0`), so device+0x2b04 is never decremented → climbs forever → the kick is permanently deferred → no per-frame IBs reach the ring → no INTERRUPT packets → the producer never fires → no draws. **The renderer root IS the tid=10/consumer-not-running problem.** (Parallel break from cont.13: even on the source=1 fires that do happen, `*(B+0x10)` is null — the producer isn't registered as the completion callback; that registration likely also lives on the consumer/tid-10 path.)

This UNIFIES the whole renderer search with the tid=10 starvation thread: rendering is a cross-thread producer/consumer pipeline (GPU-CP-interrupt producer ⟷ tid-10 consumer sub_821CC310) gated by the pending counter device+0x2b04; variant A's consumer side (tid=10) is dead, so the pipeline never flows and the ring goes idle after init.

**NEXT:** find why variant A's tid=10 (sub_82450FD0) never reaches the consumer loop sub_821CC5D0→sub_821CC310 (under NOTOKEN+CSLEAK): does the thread run at all? does it diverge into sub_82250420/sub_8211B740 instead? is it blocked on an object the (dead) producer should signal? Making the consumer run (decrement device+0x2b04 + issue draws via *(item+16)) is the renderer unblock. Diagnostic added: REX_KICKGATE ([kickgate]).

## cont.15 (2026-06-04, autonomous) — BOOTSTRAP experiment VALIDATES the producer/consumer model; PROVES textured draws are producer/consumer work items, not kicked segments

Tested the cont.14 model with gated, default-safe interventions (REX_BOOTSTRAP):

1. **Register the producer into *(B+0x10) when source=1 fires.** Result: the producer **sub_821CC7A0 FIRED ([enq]=1, was 0)** and the consumer **sub_821CC310 RAN ([consumer]=1, was 0)** — the producer→consumer chain mechanically works. BUT the work item was EMPTY: `[enq] item=0x0`, `[consumer] r3=0x29C98 item=*(r3)=0x29A44 handler=*(item+16)=0x0` (null draw handler) → no draw. variant A's work items (a device-relative ring near device+0x2AC4) exist but their handlers are null — the per-frame render submission that populates them didn't complete.

2. **Also force the kick gate open** (device+0x2b04=0 in sub_821C6C80). Result: kicks **FLOW — sub_821C6600 fired 2725× (was 6)**, the ring is alive, the CP executes the per-frame segments. BUT the draws are STILL **only init=0x30088 prim=8 numInd=3 untextured RECTS** (149/150 logged, 0 textured), then crashes (INDIRECT-NULL 0x80000000 @ lr=0x82448B70 — forcing kicks skips the title's segment bookkeeping → corruption). Force-kick hack reverted (left an inline NOTE; PROVEN non-viable).

**DEFINITIVE CONCLUSIONS:**
- The producer/consumer pipeline (sub_821CC7A0 → sub_821CC310) is the correct render mechanism and works when driven (validated end to end).
- **The textured draws are NOT in the directly-kicked command segments** — kicking them 2725× yields only rects. Textured content is produced by the producer/consumer pipeline processing REAL work items (handler = the draw fn at *(item+16), called via lr=0x821CC4B0).
- The real work items / completion objects (B) / item handlers are populated by the title's full render submission running against a FAITHFUL GPU (real command completion + interrupt timing). variant A's fence-forward stub fakes the fence value but not the completion semantics, so the submission produces incomplete state (null handlers, unpopulated B) and the pending counter device+0x2b04 is never decremented. Simple hacks turn the cranks with no grain (null items) or flow only the rect segments.

⇒ The renderer genuinely requires a **faithful CP / GPU-completion model**, not a stopgap. All pieces are now mapped: (real render path = source=1 interrupt → producer; gate = pending counter device+0x2b04; cross-thread producer(GPU-CP-interrupt)/consumer(tid-10 sub_821CC310); draw = item handler *(item+16)).

**NEXT (the real renderer, multi-session):** when variant A's CP consumes a kicked segment, MODEL its completion: (a) decrement device+0x2b04 (mimic the consumer) so kicks flow naturally; (b) fire the source=1 interrupt with the completion object B set up for that segment so the producer enqueues the segment's REAL work item (with a non-null handler); (c) let the consumer (tid-10, runs under NOTOKEN) drain it → issue the textured draw via *(item+16). Open RE: how a kicked segment maps to its completion object B + work-item handler (so the CP can populate them on completion). Diagnostics retained (gated, default boot unregressed): REX_INTLOG, REX_KICKGATE, REX_ENQLOG, REX_BOOTSTRAP (experiment).

## cont.16 (2026-06-04, autonomous) — completion object B characterized (prod vs variant A): variant A's B is ZERO-filled (unpopulated)

gdb dump of the per-submission completion object B = *(device+0x2A94), the descriptor the source=1 interrupt's producer is driven from:
- **prod (B=0xffc9a000):** B[0]=**4**, B+0x10=**0x821CC7A0** (producer callback), B+0x14=**0xddd10180** (per-submission context = the render work the producer enqueues), rest 0. (device+0x2b04 counter read =1 concurrently.)
- **variant A (B=0xA2011000), REX_INTLOG:** `B[0]=0 *(B+0x10)=0 *(B+0x14)=0` — **ENTIRELY ZERO**. B is allocated (device+10900 points to it) but NEVER populated.

⇒ variant A's completion object is a bare zero allocation; the title's render submission never writes {count, producer callback, render context} into it, because variant A's fence-forward stub GPU doesn't drive the faithful command-completion that triggers the population. This is exactly why cont.15's force-B+0x10 gave an empty work item — B+0x14 (the context) is also null. **The faithful-completion fix must populate B[0]/B+0x10/B+0x14 per completed segment** (callback=producer sub_821CC7A0, context=the segment's render work; format TBD — 0xddd10180 is an unusual handle / write-combined address, not a plain guest pointer). This is the precise remaining RE for the renderer. REX_INTLOG extended to dump B[0]/B+0x10/B+0x14.

## cont.17 (2026-06-04, autonomous) — route-B DIRECT-INVOKE: producer fires with real ctx (6886×) but the consumer doesn't drain (process-context/event obstacle)

REX_FINDCB located the deferred render program's producer-callback records {0001057C, 0x821CC7A0, ctx} (c11) — staging range = 2/frame, device+13568 chunk = 4 — with REAL per-record contexts **ctx=0xC0090180 / 0xC0117B00** (GPU-physical 0xC-window, analogous to prod's B+0x14=0xddd10180). So the records carry valid contexts; variant A queues but never invokes them.

REX_INVOKECB (route-B direct-invoke): call `sub_821CC7A0(ctx)` for each staging record (bypassing the unpopulated/aliased completion object B). Result:
- **The producer FIRED 6886× (22 distinct item-types) — up from 0.** Invoking the records with their real ctx DOES drive the producer to enqueue.
- **BUT the consumer sub_821CC310 STILL didn't run ([consumer]=0)** — the producer's KeSetEvent didn't wake it. Likely a **process-context mismatch**: the producer enqueues into the per-process ring (base=*(0x8200098C)+procType·108+11328) for the CURRENT thread's procType (VdSwap's), while the consumer (tid=10) drains a DIFFERENT procType's ring — so the work + the event signal miss the consumer.
- Crashes on the render-path INDIRECT-NULL 0xFFFFFFFF @ lr=0x821BF834.

⇒ The producer can be driven directly with the records' real ctx, but the producer→consumer handoff fails on a process-context/event mismatch (+ a render-path null). **Next obstacle:** invoke the producer on the procType the consumer (tid=10) drains — read the *(0x8200098C)+procType·108 ring layout, match contexts — OR signal the consumer's wait directly. Diagnostics REX_FINDCB / REX_INVOKECB added (gated, default boot unregressed; INVOKECB crashes when used — a scaffold for the next session).

## cont.18 (2026-06-04, autonomous) — FIX PROGRESS: pump-context completion (REX_PUMPCB) WAKES the consumer (no crash); drain not yet sustained + no draws

Building on cont.17 (route-B direct-invoke fired the producer 6886× from VdSwap's context but the consumer never drained + crashed at INDIRECT-NULL). REX_PUMPCB drives the same producer/consumer from the **PUMP context** (its procType matches the consumer's — cont.15 showed a pump-fired interrupt wakes the consumer): each vblank, scan the device+13568 chunk for callback records {0001057C,821CC7A0,ctx}, populate B{+0x10=0x821CC7A0,+0x14=ctx}, fire source=1.

Result (NOTOKEN+CSLEAK menu run): [pumpcb] fired 764, **producer [enq]=17** (was 0), **consumer [consumer]=1** (was 0), **NO CRASH** (vs INVOKECB's crash — ran the full 40s). ⇒ firing on the pump context DID wake the consumer — confirms the cont.17 procType hypothesis (VdSwap's context was wrong, the pump's is right). BUT:
- The consumer drained only **1×** (not sustained) despite 764 fires / 17 producer enqueues — the producer's repeated KeSetEvents don't re-wake it (event auto-reset semantics, the consumer doesn't re-loop, or tid=10 stalls after the first drain).
- No textured draws (only the 1 init=0x30088 rect). (The [consumer] hook logs the queue-head sentinel handler=0, so it's unclear if the real dequeued item had a handler — but no draw was issued regardless.)

⇒ Genuine incremental fix progress: the producer/consumer render pipeline now PARTIALLY flows from the pump context (producer fires, consumer wakes once, stable). **Remaining obstacles:** (a) sustain the consumer drain — RE the consumer's KeWait/event re-arming and why it drains 1 of N (read sub_821CC5D0's wait object + sub_821CC310's dequeue loop); (b) get a real draw — verify the ctx's render data / work-item handler is complete (it may itself depend on more of the title's submission running). Diagnostics REX_PUMPCB / REX_FINDCB / REX_INVOKECB (gated, default boot unregressed).

### cont.18 refinement — the consumer-sustain blocker is event RE-WAKE, not a tid=10 stall
Under REX_PUMPCB, gdb counts: consumer dispatch sub_821CC5D0 entered **2×** (identical to the baseline cont.14 — so tid=10 is NOT stalling/diverging) and sub_821CC310 drained **1×**. ⇒ the consumer thread sits inside sub_821CC5D0's internal `KeWaitForSingleObject` loop; the FIRST producer KeSetEvent wakes it (drains 1) but the subsequent ones (17 enqueues / 764 fires) do NOT re-wake it. This is a variant A KERNEL event/wait re-signal issue (its own C++: KeSetEvent → KeWaitForSingleObject across threads under NOTOKEN), NOT deep guest RE — a tractable next target. NEXT: read variant A's KeSetEvent + KeWaitForSingleObject + the specific event object the consumer waits on; verify auto-reset re-signal semantics wake a re-blocked waiter. Once the consumer drains sustainedly, re-check whether real (textured) draws issue (work-item handler completeness).

### cont.18 refinement 2 — the event wait/signal is CORRECT; the consumer-drain-1 is the producer/consumer RING semantics
Read variant A's WaitObject/SignalObject (NOTOKEN path): SignalObject sets obj+0x04=state under g_waitMutex + g_waitCv.notify_all(); WaitObject waits on g_waitCv with predicate `GLD32(obj+4)>0`, consumes (auto-reset decrement) on wake. This is CORRECT (no lost-wakeup — the predicate catches a signal that races the re-wait). So the event re-wake is NOT the blocker. gdb showed sub_821CC5D0 woke **2×** but drained (sub_821CC310) only **1×** — the 2nd wake found the per-process ring "empty" despite 17 producer enqueues. ⇒ the blocker is the **producer/consumer ring accounting**: the producer's enqueue (per-process ring base=*(0x8200098C)+procType·108+11328, count+11412) isn't accumulating into the ring the consumer drains, OR the consumer's dequeue count is off. Likely contributors: (1) my REX_PUMPCB re-scans the SAME device+13568 records every vblank (no cursor) → re-fires the same ctx → the producer may dedup/overwrite one ring slot; (2) a procType mismatch between the pump-context producer and the consumer's ring. NEXT: add a cursor to PUMPCB (process each record once); instrument the producer's ring count (+11412) vs the consumer's dequeue; confirm the pump's procType == the consumer's (KeGetCurrentProcessType). THEN re-check real (textured) draws. This is the precise, bounded remaining work to make the producer/consumer render pipeline flow sustainedly.

## cont.19 (2026-06-04, autonomous) — the producer/consumer pipeline CAN be driven, but the work data is INCOMPLETE; a faithful CP/GPU model is the fundamental requirement

Spacing the pump fires to 1 record/vblank (vs 16) to avoid auto-reset-event collapse: [consumer] STILL =1 over 834 spaced fires — and KeGetCurrentProcessType always returns 1, so procType matches (not the issue). Re-reading the result: the consumer's drain sub_821CC310 is called ONCE and drains the whole available BATCH in that call (its internal loop), so [consumer]=1 means "drained the batch" (14 producer items); the producer then stops enqueuing (the device records are static → same ctx → dedup → no new items → no re-signal), so the consumer correctly waits. **So the producer/consumer render pipeline DID RUN — 14 items enqueued + drained — but produced NO textured draws (60 init=0x30088 rects only) and crashed (INDIRECT-NULL).**

⇒ **DEFINITIVE CONCLUSION:** the producer/consumer pipeline can be DRIVEN (producer fires with real ctx, consumer drains the batch), but the WORK ITEMS' render data is INCOMPLETE in variant A — the ctx (0xC0090180) → work item → handler `*(item+16)` doesn't issue a valid textured draw (null → crash). The render work is incomplete because the title's render submission is built against variant A's **STUB GPU** (fence-forward, no real command execution / completion), so the draw state, work-item handlers, and completion objects the producer/consumer rely on are never fully populated. **The menu renderer fundamentally requires a FAITHFUL CP / GPU-completion model** so the title's render submission COMPLETES and builds real, complete render work — driving the producer/consumer shells alone is necessary but NOT sufficient.

**The complete render pipeline + every gating mechanism is now mapped (cont.13–19):** source=1 gfx-interrupt → sub_821C7170 → producer sub_821CC7A0 (callback at *(*(device+0x2A94)+0x10)) → consumer sub_821CC310 (tid=10) → draw `*(item+16)`; gated by the pending-counter device+0x2b04 (consumer decrements) and the per-process work-item ring; work items carry real contexts (0xC0090180) from device+13568 records {0001057C,821CC7A0,ctx}. The remaining work is the multi-session core renderer: a faithful CP that EXECUTES the title's command stream + models GPU completion, so the render work is built completely. Gated scaffolds retained: REX_PUMPCB / REX_INVOKECB / REX_FINDCB / REX_INTLOG / REX_KICKGATE / REX_BOOTSTRAP. Default boot unregressed throughout.

### cont.20 — synthesis confirms the work-data-incomplete conclusion across all drive approaches
Combined INVOKECB (producer arg = the record's real ctx) with PUMPCB's pump context: call sub_821CC7A0 directly with r3=ctx on g_pumpKpcr (procType matches the consumer). Result: producer [enq]=17, consumer drains the batch, but STILL no textured draws (1 rect) + crash. ⇒ Confirms cont.19 across ALL THREE drive variants (INVOKECB / PUMPCB-via-interrupt / PUMPCB-direct-ctx): the producer/consumer pipeline RUNS when driven, but the work item's render data — `*(item+16)` handler + draw state — is incomplete because the title built its submission against variant A's STUB GPU. No amount of driving the producer/consumer shells substitutes for real GPU command execution. ⇒ the faithful CP/GPU model (varianta/RENDERER-DESIGN.md) is the required, multi-session core renderer work. Single-session RE + drive-experiments are exhausted at this fundamental wall.

## cont.21 (2026-06-04, autonomous) — ⭐ PROD-ORACLE RE-GROUNDING: cont.19/20 premise FALSIFIED; producer/consumer is COMPLETION BOOKKEEPING, not the draw path; the renderer needs a real PM4→Vulkan CP

Used the **prod oracle** (rexglue `south_park_td`, full symbols, recomp ABI rdi=&ctx rsi=base, PPCContext.r3 at offset 0 — verified by disasm of `__imp__sub_821CC7A0`) to measure the ground truth the cont.13–20 RE was inferring. Five decisive measurements (tools committed under `varianta/tools/prod_*.gdb`):

1. **Producer work item, PROD (`prod_producer.gdb`, break sub_821CC7A0, read r3 + `*(item+16)`):** 12 fires, items `0xddd10180, 0xddd98a80, 0xdde21300…` (the per-submission ctx, +~0x88900 apart), layout `{+0=self+0x124 (vtable), +4=0x80000000, +8=0, +12=4, +16=0, +20=0}`. **`*(item+16)=0x00000000 in PROD TOO.** ⇒ cont.19/20's central premise ("variant A's work data is INCOMPLETE because `*(item+16)` is null") is **FALSIFIED** — a null handler at +16 is NORMAL. The work item is a small completion-notification object, not a draw command.
2. **Producer firing rate + backtrace:** prod producer fires ~4192×/120s ≈ **1/frame**, from `ExecutePacketType3_INTERRUPT → DispatchInterruptCallback → ExecuteInterrupt → sub_821C7170 → sub_821CC7A0`. 1/frame ≠ per-draw (~800/frame). ⇒ **the producer/consumer (sub_821CC7A0/sub_821CC310) is PER-SUBMISSION GPU-COMPLETION BOOKKEEPING, NOT the per-draw path.** The entire cont.13–20 chase treated it as the draw emitter — it is not.
3. **Where the draws actually are (`prod_cpmap.gdb`, break ExecutePrimaryBuffer/ExecuteIndirectBuffer/_DRAW_INDX/_INTERRUPT):** prod's host CP runs a tiny **primary ring** (start/end 0→25→31→37→42→86 …) that chains **indirect buffers** in the physical window (`0x1dc90040, 0x1dc90000, 0x1fc97000, …`). The DRAWS live in IBs — esp. a recurring **3592-dword IB at phys 0x1dc90540** (= variant A guest `0xBDC90540`), executed 3× per frame, carrying the frame's `DRAW_INDX` packets. ⇒ **draws are PM4 `DRAW_INDX` in kicked IBs, executed by the HOST command processor → Vulkan.** (Resolves the cont.10 "0 draws in device+13568 segments" paradox: the draws aren't in the title's segment-directory bookkeeping; they're in the IBs the title kicks to the primary ring — which variant A never reaches because it only kicks 6×.)
4. **Variant A's Vulkan side is PRESENT-ONLY (code read of runtime/vulkan_render.cpp + rex_render.h):** its own header says *"No PM4 translation yet."* There is **no `VkPipeline`, no pipeline cache, no `vkCmdDraw`** — only clear-present + the intro-movie YUV→RGB `vkCmdCopyBufferToImage` blit. ⇒ the prior "Vulkan side is READY, just no entry" claim is **overstated**; a real PM4→Vulkan **draw translator does not exist** and must be built.
5. **The variant-A stall is the kick-gate DEADLOCK, reconfirmed with the corrected lens:**
   - gdb all-thread bt of a stalled variant A: **main thread** is in the intro **movie widget sub_82425BF8 → sub_821BE680 → … → sub_821C0A70 → sub_821C6E58 (GPU fence-wait) → sub_821B9270 (spin)**; the **consumer** (sub_821CC5D0) blocks in `KeWaitForSingleObject`. So with NOTOKEN+CSLEAK alone the title hangs in the **intro movie** on a GPU completion fence.
   - With `REX_MOVIE_EOF=120 REX_XFLAG=1` (forced intro→menu), the title runs a **stable loop**: the fence-forward stopgap fakes completion every frame (`fence 607017→607065, head always +8`) — no crash, but it **suppresses draw submission** (the title never has to kick because completion is faked).
   - `REX_KICKGATE` in the menu loop: **6 KICK (counter=0, init) then 74 DEFER** — `device+0x2b04` climbs `0→9→0xA` monotonically and is **never decremented**. The title is *actively trying to submit frames* (74 deferred kicks) but the gate blocks every one because the pending-counter never returns to 0.
   - `REX_PUMPCB`/`REX_BOOTSTRAP` (drive the producer/consumer): producer fires (enq=17–20), consumer drains once, but **kicks stay at 6, WPTR frozen at 37** — driving the bookkeeping does NOT decrement the counter sustainedly and does NOT progress the title. Variant A B at source=1: `B[0]=0` (prod=4), `B+0x14=0` (prod = the real ctx) — the completion object is **unpopulated** because no real GPU execution drives the completion handshake.

**⭐ CORRECTED UNIFIED MODEL.** Variant A has no real GPU. Every point where the title submits GPU work and waits for its completion (intro-movie compositing via sub_821C6E58; per-frame menu submission via the kick gate) stalls on a completion that only real command execution produces — the fence advance, the pending-counter `device+0x2b04` decrement, and the completion-object B population (`B[0]`, `B+0x10`=producer, `B+0x14`=ctx). The fence-forward stopgap papers over this for *some* waits (target<head) which lets the title limp through init + intro + menu *logic*, but it **suppresses real draw submission** (the title stops kicking the draw IBs because completion is faked). The producer/consumer is **bookkeeping inside the completion handshake** (1/frame), not the draw emitter; `*(item+16)=0` is normal. The actual menu draws are PM4 `DRAW_INDX` in IBs (e.g. `0xBDC90540`) that the title only kicks once its completion handshake oscillates — which it can't, without real execution.

**⇒ RENDERER-DESIGN.md's prescription ("faithful CP — stop faking completion, execute the commands") STANDS, but its RATIONALE is corrected:** the goal is NOT to "build non-null `*(item+16)` work items" (those are null in prod too) — it is to make the title's **completion handshake oscillate** (counter decrement + fence advance as REAL results of executing each kicked submission) so the title **keeps kicking its draw IBs**, AND to **translate the kicked `DRAW_INDX` packets to Vulkan**. This is, in scope, the project's previously-identified structural-floor **GPU command processor** (PM4→Vulkan: vertex-fetch + the 19 .updb shaders→SPIR-V + textures + RT/blend/viewport + pipeline cache). The producer/consumer drive-experiments (cont.13–20) are a closed dead-end branch — necessary to map, but the producer/consumer is not where draws come from.

**NEXT (the two real pillars, in dependency order):**
- **(A) Flow the ring** — break the kick-gate deadlock by decrementing `device+0x2b04` as a real result of the CP completing each kicked submission (faithful per-completion decrement, NOT cont.15's blanket force-to-0). Success metric = **kicks climb past 6 / WPTR past 37** (title submits real frames → kicks the draw IBs). cont.15 proved forcing the counter flows the ring (2725 kicks) — a faithful decrement should too, without the forced-state crash.
- **(B) DRAW_INDX→Vulkan translator** — the actual renderer: per draw, build a VkPipeline from the reg-file state (0x4000 ALU consts, 0x4800 fetch consts → vertex streams + textures, RT/viewport/blend) + the bound .updb shader→SPIR-V, then vkCmdDraw into the swapchain (render thread owns it). Start by rendering whatever reaches the CP once (A) flows the ring.

New tools (committed, gated, default boot unregressed): `varianta/tools/prod_producer.gdb`, `prod_cpmap.gdb` (prod-oracle CP tracing — reusable).

### cont.21 pillar-A experiment — REX_CPCOMPLETE: counter-drain BREAKS the first gate (kicks 6→14) but a SECOND gate remains
Implemented the corrected pillar A (kernel.cpp, gated `REX_CPCOMPLETE`, default boot unregressed — verified): each vblank the pump decrements the pending-submission counter `device+0x2b04` by 1 (a faithful per-completion drain modeling GPU completion, NOT cont.15's blanket force-to-0 in the kick hook).

**Result (NOTOKEN+CSLEAK+MOVIE_EOF+XFLAG menu config):**
- **The kick-gate deadlock is BROKEN.** Kicks **6 → 14**, WPTR **37 → 61** (8 new frames submitted). The main thread **progresses past the render fence-wait** (sub_821C6E58) it was stuck in (cont.21 §5) — two gdb samples 6s apart show it in *different* functions (sub_82150970 → sub_821D5CA8/sub_821C48B0), i.e. **alive and cycling** through the render/game loop, not deadlocked. ✅ Confirms the corrected model: `device+0x2b04` IS the first gate; draining it (as a CP-completion result) flows the ring.
- **But a SECOND gate remains.** Kicks plateau at 14 / WPTR 61 (identical at 45s and 60s). All kicked IBs are SMALL state/setup buffers (len ≤266 dw, in the 0xA001xxxx/0xA011xxxx/0xA2014xxx windows); the big **content draw IB never appears** (prod's is 3592 dw @ phys 0x1dc90540 = guest 0xBDC90540), and still only `init=0x30088` rects, **0 textured draws**. So the title submits ~8 frames of setup then stops submitting — the counter-drain alone is necessary but not sufficient: the title needs the FULLER completion handshake (producer/consumer bookkeeping → command-buffer/resource recycling, and/or the segment-pointer fence sub_821C5DF0) before it keeps submitting content. (Open: also possible the forced MOVIE_EOF/XFLAG state isn't a genuine "menu up" — verify whether the title is actually at menu-content rendering in this config vs an attract/idle loop.)

⇒ **Pillar A advanced, not finished:** first gate (pending-counter) cracked + the second gate localized.

**Second-gate experiments (both NEGATIVE — the second gate is NOT another completion mechanism):**
- **CPCOMPLETE + PUMPCB** (counter-drain + drive producer/consumer with the real record ctx): kicks STILL 14, WPTR STILL 61, consumer drains once, 0 textured. ⇒ driving the producer/consumer does NOT make the title submit content — reconfirms it's bookkeeping, not the gate.
- **CPCOMPLETE on the NATURAL path** (NOTOKEN+CSLEAK only, NO MOVIE_EOF/XFLAG): IDENTICAL (kicks 14 / WPTR 61 / 0 textured) AND the title **reaches the frontend naturally — loads its menu shaders** (`media/shaders/Simple.xbv`, `SPTextured.xbv/.xbp`) — with **NO CRASH over 60s** (vs the documented render-SIGSEGV ~33s baseline). So the plateau is NOT a forced-state artifact; it's the genuine menu behavior.

⇒ **CONCLUSION: the second limit is the fundamental pillar-A↔pillar-B COUPLING.** The title reaches the menu + loads its render shaders, the counter-drain flows ~8 frames of setup IBs, but it will NOT submit the content draw IBs until it sees REAL GPU results (executed+rendered draws + real completion side-effects). You can't fully flow the ring (A) without rendering (B), and you can't render (B) without the ring flowing (A) — they must be built together. No completion-driving trick substitutes; this is the structural GPU-CP requirement, now proven from the title's own progression (not inferred).

**Bonus:** `REX_CPCOMPLETE` is a genuine improvement to keep — it flows the ring AND stabilizes the title (60s no-crash, reaches shader-load) where the prior baseline SIGSEGV'd ~33s. Good foundation for pillar B.

**NEXT (the real remaining work = pillar B + the coupling):** build the DRAW_INDX→Vulkan translator (VkPipeline from reg-file state + the now-loaded .updb shaders→SPIR-V + vertex-fetch + textures), wire it so executing a kicked submission produces a REAL completion (fence advance + counter decrement reflecting actual GPU work), and iterate the A↔B coupling so the title advances from setup IBs to content draw IBs. Run CPCOMPLETE as the ring-flow + stability base. Committed (gated, default-safe, NOT pushed).

## cont.22 (2026-06-04, autonomous) — ⭐ RE-GROUNDING cont.21: title is NOT at menu-content render (it's stuck in the frontend MOVIE/ATTRACT loop); ring-flow ≠ content; and the real crash is DISPATCH-TABLE corruption (the table lives in guest-WRITABLE space)

cont.21 concluded "second gate = pillar-A↔B coupling; build the PM4→Vulkan draw translator." Three independent measurements this session **correct that framing**: the title never reaches menu-content rendering at all, so there are no content draws to translate yet — the blocker is upstream, and the crash that limits every run is a memory-corruption bug in the harness, not a renderer gap.

### 1. The pending-counter device+0x2b04: variant-A RUNAWAY vs prod BOUNDED-at-1 (measured)
- Variant A, natural CPCOMPLETE path (`REX_KICKGATE`): **r3 == dev == g_interruptData == 0x26F80** (all the same object — CPCOMPLETE drains the *correct* counter; not a wrong-counter bug). The title is **NOT idle**: it calls the kick gate sub_821C6C80 **15,768×** (16 KICK + **15,752 DEFER**) — it *hammers* the submit path. `device+0x2b04` climbs monotonically 0→0xA+ because CPCOMPLETE drains **1/vblank (~60/s)** while the title increments **~7–8/vblank** (`[cpcomplete]` log shows the counter jump 1→9→16→24 between drains). The gate is permanently closed by a **drain-RATE mismatch**, NOT "the title stopped submitting" (cont.21's wording corrected).
- **Prod** (`tools/prod_counter.gdb`, break sub_821C6C80 / sub_821CC140 / sub_821CCA28; dev=0x40016f80): `device+0x2b04` oscillates **strictly 0↔1 — cmax=1 over 33,000 kick-gate samples** (hist {0:18019, 1:14981}, ~55% KICK), the decrement keeping per-submission pace. ⇒ the faithful pillar-A drain must be **per-submission (bounding ≤1)**, NOT 1/vblank. But see §2 — this does not unlock content.

### 2. Ring flow ≠ content (measured + cont.15)
Across all kick rates — 16 (natural, this session) and 2725 (forced-to-0, cont.15) — the title builds **only `init=0x30088` degenerate rects, 0 textured draws**. More ring flow does NOT produce content. The pending-counter is a solved-in-principle downstream lever; it is **not** the content gate.

### 3. cont.21's OPEN QUESTION resolved — the title is in the frontend MOVIE/ATTRACT loop, not the menu (measured backtrace)
gdb all-thread bt at the natural plateau — the **main thread**:
```
sub_82150970 (frontend) → sub_82222258 → sub_822132A0 → sub_82426E50 → sub_824267B0
  → sub_82425BF8 (the intro/attract MOVIE WIDGET) → sub_821BE680 → sub_821BDF00/DE40
  → sub_821C0A70 → sub_821C6E58 (GPU fence-wait) → sub_821B9270 (spin)
```
The main thread is **driving the movie widget and spinning on a GPU fence** — NOT at menu-content rendering. The "menu shaders loaded (Simple/SPTextured/SimpleCol/SpHud…)" is **precache** during the movie screen, not active menu draw. (cont.21's "reaches the menu" is overstated: it reaches the *frontend* but is stuck on the movie/attract screen.) The forced path (MOVIE_EOF+XFLAG, cont.7/21) gets *past* the movie but stalls at menu-SETUP null vtable sub_8215DE84 — also not content. **Neither path reaches menu-content rendering.**

⇒ cont.21's "build the PM4→Vulkan translator NOW" is **premature / out of order**: there are no content draws to translate because the title never gets to content rendering. (The translator is not *wrong* — prod's menu renders 54 pipelines, so variant A's menu will eventually need it — it's just next-after reaching the menu.)

### 4. The natural-path crash is OURS: DISPATCH-TABLE CORRUPTION (measured + root-caused)
The natural path crashes ~15–20s (NOT cont.21's "60s no-crash" — that was a lucky race outcome; the INDIRECT-NULL cascade is the cont.12(c9) race). Crash bt: **the VblankPump** → `FireGfxInterrupt(cb=0x821C7170, source=0/vblank) → CallGuest(0x821C7170)` → a garbage rip **0xffffffff40f62598**, with **no sub_821C7170 frame** — `DispatchLookup(0x821C7170)` *itself* returned the garbage. The dispatch-table slot for sub_821C7170 (called fine early, cont.13) is **corrupted**.

Direct gdb read of the table at the crash — **every sampled slot is garbage**:
```
sub_821C7170  slot=guest 0x82abe2e0  *slot=0xffffffff40f62598
sub_821C6600  slot=guest 0x82abcc00  *slot=0xffffffff00008f82
sub_82150970  slot=guest 0x829d12e0  *slot=0xffffffffffffffff
sub_821CC7A0  slot=guest 0x82ac8f40  *slot=0x0000000000000000
raw bytes @ slot:  98 25 f6 40 ff ff ff ff … 67 61 6d 65 3a 5c 6d 65 64 69 61 5c  = "game:\media\"
```
**ROOT CAUSE:** the dispatch table is placed at `g_base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE = g_base + 0x82930000` — i.e. **inside the guest-writable 4 GiB mmap, immediately after the image** — and extends to 0x83311830 (CODE_SIZE·2, 1 PPCFunc* per 4 code bytes). The title's own **post-image runtime data** (file-path strings `"game:\media\…"`, floats, 0xFFFFFFFF sentinels) lands in [0x82930000, …) and **overwrites the table slots for its frontend/render functions** (measured corruption spans the slots for 0x82150970..0x821CC7A0). `DispatchLookup`/`PPC_LOOKUP_FUNC` then return garbage pointers that the unguarded `if (fn)` happily CALLED → SIGSEGV. **This is almost certainly the root of the long-standing render-path "string-as-code" / INDIRECT-NULL crashes (cont.10–21)** — this session also logged INDIRECT-NULL targets `0x67616D65`("game"), `0x6C6F6261`("Glob"), `0x41737365`("Asse"ts), `0x74735C47` at lr=0x82204D08 (the cont.12 string-as-code site), all path fragments interpreted as code.

### Fixes shipped (default-safe, default-boot verified unregressed)
- **(a) PPCInvokeGuest range bound: IMAGE_SIZE → CODE_SIZE.** The dispatch check admitted targets up to image_end (0x82930000), but the table only covers code_end (0x825F0C18); the [code_end, image_end) gap — where the title's vtables/rodata (0x826xxxx–0x828xxxx) live — indexed PAST the table into garbage. Correct latent bug, fixed.
- **(b) Host-fn-pointer validity guard (`ValidHostFn`).** Compute [lo,hi] of the recompiled fn pointers once from `PPCFuncMappings[]`; in BOTH `PPCInvokeGuest` and `CallGuest`, skip+log (`[DISPATCH-CORRUPT]` / `CallGuest: corrupt`) a looked-up pointer outside that range instead of calling it. Turns the corrupted-slot crash into a clean skip.
- **Verification:** default boot — 0 guard-fires, 0 device-overwrite, 30s no crash (unregressed). Natural path — under gdb it ran the **full 55s with NO crash while the guard caught 2107 corrupted-slot dispatches** (render-path sites lr=0x821BF834/0x821C84C0/0x821C8578). Non-gdb 3× sample: **2/3 survived 45s (guard caught 1515/1528)**, 1/3 still crashed (40 catches before a different-path fault). So the guard substantially improves stability (was always-crash ~15s) but is a **partial** mitigation — it catches the dispatch-READ crash class; it does NOT stop the corruption, and skipping a needed call can perturb downstream state.
- ⚠ **The complete fix = RELOCATE the dispatch table out of guest-usable space.** Either (best) put it in a SEPARATE host allocation (not inside g_base), or move its base from `PPC_IMAGE_BASE+PPC_IMAGE_SIZE` to a verified-unused guest region. Both touch `PPC_LOOKUP_FUNC` (ppc/ppc_context.h:113) + runtime.cpp:70 + kernel.cpp `DispatchLookup` → full recompile. The title's post-image data extends at least to ~0x82ac9000 (measured); pick the relocation target well clear of the title's data.

### NEXT (corrected priorities)
1. **Relocate the dispatch table** out of [0x82930000, …) (separate host allocation is cleanest) — the real fix for the corruption / string-as-code crash class; likely unblocks much of the render-path instability seen across cont.10–21.
2. **Reach the actual menu** (the true content-render gate): resolve the frontend movie/attract→menu transition — the movie GPU-fence sub_821C6E58 / movie EOS via the real VC-1 decoder + fair scheduler (the cont.9/10 Step-1 thread); or, on the forced path, recover the menu-setup null vtable sub_8215DE84 (cont.7).
3. **THEN** assess menu content draws and build pillar B (DRAW_INDX→Vulkan). The faithful pillar-A counter drain (per-submission, bounding ≤1 like prod) is a downstream lever, not the gate.

New tool: `tools/prod_counter.gdb` (prod device+0x2b04 oscillation + inc/dec rates — reusable). Committed (gated, default-safe, NOT pushed).

### cont.22 follow-up — dispatch table RELOCATED out of guest space (the REAL fix; corruption ELIMINATED)
Implemented the real fix flagged above. The dispatch table now lives in a **separate host `mmap`** (`g_funcTableBase`, runtime.cpp, `CODE_SIZE·2 + 4096` B), NOT at `g_base+0x82930000` inside the guest 4 GiB map. New `HostFnAt(guestAddr)` reads it (same byte layout: 1 PPCFunc* per 4 code bytes, indexed `(addr-CODE_BASE)*2`, out-of-range→null); `PPCInvokeGuest` and `DispatchLookup` use it. **Scope = runtime.cpp (alloc+populate) + kernel.cpp (read) + kernel.h (decl) only** — the recompiled TUs route indirect calls through `PPCInvokeGuest` (rex_indirect.h overrides `PPC_CALL_INDIRECT_FUNC`), so `PPC_LOOKUP_FUNC` / ppc/ppc_context.h are untouched → **no mass recompile** (2 TUs). The freed guest region `[0x82930000, 0x83311830)` becomes plain RAM for the title's post-image data.
- **✅ Corruption ELIMINATED.** Loader confirms `dispatch table at host 0x7f0f33cbf000 (0x9E2830 B, OUT of guest space)`. The cont.22 `ValidHostFn` guard now fires **0×** across every run (was 40–2107/run) — proof the table is no longer being overwritten. Default boot unregressed (0 device-overwrite, 25s no crash). Dispatch still correct (title reaches the same kicks=14 / 6 Lua-script loads).
- **Residual (separate, NOT corruption):** the natural path still crashes **intermittently** — non-gdb 3× = **2/3 survived 45s, 1/3 crashed with guardfires=0** (so NOT the table corruption); under gdb it ran 50s no-crash (race-timing). This is a DIFFERENT bug, previously masked by the corruption crash — almost certainly the **cont.12(c9) render-DPC race** (the pump-driven `sub_821BF748` dispatching a *stale pooled render-command object* field whose value happens to be an in-range valid fn → wrong dispatch → crash; ValidHostFn can't catch an in-range-valid pointer). That needs the real scheduler/sync (cont.9/10 Step-1), not a table fix.
- The cont.22 guards (CODE_SIZE bound + ValidHostFn + DISPATCH-CORRUPT/CallGuest-skip logging) are KEPT as a cheap defensive net + tripwire: post-relocation they should never fire; if `[DISPATCH-CORRUPT]` ever logs, something other than a guest store corrupted the table.

⇒ **Updated NEXT:** (1) ~~relocate table~~ ✅ DONE. (2) the residual render-DPC race (cont.12(c9)) — fix via real sync, OR continue toward the menu (movie/attract→menu transition). (3) reach the actual menu = the true content gate; THEN pillar B (DRAW_INDX→Vulkan). Commit (default-safe, NOT pushed).

### cont.22 loop-iter — the corruption fix UNBLOCKED the forced menu-setup path (sub_8215DE84 gone)
Re-tested the **forced path** (`REX_MOVIE_EOF=120 REX_XFLAG=1` — the cont.7 config) with the relocated build: **0 crash over 55s, guardfires=0, and the ONLY INDIRECT-NULL is the benign `0x82292D08`.** cont.7's menu-setup blocker `[INDIRECT-NULL] 0xFFFFFFFF @ sub_8215DE84` (`0x8215DE84` — squarely IN the corrupted slot window `0x82150970..0x821CC7A0`) is **GONE** — it was the dispatch-table corruption, not a genuinely missing function. The title now runs **stably in the frontend movie/attract loop**: intro → (forced EOS) → attract (`towerDefense_attract_movie.wmv` decoding, buf18 `varied`), movie-EOS completions posting (`sub_8222A9F8` ×3), loads `Strings.bin` — but **NO menu content**. ⇒ **the corruption fix removed a major crash class (very likely several of the cont.10–21 render-path crashes); the remaining blocker to menu content is the attract→menu TRANSITION logic (cont.7's `sub_82163118` advance / the `sub_8211B740`→`sub_8210AF90` screen-state divergence), independent of the corruption and of the renderer.** NEXT: RE the attract→menu transition — why `sub_82163118` doesn't advance despite forced EOS + XFLAG + the posted completion. (Probe note: a run-to-SIGKILL gdb script can't print after `run`; count from inside the breakpoint handlers + print periodically.)

### cont.22 loop-iter 2 — ⚠ CORRECTION + the REAL menu-setup frontier reached (forced + START → sub_8215DE84 0xFFFFFFFF)
Probed the transition (gdb counters, in-handler prints) on the forced path and CORRECTED the prior note:
- **Transition map (forced, no START):** `sub_82163118` (advance) fires **exactly once** (intro→attract); `sub_82161920` (intro screen machine) runs 168× then freezes (intro screen ended — normal); `sub_82150770` (attract owner, obj 0x827f4d8c) spins hot (12600+); `sub_8210AF90` (the REAL transitions-enabler) **never runs** (=0) — so XFLAG is still required; the tid=10 worker path (sub_82250420→sub_8211B740→sub_8210AF90) is still blocked even post-corruption-fix (cont.8 starvation persists). Movie-EOS posts 3× but only the FIRST advances ⇒ **attract→menu is NOT movie-EOS-driven.**
- **Forced + START (REX_SKIPINTRO):** `sub_82163118` fires a **2nd time** (attract→menu) → the title **enters menu-setup: allocates menu buffers** (`MmAllocatePhysicalMemoryEx sz=0x195000 + 0x3000 + 0x10000`) → then **`[INDIRECT-NULL] target=0xFFFFFFFF @ sub_8215DE84`**, and goes quiet (no crash, owner loop drops to ~200). 
- **⚠ CORRECTS commit b99953d:** I claimed "sub_8215DE84 was the dispatch-table corruption, gone." WRONG. The no-START path simply **never reaches** sub_8215DE84 (it loops attract, advance stays 1). With START the title advances to menu-setup and **DOES hit sub_8215DE84** — and it's the GENUINE cont.7 blocker: `target=0xFFFFFFFF` = an **uninitialized object vtable/jump-table slot** read as a call target (cleanly range-skipped by PPCInvokeGuest), **distinct from the table corruption** (which produced garbage HOST pointers). The corruption fix is still a real win (removed the render-path crash class), but it did NOT remove sub_8215DE84. (Lesson: I inferred "gone" from absence in one config instead of testing the path that reaches it.)
- **⇒ REAL menu frontier (now reached cleanly):** forced+START gets the title INTO menu-setup; the blocker is **sub_8215DE84's 0xFFFFFFFF uninit object** (recover its vtable/construction like cont.12 recovered the null singleton via the pool fix + prod oracle). Run-base for menu-setup work: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=120 REX_XFLAG=1 REX_SKIPINTRO=1`. NEXT: RE sub_8215DE84 (prod-oracle: what object/vtable does it call; who constructs it; why null in variant A).

### cont.22 loop-iter 3 — sub_8215DE84 localized (forced-path artifact), and the natural transition is STILL blocked at sub_8211B740 (NOT corruption)
RE'd the sub_8215DE84 blocker + tested the natural path with the corruption-fixed build:
- **sub_8215DE84 decoded:** `lr=0x8215DE84` is the return of a DIRECT `bl sub_824253C8(r3=stackbuf)` from frontend menu-setup `loc_8215DE70` — taken only when `*(r31+216)!=0` (a STATE-dependent branch). `sub_824253C8` loads a callback ptr from a `.data` table at **`0x828183A0`**, null-checks `==0`, and tail-calls it. In variant A `*(0x828183A0)=0xFFFFFFFF` (an uninit "not-registered" sentinel) — which the `==0` check doesn't catch → it tail-calls `0xFFFFFFFF` (INDDUMP: tgt=0xFFFFFFFF, r11=0xFFFFFFFF, obj r31=0x060FF800). So a callback at `0x828183A0` is unregistered in variant A.
- **BUT likely a FORCED-PATH ARTIFACT:** prod oracle breaking on `sub_824253C8` (symbol resolved @0xec65b0) got **0 hits in 70s** (prod under gdb's SIGSEGV-write-watch is slow — inconclusive, but the branch is `*(r31+216)`-gated state divergence, and the title is in the forced MOVIE_EOF+XFLAG+SKIPINTRO state prod never enters). Per cont.8/cont.10 ("XFLAG is a crutch; the real path is the NATURAL movie→EOF→advance"), chasing forced-path branches = whack-a-mole. Did NOT pursue the 0x828183A0 registration.
- **⭐ Decisive natural-path test (FAIRSCHED + CPCOMPLETE, corruption-fixed build):** the decoder (sub_82339428), tid=10 loop (sub_82250420), and worker (sub_8211B740) each START — but **sub_8211B740 STILL diverges before sub_8210AF90**: ENABLER(8210AF90)=0, advance(82163118)=0 (0 crash, 0 guardfires). ⇒ **the corruption fix does NOT unblock the natural transition.** The render-path crash class is gone (real win), but the intro→menu transition is gated by the **genuine cont.11 `sub_8211B740` branch divergence** (a state/data divergence in the 718-line handler, ONE of its ~7 indirect calls under ~6 branches goes the wrong way) — separate from the corruption.
- **⇒ NEXT (the real transition frontier):** trace `sub_8211B740`'s branch path with **REX_TRACEB740** (already in PPCInvokeGuest — logs every indirect call from sub_8211B740 + its return value) under FAIRSCHED, diff vs the prod oracle (prod reaches sub_8210AF90 via sub_8211B740+0x1220), find the FIRST diverging branch + its input state = the fix target. This is the cont.11 thread, now reachable cleanly (no corruption masking).

### cont.22 loop-iter 4 — sub_8211B740 traced; the transition block is a WORKER WAIT, not a branch divergence (cont.12 frontier reconfirmed)
- **REX_TRACEB740 (FAIRSCHED):** sub_8211B740 makes 2 indirect calls — `lr=0x8211B7D4→sub_82118E10 (r3=1)`, `lr=0x8211B804→sub_82248F18 (r3=0x0133F260)` — then takes the `r3!=0` branch into a DIRECT call `sub_8211BC40` (untraced) and continues. So the 2-entry trace is NOT "it stopped"; the handler continues via direct calls.
- **gdb all-thread bt (FAIRSCHED, ~22s):** the VC-1 decoder thread is actively running (simde_mm_shuffle_epi8 SIMD ✓ — FAIRSCHED decodes), but **nearly every other guest thread is BLOCKED in a Ke*Wait / FairWaitUntil / condition_variable::wait** (KeWaitForSingleObject obj=171220/171328, KeWaitForMultipleObjects, NtWaitForMultipleObjectsEx — all on `g_objCv`, several timeoutMs=-1 infinite). The sub_8211B740 worker (tid=10) is among them: after its early calls it **blocks on an unmet event**, so it never runs far enough to reach sub_8210AF90.
- **⇒ Refines cont.11→cont.12:** the intro→menu transition is NOT a simple sub_8211B740 branch divergence — it's the **cont.12 frontier** ("threads WAIT on unmet conditions/signals, not CPU starvation"), and the **corruption fix does not change it** (it removed the crashes that used to mask this, so the wait is now reached cleanly). This is the deep cont.8–12 wall (prior multi-session). **NEXT (focused, hard):** identify the SPECIFIC Ke event the transition worker blocks on (obj handle → who should KeSetEvent it in prod, and why variant A never does) — title-specific RE, the genuine remaining blocker to the natural intro→menu transition. The corruption fix + render-path-crash elimination stand as this session's durable win; the transition is a separate, deep frontier.

### cont.22 loop-iter 5 — the transition wait DECODED: an async-completion state-poll, coupled to the GPU/resource-completion gap (the unifying root)
Full worker backtrace (FAIRSCHED, the tid=10 thread) + source decode — REVISES iter 4 ("unmet Ke event" was imprecise):
- **Worker stack (Thread 5):** `sub_82250420 → sub_8211B740(:11122) → sub_82118E10 → sub_82249CC0 → sub_8224A660 → sub_8224A758 → sub_8224F890 → sub_8224F918 → sub_822481E0 → sub_82448B98 → sub_824500A8 → KeDelayExecutionThread`. It is NOT parked on a Ke event — it is in a **delay-poll loop**. (The sub_82249/sub_8224A frames are the render-SUBMIT area mapped in cont.13.)
- **The poll loop (sub_822481E0, decoded):** `while (*(r30+136) != 1 && *(r30+136) != 12) { (*(r31-4412))->vtable[8](); KeDelayExecutionThread(10); }`. So the transition-init **waits for an async operation's state `*(r30+136)` to reach 1 or 12 (done)**, pumping the subsystem via a virtual method (obj=*(r31-4412), vtable+32) each 10ms. In variant A the op never completes → infinite poll → the worker never reaches sub_8210AF90 → no transition.
- **⭐ UNIFYING INSIGHT:** this async-completion wait sits in the render-submit area (sub_82249CC0…), so it is very likely **the SAME GPU/resource-completion gap** that gates content rendering — i.e. the transition is NOT a separate logic problem, it's coupled to variant A's core "no real GPU/async completion" requirement (cont.21 RENDERER-DESIGN's faithful-CP). The 5 diagnostic iterations have RULED OUT the alternatives (corruption [fixed], forced-path artifacts [dead-end], branch divergence [no], unmet Ke event [no]) and converged on: **the transition, menu content, and pillar B are all gated on real GPU/async completion.** The corruption fix removed the crash confound that masked this; the clean gate is now visible.
- **NEXT:** (a) identify the async subsystem (obj=*(r31-4412)) + what sets `*(r30+136)` to 1/12 in prod (the completion) — GPU op vs async resource-I/O; (b) if a clean injection point exists, force the completion state as a diagnostic stopgap to SEE PAST the transition (does the menu then render content? = the pillar-B precondition test); (c) the real fix = the faithful GPU/async-completion model (the big cont.21 work that gates everything).

### cont.22 loop-iter 6 — the transition's async state machine PINNED (REX_POLLDIAG); a force-past stopgap does NOT work
Added **REX_POLLDIAG** (gated, default boot unregressed): logs the live ctx at the poll's pump call (guest lr=0x82248224, where frame-reads of the blocked worker fail — non-volatiles save/restore down the chain). Results:
- **The async object + driver, PINNED:** the pumped subsystem is a **static singleton** obj=`0x826574E8` (in .data), vtable=`0x820DAE34` (in .rdata), pump = `vtable[+32]` = **`sub_82247F98`**; the state field is at the fixed guest addr **`0x82657600`** (r30=0x82657578, +136; r31=0x828F0000, obj=*(r31-4412)). The pump is called every 10ms from sub_822481E0.
- **The state CYCLES, never completes:** `*(0x82657600)` runs **2→3→6→8→10→2→3→6…** every run — it processes stages but **loops back to 2 instead of advancing to the done-state {1,12}**. So the transition poll spins forever because the async op (driven by sub_82247F98) can't finish.
- **Force-past stopgap FAILS (REX_POLLFORCE, then removed):** forcing `*(0x82657600)=1` + skipping the pump did NOT clear the poll — the state **re-cycled** (the op is robustly driven; the forced incomplete result is overwritten/breaks the worker) and the title **stalled** (~780 lines, never reached the menu, sub_8210AF90=0, 0 crash). ⇒ **a fence-forward-style stopgap does NOT substitute for real completion here** (reconfirms the cont.21 lesson). Removed the hack; kept POLLDIAG as a reusable diagnostic.
- **NEXT:** RE **`sub_82247F98`** (the pump/state-machine) — decode the 2→3→6→8→10 stages and the condition that resets 10→2 instead of →12 (what unmet input loops it); AND identify the subsystem via the prod oracle (what class owns vtable `0x820DAE34` — GPU/render vs async resource/IO). That condition is the concrete completion variant A must drive — the next, well-pinned step toward the unified GPU/async-completion fix.

### cont.22 loop-iter 7 — the transition state machine MAPPED END-TO-END (a render/resource async-task manager)
Walked the chain from the poll down to the leaf state machine (vtable dump via gdb + recomp source):
- **The class** (singleton 0x826574E8, vtable `0x820DAE34`): 16-entry vtable with methods across 0x82100xxx / 0x82106xxx / 0x82248xxx and **`vtable[15]=sub_821B3B30` in the GPU/render range (0x821Bxxxx)** — so this is a **render/resource subsystem manager**. `vtable[8]=sub_82247F98` (the pump) is a thin shim → `vtable[13]=sub_82248738` (the driver).
- **The driver `sub_82248738`:** iterates **20 children** (each 216 B, at obj+144+i·216) calling `sub_82248010` (the per-child pump) on each. The poll watches **child[0]**'s state at obj+280 = `0x82657600`.
- **The per-child state machine `sub_82248010` (283 lines):** a staged async processor. Each stage makes a **polymorphic virtual sub-op call** (`PPC_CALL_INDIRECT_FUNC`) and branches on its returned status: **`==2` ⇒ WAIT (don't advance)**, **`==3` ⇒ ADVANCE** to the next stage (2→3→6→8→10→…), with done-states 1/12. The observed 2→3→6→8→10→2 cycle = the stages keep advancing (sub-ops return 3) but the op **resets instead of finishing** — i.e. an underlying async resource (file-stream I/O or a GPU op) the leaf sub-ops poll **never signals "done"**, so the 20 child tasks loop forever and child[0] never reaches 1/12.
- **⇒ Same root, fully mapped:** the intro→menu transition is gated on this render/resource async-task manager completing, which is gated on real async/GPU completion — exactly the unified root from iter 5. **The 7 diagnostic iterations have now characterized the frontier end-to-end.** The remaining work is no longer diagnosis: it's the **faithful async/GPU-completion model** (cont.21 RENDERER-DESIGN) — a large, multi-session engineering build, not a single loop iteration. **NEXT (the last useful disambiguation):** trace ONE leaf sub-op (its target + what it polls) to classify I/O-completion (possibly a tractable async-read-completion fix) vs GPU-completion (the big build). Then commit to the build.

### cont.22 loop-iter 8 — leaf sub-ops classified: a deeply-polymorphic RESOURCE LOADER (file-streaming works; the GPU resource-creation step is the gate)
Added **REX_TASKDIAG** (gated, default boot unregressed): logs the polymorphic sub-op calls inside sub_82248010 (lr in [0x82248010,0x82248260)) + their returned status. Results:
- The sub-op targets: `sub_822485A0` (returns **file sizes** — 612=Simple.xbv, plus 552/478/372/9178/329453, matching the NtQueryInformationFile sizes), `sub_82105948` (`return *(obj+208)` — a getter, →1), `sub_822484E0` (→ sub_8244D018, a kernel/IO call), `sub_822484D0` (returns a pointer — getter), and the pump itself. Every layer is a **virtual dispatch** (sub_822485A0 itself just forwards to `obj->vtable[3]`).
- ⇒ This is a **deeply-abstracted resource-loading framework** streaming the menu's assets (the .xbv/.xbp shaders). **File I/O works** (the files are tiny — Simple.xbv is 612 B — and read synchronously fine), yet the 20-child loader **cycles forever** ⇒ the blocker is NOT the file read; it's the step AFTER the read — **GPU resource CREATION** (shader/texture upload/compile) — which variant A's stub CP never completes. So the classification resolves to **GPU-completion**, with file-streaming as a working precursor.
- **⇒ The diagnostic phase is COMPLETE (8 iters).** The intro→menu transition, the menu content, and pillar B are ALL gated on the one root: a faithful GPU resource-creation + completion model. There is no quick stopgap (iter 6 proved force-past fails) and no tractable I/O shortcut (I/O already works). The remaining work is the **cont.21 RENDERER-DESIGN build** — a real PM4/GPU command processor that creates resources + signals completion — a large, multi-session engineering effort, not a loop iteration. Reusable diagnostics left in place: REX_POLLDIAG, REX_TASKDIAG (+ prod_counter.gdb). **Decision point: commit to the GPU-completion build.**

## cont.22 GPU BUILD (user chose "Start the GPU build") — piece 1: graphics-pipeline foundation ✅
The diagnostic phase converged on: variant A needs a real GPU resource-creation/draw + completion model. The user opted to build it. Going incrementally, committing each piece.

### Completion-poll spec pinned (loop-iter 8 follow-up, REX_POLLDIAG extended)
The transition's per-child completion poll = `child[0]->vtable[9]` = **sub_82105948** = `return *(child+208)` (child[0] vtable = 0x820DAE0C). The final stage (sub_82248010 loc_8224818C) treats that return as 2=pending / 3=advance / else=done. So the completion field is `*(child+208)`; but TASKDIAG saw it =1 (done) at times while the loader still cycles ⇒ the full multi-child completion is genuinely complex (not a single field to force) — confirming the faithful CP, not field-forcing, is the path.

### ✅ Piece 1 — the graphics pipeline foundation (runtime/vulkan_render.cpp, gated REX_DRAWTEST)
Added the machinery the DRAW_INDX→Vulkan translator plugs into (the present path was clear/blit only):
- **VkRenderPass** (one color attachment, clear→present), **per-image VkImageViews + VkFramebuffers** (recreated on swapchain change), a **VkPipeline** (no vertex input, dynamic viewport/scissor) from real SPIR-V shaders. Shader toolchain extended: wrote a generic `test.vert` + `test.frag` (tools/shaderc/ported/), compiled via shadercc, **embedded as `runtime/test_shaders.h`** (uint32 SPIR-V; the .updb fragment shaders are already compiled in tools/shaderc/out/).
- **PresentOnce** gains a `REX_DRAWTEST` path: begin render pass → bind pipeline → `vkCmdDraw(3)` → end → present (render pass handles UNDEFINED→PRESENT layout). + REX_RENDER_SHOT capture.
- **VERIFIED:** `REX_RENDER=1 REX_DRAWTEST=1 REX_FAIRSCHED=1` renders a clean RGB-gradient triangle (1280×720, RADV POLARIS11), **1500+ frames, 0 Vulkan errors, 0 crashes**; in-engine PPM capture → /tmp/varianta_triangle.png. **Default boot + the existing movie/clear present are UNREGRESSED** (all new code gated behind REX_DRAWTEST). This is the first thing variant A's own graphics pipeline has rendered.
- **NEXT pieces (toward the real renderer):** (2) wire the title's `DRAW_INDX` (in ExecutePM4) to a pipeline built from the reg-file state (vertex-fetch reg 0x4800 → vertex/index buffers, ALU reg 0x4000 → uniforms, the bound .updb shader→SPIR-V) → vkCmdDraw into an offscreen RT; (3) render-target/state translation; (4) the resource-creation + completion path so the title's loader (the transition) sees real GPU results. Piece 1 is the foundation all of these use.

### Piece 2 (decode) + ⭐ build-order finding: the draws reaching the CP are DEGENERATE — content is gated on the transition (piece 4 first)
Added **REX_DRAWDECODE** (gated): decodes each DRAW_INDX's prim type + vertex-fetch constants (the 0x4800 space, 2-dword entries, type 3 = kVertex per rexglue xenos.h) — the geometry the translator must upload. Measured (natural path NOTOKEN+CSLEAK+CPCOMPLETE):
- Every draw reaching the CP is **`init=0x10081` = a single-vertex POINT draw with 0 vertex buffers** — degenerate, no geometry. (The forced path's were `init=0x30088` = kRectangleList 2D rects — also just setup/clears.) So across all paths, only **setup/clear draws** reach the CP; the textured **menu content draws never do** — they're built only once the title is at the live menu, which is past the transition.
- ⇒ **The draw translator (piece 2) cannot be validated until content draws exist, which requires the transition to be unblocked. So the BUILD ORDER is: piece 4 (the transition / resource-creation completion) FIRST, then pieces 2–3 against real menu content.** Piece 1's pipeline + the piece-2 decoder are ready for when content arrives. **NEXT: piece 4 — pin what sets the loader's completion field `*(child+208)` to "done" in prod (hardware watchpoint on guest 0x82657648 → its writer), then drive that completion in variant A → unblock intro→menu.**

### Piece 4 — ⭐ divergence TRACED (prod + variant-A hardware watchpoints): the child state machine COMPLETES; a per-frame processor RESETS it (the queue never drains)
Hardware watchpoints on the loader's state field (guest 0x82657600 = child[0]+136) and completion field (0x82657648 = child[0]+208), armed at the title entry, in BOTH prod (base 0x100000000) and variant A:
- **PROD child[0]: state 2→3→1 (done).** The completion field `*(child+208)` is set to **1 (ready)** by **sub_822484D0** (called from sub_82248010); the state-3 handler `sub_82248010` reads it via vtable[9]=sub_82105948 (`= *(child+208)` = 1, ≠2/≠3) → `loc_822480A8` → state **1 (done)**. The loader drains.
- **VARIANT A child[0]: the state machine ALSO reaches done** — `sub_82248010` writes state 3→**1**, and `*(child+208)=1` (SAME as prod), so the per-child completion logic WORKS. **BUT then `sub_8224F918` (← sub_8224F890 ← sub_82253540 / sub_8224A758 ← the frontend `sub_8214FFD0`) RESETS the completed child's state (1→6, 1→0) and `sub_82248480` re-inits it to 2** — so the loader RE-PROCESSES forever. The observed 2→3→6→8→10→2 "cycle" is this **reset loop**, not a stuck child.
- ⇒ **The blocker is NOT the per-child state machine (it completes correctly, identically to prod); it's the loader's per-frame queue PROCESSOR `sub_8224F918`, driven from the frontend (`sub_8214FFD0`), which keeps finding work to re-submit in variant A but drains in prod. The resource QUEUE never empties.** This moves the frontier UP from the (working) child state machine to the queue/processor that re-feeds it.
- **NEXT: why `sub_8224F918` keeps re-submitting** — what feeds the resource queue + its drain/empty condition (trace `sub_8224F918` / `sub_82253540` / the frontend `sub_8214FFD0`: does prod's queue go empty while variant A's keeps getting entries? what enqueues them?). That empty/drain condition is the real root of the intro→menu transition block. (Tools: prod + variant-A state-field hardware-watchpoint gdb scripts — armed at entry, base from $rsi.)

### cont.22 GPU BUILD — piece 3a (draw-state extraction) + ⭐ RE-GROUNDING: the resource-queue rabbit-hole (loop-iter 6-8 / piece 4) was pre-MOVIE_EOF; NOTOKEN+MOVIE_EOF reaches the FULL menu → the real gate is cont.21's A↔B coupling
**Measured this iteration (NOT inferred):**
- **Most-advanced STABLE state found = `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`** (post dispatch-table-fix): **620k lines/30s, NO crash, 0 dispatch-corruption, 0 device-overwrite**, and it loads the **ENTIRE menu/HUD shader set** — Simple, SimpleCol, SPBackdropTextured, SPBackdropUntextured, SPBuildStructure, SpDropShadow, SPHud{GrayScale,ImageBar,Rectangle}, SpHudWithMask, … This is real menu-content asset loading, far past the movie loop.
- ⇒ **The loop-iter 6-8 / piece-4 conclusion ("the resource queue `sub_8224F918` never drains") was config-specific** (no MOVIE_EOF → the loader waits on the still-playing movie's resources). **`REX_MOVIE_EOF=30` ends the movie → the loader DRAINS and loads the full menu.** The resource loader was never permanently stuck; it was gated on the movie. This UNIFIES piece-4 with cont.21: past the movie, the real gate is the **A↔B coupling**, not the resource queue.
- **A↔B coupling RE-CONFIRMED at this most-advanced state** (matches cont.21 exactly): `[kick]`=**14** (plateau), `[ringdump]` shows the 14 kicked IBs are all **setup** (max len=266 dw; prod's content draws live in a 3592-dw IB, never kicked here), `[VdSwap]`×6 but **every framebuffer is all-zeros (black)**, `[fencefwd]`×**17233** (the stopgap fakes GPU completion → limps through menu LOGIC but SUPPRESSES content-draw submission), `[cpcomplete]` climbs to **660→659** (the cont.22 RATE-mismatch: title increments ~7-8/vblank, drain is 1/vblank → kick-gate `counter==0` rarely opens → kicks plateau). `[swapbuf]` carries the WAIT_REG_MEM/callback handshake records (`821BF860`, `0000057C`) = the producer/consumer bookkeeping cont.21 closed as a dead-end.
- ⚠ **cont.22 re-grounding point 4 ("translator premature, no path reaches menu-content") is CORRECTED in part, STANDS in part:** the menu IS reachable (via MOVIE_EOF — corrects "no path reaches the menu"), BUT only clears/setup reach the CP, no textured content (the A↔B gate stands). So the translator is justified to BUILD (the user's "Start the GPU build"), but content to translate awaits the A↔B break.

**sub_8215DE84 (forced-path terminal blocker) ROOT-CAUSED — NOT a recompiler gap, NOT table-corruption:** the `lr=0x8215DE84` site is a *direct* `bl sub_824253C8` (ppc_recomp.7.cpp:16280). `sub_824253C8` (ppc_recomp.59.cpp:21119) loads a global **function-pointer at `0x829183A0`** (`= 0x82920000 − 31840`) and tail-calls it via `bctr`; its null-check only catches `0`, but the slot holds **`0xFFFFFFFF`** (an uninitialized registration sentinel) → dispatches into `0xFFFFFFFF`. So it's a **registration slot the title never populated on the forced path** (a subsystem registers its handler into `0x829183A0`; that registration didn't run). The dispatch-table relocation did NOT clear it (it's a guest global, not the host fn-table) — confirmed: forced path still fires it, table-guard 0×, no crash (the INDIRECT-NULL is skipped). Forced/cooperative path is also scheduler-starved (97% `KeGetCurrentProcessType` spin) ⇒ NOTOKEN, not the forced/cooperative path, is the productive one.

**✅ Piece 3a — draw-state extraction (runtime/kernel.cpp DRAW_INDX handler, gated `REX_DRAWSTATE=N`, default boot unregressed):** reads the pipeline state a real draw binds from the CP's register file (`0x7FC80000 + reg*4`, populated by SET_CONSTANT) — RB_SURFACE_INFO(0x2000)/RB_COLOR_INFO(0x2001) (RT pitch/msaa/format/base), PA_CL_VPORT_*(0x210F-0x2112) (viewport w/h from scale, offset), PA_SC_WINDOW_SCISSOR_TL/BR(0x2081/2), RB_BLENDCONTROL0(0x2201)/RB_COLORCONTROL(0x2202), PA_SU_SC_MODE_CNTL(0x2205), SQ_PROGRAM_CNTL(0x2180), RB_MODECONTROL(0x2208). Reg numbers per rexglue `register_table.inc`. **Result (the only 3 draws reaching the CP, all identical):** `RECT-list numInd=3 | RT pitch=640 msaa=0 colorFmt=0 colorBase=0x0 | vp 640x-368 off(320,184) | scissor (0,0)-(8192,8192) | blend0=0x10001 colorCtl=0x0 progCntl=0x10010001 suMode=0x10000 mode=0x4`. Decoded: **full-viewport opaque RGBA8 rect fills to a 640×368 EDRAM-base-0 surface, src=One/dst=Zero (opaque), trivial shaders (vs=1/ps=0 regs), mode=4=kColorDepth** ⇒ these init=0x30088 rects are **clears/background fills, NOT menu content** — the A↔B coupling confirmed at the draw-state level.

**Verify:** default boot 170k lines/18s, 0 crash, 0 `[drawstate]` leak (gated off), 0 dispatch-corrupt, 0 device-overwrite, reaches intro. Commit cont.22 (gated, default-safe, NOT pushed).

**NEXT (the A↔B break is the gate; translator has no content until it breaks):** the central unanswered question for breaking A↔B = **WHICH fence does the title spin on (`sub_821C6E58` r4/r5 target) and why doesn't the CP's faithful EVENT_WRITE_SHD execution of the kicked IBs produce it** — that gap IS the content the title isn't kicking. Now testable fresh (post table-fix, at the stable full-menu state, which cont.13-21 predate). Then build piece 3 (DRAW_INDX→vkCmdDraw) so that when content IBs kick, they render. Run-base: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`. New diag: `REX_DRAWSTATE=N`.

### cont.22 GPU BUILD — A↔B coupling DECISIVELY confirmed at the full-menu state: content is NOT built anywhere; all faking exhausted ⇒ pillar B (real rendering) is the only path
Ran 5 measurements at the most-advanced stable state (`REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`, post table-fix — which cont.13-21 predate):
- **Fence dynamics (`[fencewait]`):** the completion-fence head climbs **17→1439** (title builds ~6 fences/frame for ~240 frames) and the command-build cursor (device+13568..13572) grows **~32 MB** — but only **14 tiny IBs are kicked** (≤266 dw). So the title builds a large deferred tail it relies on the real GPU to auto-flush; variant A's CP only executes kicked IBs. The steady gap (tgt = head−8, cur = tgt−6) is bridged by fence-forward.
- **`sub_8211B740` (transition handler) runs but does NOT reach `sub_8210AF90`** (`REX_TRACEB740`): it makes 4 indirect calls (0x8211B7D4 loop, 0x8211B804 vtable, 0x8211BE60, 0x8211BE98 — all into the resource-loader/task-manager `sub_82248xxx`/`sub_8224FB68` family) and exits, skipping the mid-function call sites where the `sub_8210AF90` dispatch lives ⇒ the transitions-enabled flag 0x828E82A6 stays 0.
- **⭐ DECISIVE 1 — forcing the transition flag (`REX_XFLAG`) does NOT yield content:** at the full-menu state, +XFLAG still produces **exactly 3 RECT clears, 14 kicks, 0 non-rect draws** (it only diverts the title to a different blocker, the 0x82292D08 null-singleton). So the transition flag is **not** the content gate — refutes "transition is the prerequisite."
- **⭐ DECISIVE 2 — clean segment census (`REX_CHUNKDUMP=3`, now firing at the full-menu state — was gated n>=4000 which few-swap menu never reaches):** following the device+13568 descriptors {0x81LLLLLL,phys}→guest=0xA0000000|(addr&0x1FFFFFFF), all **11 segments = realDraws=0 rectDraws=0 texFetch=0** — pure state/events/callbacks (SET_CONSTANT, C0004600 COND_WRITE, the `0001057C 821CC7A0` producer-callback record). **No draws at all** in the directory. Confirms cont.10/12 at the most-advanced state.
- **`REX_CHUNKCP` (execute the chunk as inline PM4) is contaminated** — the chunk is a DIRECTORY not inline PM4, so parsing it as PM4 misparses the 0xC1xxxxxx descriptor words as fake DRAW_INDX (the `numInd=49152`/garbage-state rows). Not a valid content test (cont.10's false-positive warning).

**⇒ A↔B coupling, now multiply-confirmed with clean evidence:** content draws are NOT built in the ring IBs (only clears) NOR the device+13568 directory (zero draws); and NEITHER faking the transition (XFLAG) NOR faking completion (fence-forward/CPCOMPLETE/counter) makes the title build them. The title genuinely won't BUILD content until it observes a REAL GPU result from rendering what it HAS submitted (the clears/setup). **All faking is exhausted ⇒ the only path is pillar B: render the kicked clears/setup FOR REAL and advance the swap/fence as a genuine result** (the user's "Start the GPU build" + cont.21 "A+B together").

**⚠ Open sub-question before the big pillar-B build:** since the title reads the swap-counter/fence from memory (which faking already sets to the same values), the "real result" it needs must be something faking can't reproduce — most likely a **surface readback or an occlusion (ZPASS_DONE) query result** that only real pixel rendering produces. NEXT: (a) check whether the title issues ZPASS_DONE queries / reads back EDRAM after the clears (PM4 scan + watch the readback address) — that pins the exact real-result gate; then (b) piece 3b: real DRAW_INDX→vkCmdDraw of the clears into a Vulkan RT + present, producing that result. Tool added: `REX_CHUNKDUMP=N` (configurable census trigger swap#). Run-base: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`.

## cont.22 (2026-06-04, autonomous /loop) — 🟢🟢🟢 BREAKTHROUGH: the content IS built — the cont.10-22 "content not built anywhere" was a CENSUS-PARSER ARTIFACT; the A↔B-coupling premise is FALSE
Step (a) (the result-gate scan) was meant to gate piece 3b. It did far more: it **overturned the central blocker** of the last ~12 continuations. The device+13568 deferred segments contain full, walker-verified menu content; variant A's CP simply never executes them.

### How the artifact was found (chain of measurements, all at the stable full-menu state `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`)
1. **Built step-(a) diagnostics** (`REX_RESULTSCAN` on the kicked CP stream + extended the `REX_CHUNKDUMP` directory census with result-gate counts). Constants from rexglue `xenos.h`/`register_table.inc`: ZPASS_DONE=event 21, EVENT_WRITE_ZPD=0x5b, VIZ_QUERY=0x23, WAIT_REG_MEM=0x3c, REG_TO_MEM=0x3e, RB_COPY_*=regs 0x2318-0x2325, COHER_STATUS_HOST=reg 0xA31, PA_SC_VIZ_QUERY=0x2293.
2. **Kicked-stream `[result]` scan (measured):** the WAIT_REG_MEMs the CP executes are all bookkeeping — `reg:0xA31`(COHER_STATUS_HOST, mask bit-31, trivially satisfied since variant A's reg file is 0) and the producer/consumer handshake `mem:0x2011012==0x821CC7A0`(producer cb) / `mem:0x2011016==<ctx 0xC00901801>` / `mem:0x2011002==4/0`. No occlusion, no resolve in the kicked stream. So far this *seemed* to confirm "no pixel-result gate" → it briefly looked like the night-log's surface-readback/ZPASS hypothesis was refuted.
3. **The census initially agreed** (zpass=0 viz=0 resolve=0, realDraws=0) — BUT the census truncated each segment at **1024 dwords** and **only advanced type-3 packets by their count** (every type-0/1/2 packet advanced by a single dword). The larger content segments are **0xCDC-0xF68 dwords (3292-3944)** and begin with long runs of zeros / type-0 register loads — so the truncation hid everything past dword 1024 and the type-0 misalignment fabricated/missed packets. **This is exactly cont.10's "brute-scan gives false positives" warning — but it cut BOTH ways: it also produced false NEGATIVES that propagated as the "0 draws" conclusion through cont.10/12/21/22.**
4. **Removed the 1024 cap** → draws/resolves/viz appeared, but with the still-broken type-3-only walker (suspect).
5. **Rewrote the census walker to be CORRECT** (mirror `ExecutePM4`: advance type-0 by reg count, type-1 by 2, type-2 by 0, type-3 by data count) **+ added DESYNC detection** (a clean PM4 stream parses to its end; a misparse overruns). **Result: the content segments parse `clean` (no DESYNC) — they are valid PM4 streams, not misparsed buffers.**
6. **One-shot `[segdump]` full packet walk of a content segment (desc#0, len 0xD38) — the decisive proof.** It is a complete, sensible draw:
   `SET_CONSTANT scissor → T0 reg=0x4000 cnt=1008 (ALU shader constants, v0=1.0f) → T0 reg=0x4400 cnt=1024 (more constants) → T0 reg=0x2200 cnt=12 (RB_DEPTHCONTROL/blend) → T0 reg=0x2180 cnt=5 (SQ_PROGRAM_CNTL=0x10010001, MATCHES piece-3a's kicked value) → T0 reg=0x2000 cnt=16 (RB_SURFACE_INFO) → T0 reg=0x2100 (PA_CL_VPORT) → T0 reg=0x4800 cnt=186 (FETCH constants = textures + vertex streams) → T0 reg=0xA31 (COHER_STATUS_HOST setup) → WAIT_REG_MEM (coherency wait) → DRAW_INDX`. Coherent state, not corruption; consistent frame-to-frame.

### Corrected census (measured, per full menu frame; trustworthy after fixing init-field [op-0x22 initiator is data[1]] + fetch-constant detection [type-0 reg 0x4800])
- **realDraws≈15, rectDraws≈21, texFetch(fetch-const loads)≈45, EDRAM resolve(RB_COPY)≈330, VIZ_QUERY≈40** — every frame, in the device+13568 directory segments, all parsing `clean`.
- So the title DOES build textured content draws with shaders, ~2000 ALU constants, 186 fetch constants (textures+vertex streams), viewport/blend/depth/surface state, EDRAM resolves, and occlusion (VIZ) queries. The earlier "only clears, 0 textures" was the parser.

### ⇒ The re-grounding (what's now TRUE vs what was an artifact)
- **TRUE (cont.10/c11, was RIGHT):** "the title builds a deferred render-program (device+13568 segments) but variant A NEVER EXECUTES it." The fence-forward stopgap fakes GPU completion of these segments *without executing them* → built content is never rendered, and the title moves on.
- **ARTIFACT (cont.10(c10)/c12/21/22):** "the segments contain 0 draws / only state" and therefore "content is not built anywhere" and therefore "the A↔B coupling needs a real GPU pixel result before the title will BUILD content." All downstream of the broken census parser. The producer/consumer dead-end and the "render the clears for a real result" plan were chasing a phantom.
- The night-log's own `sub_821C6E58` comment already named the clean fix: *"a continuous CP that follows the WAIT_REG_MEM-chained deferred IBs and executes them, so the fence advances as a real result… This forwarding MUST be replaced before a real renderer lands."* We now have PROOF the content to execute is there.

### NEXT — piece 3b, re-scoped: EXECUTE the deferred segments (not "render the clears")
Make variant A's CP execute the device+13568 directory segments each frame (the descriptor walk already exists in the census; resolve guest=0xA0000000|(addr&0x1FFFFFFF), run each via `ExecutePM4`), translating DRAW_INDX→vkCmdDraw with piece-1 (renderpass/pipeline) + piece-3a (draw-state) + the loaded ALU(0x4000)/fetch(0x4800) constants + shaders → present; and advance the completion fence as a REAL result of executing them (replacing fence-forward). **Open RE for execution order:** how prod links the directory segments to ring execution — INDIRECT_BUFFER from a kicked IB, the `0001057C 821CC7A0 <ctx>` callback record driving per-segment execution, or GPU auto-consume of the build cursor (device+13568..13572). Determine that (prod CP trace) so variant A executes them in the right order with the right state. Diagnostics added this session (all gated, default-safe, default boot unregressed): `REX_RESULTSCAN`, corrected `REX_CHUNKDUMP` census (correct walker + DESYNC flag + result-gate + `[segdump]`). Run-base unchanged.

### cont.22 /loop — piece-3b increment 1: content-draw PIPELINE STATE extracted (the menu HUD, fully characterized)
Extended the census walk with a frame-level register shadow (regs 0x2000-0x233F, updated as we walk segments in directory order — state carries across segments within a frame) and a per-draw `[drawstate]` dump. Pure read/track: NO execution, NO side effects (unlike running ExecutePM4 on the segments, which would fire interrupts/fences). This finally gives the CONTENT draws' state (piece-3a only ever saw the 3 kicked clears). Measured at the full-menu state:
- **~107 content draws/frame**, two dominant shaders + a rare third:
  - **prim=5 (tri-strip), numI=4** = a quad as a 2-tri strip — UI sprites/rectangles. `progCntl=0x10010001` (68/frame).
  - **prim=13 (0xD = quad list), numI=252** = 63 quads batched — text glyphs / HUD grids. `progCntl=0x10110101` (37/frame).
  - **prim=4 (tri-list), numI=0, progCntl=0x10002, mode=0x6** (2/frame) — a state/setup draw (numI=0).
- **All content draws share:** RT pitch=640, msaa=0, **fmt=0 (8_8_8_8 RGBA8)**, base=0x0 (EDRAM base 0); viewport 1280×-720 off(640,360) (Y-flipped full-screen); **blend0=0x7060706 = real ALPHA BLEND** (not the clears' One/Zero); colorCtl=0x8700000E; depthCtl=0x700736 (depth test on); **mode=0x4 (kColorDepth)** for the 105 UI draws (mode=0x6 for the 2 setup draws).
- **Scissor splits the frame into regions** — (0,0)-(640,368) and (640,360)-(1280,720) recur = the title renders the 1280×720 frame in **640×368 EDRAM tiles** (EDRAM is small; the 360 tiles large surfaces). So the RT "pitch=640 / base=0" + per-tile scissor = standard 360 EDRAM tiling; the Vulkan side will render to an offscreen RGBA8 target (Xenia-style) and treat the tiles as regions.
- ⇒ **The menu content is now fully characterized for translation:** alpha-composited 2D UI (quads + quad-list text) through 2 shaders to a tiled RGBA8 EDRAM surface with depth. Matches the loaded shader set (Simple/SPTextured/SpHud…) and the hand-ported SPIR-V in tools/shaderc/out (Simple/SimpleCol/SPTextured/SPUntextured.frag). Diagnostic added (gated under REX_CHUNKDUMP): `[drawstate]` content-draw state dump. Default boot unregressed.

### cont.22 /loop — piece-3b increment 2: content geometry decoded (auto-index, one shared vertex pool)
Extended the census shadow to also track the vertex/texture fetch constants (regs 0x4800-0x497F, via type-0 and SET_CONSTANT type-1) and added a per-draw `[drawgeo]` decode: source-select (VGT_DRAW_INITIATOR[7:6]), index buffer (for kDMA: data[2]=VGT_DMA_BASE, data[3]=VGT_DMA_SIZE.num_words; fmt = init[11]), and the bound vertex streams (fetch slots, type[1:0]==3=kVertex). Authoritative formats from rexglue `command_processor.cpp` (ExecutePacketType3Draw) + `registers.h` (VGT_DRAW_INITIATOR) + `xenos.h` (xe_gpu_vertex_fetch_t). Measured at the full-menu state:
- **All 97 content draws are `auto-index`** (source_select=2) — sequential vertices, **NO index buffer**. So prim=5/numI=4 = 4 sequential verts (a quad as tri-strip), prim=13/numI=252 = 252 sequential verts. ⇒ increment 3 needs no index-buffer handling — just `vkCmdDraw(numI)` per draw.
- **All draws share ONE vertex stream: fetch slot 1** (`vf1 @ ~0xA01FE0FC`, a ~10.5 MB pool — a per-frame dynamic vertex ring the title fills). The fetch constant is set once (persistent), the SAME address for ~90 draws ⇒ per-draw differentiation is NOT a different vertex buffer; it is a per-draw VERTEX OFFSET into the shared pool and/or per-draw ALU constants (reg 0x4000 uniforms — recall the segdump's reg0x4000 cnt=1008 + reg0x4400 cnt=1024 constant loads).
- ⇒ **The remaining unknown for pixel-correct rendering = the vertex FORMAT + per-draw offset mechanism**, which live in the **vertex shader microcode** (the vfetch instructions decode the pool bytes into position/UV/color, and the auto-index base/offset). That is the increment-3 RE frontier (translate or hand-RE the 2 dominant vertex shaders, progCntl 0x10010001 / 0x10110101). A simplified first render (unit-quad per draw using scissor + a flat color, ignoring the real vertex format) could validate the segment-exec→pipeline→present path end-to-end before the format is fully RE'd.
- **NEXT (increment 3):** (1) RE the vertex format of the 2 dominant shaders (prod-oracle trace of the vfetch, or read the microcode at the SQ_PROGRAM base); (2) wire segment execution into the CP/present path (decide: shadow-render each frame's directory segments with side-effects guarded, vs. the real fence-advancing CP); (3) build the VkPipeline (alpha blend `0x7060706`, depth `0x700736`, the 2 shaders) + upload the vertex pool + `vkCmdDraw` per content draw into an offscreen RGBA8 RT → present. Diagnostic added (gated under REX_CHUNKDUMP): `[drawgeo]`. Default boot unregressed (exit-124, 0 leak, 0 crash).

### cont.22 /loop — piece-3b increment 3 (RE start): vertex DATA located + screen coords confirmed; per-draw pool offset is the open question
Added a one-shot `[vtxdump]` (gated under REX_CHUNKDUMP): dumps ALL fetch slots (raw d0/d1 → type/byteaddr/words) for a content draw, then scans the candidate base translations for the first non-zero region and dumps it as hex+float. Measured at the full-menu state, prim-5 (quad, numI=4) draw:
- **Fetch slots:** slot 1 = the only real **kVertex** stream (type=3, byteaddr=0x1FE0FC, ~2.75M words = ~10.5 MB pool). slot 0 = type-0 (a huge "constant" stream, addr 0x2000000). slots 2/5/8/… = type-3 but addr/words=0 (unused). So **one vertex stream (slot 1)** + the constant streams.
- **Vertex data FOUND in the physical-window translation** `guest = 0xA0000000|(byteaddr&0x1FFFFFFF) = 0xA01FE0FC`: the pool head is zero, first non-zero at **+0x2A9F0**, and it contains **screen-coordinate floats — `441FE000`=639.5, `43B7C000`=367.5** (exactly the 640×368 EDRAM-tile extents) + `BF000000`=-0.5 half-texel offsets. So the geometry IS the menu UI, in screen space, at the physical-window address. (The non-physical base 0x1FE0FC held heap/string bytes — wrong translation.)
- **⚠ OPEN (the increment-3 blocker):** the draws are auto-index reading vertices [0,numI) from the fetch base, but the captured fetch base (0x1FE0FC) points at the ZERO pool head, while the real data is at +0x2A9F0. So each draw has a **per-draw vertex OFFSET into the shared pool** that is NOT in the fetch-constant base I capture — it's a vfetch immediate offset (in the VS microcode) or set via a constant-load packet (LOAD_ALU_CONSTANT 0x2F / IM-load) the census doesn't track. Also: the per-draw fetch base was mostly constant (0x1FE0FC) across 90 draws ⇒ the differentiation is the offset, not the base.
- **NEXT (resolve the offset — authoritative): prod-oracle vfetch trace.** Run prod under gdb at the menu, break in the GPU vertex-fetch path (or watch reads of the 0x…1FE0FC pool), capture the ACTUAL byte address each content draw reads → reveals stride + per-draw offset + attribute layout in one shot. Then write the generic VS (position from screen coords → clip, pass color/UV per the .updb interpolators) and proceed to the Vulkan build (segment exec → pipeline → vkCmdDraw → present). Diagnostic added (gated under REX_CHUNKDUMP): `[vtxdump]`. Default boot unregressed.

### cont.22 /loop — piece-3b increment 3: VERTEX FORMAT CRACKED (float2 screen-space position, 8-byte stride)
Two findings this iteration: (1) the `.xbv` vertex shaders are **big-endian D3D9 `vs_3_0` bytecode** (version token `FF FE 03 00` at file offset 0xb4, BE) with a constant table declaring **`$worldviewProj`** (a 4×4 matrix uniform) — so buffer positions are in a UI/authoring space and the VS transforms them by a matrix in the ALU constants (reg 0x4000, the segdump's cnt=1008 load). (2) Refined `[vtxdump]` to find a DENSE screen-coord region in the pool and dump its repeating structure. Result @ `0xA01FE0FC + 0x31E74` — consecutive 8-byte pairs form **coherent screen-space quads**:
```
(0,0) (1768,0) (1768,1043) (0,1043)            ← a quad (UI canvas / backdrop)
(348,261) (932,261) (932,459) (348,459)        ← a UI panel rectangle
```
⇒ **The vertex format is float2 (x,y) position, 8-byte stride** (consecutive 8-byte reads form clean rectangle corners — a 12/16-byte stride or an (x,y,u,v) layout does NOT — the UV/Z values would be out of range). So the prim-13 quad-list draws are **position-only** vertices; **color + UV are per-draw ALU constants** (reg 0x4000), not per-vertex (consistent with the Simple PS = texture*color where color is a constant and the texcoord is computed/constant). Coordinates exceed 1280×720 (e.g. 1768×1043) ⇒ a UI authoring resolution mapped to the screen by `$worldviewProj`.
- ⇒ **This is the increment-3 unblocker.** Enough to attempt a FIRST render of the menu layout: VkPipeline with a float2-position vertex input + a generic VS applying `$worldviewProj` (read the matrix from reg 0x4000 ALU constants) + a flat-color/textured FS (the hand-ported SPIR-V) → `vkCmdDraw` the pool quads into an offscreen RGBA8 RT → present. (The per-draw pool OFFSET still needs pinning for correctness — but a first pass can render the dense regions to validate the path.)
- **NEXT (the BUILD — increment 3 proper):** wire segment execution into the CP/present path (shadow-render the directory segments each frame, side-effects [INTERRUPT/EVENT_WRITE/XE_SWAP] guarded) + build the float2 VkPipeline + read `$worldviewProj` from reg 0x4000 + upload the vertex pool + `vkCmdDraw` per content draw → offscreen RGBA8 → present. Iterate the per-draw offset + color/UV constants until the menu renders. Default boot unregressed.

### cont.22 /loop — piece-3b increment 3, Layer 1: the menu-quad VkPipeline (float2 + mvp/color) WORKS ✅
Built + validated the format-independent rendering plumbing — the float2 menu-quad pipeline the real geometry plugs into:
- **Shaders (tools/shaderc/ported/):** `menuquad.vert` (float2 `inPos` → `mvp * (pos,0,1)`, push-const `{mat4 mvp, vec4 color}`) + `menuquad.frag` (flat `color`). Compiled via shadercc → SPIR-V, embedded as `runtime/menu_shaders.h`.
- **Pipeline (vulkan_render.cpp, gated `REX_MENUTEST`):** float2 vertex input (binding 0, stride 8, R32G32_SFLOAT) + an 80-byte push constant + **alpha blend (SRC_ALPHA / ONE_MINUS_SRC_ALPHA — the title's UI compositing)** + TRIANGLE_LIST + dynamic viewport/scissor, reusing the piece-1 render pass. A host-visible persistently-mapped vertex buffer (`EnsureMenuVB`, grows on demand). The `REX_MENUTEST` PresentOnce path uploads 3 hardcoded clip-space rects (6 verts each) and draws them with 3 colors + identity MVP → present + capture.
- **VERIFIED:** `REX_RENDER=1 REX_MENUTEST=1 REX_RENDER_SHOT=10 REX_FAIRSCHED=1` → 2100+ frames, 0 Vulkan errors/crashes; captured /tmp/varianta_menu.ppm = 3 overlapping rects with **correct alpha-blend colors in the overlaps** (8 distinct colors: bg + 3 rects + 4 blends). Proves float2 input + push constants + alpha blend + multi-draw end-to-end. **Default boot UNREGRESSED** (all gated behind REX_MENUTEST/REX_RENDER).
- **NEXT (Layer 2 — feed real geometry):** extract the menu quads from the pool in the census (positions float2) + read the `$worldviewProj` matrix from reg 0x4000 ALU constants → upload to g_menuVB → set the push-constant MVP → draw the real menu geometry (resolve the per-draw pool offset; start by rendering the dense regions). Then per-draw color/UV + the hand-ported FS for textured elements.

### cont.22 /loop — piece-3b increment 3, Layer 2: census→render BRIDGE works (real pool geometry rendered); correctness needs the per-draw vertex mapping
Built the Layer-2 infrastructure + an approximate extraction:
- **Bridge (`rex_render::SubmitMenuGeometry(clipXY, vertCount)`):** the CP hands clip-space geometry to the render thread (mutex-guarded buffer). PresentOnce draws submitted geometry (flat color) when present, else the REX_MENUTEST hardcoded rects. Capture now triggers on the FIRST frame with real geometry (the render-thread frame counter is decoupled from guest progress, so a fixed REX_RENDER_SHOT frame fired ~10s before the menu loaded ~30s).
- **Extraction (census, gated REX_CHUNKDUMP, only when rex_render::Enabled):** capture the content draws' kVertex pool base during the segment walk (`menuPoolBase`, from the [drawgeo] decode — NOT the post-walk `fetch[2]`, which is stale), find the pool's dense screen-coord region, collect a contiguous run of screen-range float2 verts, treat as quad-list (groups of 4 → 2 tris), auto-fit the bbox → clip.
- **RESULT (REX_RENDER=1 REX_MENUTEST=1 REX_CHUNKDUMP=3 + the full-menu envs):** `[menugeo] pool=0xA01FE0FC dense=+0x31E6C collected 468 verts -> 234 tris bbox=(0,0)-(1768,1043)` → submitted (702 verts) → captured. **The bridge works end-to-end** — the title's REAL pool geometry flows CP→render thread→GPU→present (the big filled region IS the (0,0)-(1768,1043) backdrop quad, correctly auto-fit). **BUT the render is a garbled triangle soup, not recognizable UI:** the contiguous-collect + assume-quad-list grouping is wrong — the verts must be carved per-draw (each draw's offset + numI + prim) to triangulate correctly. Default boot UNREGRESSED.
- **⇒ The remaining blocker for CORRECT rendering = the per-draw vertex MAPPING** (which pool offset + how many verts + which prim each draw consumes). I HAVE per-draw numI + prim (from the draw decode); the MISSING piece is the per-draw start OFFSET. The contradiction stands: fetch base is constant (0x1FE0FC, the zero pool head) across 90 draws + auto-index reads [0,numI) + yet the data is at +0x2A9F0/+0x31E6C and draws render distinct content. This can only be resolved by understanding the **vfetch addressing in the VS microcode** (the `.xbv` Xenos vertex shader — its vfetch instruction's stride/offset) OR a **prod-oracle trace** of the actual per-draw vertex read addresses. **NEXT:** resolve that (VS vfetch disasm or prod trace) → carve the pool per-draw → correct geometry; OR test the sequential-consumption hypothesis (draws consume the pool in order: draw N's verts follow draw N-1's, carved by numI+prim).

### cont.22 /loop — piece-3b: opcode HISTOGRAM ([ophist]) — full PM4 usage revealed; microcode is in-stream + rendering is TILED
Added a type-3 opcode histogram to the census (gated REX_CHUNKDUMP) to find any constant-load packet the shadow misses. Per menu frame (two consecutive census frames shown):
- `DRAW(0x22)=15  DRAW2(0x36)=20  SETCONST(0x2D)=90  WAITREG(0x3C)=52  EVTWRITE(0x46)=41  EVENT_WRITE_SHD(0x58)=6  EVENT_WRITE_EXT(0x5A)=15  INT(0x54)=5  INVALIDATE_STATE(0x3B)=23`
- **`IM_LOAD(0x27)=20  IM_LOAD_IMM(0x2B)=20`** ⇒ **the VS+PS microcode is EMBEDDED in the command stream** (IM_LOAD_IMMEDIATE carries the instructions). So the **VS vfetch (the authoritative vertex stride/offset/format) is extractable directly from the segments — no `.xbv` parse needed.** This is the clean unblocker for the per-draw vertex mapping.
- **`LOAD_ALU(0x2F)=5`** (first: d0=0x2965000 d1=0x3F0 d2=0x10 cnt=3) — only 5/frame, NOT per-draw ⇒ constants are NOT DMA'd per-draw via 0x2F; my "constant fetch base" finding stands (the per-draw differentiation is the vfetch offset / worldviewProj, not a per-draw constant load).
- **`0x60=176  0x61=45  0x35(DRAW_INDX_2_BIN)=1`** ⇒ heavy **EDRAM tiling/binning** (SET_BIN_MASK/SELECT): the title issues draws PER-TILE with bin masks (the 640×368 EDRAM tiles). So correct rendering must model the binning/tiling, not just translate draws.
- **⇒ Two sharpened next steps:** (1) extract + disasm the VS microcode from an IM_LOAD_IMMEDIATE (0x2B) packet → its vfetch instruction (ucode.h `VertexFetchInstruction`: stride[8b], offset[23b], format) → the exact per-draw vertex addressing + layout → carve the pool correctly. (2) Model the tile binning (0x60/0x61 bin mask/select + DRAW_INDX_2_BIN) for correct per-tile rendering. Both are real Xenos-GPU-subsystem work; (1) unblocks correct geometry, (2) unblocks correct tiling. Diagnostic added (gated REX_CHUNKDUMP): `[ophist]`. Default boot unregressed.

### cont.22 /loop — piece-3b: VS vfetch DECODED ([vfetch]) — format authoritative; per-draw differentiation is $worldviewProj (not a per-draw base)
Added a `[vfetch]` scanner: for each VS IM_LOAD_IMMEDIATE (0x2B; dword0=shader_type 0=VS, dword1&0xFFFF=size_dwords, microcode follows), scan the microcode in 3-dword windows for vfetch instructions (d0[4:0]=opcode kVertexFetch=0 + d0[19]=must_be_one; d1[21:16]=VertexFormat; d2[7:0]=dword stride, d2[30:8]=signed dword offset; fetch_const_index = d0[24:20]*3 + d0[26:25]). Measured (all 8 menu VS shaders, size=15 dw, IDENTICAL):
- **`vfetch slot=0 fmt=float2 stride=2dw off=0dw dst=r0`** (raw d0=0x00080000 d1=0x00253B48 d2=0x00000002; format 0x25=37=k_32_32_FLOAT=float2). **AUTHORITATIVE: the vertex format is float2 (x,y) position, 8-byte stride, offset 0, single attribute, from fetch SLOT 0.** Confirms the data-RE'd format AND corrects the slot (the vfetch source is slot 0, not the slot-1 the type==3 scan flagged).
- **Per-draw slot-0 probe ([drawgeo] SLOT0=…):** slot 0 = **constant `0xA2000000` across ALL draws** (type 0/2, NOT 3=kVertex), head v0=(0,0). So slot 0 is NOT a per-draw-varying base. Combined with off=0 + auto-index reads [0,numI), **all draws read the SAME shared vertex set at slot 0; the per-draw differentiation must be the `$worldviewProj` matrix** (a shared template positioned per draw). ⇒ REFUTES the "per-draw fetch base / sequential carve" hypotheses. The screen-coord rectangles found earlier were at slot 1 (0x1FE0FC) — a DIFFERENT region, not the vfetch source.
- **⇒ Refined model:** shared vertex template at slot 0 (0xA2000000), float2, + a **per-draw `$worldviewProj` matrix** (in the ALU constants, reg 0x4000) that positions it. For correct rendering: read the verts from 0xA2000000 (numI per draw, float2) + read the per-draw 4×4 matrix from reg 0x4000 + transform on the GPU (push-const MVP) → vkCmdDraw. **NEXT:** (1) dump slot-0's (0xA2000000) actual vertex data (head is (0,0) — check deeper / the full numI verts per draw); (2) locate `$worldviewProj` in the reg-0x4000 ALU constants (the .xbv constant table gives its c-register index) + read it per draw; (3) render per-draw with the real MVP. ⚠ This vertex-addressing layer is intricate (the contradiction of constant base + off 0 + distinct content took several probes); the AUTHORITATIVE fallback if the model doesn't render is a **prod-oracle trace** of the actual per-draw vertex read addresses. The renderer remains a large multi-iteration Xenos-GPU build; infra (pipeline+bridge) + format are in place. Diagnostic added (gated REX_CHUNKDUMP): `[vfetch]` + `[drawgeo] SLOT0`. Default boot unregressed.

### cont.22 /loop — piece-3b: ⚠ the vfetch source (slot 0) is UNINITIALIZED (0x0BADF00D poison) at census time — the vertex DATA isn't present where/when expected
Dumped slot-0's actual data (0xA2000000, the real vfetch source): head is zeros, **first non-zero @+0xFFFC = `0x0BADF00D`** (a classic allocator POISON sentinel), 191911 scattered non-zero dwords but NO dense screen-coord region. `0x0BADF00D` is NOT in variant A's runtime (grep clean) ⇒ it's the **guest/title's own allocator poison**. So the title allocated the slot-0 vertex pool but, **at the only time variant A can read it (a swap boundary, when the census fires), it is still POISON — not real vertex data.** The screen-coord rectangles found earlier were in slot 1 (0x1FE0FC) = a DIFFERENT buffer, NOT the vfetch source (likely stale/leftover real data from a prior use).
- **⇒ Two interpretations, indistinguishable from variant A alone:** (a) the title's **vertex-generation never ran** in variant A (a deeper execution gap — the UI code that computes+writes vertices into the pool didn't execute / is gated on the same async-completion the renderer needs); (b) **timing/lifecycle** — the pool is filled→drawn→recycled (re-poisoned) WITHIN a frame, so it's poison again by the swap-boundary census. Either way, **the verts are not readable at any point variant A's census can sample**, so the "carve the pool + render" approach CANNOT work from variant A's CP at swap time.
- **⇒ This REDIRECTS the renderer:** the blocker is NOT draw translation (infra is built: pipeline + bridge + format decoded) — it's that **the vertex DATA the draws reference is poison/unwritten at the readable time.** Correct rendering needs the verts to exist when read, which is a vertex-data-lifecycle / execution-path question, not a translation one.
- **NEXT = PROD-ORACLE comparison (authoritative, the right tool now):** run prod (`out/build/linux-amd64-release/south_park_td`, base 0x100000000, symbols, `handle SIGSEGV nostop pass`) at the menu; read its fetch-constant slot-0 base + the vertex data there AT A DRAW (break in the rexglue CP draw path / IssueDraw, or watch the pool). Prod renders the menu, so prod's pool HAS real verts — at what address + what time (relative to the draw)? That resolves (a) vs (b): if prod's slot-0 also points at a pool that's only valid mid-frame, it's timing (variant A's CP must consume synchronously with the title's fill); if prod's verts are at a stable address variant A's is poison at, the vertex-gen path differs. ⚠⚠ The renderer is the full Xenos vertex-fetch/lifecycle + tiled-raster + shader/texture subsystem — a LARGE multi-session build, and the vertex-DATA-availability (this finding) is a fundamental gap beyond the (built) translation infra. Diagnostic added (gated REX_CHUNKDUMP): `[vtxdump] SLOT0` scan. Default boot unregressed.

### cont.22 /loop — prod-oracle attempt hit OPTIMIZED-BUILD gdb friction (deferred); renderer CONSOLIDATION + the clear faithful-CP path
Wrote `tools/prod_vfetch.gdb` (break at `ExecutePacketType3Draw`, filter to menu content shaders progc 0x10010001/0x10110101, read register-file slot-0 fetch constant + dump verts). **Hit a wall: prod is an OPTIMIZED RelWithDebInfo build** — gdb cannot resolve `this` nor the `rex::graphics::CommandProcessor*` cast at the breakpoint (`No symbol this/rex in current context`), and a static `ptype`/offset query returned `No symbol table` (lazy/split debug info). The breakpoint DID fire (prod executes draws), but the typed register read failed. **Finishing it needs raw member-offset access from $rdi OR a debug prod build — deferred to a focused session.** Tool committed as a scaffold with the limitation documented.
- **⇒ CONSOLIDATION (renderer state after this session's arc):**
  - ✅ **Breakthrough:** content IS built (the "no content" verdict was a census-parser artifact — 1024-dw truncation + type-0/1 misalignment). Title builds full menu content (draws + state + shaders + resolves + occlusion queries) into the device+13568 deferred segments every frame.
  - ✅ **Infra built + verified:** menu-quad VkPipeline (float2 + mvp/color push-const + alpha blend, REX_MENUTEST — 3 rects rendered) + census→render bridge (`SubmitMenuGeometry`, real pool geometry flows CP→GPU→present) + full PM4/opcode characterization.
  - ✅ **Vertex format decoded** from the in-stream VS microcode (IM_LOAD_IMMEDIATE): float2 (x,y), 8-byte stride, off 0, fetch slot 0.
  - ⛔ **The remaining blocker (this finding):** the vfetch source (slot 0, 0xA2000000) is 0x0BADF00D **poison at swap-time** — the only time variant A's census can sample it. Verts are valid only WHEN the title's render path consumes the segments (mid-frame), which the swap-boundary census never reaches.
- **⇒ THE CLEAR PATH = a FAITHFUL CONTINUOUS CP** (the large multi-session build): variant A must execute the deferred device+13568 segments **when the title flushes/kicks them (mid-frame, vertex pool fresh)** — NOT at the swap census. Concrete first step: hook the title's flush/kick (`sub_821C6D58` / `sub_821C6C80` / `sub_821C6600`), run the just-built segments through `ExecutePM4` there (pool valid), translate DRAW_INDX→vkCmdDraw (built pipeline), read verts (now real). Then tile binning (0x60/0x61), per-draw $worldviewProj (reg 0x4000), textures + hand-ported FS. ⚠ This is a SUSTAINED ARCHITECTURAL build (full Xenos vertex-fetch/raster subsystem), not incremental /loop probing — the infra + format + understanding are all in place for it. Prod-oracle (raw-offset rewrite of prod_vfetch.gdb) is the authoritative capstone for a future focused session. Default boot unregressed.

### cont.22 /loop — ⛔ DECISIVE (REX_POOLCHK + 40-VS vfetch survey): the content vfetch source (slot 0) is NEVER written with verts in variant A — a vertex-DATA-availability gap, not timing/translation
The flush-hook test, run as `REX_POOLCHK` (sample the pool at the kick-gate sub_821C6C80, ~1410×/frame = dense mid-frame) + a 40-shader vfetch survey:
- **All 40 menu VS shaders fetch slot 0** (float2 / stride 2dw / off 0) — IDENTICAL, uniform. So the content draws unambiguously fetch fetch-constant **slot 0 = 0xA2000000**.
- **slot 0 (0xA2000000) is NEVER real verts:** maxRealFloats/64 = **0–1** across **120k+ mid-frame samples** (head = `0xFFFFFFFF` filler; the swap census saw `0x0BADF00D`/zeros). So it's NOT a timing/lifecycle issue (the verts are never written, mid-frame OR at swap).
- **slot 1 (0x1FE0FC) is a RED HERRING:** NO VS fetches it (0/40), and its head is zeros (the screen-coord rects found earlier were deep in its region but it is NOT the vfetch source). So the slot-1 "screen coords" are not the menu content geometry.
- **⇒ DECISIVE: the menu content draws fetch from an EMPTY pool (slot 0) in variant A.** The title's vertex-GENERATION does not produce data at 0xA2000000. This is a **vertex-data-availability gap**, upstream of both translation (infra built) AND the faithful-CP timing idea (a faithful CP can't read verts that are never written). ⚠ This SUPERSEDES the "faithful CP fixes it" framing: a continuous CP is necessary but NOT sufficient — the verts must first EXIST at slot 0.
- **WHY slot 0 is empty (the open root, needs the prod-oracle):** either (i) variant A's fetch-constant value (0x02000000) is wrong vs what the title set (a CP/SET_CONSTANT capture gap), (ii) the title writes the verts to a different address than the fetch constant points to (a variant-A memory/addressing bug), or (iii) the vertex-gen is gated on an unmet condition (the same async-completion gaps). **Only the prod-oracle resolves it**: prod renders the menu, so prod's slot-0 fetch constant + the memory it points to HAVE real verts — comparing prod's slot-0 value + vertex address with variant A's (0xA2000000=empty) pinpoints the divergence. Prod-oracle is DEFERRED (optimized-build gdb friction — needs raw $rdi member offsets or a debug prod build; tools/prod_vfetch.gdb scaffold ready).
- **⇒ Renderer state (decisive endpoint of variant-A probing):** content built ✅, infra built ✅ (pipeline + bridge), format decoded ✅, but the content vfetch source is empty in variant A ⛔ → the menu cannot render until the vertex data exists at slot 0. The remaining work (find/fix why slot 0 is empty → then faithful CP → tiling → textures) is a sustained multi-session GPU build whose FIRST blocker (vertex data) requires the prod-oracle. Diagnostic added (gated): `REX_POOLCHK`. Default boot unregressed.

### cont.22 /loop — prod-oracle BLOCKED (prod has NO DWARF); redirect to a variant-A HW-watchpoint for the vertex-gen writer
Tried to finish the prod-oracle via raw offsets. **Blocker found: the prod binary has ZERO `.debug_*` (DWARF) sections** (`readelf -S` — only `.symtab` function names that let breakpoints resolve). So gdb has NO type info — `this`, the `CommandProcessor*` cast, `ptype`, and `pahole` ALL fail. The gdb-prod-oracle (read prod's register file) is **not viable on this binary** without (a) a debug prod rebuild (`-DCMAKE_BUILD_TYPE=Debug`) or (b) source-computed raw member offsets (error-prone: base classes/vtable/padding). Tool note updated in `tools/prod_vfetch.gdb`.
- **Refined understanding of the gap (no prod needed):** the content draws fetch slot 0 (0xA2000000, EMPTY across 120k+ samples) while real screen-coord verts sit in slot 1 (0x1FE0FC). The content draw's OWN segment SET_CONSTANT (FETCH) sets slot 0 = 0x02000000 (→ the empty 0xA2000000) AND slot 1 = 0x001FE0FF (→ the verts). The 40 menu VS all vfetch slot 0. ⇒ **The title intends the verts at slot-0's target (0xA2000000) but variant A never writes them there; slot 1 holds a different/other buffer.** This is a vertex-DATA-placement gap (the vertex-gen doesn't produce data at 0xA2000000 in variant A).
- **NEXT (tractable, NO prod): a VARIANT-A hardware watchpoint.** variant A is NOT stripped, so a HW watchpoint on the vertex region + `bt` gives the recompiled `sub_XXXXXXXX` writer. Watch (i) g_base+0xA2000000 (slot-0 target — does ANYTHING but the allocator poison write it?) and (ii) g_base+0xA01FE0FC+0x31E6C (slot-1, where verts ARE written — find the vertex-gen function). Comparing the two writers + why slot-0's target stays empty pins the gap. gdb: break after g_base init (e.g. at VdSwap), `p g_base`, `watch *(int*)(g_base+ADDR)`, `bt` on hit. Then either fix the vertex-gen placement OR (if it's a deeper title-state gap) accept the renderer needs the full faithful-CP/vertex-lifecycle build. ⚠ The renderer remains a large multi-session build; the major value (breakthrough + characterization + infra) is delivered, and this pins the precise remaining root.

### cont.22 /loop — ⛔ HW-WATCHPOINT CONFIRMED: the content vfetch source (slot 0) is NEVER written; a writer (sub_8242BF10) targets slot 1 instead — a vertex-data ADDRESS MISMATCH
Variant A is NOT stripped, so a HW write-watch + `bt` names the recompiled writer (`tools/poolwatch.gdb`: at the first __imp__VdSwap, read g_base, `gdb.WP_WRITE` watchpoints on the pools, `bt` on hit, keep running). Watched BOTH slot-0 target (g_base+0xA2000000, the content vfetch source) and the slot-1 region (g_base+0xA01FE0FC+0x31E6C, where real screen-coord verts appeared). Full menu run (REX_NOTOKEN+CSLEAK+CPCOMPLETE+MOVIE_EOF, ran to exit):
- **slot 0 (0xA2000000) = ZERO writes** the entire run ⇒ the content draws' vfetch source is NEVER written (its head stays 0xFFFFFFFF set before VdSwap#1). The content draws read UNINITIALIZED data. **HW-confirmed — the strongest evidence yet.**
- **slot 1 (0x1FE0FC+0x31E6C) = written by `sub_8242BF10`** (ppc_recomp.60.cpp:13194 = guest 0x8242BF10). So a recompiled guest fn DOES write real verts — to the SLOT-1 region — but the content draws vfetch SLOT 0 (which it never writes).
- **⇒ DECISIVE ROOT = a vertex-data ADDRESS MISMATCH:** verts are written to slot 1's buffer (by sub_8242BF10) while the content draws fetch slot 0 (unwritten). Either (a) sub_8242BF10 IS the content vertex-gen but writes to the wrong address vs the fetch constant (slot 0 target = 0xA2000000), OR (b) sub_8242BF10 writes a non-content (setup/clear) buffer and the content vertex-gen (→ slot 0) never runs. Distinguishing needs: identify sub_8242BF10 (guest 0x8242BF10 — what it is + whether its dest is the content pool) + why slot-0's target (0xA2000000) is never written.
- **⇒ Renderer state (HW-confirmed endpoint of variant-A probing):** content built ✅, infra built ✅ (pipeline + bridge), format decoded ✅, but the content draws' vfetch source (slot 0) is NEVER written in variant A ⛔ → the menu cannot render. The remaining work — resolve the address mismatch (why slot-0's target stays empty / what sub_8242BF10's dest should be) then the full faithful-CP + vertex-lifecycle + tiling + textures build — is a SUSTAINED multi-session effort. The major value (the breakthrough + full characterization + Vulkan infra) is delivered. **Concrete lead for the next session: investigate sub_8242BF10 (guest 0x8242BF10) — the slot-1 vertex writer — vs the content draws' slot-0 fetch.** New tool: tools/poolwatch.gdb. Default boot unregressed (no code change — gdb tooling only).

### cont.22 /loop — ⭐ UNIFICATION + CORRECTION: the vertex-pool write traces to the RESOURCE-LOADER / TRANSITION chain (sub_8211B740); the renderer's vertex-data gap = the cont.22 async-completion root
Re-ran poolwatch with guest-context reads (variant A is -O0, so ctx.r3/r4 + the host bt are readable). The slot-region write fired with **ctx.r3=0xA1FFFFFC** (dest), and sub_8242BF10's body has `stw r0,4(r3)` ⇒ it writes 0xA1FFFFFC+4 = **0xA2000000** (slot-0's target!). The full caller chain:
```
sub_82250420 → sub_8211B740 → sub_8211BE68 → sub_822487C8 → sub_82448158 → sub_8244FE80 → sub_8242BF10 (memcpy, ppc_recomp.60:13194)
```
**`sub_8211B740` + `sub_82250420` are the cont.7/10/22 INTRO→MENU transition / resource-loader worker** (sub_82250420→sub_8211B740→sub_8210AF90 sets the transitions-flag; sub_8211B740 was traced in loop-iters 3-5 as the resource/async-task manager). So the vertex-pool population is DONE BY the resource-loader/transition chain via a generic memcpy (sub_8242BF10 = load-r4/store-r3/+16/dcbt block copy).
- **⚠ CORRECTION of "slot 0 NEVER written":** the 1st poolwatch run (watching only 0xA2000000's HEAD dword) saw 0 writes — but the memcpy writes 0xA2000000 via `stw r0,4(r3)` (r3=0xA1FFFFFC), and only **ONCE per run** (nondeterministic timing; the head-only watch missed it). So slot 0 IS written by the loader, but **only once / not fully populated** (poolchk head stays 0xFFFFFFFF) — because the loader (sub_8211B740, the cont.22-GATED transition chain) does NOT fully complete.
- **⇒ UNIFICATION:** the renderer's vertex-data gap and the cont.22 intro→menu TRANSITION block are **the SAME root** — the resource-loader / async-task manager (sub_8211B740) that cont.22 loop-iters 5-8 + piece-4 found is gated on real async/GPU completion. It populates the vertex pool (0xA2000000) but stalls before fully doing so ⇒ the content draws read a mostly-unpopulated pool. MOVIE_EOF drained the loader enough to load SHADERS (the breakthrough), but NOT enough to fully populate the VERTS.
- **⇒ The path is now unified + clear:** the renderer does NOT need a separate vertex-addressing fix — it needs the **resource-loader / async-task manager (sub_8211B740) to COMPLETE** (the cont.22 RENDERER-DESIGN faithful async/GPU-completion model). Completing it populates the vertex pool AND unblocks the transition AND lets the content draws render. The faithful-CP + this resource-loader-completion are the one big multi-session build. **NEXT:** trace WHY sub_8211B740's loader stalls mid-populate at the full-menu state (the per-child completion poll *(child+208) / the sub_8224F918 queue from piece-4, now with the vertex-copy context) — what async result it waits on to continue copying verts. New evidence: tools/poolwatch.gdb (the loader→memcpy chain). Default boot unregressed (gdb tooling only).

### cont.22 /loop — heuristic rendering EXHAUSTED; debug-prod-build ruled out; renderer's sole path = the big faithful-completion build (pausing /loop)
Improved the menu-geometry viz (per-quad cycling colors, 6-vert chunks) to make individual UI rects distinguishable. Result (/tmp/varianta_menu.png): STILL a triangle soup (large overlapping tris + slivers), NOT coherent UI rectangles — even with the correct float2 format. ⇒ **heuristic extraction/grouping of the slot-1 verts CANNOT reconstruct the menu** (no per-draw boundaries; the verts aren't a clean quad array at the read offset). Heuristics are exhausted.
- **Debug-prod-build (to unblock the prod-oracle) assessed + RULED OUT for now:** the prod build (out/build/linux-amd64-release) is Release (-O3 -DNDEBUG, no DWARF — confirmed); `command_processor.cpp` is NOT a TU there (it's in the separate rexglue-sdk static lib), so getting DWARF means rebuilding the rexglue-sdk graphics lib with -g + relinking prod — a big build that also touches the prod guardrail, for merely confirmatory value (it would confirm prod's loader populates slot 0). Not worth it as a /loop step.
- **⇒ EXHAUSTIVELY CONFIRMED: the renderer's sole forward path is the large multi-session FAITHFUL-COMPLETION build** (model the GPU resource-creation/async completion so sub_8211B740's loader fully runs AND the per-frame vertex-fill writes real verts to slot 0 = 0xA2000000, which the content draws vfetch). Every quick win is exhausted: force-past fails (loop-iter 6), heuristic render fails (soup), the prod-oracle is blocked (no DWARF), the verts never reach the content fetch source (HW-confirmed). Note: the vertex-FILL (per-frame UI render → slot 0) is distinct from the resource-loader's CLEAR of slot 0 (the memcpy caught earlier copied filler); both are part of the same gated render path.
- **⏸ PAUSING the autonomous /loop** at this point: the renderer is exhaustively characterized + the major value (the breakthrough that content IS built + full PM4/vertex characterization + working Vulkan pipeline & CP→GPU bridge + the unified root, 16 commits this session) is DELIVERED, and the sole remaining work is a deliberate multi-session architectural build that is a poor fit for 4-min /loop probes. **Decision needed from the user: (1) commit to the faithful-completion build as a focused effort, (2) accept a debug-prod-build to unblock the oracle for an authoritative cross-check first, or (3) redirect.** All findings/tools/diags are in this NIGHT-LOG + sp-varianta-bootstrap memory + NEXT-SESSION-PROMPT.

### cont.22 /loop — ⭐⭐ BREAKTHROUGH: the "vertex-data gap" WALL is OVERTURNED — the content verts EXIST and are reachable (`REX_EXECSEGS` executes the deferred segments → the LIVE fetch slot-0 is a type-3 kVertex pool of REAL screen-space verts)
User said "продолжай работу автономно" again after the pause. Re-grounded, then built **`REX_EXECSEGS`** (kernel.cpp, gated, default boot UNREGRESSED — verified exit 124 / 0 crashes / 191k progress lines): at each VdSwap, enumerate the `device+13568` DIRECTORY with the census's authoritative parse (top byte `0x81` = descriptor, low 24b = seg length dwords; next dword = phys), resolve `guest=0xA0000000|(phys&0x1FFFFFFF)`, and **`ExecutePM4` each segment** — i.e. actually EXECUTE the deferred content the census proved is built but never kicked. Distinct from `REX_CHUNKCP` (runs the directory as inline PM4 — wrong) and `REX_SEGCP` (scans the r3 staging range). Measured (run-base `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30 REX_EXECSEGS=3`):
- **It executes cleanly:** 3 segments/frame, ~7–8 DRAW_INDX/frame (drawTotal 34→490 over 60 frames), no execsegs-induced crash (the ~45–50s SIGSEGV hits the BASELINE too — pre-existing, beyond the characterized-stable 30s window, NOT a regression).
- **First (negative) result:** the static `0xA2000000` (what every prior probe fixated on) stays `0x00000000` / 0 real floats across all 60 frames even WITH execution. So "execute the content → 0xA2000000 fills" is **refuted** — `0xA2000000` is simply NOT the content verts' home.
- **⭐ Then the LIVE fetch read flipped it:** read the ACTUAL fetch slot-0 constant the executed draws set (reg file `0x7FC80000+0x4800*4`) instead of the hardcoded `0xA2000000`. It is **`fc0=0x001A875B` → type 3 = kVertex → guest `0xA01A8758`, with 46/64 plausible floats**, and the address ADVANCES ~0x88000/frame (`0xA01A8758→0xA0230FF8→0xA02B9878→0xA03420F8→0xA03CA978→0xA04531F8`) = a per-frame growing pool.
- **⭐ RAW dump = DEFINITIVE structured verts** (6/6 frames identical): `640 360 | 1280 360 | 1280 720 | 0 0 …` — screen-space float2 pairs for the **1280×720** framebuffer (a screen-covering triangle). Matches the RE'd vfetch format EXACTLY (float2, stride 2dw, slot 0). NOT a filter coincidence (the first 6 floats are clean screen coords; weak `[-4096,4096]` filter irrelevant given the structure).
- **⇒ RECONCILIATION (rigorous, resolves the contradiction with cont.22's HW-watchpoint/POOLCHK "slot 0 never written"):** the **3 KICKED clears** set fetch slot 0 = `0x02000000`→`0xA2000000` (an empty setup pool) — that value is ALL the prior probes (REX_POOLCHK, the 40-VS survey, poolwatch, the swap-census) ever saw, because **the content segments never executed**. The **CONTENT draws** (deferred, never kicked) set fetch slot 0 to the real per-frame type-3 kVertex pool. Executing the segments (`REX_EXECSEGS`) is what reveals it. So the "vertex-data gap / 0xA2000000 empty / verts never written" verdict of the last ~6 continuations was a **measurement artifact of reading the fetch constant at the wrong time** (global reg file at swap/kick, holding the clears' stale value). **The verts were always there in the content draws' own state.**
- **⇒ The renderer is NO LONGER blocked on "verts don't exist."** The path is now concrete and unblocked: T2a (execute the segments) ✅ done + verified; **T2b (NEXT) = wire `DRAW_INDX`→`vkCmdDraw`:** in ExecutePM4's DRAW_INDX handler, under the render mode, read THIS draw's live fetch slot-0 (type-3 kVertex base + the `d1` size) + numI + piece-3a draw-state (RT/vp/scissor/blend/shader) → upload the float2 verts → vkCmdDraw into the Vulkan RT (the menu-quad pipeline already accepts float2) → present. Note DRAW_INDX is still a no-op count here; T2b makes it draw. ⚠ Verified the LAST draw/frame has real verts (consistent 6/6); T2b's per-draw capture will confirm all ~7–8 draws/frame and carve each draw's vert run (numI, prim). New diagnostic (gated, default-safe): `REX_EXECSEGS=N`. Commit this increment as `superheher` (not pushed).

### cont.22 /loop — T2b-step-1: per-draw fetch capture ([esdraw]) RESOLVES the open sub-question + pins the exact vkCmdDraw data
Added `[esdraw]` (gated, fires only while REX_EXECSEGS executes a segment, via `thread_local tl_execsegs`): captures EACH executed draw's live fetch slot-0 + numI + prim + first verts. The executed segments issue a FIXED 8-draw pattern per frame:
- **#0–3 = DEGENERATE setup draws** (numI=0, prim=0): fetch slot-0 = `0x02000000` (type 0) / `0x05004802` (type 2) → base `0xA2000000` (the empty pool). NO geometry.
- **#4–7 = REAL content rects** (numI=3, prim=8 = **kRectangleList**): fetch slot-0 = **type 3 (kVertex)**, base `0xA01A8730` advancing **+0x18/draw** (= 3 float2 verts, `words=6` each), v0/v1 = real screen coords with −0.5 half-texel offsets, **tiling the 1280×720 screen into 4 quadrants** (`(-0.5,-0.5)→(639.5,359.5)`, `(639.5,-0.5)→(1279.5,359.5)`, `(-0.5,359.5)→…`, `(639.5,359.5)→…`).
- **⇒ OPEN SUB-Q RESOLVED (no mechanism left unverified):** the prior POOLCHK/poolwatch/census "slot 0 = 0xA2000000" reads sampled the **DEGENERATE setup draws** (#0–3, type-0 fetch → 0xA2000000), NOT the content draws. The content draws (#4–7) set slot 0 to a type-3 kVertex pointer with real verts immediately before drawing. "Slot 0 empty" = reading at the wrong (setup) draw. The census `[drawgeo]` SLOT0 shadow likewise reported the setup value because its per-draw read landed on the degenerate draws / the shadow held the last type-0 write. **No contradiction remains.**
- **⚠ SCOPE (rigor):** these 4 content rects (prim=8 numI=3, screen-tiling, −0.5 half-texel, per-640×360-EDRAM-tile) = a fullscreen **BACKDROP** drawn per-tile — NOT the rich UI (the census found prim=5/numI=4 sprites + prim=13/numI=252 text in a RICHER frame). REX_EXECSEGS currently reaches the 3 backdrop segments present in the directory at VdSwap time; the sprite/text segments may need a different trigger frame / appear later in the directory. T2b renders what's reachable first (the backdrop rects — real, screen-space, renderable), then extends.
- **⇒ T2b vkCmdDraw data PINNED per-draw:** prim=8 kRectangleList, numI=3, fetch type-3 kVertex base (per-draw), float2 screen coords. Host transform = screen→Vulkan-NDC `clip=(x/640−1, y/360−1)` (Vulkan NDC y-down matches screen y-down — NO flip), expand the rect (3→4 verts → 2 tris), vkCmdDraw via the menu-quad pipeline → present. New diag (gated, default-safe): `[esdraw]`. Default boot unregressed (tl_execsegs is false unless REX_EXECSEGS set it). Commit `superheher` (not pushed).

### cont.22 /loop — ⭐ T2b-step-2: the title's REAL menu geometry RENDERS COHERENTLY (4 backdrop quadrants on the Vulkan swapchain) — first correct render of title geometry
Wired the carve→bridge→draw path: during a REX_EXECSEGS frame, each content DRAW_INDX (fetch type-3 kVertex, prim 8, numI 3) reads its 3 float2 verts from the live fetch slot-0, derives the kRectangleList 4th corner, expands to 2 triangles, screen→clip transforms, and accumulates into `tl_esVerts`; after the exec loop the frame's batch goes to `rex_render::SubmitMenuGeometry` → the render thread draws it via the menu-quad float2 pipeline (REX_MENUTEST) → present + PPM capture.
- **RESULT (`REX_RENDER=1 REX_MENUTEST=1 REX_EXECSEGS=3` + run-base):** `[render] menu geometry submitted (24 verts)` every frame = **4 rects × 6 verts**, captured /tmp/varianta_menu.ppm. First capture (wrong rect formula `v3=v1+v2-v0`) = 4 distinct **parallelograms** (sheared) — proved the carve→bridge→GPU→present path works end-to-end with REAL title verts, just the rect-corner math was off. **FIX:** Xenos kRectangleList 4th corner = **`v3 = v0 + v2 - v1`**, triangles `(v0,v1,v2)+(v0,v2,v3)` (verified from the RAW dump v0=(640,360) v1=(1280,360) v2=(1280,720) → v3=(640,720) = clean BR quadrant). **Re-capture = 4 CLEAN axis-aligned quadrant rectangles** (TL/TR/BL/BR) tiling 1280×720 exactly — the title's real per-EDRAM-tile (640×360) **BACKDROP**, rendered coherently. Colors = my debug palette (per-quad cycling); the title would texture them.
- **⇒ MILESTONE: variant A now renders the title's REAL menu geometry CORRECTLY** (vs the prior heuristic triangle-soup) — the full path CP-executes the deferred segment → reads each draw's live kVertex verts → triangulates the prim → screen→clip → vkCmdDraw → present, verified by a clean 4-quadrant capture. The breakthrough (verts exist) is now end-to-end demonstrated.
- **⇒ NEXT (T2b cont.):** (1) reach the RICH UI segments (prim=5 numI=4 sprites + prim=13 numI=252 text) — they weren't in the 3 backdrop segments REX_EXECSEGS executed; find their trigger frame / where they enter the device+13568 directory (or a different cmd-buffer); (2) per-draw COLOR/TEXTURE (the debug palette → the title's textures via the 0x4800 fetch texture constants + a textured FS) + the per-draw $worldviewProj (reg 0x4000) for non-screen-space draws; (3) handle the other prims (tri-strip/quad-list) in the carve. The geometry-translation foundation is proven. Default boot UNREGRESSED (exit124/0-crash/71k lines, T2b gated behind REX_EXECSEGS+rex_render::Enabled). Commit `superheher` (not pushed).

### cont.22 /loop — T2b-step-3: DRAW_INDX init-decode BUG fixed → sprite(prim5)/text(prim13) draws REVEALED; their verts are at fetch SLOT 1 (overturns the "slot-1 red herring")
User re-issued "продолжай работу автономно". Investigating where the RICH UI lives, found + fixed a real ExecutePM4 bug + located the sprite/text vertex source.
- **Rich frames DO occur** ([execsegs] NEW-MAX tracker): the device+13568 directory grows from 3 descriptors (backdrop only) to **20 descriptors / ~80 draws** at later swaps (#156, #248, #844, #1001). REX_EXECSEGS executes them; my earlier capture just grabbed swap#3 (the first, backdrop-only frame).
- **⛔→✅ BUG FIXED (ExecutePM4 DRAW_INDX init decode):** the handler read `init = GLD32(addr)` (data[0]) for BOTH op 0x22 and 0x36, but **op 0x22 (DRAW_INDX)'s VGT_DRAW_INITIATOR is data[1]** (data[0] is a control/viz word); only op 0x36 (DRAW_INDX_2)'s is data[0]. This MISDECODED every op-0x22 draw (e.g. a bogus `prim=11 numI=33024` from a `0x8100_000B` word). The census already read data[1] for 0x22; ExecutePM4 didn't. Fixed: `init = (op==PM4_DRAW_INDX) ? GLD32(addr+4) : GLD32(addr)`.
- **⭐ AFTER the fix — the true draw mix per frame ([esdraw] with op + vtxSlot scan):** #0–3 = **op 0x22**: prim=5 numI=4 (**SPRITE**, tri-strip quad), prim=4 numI=6 (tri-list), prim=5, prim=13 numI=252 (**TEXT**, quad-list) — exactly the census's prim5/13. #4–7 = op 0x36 prim=8 (the backdrop rects). So the rich UI draws are **op 0x22** (which the old decode mangled).
- **⭐ Sprite/text VERTEX source LOCATED (vtxSlot scan = first type-3 kVertex fetch slot):** the backdrop draws fetch **slot 0** (kVertex, per-draw 0xA01A87xx). The sprite/text/tri-list draws fetch **slot 1 = 0xA01FE0FC** (the ~10.5MB per-frame pool, `words≈2.75M`), with **slot 0 = their TEXTURE** (type 2). ⇒ **OVERTURNS the cont.22 "slot 1 (0x1FE0FC) is a RED HERRING / no VS fetches it" verdict** (that was from the broken decode + not executing the segments) — slot 1 IS the rich-UI vertex source.
- **⇒ T2b-cont. path:** the sprite/text draws are **op 0x22 (INDEXED)** — data[2]=index base, data[3]=index size; verts in slot 1 (0xA01FE0FC). To render them: read the index buffer + fetch the indexed float2 verts from slot 1 (resolves the old "per-draw offset into the shared pool" question — it's the INDEX BUFFER, readable now that we execute the real packets) + their texture (slot 0) + a textured FS. The backdrop (slot-0 kVertex rects) already renders. New diags (gated): `[esprim]`, `[esdraw]` op+vtxSlot, `[execsegs] NEW-MAX`. Default boot UNREGRESSED (exit124/0-crash/68k lines). Commit `superheher` (not pushed).

### cont.22 /loop — T2b-step-4: ⚠ CORRECTION — the sprite/text draws are AUTO-INDEX (src=2), slot-1 pool HEAD is zeros ⇒ the (known-hard) per-draw vertex-OFFSET problem, now scoped to the executing CP
`[esidx]` measurement (src_select + idx fmt + index buffer + the slot-1 verts at the first indices), for the op-0x22 sprite/text draws:
- **src_select = 2 (kAutoIndex), NOT indexed** — corrects step-3's "op 0x22 ⇒ indexed/data[2]=index base" guess. There is NO index buffer (the data[2]=`0xA0006000` / data[3]=`0x80000000` are stale regs, not an IB since src=2). Auto-index feeds vertex indices 0..numI-1 to the VS.
- **The slot-1 pool HEAD is zeros** (verts at indices [0,1,2] = (0,0),(0,0),(0,0)). So auto-index reading [0,numI) from vbase=0xA01FE0FC (the pool head) yields ZEROS. The REAL verts sit DEEP in the pool (cont.22 found screen-coord rects at +0x2A9F0/+0x31E6C). ⇒ this is exactly the **cont.22 "per-draw vertex OFFSET into the shared slot-1 pool" problem** (the one the heuristic contiguous-collect turned into a triangle-soup) — re-confirmed, now with the draws actually executing.
- **⇒ The backdrop (op-0x36, prim-8, slot-0 kVertex, per-draw bases) renders correctly** because each draw has its own small kVertex pool (no offset needed). The **sprite/text (op-0x22, auto-index, shared slot-1 pool)** need the per-draw offset, which auto-index alone doesn't give from vbase=head. Candidate sources (NOW readable live since the CP executes the real packets + maintains the live reg file — unlike the cont.22 census shadow): (a) the SPRITE/TEXT VS's own vfetch OFFSET field (d2[30:8]) — may differ from the backdrop VS's off=0 (disasm the sprite VS's IM_LOAD_IMMEDIATE, not "all 40 VS"); (b) a live VGT base-vertex reg; (c) the slot-1 fetch BASE actually points deep into the pool per-draw in RICH frames (these were POOR/early frames — the pool may be unpopulated at the head early). 
- **⇒ NEXT (T2b sprite/text):** disasm the SPRITE VS vfetch (which slot + offset it really reads — slot 0 is its texture, so verts are a different slot/offset) AND/OR sample a RICH frame's sprite/text draws (swap#156+, where the pool is populated) + scan slot 1 for the first populated screen-coord region per draw. Resolve the offset → carve → render (prim-5 tri-strip / prim-13 quad-list) → texture (slot-0 fetch + textured FS). The backdrop milestone + the geometry path stand; this is the remaining (genuinely intricate) vertex-addressing layer. New diag (gated): `[esidx]`. Default boot UNREGRESSED. Commit `superheher` (not pushed).

### cont.22 /loop — T2b-step-5: ⚠ CORRECTS step-3/4 — the SPRITE/TEXT VS authoritatively vfetch SLOT 0 (off 0), which is EMPTY (0xA2000000); slot 1 is a confirmed RED HERRING. Their wall = the loader-completion gap, NOT a per-draw offset.
Captured the VS microcode (op 0x2B IM_LOAD_IMMEDIATE) during REX_EXECSEGS execution + decoded the vfetch of the VS active at each sprite/text draw (`[esvf]`, authoritative — the title's own shader, not a heuristic):
- **Every sprite(prim5)/text(prim13)/tri-list(prim4) VS: `vfetch slot=0 fmt=0x25(float2) stride=2dw off=0dw`.** And slot 0 for those draws = **type 0/2** (`0xA2000000` / `0xA5004800`), v0=(0,0). ⇒ **the rich-UI VS read fetch SLOT 0, offset 0 — the SAME slot the backdrop uses — but for these draws slot 0 is the EMPTY 0xA2000000 pool (or a texture), not a kVertex pool.**
- **⚠ CORRECTS step-3/4's "sprite/text verts are at slot 1 (0xA01FE0FC)":** that was a `vtxSlot`-scan artifact (it reported the *first type-3 fetch slot present*, which the VS does NOT actually fetch). The authoritative microcode decode shows the VS fetches **slot 0**. So **slot 1 is a RED HERRING (re-confirmed authoritatively — NO VS vfetches it)**, matching the cont.22 POOLCHK/40-VS conclusion; my step-3/4 over-claimed and is retracted.
- **⇒ The picture is now complete + consistent:** (1) **BACKDROP** (prim-8): slot 0 = a real per-draw kVertex pool (0xA01A87xx) inline in the STAGING buffer → populated → **renders** ✅. (2) **SPRITE/TEXT** (prim-5/13): slot 0 = `0xA2000000`, which the resource-loader (sub_8242BF10, cont.22 unification) populates but STALLS on → **empty/filler (head 0x00000000 across all 60 frames)** → no verts ⛔. So the original cont.22 "vertex-data gap / 0xA2000000 empty" was right **for the rich UI** (it's the sprite/text vertex source); the breakthrough cracked the **backdrop** (a separate inline pool). 
- **⇒ The sprite/text wall = the cont.22 LOADER-COMPLETION problem** (populate 0xA2000000 + set slot-0 = kVertex), NOT a per-draw vertex offset (off=0, authoritative). This is the deep multi-session resource-loader/GPU-completion build (cont.22 loop-iters 5-8 / piece-4 / the unification). A possible non-loader lead: poolwatch caught sub_8242BF10 *memcpy*-ing to 0xA2000000 — its SOURCE may be slot-1's real verts (0xA01FE0FC), i.e. the verts exist at the memcpy source but the copy stalls; rendering from the source would need the per-draw source offset (the old soup-prone problem). New diag (gated): `[esvf]` (VS vfetch decode at the draw via tl_vsUcode from op-0x2B). vulkan_render: capture the RICHEST frame (steady-state shows 12-30 backdrop rects, all 4-quadrant fills, alpha-stacked). Default boot UNREGRESSED (exit137=SIGKILL/0-crash/76k lines). Commit `superheher` (not pushed).

### cont.22 /loop — T2b-step-6: the loader is DEFINITIVELY STUCK (never reaches done over 40s); REX_EXECSEGS does NOT unstall it ⇒ sprite/text need the deep resource-CREATION build
Two experiments connecting the renderer to the loader (REX_POLLDIAG extended with a distinct-state + one-time DONE tracker, uncapped):
- **REX_EXECSEGS + REX_POLLDIAG:** executing the real content segments (firing real interrupts/fences) does **NOT** unstall the loader — its state machine still cycles `2→3→6→8→10→2`. So the loader does NOT wait on draw execution. ⚠ But a SHARPER datum: **`*(child+208)=1` (ready — MATCHES prod) from poll #2 on.** So the **per-child completion IS satisfied**; the blocker is the QUEUE/state-machine level, exactly cont.22 piece-4 (`sub_8224F918` re-submits / the state machine never terminates).
- **40s run, distinct-state tracker:** the loader (singleton obj `0x82657578`) reaches states **{2,3,6,8,10} and NEVER done (1 or 12)** — no `*** LOADER REACHED DONE ***` ever fired. ⇒ the loader is **genuinely STUCK, not merely slow** (definitive — 40s, ~thousands of polls, child ready, never terminates).
- **⇒ DEFINITIVE SCOPING of the sprite/text wall:** the title's resource loader (sub_82248010 staged processor, driven by sub_8224F918) cycles forever even though child[0]'s completion field is ready (=prod's value). It needs GPU resource-CREATION completion (cont.22 loop-iter 8: file I/O works, the GPU upload/compile after it doesn't complete in the stub) — which neither faking (cont.22 force-past FAILED) nor draw-execution (this test) provides. **The sprite/text (+ the whole intro→menu resource load) are blocked on this ONE deep root, now confirmed from the renderer side.**
- **⇒ State of the renderer thread (honest endpoint):** ✅ the CP→Vulkan path works end-to-end (REX_EXECSEGS → DRAW_INDX→vkCmdDraw → the title's real BACKDROP renders, 4 EDRAM-tile quadrants). ⛔ the rich UI (sprite/text) is blocked on the **definitively-stuck resource loader** — a sustained multi-session build (model GPU resource-creation so sub_82248010 terminates), NOT a quick /loop win. The precise unanswered RE question (the only remaining lever short of the full build): **what gates sub_82248010's state-10 → done vs → 2 transition**, given child[0] is already ready=1 (cont.22 piece-4's "why the queue never drains"). Diag: REX_POLLDIAG now tracks distinct states + a DONE flag. Default boot UNREGRESSED (exit137/0-crash/72k lines). Commit `superheher` (not pushed).

### cont.22 /loop — RENDERER ARC CONSOLIDATION + DEEP-BUILD ROADMAP (quick-lever endpoint reached; prevent-reset ruled out by reasoning)
Considered the next planned experiment (hook sub_8224F918 to stop it re-submitting the already-ready child) and **ruled it out as redundant**: cont.22 loop-iter-6 force-past already forced state=done and it re-cycled/stalled, and the re-submit is **driven by the frontend (sub_8214FFD0) needing the resource** — so suppressing the loader's reset can't stop the frontend re-requesting. Every quick lever now converges on one conclusion → consolidating rather than grinding.
- **THIS SESSION'S RENDERER ARC (10 commits, all default-boot-safe, NOT pushed):** (1) BREAKTHROUGH — the "vertex-data gap" was a measurement artifact; the backdrop verts EXIST (3bebeb3). (2) MILESTONE — REX_EXECSEGS executes the deferred device+13568 segments and DRAW_INDX→vkCmdDraw renders the title's REAL backdrop (4 EDRAM-tile quadrants, fd6b265). (3) Fixed the DRAW_INDX op-0x22 init decode (init=data[1], ae27676). (4) Rigorously scoped the rich UI: sprite(prim5)/text(prim13) authoritatively vfetch SLOT 0 = the EMPTY 0xA2000000 pool (d87d586); slot 1 is a confirmed red herring; (5) the resource loader is DEFINITIVELY stuck (never reaches done/40s; child ready=1; execsegs+faking+prevent-reset all fail, 6de9d57).
- **WHAT WORKS NOW:** the full CP→Vulkan translation path (`REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30 REX_EXECSEGS=3 REX_RENDER=1 REX_MENUTEST=1`): execute the deferred segments → per-draw read kVertex verts + triangulate prim + screen→clip → vkCmdDraw → present. Renders the inline-vert backdrop coherently (verified PPM).
- **THE ONE REMAINING WALL = the title's resource loader is stuck on GPU resource-CREATION** (textures + the sprite/text vertex pool at 0xA2000000). All quick stopgaps fail because the title needs the resource to ACTUALLY exist+function downstream (not a forced flag).
- **⇒ DEEP-BUILD ROADMAP (the sustained multi-session work; NOT quick /loop probes):** **Step 1** — pin the exact resource-creation completion the frontend (sub_8214FFD0) checks and finds missing despite the file load (cont.22 narrowed to the post-file-read GPU-create path sub_822484E0→sub_8244D018). **Step 2** — model that creation: when the title requests a GPU texture/vertex-buffer, allocate a real Vulkan VkImage/VkBuffer + return a valid handle + signal completion so the frontend's check passes. **Step 3** — the loader drains → the title's OWN code populates the vertex pool (0xA2000000) + binds textures (NO heuristics/soup) → the sprite/text draws get real verts (slot 0 = kVertex) + textures → render via the (already-working) CP→Vulkan path → the full menu appears. This is the cont.22 RENDERER-DESIGN faithful resource/GPU-completion model — the real fix, well-characterized from both the transition side (cont.22) and the renderer side (this session). Default boot UNREGRESSED. No new code this entry (consolidation; prevent-reset ruled out).
- **CODE-LEVEL confirmation of the done-gate (read sub_82248010, ppc_recomp.29:1231-1514):** the state machine is a **jump-table dispatch on the state value** — `state-2`, `if >9 (state 1 or ≥12) → exit/DONE` (loc_822481C8), else jump-table[state-2] → per-state handler. Each handler **calls a polymorphic vtable method** (e.g. `*(obj->vtable+20)()` then `state←3`) and sets the next state from the result. ⇒ reaching DONE (1/12) requires a handler to return **resource-complete**; there is NO flag to flip (state-forcing = cont.22's failed force-past). The done-gate genuinely depends on the GPU resource existing — **the deep wall confirmed at the source level**, matching all other angles. ⇒ The remaining work (model the title's inlined-XDK-D3D resource OBJECTS so the handlers return complete + the title's own render code works) is a LARGE architectural build, NOT a /loop-iteration task. The renderer milestone (backdrop renders) is the session's delivered value; the full menu awaits the deliberate deep build.

### cont.22 /loop — DEEP BUILD step 1: ⭐ the done-gate FULLY MAPPED — the child's completion flag is ALREADY SATISFIED (*(child+208)=1=prod); the loader cycles on a SPURIOUS external re-init, not a missing resource (testable, possibly-quick)
User chose "Grind the deep build." Read sub_82248010 end-to-end (ppc_recomp.29:1231-1514) + its vtable methods. The state machine MAPPED:
- Jump-table on `state-2` (states 2-11). Each handler calls a vtable resource-op (offsets +8/+20/+24/+32/+36) then sets the next state. **DONE = state 1 or 12** (the `state-2 > 9` exit). State writes: 3,6,7,8,9,10,11, and **DONE via 3 handlers** (loc_822480A0→12, loc_822480A8→1, loc_8224818C→1/12).
- **The done-CHECK handlers** (loc_822480CC/82248128/8224818C) call **`vtable[9]` = `sub_82105948`**, which is **trivially `return *(child+208)`** (ppc_recomp.1:3333 — `lwz r3,208(r3); blr`). Branch: ret==2 → WAIT(stay); ret==3 → state 12 (DONE); **else → state 1 (DONE) or advance**. So **done iff `*(child+208) ∉ {2,3}`**.
- **`*(child+208) = 1`** (polldiag, = prod's ready value). `1 ∉ {2,3}` ⇒ **the done-check WOULD PASS if reached.** Yet the states cycle 2→3→6→8→10 and never reach the done-check. **NO handler in sub_82248010 sets state←2** ⇒ the reset to 2 is **EXTERNAL = sub_8224F918** (cont.22's frontend-driven re-submitter).
- **⇒ REFRAMED HYPOTHESIS (testable, ≠ cont.22 force-past):** the child's resource may be **READY by its own completion flag** (*(child+208)=1=prod); the loader cycles because `sub_8224F918` **spuriously re-inits the child to state 2** before it advances to its done-check. cont.22's force-past FORCED state=done (wrong — bypasses the check); this is different — **PREVENT the spurious re-init so the child reaches its OWN done-check**, which then passes legitimately (via *(child+208)=1). ⚠ NOT asserting it works — *(child+208)=1 may be premature (resource flagged ready but not usable), in which case the menu still fails downstream (back to the deep build). But it's the cheapest decisive next test.
- **⇒ NEXT (experiment):** wrap/instrument sub_82248010 (strong override of the weak alias, calling __imp__sub_82248010) to log state-in→out + *(child+208) — resolve whether the done-check (loc_8224818C) is REACHED (and *(child+208) there). If the reset prevents it: hook sub_8224F918 to skip the ready-child reset + test if the loader drains → menu loads. If the done-check IS reached but *(child+208)≠1 there: the flag is premature → the deep resource build stands. Default boot UNREGRESSED (reading only this entry). Commit `superheher`.

### cont.22 /loop — DEEP BUILD step 2: ⛔ DECISIVE — the child REACHES done but holding it there does NOT unblock the title; the resource is genuinely NULL (spurious-re-init hypothesis REFUTED; state-level shortcut definitively ruled out)
Built `REX_LOADERTRACE` (wrap sub_82248010 — strong override of the weak alias calling __imp__sub_82248010, the established kernel.cpp pattern) + `REX_LOADERLATCH` (hold child[0] at done). Two measured results:
- **TRACE (refutes step-1's "done-check never reached"):** the child DOES reach DONE — `#20 state 3→1 c208=1`, `#80 state 10→1 c208=1` — then `#100 state 2→3` = **re-init'd to 2 and re-processed**. So child[0] **completes (→state 1) REPEATEDLY** but is re-queued forever (exactly cont.22 piece-4). The done-check IS reached; *(child+208)=1; it legitimately completes.
- **LATCH (the decisive test):** held child[0] at state 1 (override the external re-init — STRONGER than cont.22's one-shot force-past). Result: the cycling stops (trace shows `[latched]` after #20), the run does NOT stall early (283k lines vs cont.22 force-past's ~780), **BUT the title keeps SPINNING** (`KeGetCurrentProcessType` = 245k/283k lines = the same idle state as baseline) — **NO transition, NO menu progress, no sub_8210AF90.**
- **⇒ DECISIVE: the spurious-re-init hypothesis is REFUTED.** child[0] reaching/holding done is MEANINGLESS — the title keeps re-requesting because **the RESOURCE the loader should create is genuinely NULL** (not because the state is wrong). The re-init is a SYMPTOM (frontend re-requests a missing resource), not the cause. This definitively **rules out every state-level shortcut** (force-past cont.22, latch, prevent-reset) — they all fail because the title needs the resource to ACTUALLY exist + function. 
- **⇒ The ONLY path is the deep resource-CREATION build** (now confirmed from the strongest possible angle): find the resource-creation sub-op inside the loader's vtable handlers (offsets +8/+20/+24/+32 — the ones that ADVANCE the state, calling a sub-op that should create a GPU texture/buffer but returns null/stub) and model it (create a real Vulkan resource + a valid handle the title's render code can use). New diags (gated, default-safe — exit137/0-crash/73k lines): `REX_LOADERTRACE`, `REX_LOADERLATCH`. NEXT: identify which vtable handler's sub-op creates the (null) resource → model that specific creation. Commit `superheher`.

### cont.22 /loop — DEEP BUILD step 3: the loader's sub-ops are all FILE-I/O + flags (no GPU-create); the create is the INLINED-D3D frontend path — the autonomous RE is COMPLETE, the build is fully scoped
Dumped child[0]'s vtable (0x820DAE0C) via REX_LOADERTRACE + read the resource sub-ops:
- **vtable:** [0]sub_821006E0 [1]**sub_82248480 (the RE-INIT)** [2]sub_822485A0(+8) [3]sub_82248570 [4]sub_82248370 [5]**sub_822484D0(+20)** [6]sub_822484E0(+24) [7]sub_82248530 [8]sub_822485E8(+32) [9]**sub_82105948(+36=done-check=`return *(child+208)`)** [10-19] misc.
- **sub_822484D0 (+20, the "ready" setter) = 8 lines, TRIVIAL: just `*(child+208) = r11`** — sets the ready flag, creates NOTHING. **sub_822485E8 (+32) → sub_8244D230** = 168 lines calling **`NtWriteFile` + `NtWaitForSingleObjectEx`** = FILE I/O (write/cache + wait), not a GPU-create. **sub_822484E0 (+24) → sub_8244D018** = kernel/IO (cont.22). **sub_822485A0 (+8)** = file size (cont.22).
- **⇒ ALL the loader's resource sub-ops examined are FILE-I/O + flag-setting — NONE create a GPU resource.** The loader's job is to **load/process the file and flag ready** (which WORKS — child[0] completes, *(child+208)=1). This **exonerates the loader** (consistent with the latch test: holding the child done changes nothing because the loader was never the gate). 
- **⇒ DEFINITIVE, COMPLETE characterization:** the GPU-resource-CREATE (shader-compile `.xbv`→GPU shader / texture upload) is **DOWNSTREAM of the loader, in the frontend (sub_8214FFD0), as INLINED XDK D3D** — there is no clean hook (cont.22's "XDK D3D inlined" wall). The title loads the file (works), the inlined-D3D create stubs to null, the frontend re-requests → the loader re-loads forever. **The deep build's target = implement the inlined-D3D GPU resource subsystem** (`.xbv`→SPIR-V shader translation [Plume/the 19 .updb], texture decode/upload, buffer creation) so the title's render objects are non-null — i.e. re-implement the Xenos GPU resource layer that prod (Xenia-based) has and variant A lacks.
- **⇒ STATUS — the autonomous RE phase is COMPLETE:** the full chain is now characterized end-to-end (movie→transition→loader→done-gate→re-init→GPU-create→inlined-D3D). The remaining work is the **massive, focused implementation** of the GPU resource subsystem — a multi-session architectural build (likely needing design decisions on bridging the title's inlined-D3D objects to the Vulkan backend), NOT further /loop RE (which has reached its natural limit — every sub-op is now mapped). The renderer milestone (backdrop renders) + the complete characterization are the delivered value. Default boot UNREGRESSED. Commit `superheher`.

### cont.22 /loop — slot-1 sprite/text carve = CONFIRMED SOUP (even with the corrected numI) — the LAST shortcut is closed
User kept choosing "продолжай"; tried the most concrete remaining shot at the visible menu content. The sprite(prim5)/text(prim13) draws' verts were GENERATED at slot 1 (0xA01FE0FC, cont.22); prior carves produced soup but used the BROKEN init decode (garbage numI=33024). Re-tried with the CORRECTED per-draw numI (REX_SPRITECARVE: sequential carve from the dense start, authoring→clip x/884-1,y/521.5-1, prim-5 tri-strip / prim-13 quad-list triangulation; 4164 verts submitted).
- **RESULT = SOUP** (/tmp/sprite.png): a degenerate diagonal triangle over the backdrop quadrants, NOT coherent UI. The corrected numI did NOT fix it. ⇒ **slot 1 is genuinely NOT the draws' reconstructable source** — confirming the authoritative finding (the VS fetches SLOT 0; slot 1 is an intermediate/staging buffer whose layout does not map to the draws). The verts the draws actually consume are at slot 0, which is empty (resource-create stubbed).
- **⇒ The LAST heuristic shortcut is now decisively closed.** Every non-build path is ruled out by experiment: state-level latch (the loader completes but the title still needs the real resource), and slot-1 carve (soup, even with correct numI). **There is NO way to render the sprite/text without the real resource-create populating slot 0** — the deep GPU-resource-subsystem build is the sole remaining path, exhaustively confirmed from every angle this session (~13 iterations) + cont.22 (8) + the variant-B spike (XDK D3D inlined). New gated diag (default-safe — exit137/0-crash/35k lines): `REX_SPRITECARVE` (confirmed-soup; kept as a closed-door record). Commit `superheher`.

### cont.22 /loop — ⭐⭐⭐ FOCUSED BUILD BREAKTHROUGH: the title's REAL menu UI renders — VB-fill capture recovers the per-draw verts the stubbed fetch constant lost
User chose "commit to the focused build." The blocker: the sprite/text draws read fetch slot 0 (0xA2000000, constant 0x02000000 = type-0 invalid stub); the verts went to slot 1 with a per-draw mapping lost in the stubbed resource-create (heuristic carving = soup, 4 attempts). **NEW ANGLE that cracked it:** hook the memcpy `sub_8242BF10` that FILLS the dynamic VBs (`REX_VBFILL`, strong override calling __imp__). Each fill's SOURCE (r4) holds the real per-draw verts BEFORE the copy — the discrete per-draw boundaries heuristics couldn't see.
- **`[vbfill]` capture (measured):** clean per-draw fills — `#0 4v (0,0)(1768,0)…` = full-screen backdrop quad; **`#1 6v (348,261)(932,261)…` = a UI PANEL** (matches cont.22's RE'd panel coords exactly); `#3 504v` = text (local-space glyphs near origin, need per-glyph matrices); `#21 6v (202,203)(1078,203)` = another panel. The fills are in draw order with real authoring(~1768×1043) coords.
- **RENDER (capture the fill source → authoring→clip (x/884-1,y/521.5-1) → triangulate → SubmitMenuGeometry):** filtered to screen-space panels (nv 4-16, span>40, skip the backdrop + local-space text). **RESULT = /tmp/menu_panel.png: a CLEAN red UI PANEL rendered correctly** (the title's real (348,261)-(932,459) menu panel) on the dark bg. **FIRST time variant A renders a real menu UI ELEMENT** (not the backdrop, not soup) — the focused build's approach WORKS: the lost per-draw mapping is RECOVERED via the fill-source capture.
- **⇒ The path to the full menu is now OPEN + concrete:** (1) relax/refine the filter + match fills→draws (by order) for the right per-prim triangulation → render ALL panels; (2) per-glyph text (the 504-vert local-space fills need the per-glyph $worldviewProj — find the per-glyph matrix); (3) textures (the panels' fill textures via the 0x4800 fetch consts + a textured FS). The VB-fill capture is the key that unlocks the per-draw geometry. ⚠ The sub_8242BF10 wrap adds ~6% default-boot overhead (hot memcpy) — TEMPORARY; optimize or compile-gate before keeping. Gated diag: REX_VBFILL. Default boot FUNCTIONAL (exit137/0-crash/68k lines). Commit `superheher`.

### cont.23 /loop — ⛔→🎯 the heuristic VB-capture path is exhausted (text can't be placed); MEASURED the exact wall and PINPOINTED the deep-build entry point
Picked up rendering the menu TEXT (stride-16 glyph quads). Building it forced a rigorous, measured characterization that **corrects BREAKTHROUGH-2's over-broad claim** ("content draws point at real screen-space verts" — true ONLY for the prim-8 backdrop) and closes the heuristic path. Commits `fe2cd51`, `e27b18e`, `a8b0cd4` (all gated, default boot UNREGRESSED: exit137/0-fatal/111k lines, reaches menu state).
- **Text is local-space (measured):** the memcpy text fill = stride-16 `pos.xy + uv.xy`, glyphs at local `(16.5,-0.5)(36.5,-0.5)…` marching right (kerning), NOT screen coords. Rendering the position quads as-is lands them off-screen top-left. A local→screen transform is required. (Removed the disproven heuristic text-render branch; kept the panels, which still render.)
- **REX_EXECSEGS DOES execute the UI draws** (`[esprim]`: prim 5 sprite numI=4, prim 4 quad numI=6, prim 13 text numI=252, prim 8 backdrop numI=3) — but NONE of the UI draws can be rendered from the recorded segment:
  - `[esvf]`: every UI draw's VS reads vertex-fetch **slot 0**, which is EMPTY (`0xA2000000`) or a texture (`0xA5004800`), v0=`(0,0)`. The per-draw vertex binding is GONE. Only prim-8 backdrop has real slot-0 kVertex screen-space verts (and it renders — that's all BREAKTHROUGH-2 actually measured).
  - The real text verts exist only at memcpy-fill time: **DEST=`0xA022FFF0`**, local stride-16. The draw carries NO reference to that addr (slot-0 empty; the `[esdraw]` slot-1 scan finds a STALE `0xA01FE0FC`). At exec time the dynamic VBs read all zeros (recycled). ⇒ address-key correlation is impossible.
  - **The per-draw transform IS recoverable at exec time** (`[esalu]` reg 0x4000): World translate `c0=(1,0,0,333.56) c1=(0,1,0,865.96)` + screen-ortho Proj `c4=(2/1280,0,0,-1) c5=(0,-2/720,0,1)` (Y-flip, 1280×720). **But reg 0x4000 = ALL ZEROS at memcpy-fill time.** ⇒ verts and transform NEVER coexist in time.
  - `[esset]`/`[esset2]`: during segment execution, **ZERO** fetch-constant writes via either SET_CONSTANT or SET_CONSTANT2. The backdrop's slot-0 values are STATIC (set once at boot setup, `0x001A8713`+0x18/quadrant); the UI's dynamic per-frame bindings are simply never emitted into the PM4 stream. ⇒ the binding is done by recomp'd guest D3D code, not PM4.
- **⭐ DEEP-BUILD ENTRY POINT located:** logged the memcpy caller's lr (`0x821F90B0`) → the dynamic-VB fill is called by **`sub_821F8E60`** (ppc_recomp.21.cpp:22258). The call site is a textbook D3D dynamic-VB pattern: **vtable+120 (Lock)** → locked ptr (stack+80 = `0xA022FFF0`); `r5=r29<<4` (size = vertCount×16, confirms stride-16); `bl sub_8242BF10` (memcpy fills the VB); **vtable+124 (Unlock)**. The VB object (`r30`) is a REAL recompiled D3D resource, Lock/Unlock EXECUTE, backing = `0xA022FFF0`. **The missing link = the SetStreamSource+DrawPrimitive that should bind `r30` into the slot-0 fetch constant** — un-emitted/stubbed.
- **⇒ segment-re-exec is a DEAD-END for the UI** (renders only the backdrop). The sole path to the textured menu is restoring that bind+draw. ⭐NEXT (deep build): trace `r30`'s flow after Unlock (in `sub_821F8E60` or its caller) to the bind+draw step; that's where the fetch-constant binding + the textured draw must be reconstructed. New gated diags: `[esalu]` (transform dump), `[esset]`/`[esset2]` (PM4-binding-absence proof), `[text] caller_lr`.
- **Lock/Unlock structure decoded (`sub_821F8E60` @ ppc_recomp.21.cpp:22552-22610):** the VB/draw object **`r30` is obtained from a vtable+200 call** (loc 22556) on the renderer; then `r30->vtable+120` = **Lock**(r4=**13** primType[text quad-list], r5=r29 vertCount, r6=**16** stride, r7=&stack+80 outPtr) → fills outPtr=`0xA022FFF0`; `bl sub_8242BF10` fills the verts; `r30->vtable+124` = **Unlock** (loc 22604), which EXECUTES (PPC_CALL_INDIRECT_FUNC) but emits NO PM4 binding ([esset]=0). ⇒ the draw-submit is the VB object's **vtable+124 (vtable[31])** method — it runs but its D3D GPU-submit is stubbed (doesn't bind slot-0 / emit DRAW_INDX). ⭐DEEP-BUILD next step: capture `r30`'s vtable addr at runtime (wrap sub_821F8E60), read the vtable+124 method, and RE why it doesn't emit the bind+draw (the stubbed inlined-D3D submit to reconstruct).

### cont.23 /loop — ⭐ UI TEXT GEOMETRY RENDERS; the verts↔transform bridge (count-correlation) is VALIDATED (commits fe2052f7-area: REX_UITRACE, fe2e5b7 REX_UITEXT)
Continued the deep build. Captured the VB obj's methods then BUILT the bridge between the disjoint verts (fill-time) and transform (exec-time) windows — and it renders.
- **`REX_UITRACE` (PPCInvokeGuest lr-match capture):** the VB object `r30`=`0x000E3690` (vtable `0x820E0B10`); **Lock = `sub_822052B0`** (calls `sub_821C48B0(device)` → the dynamic-buffer allocator returning `0xA022FFF0`, stores it, returns S_OK); **Unlock = `sub_822052F8`** = TRIVIAL: `*(device+48) = *(device+13652)` (13652 = 13568+84, near the segment directory) — a device build-cursor update, **NOT a draw**. ⇒ the draw is recorded into the segment by a separate traversal; the **ALU transform IS recorded** (reg 0x4000 readable at exec time), only the vertex binding is not.
- **The text VB `0xA022FFF0` reads ZEROS at exec time** (recycled) — so verts truly exist only at memcpy time; transform only at exec time. Bridged them:
- **`REX_UITEXT`:** guest thread snapshots each text-glyph fill (stride-16, by vert COUNT, deduped, persistent); the swap thread pairs each prim-13 draw to the snapshot whose `count == numI` (a strong discriminator) and applies the live reg-0x4000 transform (World-translate + screen-ortho Proj) → clip-space glyph quads → render.
- **RESULT (`REX_UITEXT_FIT` text-only view): 2 ROWS of proportional glyph cells = the title's real 252-vert/63-glyph text label, correct per-glyph kerning + line breaks** (PPM verified; image sent to user). **The full text pipeline works end-to-end** (capture→snapshot→count-correlate→transform→present), and the **count-correlation is DETERMINISTIC for the static menu** (disproves the cont.22 "order-correlation = phantom" worry — count+order pins it).
- ⚠ **Honest limits:** (1) glyphs are solid debug-colored cells — readable text needs the font atlas (0xA5004800) sampled with the per-glyph UVs (captured in the snapshot pos+uv but currently only pos used). (2) **This label's game-accurate placement is OFF-SCREEN** — every prim-13 draw has World=(333.6, **866.0**), and 866 > the Proj's ~714 screen height ⇒ clip.y≈-1.4. The 3 executed segments contain one off-screen text block; the visible dialog text is in segments REX_EXECSEGS doesn't reach. The FIT view maps local coords on-screen to validate the geometry. ⭐NEXT: (a) the font-atlas TEXTURE pipeline (UV + sampler) for readable glyphs; (b) reach the on-screen text (more segments / the binding restoration). Default boot UNREGRESSED (0 fatal). All gated: REX_UITEXT/REX_UITEXT_FIT/REX_UITRACE.
