# CODEGEN-ASLOCAL execution LOG

Running narrative for making register/flag `*_as_local` promotion CORRECT on this
SEH/setjmp title via the **decouple** strategy (keep `PPCContext` full, emit registers as
C++ locals, sync locals↔ctx at snapshot boundaries). Companion to `docs/CODEGEN-ASLOCAL-PLAN.md`.
Raw A/B lines append to `tools/perf/results.log`.

## Baseline (inherited, re-confirmed this session)
- port `south_park_td` md5 `024e365a`, `librexruntime.so` md5 `1996b550`.
- `.text` = **23,361,720 B (22.28 MB)**; combat floor **p10 = 13.5** (min 10.7, avg 30.2, max 57.5).
- Fallbacks staged: `south_park_td.baseline` (024e365a), `librexruntime.so.good` (1996b550).
- Working-tree backup of the SDK before any edit: `docs/aslocal-backups/sdk-working-tree-START.patch`.

---

## PHASE A — re-validate the gate (DONE)
- **Baseline gate = PASS** (`detdiff.sh gate baseline_reval 40`): markers=142 assets=8 warns=20
  errs=0 naninf=0 pipes=16 pacing=19 in_level=True fps_med=60.0 → `status=pass reason=equivalent`.
- **Injected-bug control = FAIL** (flip `+`→`-` in `build_add`, regen+build md5 `aa648890`,
  `.so` unchanged 1996b550): `status=fail reason=session_failed_no_in_level` (corrupted `add`
  broke boot/level entirely). Reverted `build_add` cleanly (matches HEAD). **Gate has teeth.**
- Crash diagnosis is inherited (CODEGEN-SIZE-REPORT §3): every `*_as_local` flag crashed
  identically at the SEH unwind because defining `REX_CONFIG_*_AS_LOCAL` REMOVED struct members
  from the **port's** `PPCContext` while the prebuilt `.so` kept the full layout → offset
  mismatch → corruption once FP/vector+setjmp code ran. The decouple eliminates this primary
  fault by keeping the struct full, so I went straight to implementing it (no point burning a
  ~7.5-min cycle re-observing the known crash).

---

## PHASE B — make `cr_as_local` correct via DECOUPLE (in progress)

### The fix (3 edits, all working-tree)
1. **`resources/templates/codegen/init_h.inja`** — stop emitting the `#define REX_CONFIG_*_AS_LOCAL`
   macros (kept `skip_lr`/`skip_msr`). These macros are the *only* thing that shrinks `PPCContext`
   (they guard the struct members in `context.h`). Not emitting them ⇒ struct stays FULL ⇒ the
   prebuilt `.so` still matches, and `env=ctx` still has `ctx.cr0-7` to snapshot. The `*_as_local`
   config flags continue to drive the codegen tool's accessor + local-declaration emission.
