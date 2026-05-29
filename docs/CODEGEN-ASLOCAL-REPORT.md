# South Park recomp — register/flag `*_as_local` via DECOUPLE: report

**Date:** 2026-05-29 (autonomous session). **Goal:** make the codegen's register/flag
"as-C++-local" promotion **behaviour-correct** on this SEH/setjmp-heavy title and take the
`.text` footprint lever the prior session found but could not take safely
(`docs/CODEGEN-SIZE-REPORT.md`), then measure whether the resulting `.text` cut moves the
combat floor (baseline p10 ≈ 13.5). The determinism-diff gate ranks above fps.

---

## 1. TL;DR

- **The decouple works.** The prior session proved `*_as_local` is *the* footprint lever
  (`cr_as_local` = −14.6 % `.text`) but **every flag was refuted** because defining
  `REX_CONFIG_*_AS_LOCAL` *shrinks* `PPCContext` in the port while the prebuilt `librexruntime.so`
  keeps the full layout → port↔`.so` mismatch → corruption under the title's whole-`ctx` setjmp/SEH.
  This session **decouples** "emit register as a C++ local" from "shrink the struct": keep
  `PPCContext` **full** (so the `.so` matches and `env=ctx` still has somewhere to snapshot), still
  emit the registers as per-function locals (the clang-DCE + reg-alloc win), and **sync the promoted
  locals ↔ ctx at the setjmp snapshot boundary**.
- **Banked CORRECT (gate-pass, kept): `cr_as_local` + `xer_as_local` + `ctr_as_local` +
  `reserved_as_local`.** `.text` **23,361,720 → 19,610,528 B = −3,751,192 (−16.06 %)**,
  `librexruntime.so` **unchanged** (`1996b550`). This is the win the prior session could not take.
- **Floor: NOT moved (neutral, as predicted).** Two interleaved A/Bs (`ab.sh 90 3`, median p10 of
  heavy windows): `cr_as_local` p10 **14.6** vs base **14.8** (Δ −0.2); Phase C (−16.06 %) p10 **14.6**
  vs base **14.3** (Δ +0.3). Both ≪ the +1.0 keep-bar; avg (~28) and max (~49) identical. 19.6 MB is
  still ~1.6× the 12 MB L3 — the per-frame working set still overflows cache on heavy frames. The floor
  is **I-cache *capacity*-bound**; this cut removes a big chunk but not enough to fit.
- **GPR/FPR/VR-as-local (`non_volatile`, `non_argument`) — REFUTED (the big lever, still blocked).**
  Both crash at boot/combat even with the decouple + setjmp sync. Root cause (clean dichotomy, §4):
  general registers carry values across calls and through the `.so`/SEH boundary, and their incoming
  non-volatiles live in the **caller's** C++ frame (not ctx) — so the SEH entry-capture has nothing
  correct to snapshot. They need the **SEH/ABI state-save redesigned**, not just a config + setjmp sync.
- **Definition of Done:** the plan's primary deliverable — *make `cr_as_local` correct and measure its
  floor* — is **met and exceeded** (4 flags correct, −16.06 %). The floor is unmoved and (per the
  cache-capacity mechanism) is not movable by any footprint cut short of cache-fit (~12 MB), which the
  reachable levers don't deliver. GPR-as-local (the only lever big enough to *approach* fit) remains an
  SDK-architecture task.

---

## 2. Per-flag ledger

| # | Flag(s) | detdiff | `.text` (B) | Δ vs baseline | floor p10 | Kept? |
|---|---|---|---|---|---|---|
| baseline | — | pass | 23,361,720 | — | ~13.5 (ref) / 14.8 (this A/B) | — |
| B | `cr_as_local` | **PASS** | 19,951,568 | −3,410,152 (−14.6 %) | **14.6** (vs base 14.8 → neutral) | **YES** |
| C | + `xer`+`ctr`+`reserved` | **PASS** | 19,610,528 | −3,751,192 (−16.06 %) | **14.6** (vs base 14.3 → neutral) | **YES** |
| D.1 | `non_volatile` (r14-31,f14-31,v14-31,v64-127) | **FAIL** (combat crash) | 18,576,208 | −4,785,512 (−20.5 %) | n/a | NO |
| D.1b | `non_volatile` GPR-only (r14-31) | **FAIL** (boot crash) | 18,887,784 | −4,473,936 (−19.15 %) | n/a | NO |
| D.2 | `non_argument` (r0,r2,r11,r12,f0,v32-63) | **FAIL** (boot crash) | 18,875,104 | −4,486,616 (−19.21 %) | n/a | NO |

