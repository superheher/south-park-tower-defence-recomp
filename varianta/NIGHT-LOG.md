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