2. **`src/codegen/builders/context.cpp`** (setjmp emit) — added the locals↔ctx sync around the
   `env=ctx`/`ctx=env` snapshot. Enumerates the promoted register class from the config flags
   (`forEachPromoted`), marks each as a local (so it's declared), saves `ctx.<reg> = <reg>` before
   `env=ctx`, reloads `<reg> = ctx.<reg>` after `ctx=env`. Generic over cr/xer/ctr/reserved + GPRs
   so Phases C/D reuse it. Syncs the whole class for unconditional correctness (clang DCEs dead).
3. **`south_park_td_manifest.toml`** `[entrypoint]` — `cr_as_local = true`.

### Why CR needs only the setjmp sync (no SEH change)
The SEH entry-capture/catch (`function_graph.cpp`) touches only `ctx.{r1,lr,r13-31,r3}` — never
CR/XER/CTR. CR is volatile-ish and per-frame in the local model (the caller's CR lives in the
caller's locals, untouched by a call), so cross-call CR persistence is automatic. The only place
the whole ctx is snapshot/restored for CR is the title's setjmp (`env=ctx`/`ctx=env`), which the
sync now covers.

### Result — `cr_as_local` MADE CORRECT (first bankable win)
- **Build:** port md5 `8aeb1b29`, **`.text` = 19,951,568 B = −3,410,152 (−14.6%)** vs baseline.
  `.so` md5 `1996b550` **UNCHANGED** (decouple worked: struct full ⇒ no port↔.so mismatch).
- **Generated-code shape verified:** init.h emits NO `REX_CONFIG_CR_AS_LOCAL`; 6,225 `PPCCRRegister
  crN{}` local decls; 44,854 bare `crN.` accesses (hot path on locals); only **12 `ctx.cr0`** left
  (the setjmp save/reload sites) vs ~224K in baseline. setjmp sync emitted correctly
  (`ctx.crN = crN;` before `env=ctx`; `crN = ctx.crN;` after `ctx=env`).
- **detdiff gate = PASS** (`reason=equivalent`, markers=142 exact, errs=0, naninf=0, in_level).
  The prior session REFUTED this exact flag; the decouple fixes it. ✓
- **Floor A/B (`ab.sh 90 3` base vs cr_aslocal): NEUTRAL.**
  - base median_p10 = **14.8** (15.4, 14.8, 14.6); avg ~29.0; max ~50.6
  - cr_aslocal median_p10 = **14.6** (14.6, 15.0, 14.6); avg ~28.4; max ~48.6
  - Δp10 ≈ **−0.2** (within run-to-run noise; ≪ +1.0 keep-bar). avg/max identical.
- **Mechanism:** as predicted — −14.6% (→19.95 MB) still ≫ 12 MB L3 (~1.66×), so the per-frame
  working set still overflows cache on heavy frames. The floor is I-cache **capacity**-bound; cr
  removes a big chunk but not enough to fit. The floor-mover (if any) is the much larger
  GPR-as-local cut (Phase D — the 2.82 M ctx round-trips).
- **Banked:** best-known-good staged = `south_park_td.cr_aslocal` (8aeb1b29), gate-confirmed booting.
  This is the win the prior session could not take. **`cr_as_local` is now a correct, shippable
  −14.6% footprint reduction.**

---

## PHASE C — xer/ctr/reserved as_local (in progress)
Manifest: added `xer_as_local`/`ctr_as_local`/`reserved_as_local = true` (on top of cr). No code
change needed — the generic `forEachPromoted` setjmp sync already covers them. All volatile/scratch
(SEH catch never touches them), so setjmp sync suffices. Stacked into one build+gate; bisect if fail.
### Result — xer/ctr/reserved ALSO correct (banked)
- **Build:** md5 `dc32b4e1`, **`.text` = 19,610,528 B = −341,040 more (−3,751,192 total, −16.06%)** vs
  baseline. `.so` unchanged `1996b550`.
- **detdiff gate = PASS** (`reason=equivalent`, markers=142, errs=0, naninf=0, in_level).
- **Banked.** Best-known-good = `south_park_td.xer_ctr_reserved` / `.phaseC_final` (dc32b4e1).
- Skipped a separate floor A/B (−16% still ≫ L3 → neutral like cr); definitive floor A/B in Phase E.

---

## PHASE D — GPR/FPR/VR-as-local (the big lever) — REFUTED

### D.1 `non_volatile_as_local` (r14-31, f14-31, v14-31, v64-127) — FAIL
- Build md5 `dab49c0d`, **`.text` = 18,576,208 B (−1,034,320 more, −20.5% total)**, `.so` unchanged.
- **detdiff gate = FAIL** (`session_failed_no_in_level`): boots, reaches Stan's House, then **crashes
  during the combat dwell** (hard fault — no SEH-recovery log; subsequent boots flaked → memory
  corruption). My setjmp sync covers setjmp but NOT the SEH/`.so` register-flow.

### D.1-bisect GPR-only `non_volatile` (r14-31 only; f/v kept in ctx) — FAIL (worse: at BOOT)
- To isolate GPR vs FP/VMX, temporarily promoted only r14-31 (4 edits: f()/v() accessors drop nonvol,
  forEachPromoted drops f/v sync, elision narrowed to `__save/__restgpr*` so FP/VMX helpers still run).
- Build md5 `71a3b740`, `.text` = 18,887,784 B (−19.15% total). **gate = FAIL at BOOT** (no
  camp_diagram, all 3 attempts). ⇒ **the GPR (r14-31) promotion itself is the root cause**, not f/v.
- Reverted all 4 bisect edits.

### D.2 `non_argument_as_local` (r0, r2, r11, r12, f0, v32-63) — FAIL (at BOOT)
- Hypothesis: volatile + SEH-untouched ⇒ safe like cr/xer/ctr. **Wrong.** Build md5 `acc8a6e3`,
  `.text` = 18,875,104 B (−19.21% total). **gate = FAIL at BOOT.** Reverted.

### Phase D conclusion — clean dichotomy (root cause)
- **Flag/special registers (cr/xer/ctr/reserved) → as-local WORKS** (decouple + setjmp sync). They
  are accessed only via dedicated accessors, are volatile, write-before-read within a function, and
  are **never read by SEH/RtlCaptureContext/`.so` context-capture** — so the *only* boundary that
  snapshots them is the title's setjmp, which the sync covers.
- **General registers (r/f/v) → as-local BREAKS at boot.** They carry values ACROSS calls
  (args/returns), through the `.so` boundary, and are captured/restored by the SEH entry-capture
  (`__seh_rN = ctx.r13-31`) and `RtlCaptureContext` (`ctx.r14-31`/`f14-31`). Critically, under
  GPR-as-local the **incoming non-volatiles live in the CALLER's C++ locals, not ctx** — so the SEH
  entry-capture has nothing correct to snapshot, and the catch restores garbage into `ctx.r14-31`.
  The setjmp sync cannot fix this; it needs the **SEH/ABI state-save redesigned** to enumerate +
  preserve the promoted r/f/v across the unwind boundary and thread incoming values through. This is
  the SDK-architecture change the prior session (CODEGEN-SIZE-REPORT §6) flagged as out of scope.
- **Reverted to Phase C** (the FINAL correct build). All 4 bisect edits + both manifest flags removed;
  rebuild reproduced Phase C **byte-identically** (md5 dc32b4e1) ⇒ clean, deterministic, reproducible.

---

## PHASE E — floor on the best correct build (Phase C, −16.06%) — NEUTRAL

- **Floor A/B (`ab.sh 90 3`, base vs Phase C):** base median_p10 = **14.3** (14.2, 15.0, 14.3);
  phaseC median_p10 = **14.6** (14.9, 14.6, 14.4). **Δ +0.3 → neutral** (≪ +1.0 keep-bar; avg ~28 and
  max ~49 identical). Combined with cr's A/B (14.6 vs 14.8), the floor is **definitively unmoved** by
  the −16.06% correct cut.
- **Profile (Phase C):** standalone profile launches landed at a light ~60 fps menu frame (the
  boot/nav-to-Stan's-House flaked for the single-shot launches, unlike the gate's 3× retry), so this is
  a lower bound: still front-end-bound ~44–46%, fetch-latency (~33%) > fetch-bandwidth (~13%),
  DSB:MITE ≈ 18–20:80–82, L1i-miss ~0.5–0.6B/12s, branch-miss ~11%, IPC ~0.76, GPU idle ~20–24%.
  Bottleneck character unchanged vs the inherited combat baseline (721M L1i, 47–56% front-end). Even a
  light frame being ~45% front-end-bound underscores the pervasive recompiled-footprint stall.
- **Conclusion:** the floor is I-cache **capacity**-bound; the reachable correct cut (19.61 MB ≈ 1.63× L3)
  shrinks the footprint but does not fit, so it is floor-neutral. Floor moves only with a *large*
  absolute reduction toward ~12 MB cache-fit — only the (blocked) GPR-as-local lever is that big.

## FINAL STATE
- Staged best-known-good: `south_park_td` md5 **dc32b4e1** (Phase C, .text 19,610,528, −16.06%),
  `librexruntime.so` **1996b550** (unchanged). Fallbacks: `.baseline` (024e365a), `.so.good` (1996b550),
  `.cr_aslocal`, `.xer_ctr_reserved`, `.phaseC_final`. shm clean, no game procs.
- SDK working-tree edits (working-tree-only, NOT committed): `init_h.inja` (decouple) +
  `builders/context.cpp` (setjmp sync). `function_graph.cpp` untouched. arithmetic.cpp clean.
  Pre-session backup: `docs/aslocal-backups/sdk-working-tree-START.patch`.
- Report: `docs/CODEGEN-ASLOCAL-REPORT.md`.