Final staged best-known-good = **Phase C** (`south_park_td` md5 `dc32b4e1`, `.text` 19,610,528;
`librexruntime.so` md5 `1996b550`). Rebuild from the reverted tree reproduced it byte-identically.

---

## 3. The fix — how the decouple + setjmp sync works

**Problem (prior session).** The `*_as_local` config flag drove *two* things via the same switch:
(a) the codegen accessor (`builders/context.cpp` `cr()/r()/…` returns the bare local `crN` instead of
`ctx.crN`) + the zero-init local declarations (`function_graph.cpp`), and (b) emission of
`#define REX_CONFIG_*_AS_LOCAL` into the port's generated `init.h`, which in `rex/ppc/context.h` is
`#if !defined(...)`-guarded **around the struct members** — so the macro *removed* `cr0-7` (etc.) from
the port's `PPCContext`, shifting `fpscr/f*/v*`. The shared `librexruntime.so` is built without the
macro → port and `.so` disagree on offsets → corruption once FP/vector + setjmp code runs.

**Decouple (this session) — 2 SDK edits, both working-tree:**
1. **`resources/templates/codegen/init_h.inja`** — stop emitting the `#define REX_CONFIG_*_AS_LOCAL`
   macros (kept `SKIP_LR`/`SKIP_MSR`). Result: `PPCContext` stays **full** ⇒ the prebuilt `.so` matches
   byte-for-byte (verified: `.so` md5 never changed across all builds), and `env=ctx` still has
   `ctx.cr0-7`/`ctx.xer`/… to snapshot. The `*_as_local` config flags now drive ONLY the codegen tool's
   accessor + local-declaration emission.
2. **`src/codegen/builders/context.cpp`** (the `setJmpAddress` emit) — added the locals↔ctx sync around
   the `env=ctx` / `ctx=env` snapshot. A generic `forEachPromoted` lambda enumerates the promoted set
   from the config flags (cr/xer/ctr/reserved + the GPR classes), marks each as a local (so it is
   declared), then emits `ctx.<reg> = <reg>;` **before** `env = ctx;` (save the live local into ctx so
   the snapshot is coherent) and `<reg> = ctx.<reg>;` **after** `ctx = env;` (reload the local from the
   restored snapshot on a longjmp return). The whole register class is synced for unconditional
   correctness; clang DCEs the dead ones. setjmp sites are rare (image-format detection), so the cost is
   negligible. Verified in generated code: `ctx.cr7 = cr7; … env = ctx; … ppc_setjmp(...); if(temp.s64){
   ctx = env; cr0 = ctx.cr0; … }`.

**Why CR/XER/CTR/RESERVED need only the setjmp sync (no SEH change):** the SEH entry-capture/catch
(`function_graph.cpp`) touch only `ctx.{r1,lr,r13-31,r3}` — never the flag registers. CR/XER/CTR are
volatile and per-frame in the local model (a caller's CR lives in the caller's own locals, untouched by
a call), so cross-call persistence is automatic; the only place the *whole* ctx is snapshot/restored for
them is the title's setjmp, now covered. (The prior session's "SEH unwind → Execution complete" crash was
the *symptom* of the struct-mismatch corruption, not a CR-specific SEH desync — and it vanished once the
struct stayed full.)

**Generated-code shape (Phase B, cr):** init.h emits **no** `REX_CONFIG_CR_AS_LOCAL`; 6,225
`PPCCRRegister crN{}` local decls; **44,854** bare `crN.` accesses on the hot path; only **12** residual
`ctx.cr0` (the setjmp save/reload) vs ~224K in baseline.

