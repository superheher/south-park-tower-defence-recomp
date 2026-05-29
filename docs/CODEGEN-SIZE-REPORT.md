# South Park recomp — codegen footprint-reduction report

**Date:** 2026-05-29 (autonomous session). **Goal:** reduce the recompiled hot `.text`
(22.28 MB ≫ 12 MB L3) **at the codegen level** to raise the combat floor (median p10 swaps/s,
Stan's House) toward native — **without changing observable behaviour** (gated by a mandatory
determinism-diff). Successor to `docs/PERF-OVERNIGHT-REPORT.md` (which proved post-link tools are
floor-neutral; the floor is I-cache **capacity**-bound).

---

## 1. TL;DR

- **Baseline:** `.text` **23,361,720 B (22.28 MB)**, combat floor **p10 = 13.5** (min 10.7, avg 30.2,
  max 57.5), frontend-bound 47–56 %, L1i-miss 721 M, DSB:MITE ≈ 14:86, IPC 0.78, GPU idle (~26 %).
- **Best result: the unchanged baseline.** **No codegen change cleared the keep-bar** (floor > +1.0
  OR a clear footprint cut with no regression AND detdiff-pass). Staged best-known-good = baseline
  port `024e365a` + `librexruntime.so` `1996b550`.
- **The single dominant footprint lever was found and quantified:** the codegen's built-in
  **register/flag "as C++ local" promotion** (`cr_as_local` etc.). `cr_as_local` ALONE cuts
  **−3.41 MB (−14.6 %)** of `.text` — confirming the eager CR flags are *the* largest bloat source.
- **…but it is fundamentally incompatible with THIS title and was refuted by the determinism gate.**
  Every `*_as_local` flag (cr / xer / ctr / reserved) crashes the game identically (early-init
  image-format detection → `SEH: unwind … → Execution complete`). Root cause (two reinforcing
  reasons, both proven, §3): (a) the `REX_CONFIG_*_AS_LOCAL` macros live in the **port's** generated
  `init.h` and **remove fields from `PPCContext`**, but the prebuilt runtime `.so` is *not* rebuilt
  with them → **port↔.so struct-layout mismatch**; (b) the title's **setjmp** does `env=ctx`/`ctx=env`
  (snapshot/restore the WHOLE ctx) and **SEH** captures `ctx.{r13-31}` at entry — both assume every
  guest register lives in ctx, which AS_LOCAL violates.
- **Safe, in-ctx alternatives yield ≈ 0** (§3): outlining the CR compare *regressed* (+0.17 MB —
  it's too compact, call-ABI > inline); per-function dead-CR0 elision was −896 B (clang already DCEs
  the in-ctx dead stores it can); `base __restrict` was −80 B (ctx was already `__restrict`).
- **Definition of Done (honest):** the combat floor was **not** moved, and **cannot** be moved by
  *safe* codegen footprint reduction within the current SDK design. The only lever that materially
  shrinks the hot working set (AS_LOCAL register promotion, which also attacks the 2.82 M ctx
  round-trips) requires an **SDK-level redesign** of the setjmp/SEH state-save + a runtime-`.so`
  rebuilt with matching context layout — out of scope here, but it is the genuine path forward (§6).

---

## 2. Ledger

| # | Codegen change | detdiff | Δ`.text` | base→cand floor | Verdict | Kept? |
|---|---|---|---|---|---|---|
| Re-baseline | — | pass (self-consistent ×3) | 23,361,720 B | p10 13.5 | baseline | — |
| P3a | `cr_as_local` (CR fields → C++ locals, clang DCE) | **FAIL** (setjmp/SEH) | **−3,411,112 (−14.6 %)** | n/a | refuted: correctness | NO |
| P4a | `xer_as_local`+`ctr_as_local`+`reserved_as_local` | **FAIL** (setjmp/SEH) | −730,364 (−3.13 %) | n/a | refuted: correctness | NO |
| P3b | outline `CRRegister::compare` (`[[gnu::noinline]]`) | pass (semantics) | **+169,746 (REGRESSION)** | n/a | refuted: compact op, call-ABI > inline | NO |
| P3c | per-function dead-CR0 elision (CR stays in ctx) | (not gated) | **−896 (≈0)** | n/a | negligible: clang already DCEs in-ctx | NO |
| P3d | `cr_as_local` + sync-locals↔ctx-at-setjmp | **build FAIL** | — | — | impossible: macro removed `ctx.cr0-7` → nothing to sync | NO |
| P2  | `base __restrict` (header-only) | pass | **−80 (≈0)** | n/a | neutral: ctx already `__restrict` | NO |
| P6  | `non_volatile_as_local` (r14-31 → locals) | **won't compile** | (−large) | — | SEH entry-capture hardcodes `ctx.r13-31` (removed by macro) | NO (by inspection) |
| P6  | `non_argument_as_local` (r0/r2/r11/r12 → locals) | — | (−modest) | — | structurally doomed: same port↔.so struct mismatch | NO (by inspection) |
| P5  | accessor/idiom compaction | — | — | — | deferred: Phase 1 found `REX_LOAD/STORE` already compact (MOVBE) | — |

