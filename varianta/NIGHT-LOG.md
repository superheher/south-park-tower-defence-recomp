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