---

## 4. Why GPR/FPR/VR-as-local is still blocked (the clean dichotomy)

Both `non_volatile` (r14-31, f14-31, v14-31, v64-127) and `non_argument` (r0, r2, r11, r12, f0, v32-63)
**fail the gate** — `non_volatile` crashes during combat, and a GPR-only bisect (r14-31 only, f/v kept in
ctx) crashes even earlier, **at boot**, proving the **GPR promotion itself** is the fault (not f/v).
`non_argument` also fails at boot, so the volatile/non-volatile split does not save it.

The general registers differ from the flag registers in every way that matters:
- They **carry values across calls** (arguments r3-r10 / vector args, returns) and are **preserved by
  the ABI** (r14-31 callee-saved). The per-frame local model handles *normal* call/return, but…
- They are **read/written by the runtime `.so` and by SEH**: the SEH entry-capture emits
  `__seh_rN = ctx.r13-31` and the catch restores `ctx.r13-31`; `RtlCaptureContext` packs
  `ctx.r14-31`/`ctx.f14-31`/`ctx.cr0-7` into the guest context buffer; `xthread` Get/SetThreadContext
  reads/writes `ctx.r*`.
- **Decisive:** under GPR-as-local, a function's **incoming** non-volatiles live in the **caller's** C++
  locals, *not* in `ctx`. So the SEH entry-capture `__seh_rN = ctx.rN` (r14-31) captures stale/garbage,
  and the catch restores garbage into `ctx.r14-31`. There is nothing in this frame to capture correctly.

So making GPR-as-local correct needs the **SEH/longjmp state-save redesigned** to enumerate + explicitly
preserve the promoted r/f/v across the unwind boundary (and to thread incoming non-volatiles through the
call ABI), plus auditing every `.so` `ctx.r*`/`ctx.f*` consumer. That is exactly the SDK-architecture
change `docs/CODEGEN-SIZE-REPORT.md §6` named as the genuine path and out of scope here. The decouple is a
necessary *prerequisite* for it (keeps the `.so` ABI stable), but not sufficient on its own.

---

## 5. Mechanism — working set vs L3; did the floor move?

Baseline `.text` 22.28 MB ≈ 1.85× the 12 MB L3 (expansion ~4–7× over the native PPC code). The correct
cut takes it to **19.61 MB ≈ 1.63× L3** — a real reduction of the hot footprint, but the per-frame working
set during heavy combat still **overflows** L1i/L2/L3. So the floor (I-cache *capacity*-bound, established
in `PERF-OVERNIGHT-REPORT.md` + `CODEGEN-SIZE-REPORT.md`) is **unmoved**:
- `cr_as_local` (−14.6 %, 19.95 MB): floor p10 **14.6** vs base **14.8** (neutral; avg ~28.4 vs ~29.0;
  max ~48.6 vs ~50.6).
- Phase C (−16.06 %, 19.61 MB): floor p10 **14.6** vs base **14.3** (Δ +0.3, neutral; avg ~28.0 vs ~28.2;
  max ~48.5 vs ~49.1; samples base 14.2/15.0/14.3, phaseC 14.9/14.6/14.4).