---

## 3. Mechanism — why the win exists but can't be taken safely

**The bloat is real and CR-dominated.** `cr_as_local` (CR0-7 emitted as zero-init C++ locals instead
of `ctx.cr*`) lets clang's mem2reg+DCE eliminate the dead eager CR0 updates AND keep live CR in host
registers. Effect: `ctx.cr` accesses 224 K → **0**, `.text` **−14.6 %**. That single flag proves the
~224 K eager record-form CR0 compares are the largest single contributor to the footprint.

**Why AS_LOCAL cannot be enabled here — two reinforcing, proven blockers:**

1. **Port↔runtime-`.so` `PPCContext` layout mismatch.** `cr_as_local` makes the codegen emit
   `#define REX_CONFIG_CR_AS_LOCAL` into the **port's** `generated/default/<title>_init.h`. In
   `include/rex/ppc/context.h`, that macro is `#if !defined(...)`-guarded around the `cr0..cr7`
   members — so the **port's** `PPCContext` loses those fields (shifting `fpscr`, `f0-31`, `v0-127`
   offsets). The shared runtime `librexruntime.so` is **not** rebuilt with that macro (it is built
   from SDK sources, before/independent of codegen, and the macro is port-side only), so the `.so`
   keeps the full struct. Port and `.so` then disagree on every shifted offset. The game boots until
   FP/vector + setjmp-heavy code (image-format detection during profile load) runs, then corrupts.
   The `.so` *cannot* simply adopt the macro: it contains hand-written code that reads `ctx.cr*`.

2. **The title's setjmp/SEH assume all guest state lives in `ctx`.** setjmp (`builders/context.cpp`)
   emits `env = ctx` (snapshot) / `ctx = env` (restore-on-longjmp) — a whole-`PPCContext` copy. With
   registers in locals there is nothing in `ctx` to snapshot, so a longjmp resurrects stale state.
   SEH (`function_graph.cpp`) captures `__seh_r13..r31 = ctx.r13..r31` at entry and restores them in
   the catch — `non_volatile_as_local` removes those very fields, so it **won't even compile**. A
   "sync locals↔ctx around setjmp" fix is impossible because the macro already deleted `ctx.cr0-7`.

   The gate caught all of this: every `*_as_local` build reached early init then logged
   `SEH: unwind through sub_824499A0 → return to caller` / `Execution complete` (the title aborting
   via its first-cut SEH recovery) — never reaching the level. `DETDIFF status=fail`.

**Safe in-`ctx` reductions are negligible** because clang already optimizes what it can while CR/XER
remain escaping `ctx` memory (passed by `&` to every callee): per-function dead-CR0 elision = −896 B,
`base __restrict` = −80 B, and *outlining* the compare *grew* `.text` (+0.17 MB: the op is only ~7
compact instructions inline; the 4-arg call sequence + folded body is larger). The win requires the
operands to become **non-escaping locals** — i.e. exactly the AS_LOCAL promotion that's blocked.

**Cache-fit story:** baseline `.text` 22.28 MB is ~1.85× the 12 MB L3; expansion ratio over the
~3–5 MB native PPC code is ~4–7×. Even the −14.6 % `cr_as_local` cut (→ 19.03 MB) would still exceed
L3 — it might dent the floor but not "fit." Fitting needs the 2.82 M ctx round-trips reduced too
(GPR-as-local), which is the same blocked mechanism. So no *tractable, safe* lever reaches cache-fit.

---

## 4. Correctness — the determinism-diff gate