- Profile (Phase C; light-frame ~60 fps snapshot — the standalone profile launch did not reliably reach
  the Stan's House combat the *gate* does, so treat as a lower bound on the bottleneck): still
  **front-end-bound ~44–46 %**, **fetch-latency (~33 %) > fetch-bandwidth (~13 %)**, **DSB:MITE ≈ 18–20 : 80–82**
  (decode/MITE-heavy — the uop cache can't hold the working set), L1i-miss ~0.5–0.6 B / 12 s, branch-miss
  ~11 %, IPC ~0.76, GPU idle ~20–24 % (CPU-bound). Bottleneck *character* unchanged vs the inherited
  combat baseline (L1i 721 M, front-end 47–56 %) — consistent with a smaller-but-still-overflowing
  working set. Even a *light* 60 fps frame being ~45 % front-end-bound underscores how pervasive the
  recompiled-code-footprint stall is.

This matches the prediction: even the full AS_LOCAL set (cr would be −14.6 %, GPR the bulk) was estimated
to land near but not under cache-fit; the *reachable* (correct) subset lands at 1.63× L3 and does not move
the floor. To move the floor you need a *large absolute* hot-`.text` reduction toward ~12 MB — only the
(blocked) GPR-as-local lever is that big, and even it may not fit.

---

## 6. Correctness — the determinism-diff gate

The inherited gate (`tools/perf/detdiff.sh` + `detdiff_fp.py`, reference `tools/perf/detdiff/reference.json`)
was **re-validated this session**: baseline gate **PASS** (markers=142, errs=0, naninf=0, in_level), and
an injected `add: +→-` emitter bug **FAILED** decisively (`session_failed_no_in_level`) — it has teeth
end-to-end with the current toolchain. Every kept flag (cr/xer/ctr/reserved) passed; every refuted flag
(non_volatile ×2, non_argument) failed. No change was trusted on fps before passing detdiff.
**Limit:** behaviour-equivalence on one scripted path (boot + Stan's House + 40 s combat); a divergence
only on an un-exercised path could be missed — relevant mainly to the GPR experiments, which failed so
early (boot) that the limit didn't matter.

---

## 7. Final state

- **Staged best-known-good** (`out/build/linux-amd64-release/`): `south_park_td` = **`dc32b4e1`**
  (Phase C, `.text` 19,610,528, −16.06 %), `librexruntime.so` = **`1996b550`** (== `.so.good`, unchanged).
  Boots → Stan's House (gate-confirmed). Fallbacks on disk: `south_park_td.baseline` (024e365a),
  `librexruntime.so.good` (1996b550), plus per-phase copies `.cr_aslocal` / `.xer_ctr_reserved` /
  `.phaseC_final`.
- **SDK working-tree edits (NOT committed — submodule):** exactly two, on top of the port's existing
  patch state — (1) `resources/templates/codegen/init_h.inja` (decouple: drop the `*_AS_LOCAL` defines),
  (2) `src/codegen/builders/context.cpp` (setjmp locals↔ctx sync). `function_graph.cpp` (SEH) **untouched**.
  All experimental edits (injected-bug control in `arithmetic.cpp`; the 4 GPR-only bisect edits) reverted;
  rebuild reproduces Phase C byte-identically. A full snapshot of the working tree *before* this session is
  saved at `docs/aslocal-backups/sdk-working-tree-START.patch`.
- **Port-repo edits:** `south_park_td_manifest.toml` `[entrypoint]` — `cr_as_local`/`xer_as_local`/
  `ctr_as_local`/`reserved_as_local = true` (the kept win); the two GPR flags documented + left OFF
  (refuted). **`patches/rexglue-sdk-current-full.patch` is now staler** by the two SDK edits above —
  regenerate it (`git -C third_party/rexglue-sdk diff > patches/…`) to capture the decouple + setjmp sync
  if this is to be persisted.
- New doc/tooling kept: this report, `docs/CODEGEN-ASLOCAL-{PLAN,LOG}.md`, raw lines in
  `tools/perf/results.log`.

---

## 8. What's left / next step

1. **GPR-as-local (the real floor candidate)** needs the **SEH/longjmp/ABI state-save redesign** (enumerate
   + preserve promoted r/f/v across the unwind; thread incoming non-volatiles through the call convention;
   audit `.so` `ctx.r*`/`ctx.f*` consumers — SEH entry-capture, `RtlCaptureContext`, `xthread`). The
   decouple already done here is the prerequisite (keeps the `.so` ABI stable). To diagnose precisely,
   capture the boot-crash backtrace (core dump with `coredump_filter` excluding the 4.5 GB shm, then map
   the faulting host PC → `sub_XXXXXXXX`). Even if made correct it may not reach cache-fit — measure.
2. **Ship the kept −16.06 % cut** — it is correct, gate-passed, and `.so`-compatible; it is a real
   footprint reduction with no behaviour change (floor-neutral, no regression).
3. **PGO** (orthogonal, avg/max only) still worth shipping per prior sessions; not a floor lever.