Built `tools/perf/detdiff.sh` + `detdiff_fp.py` (Phase 0). It drives a **fixed scripted session**
(boot → nav → Stan's House → 40 s combat dwell) and compares a **semantic fingerprint** of `run.log`
(ordered game-logic markers; asset-load set; warning/error/critical sets; NaN/Inf count; GPU pipeline
set; pacing/in-level reached) to a baseline reference whose **noise mask is empirical** (required =
intersection across K baselines, allowed = union). Behaviour-equivalence, **not** bit-exact.

- **Self-consistency:** 3 baseline runs were **bit-identical** (142 markers, 0 errors, 0 NaN);
  all gate `pass`. Empty noise mask → a clean, strict gate.
- **Sensitivity (injected-bug positive control):** flipping `+`→`-` in the `add` emitter, regen+build
  → the gate **failed** decisively (new error `HalReturnToFirmware`, 25 missing milestones, 7 missing
  assets, not-in-level, pacing-stalled). It cannot be fooled by a wrong emitter.
- **Hardening:** `run_session` polls `run.log` (doesn't block on `gamectl play`'s hanging tail) and
  **re-verifies the final log is still in-level after the dwell** — this caught a candidate that
  crashed mid-dwell, preventing a false pass.
- **Every refutation in §2 was gate-caught.** No change was trusted on fps before passing detdiff.
- **Limits:** behaviour-equivalence on one scripted path (boot + Stan's House + 40 s), not a
  bit-exact interpreter reference; a divergence only on an un-exercised path could be missed.

---

## 5. Final host & build state

- **Host knobs:** governor `performance`, `no_turbo=0`, `perf_event_paranoid=-1`, `kptr_restrict=0`.
- **Staged best-known-good (`out/build/linux-amd64-release/`):** `south_park_td` = **024e365a**
  (== `south_park_td.baseline`), `librexruntime.so` = **1996b550** (== `librexruntime.so.good`).
  Boots → Stan's House (detdiff-confirmed).
- **SDK working tree: reverted to original patch state** — all experimental edits removed
  (`builders/arithmetic.cpp`, `builders/context.cpp`, `function_graph.cpp`, `include/rex/ppc/context.h`).
  **No net SDK source change**, so `patches/rexglue-sdk-current-full.patch` is no more out of date than
  before. The port manifest `south_park_td_manifest.toml` carries only **comments** documenting the
  refuted `*_as_local` flags (all flags OFF) — no behavioural change.
- **New tooling kept (harness, no game-behaviour change):** `tools/perf/detdiff.sh`,
  `tools/perf/detdiff_fp.py`, `tools/perf/regen_build.sh`, `tools/perf/measure_baseline.sh`.
- Build cadence: codegen change ≈ 140 s; `context.h` (header) change ≈ 800 s (rebuilds SDK runtime
  + all 50 port TUs).

---

## 6. What's left / the genuine path to the floor

1. **The real fix (SDK-architecture change, where the −14.6 %+ lives):** make AS_LOCAL viable by
   (a) rebuilding `librexruntime.so` with the **same** `REX_CONFIG_*_AS_LOCAL` macros as the port (so
   the `PPCContext` layouts match) and porting the `.so`'s hand-written `ctx.cr*`/`ctx.r*` accesses;
   and (b) redesigning **setjmp/longjmp and SEH** to save/restore the local-promoted registers
   explicitly (enumerate them into the setjmp buffer / SEH frame) instead of snapshotting the whole
   `ctx`. Then `cr_as_local` (−14.6 %), `xer_as_local`, and especially `non_volatile/non_argument`
   (the **2.82 M ctx round-trips** — the bulk of `.text`) become available, plausibly approaching
   cache-fit and a real floor lift. This is the highest-value next step but it is recompiler-runtime
   engineering, not a config tweak.
2. **PGO (orthogonal, avg/max only):** the prior session showed PGO raises avg/max with no floor
   regression; ship it for smoother typical play (collect via gdb-dump of `__llvm_profile_write_file`
   on the live PID — the game has no clean exit). Not a floor lever.
3. **Spend the idle GPU (~26 % at dips):** higher internal resolution / filtering is ~free on the
   CPU-bound floor.
4. **Don't re-try:** any `*_as_local` flag as-is, outlining the compare, in-ctx dead-flag elision,
   or the post-link tools (ICF/BOLT/PGO/-Os) — all measured neutral/negative here or prior.

---

## 7. Memory

`sp_combat_perf_frontend_bound` updated: the codegen footprint lever is the built-in
register/flag-as-local promotion (`cr_as_local` = −14.6 % `.text`), but it is correctness-blocked on
this title by (a) port↔runtime-`.so` `PPCContext` layout mismatch and (b) the whole-`ctx` setjmp/SEH
state model; safe in-`ctx` reductions are ≈0; the floor is unmoved; the genuine path is an SDK-level
setjmp/SEH + runtime-layout redesign. Full detail: `docs/CODEGEN-SIZE-LOG.md`, raw lines
`tools/perf/results.log`.
