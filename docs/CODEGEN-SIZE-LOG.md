# South Park recomp — codegen footprint-reduction LOG (CODEGEN-SIZE-PLAN execution)

Running narrative + ledger for the codegen-level `.text`-shrink effort. Companion to
`docs/CODEGEN-SIZE-PLAN.md`; raw A/B lines append to `tools/perf/results.log`.

## best_so_far (current best-known-good codegen state)
- **Baseline (unchanged):** port `south_park_td` md5 `024e365a`, `librexruntime.so` md5 `1996b550`.
- **`.text` = 23,361,720 B (≈22.28 MB).** Staged in `out/build/linux-amd64-release/`.
- SDK working tree: original patch state (no net codegen change yet).
- Fallbacks on disk: `south_park_td.baseline` (024e365a), `librexruntime.so.good` (1996b550).

---

## PHASE 0 — correctness harness + re-baseline

### detdiff gate (built)
- `tools/perf/detdiff_fp.py` — semantic fingerprint extractor (markers/assets/warnings/errors/
  NaN-Inf/pipelines/pacing/reached, normalized to strip timestamps/thread-ids/host-addresses).
- `tools/perf/detdiff.sh` — drives a FIXED scripted session (gamectl play boot+nav → Stan's House,
  then a fixed combat dwell), captures the fingerprint, compares to a baseline reference whose
  noise mask is derived empirically (required = intersection across K baselines, allowed = union).
  Behaviour-equivalence, NOT bit-exact. Verdict: `DETDIFF status=pass|fail reason=...`.
  - **Harness fix:** does NOT block on `gamectl play` returning — under a detached (no job-control)
    shell, play's tail parks in `do_wait` on the never-exiting game. detdiff launches play in the
    background and polls run.log for the deterministic nav milestones instead, then reclaims the
    game (which also frees the stuck wrapper) via an EXIT trap.

### nondeterminism floor (measured)
- 3 baseline runs (dwell 40s) were **bit-identical** in the fingerprint:
  markers=142, assets=8, warnings=20, errors=0, naninf=0, pipelines=16, pacing≈19, in_level=true.
- `required == union == 142` markers → essentially **empty noise mask**; the gate is clean+strict.
- **DETDIFF-SELFCHECK status=pass** (all 3 baselines gate `pass` vs the reference).

### injected-bug positive control
- _pending_ (flip `build_add` `+`→`-` in `builders/arithmetic.cpp`, regen+build, gate MUST fail).

### baseline footprint/floor snapshot (measured this session, 2026-05-29)
- **`.text` = 23,361,720 B (22.28 MB).**
- **Floor: min=10.7, p10=13.5, avg=30.2, max=57.5** (`floor.sh 120`, heavy=yes).
- Mechanism: frontend-bound 47% (Main) / 56% (CP); **fetch-latency 33% ≫ bandwidth 14%**;
  **L1i-miss 721 M** (~3× dcache 230 M); **DSB:MITE = 3.22 B : 19.4 B ≈ 14:86**; IPC 0.78.
- catch_dip @20.8 fps: GPU 26% (idle), Main 99.9%, CP 90.9% → **CPU/frontend-bound, GPU idle** ✓.
- Consistent with prior session (`docs/PERF-OVERNIGHT-REPORT.md`): the I-cache *capacity* wall.

---

## PHASE 1 — per-instruction cost + the real lever (investigation, no change)

**Key finding — the footprint lever is already built into the codegen, gated OFF.** The recompiler
has a dormant "registers/CR/XER as C++ locals" mechanism (XenonRecomp heritage). When a register is
"local", the emitter declares it as a zero-init C++ local (`function_graph.cpp:626-661`) and the
accessors (`builders/context.cpp` `r()/cr()/xer()/...`) emit the bare name `cr0` instead of
`ctx.cr0`. clang's mem2reg + DCE then:
- **eliminates dead flag computations** — the ~224 K eager CR0 updates (`emitRecordFormCompare` →
  `ctx.cr0.compare<int32_t>(...)`) and ~163 K XER CA/OV writes that are never consumed (Phases 3–4);
- **keeps live registers in host registers across the function**, cutting the 2.82 M `ctx.<reg>`
  round-trips (Phase 6); save/restore helper calls become no-ops (`context.cpp:197-201`).

**The flags are TOML manifest keys** (parsed in `config.cpp`, default false), settable in the
port-repo manifest (no SDK commit): `cr_as_local`, `xer_as_local`, `ctr_as_local`,
`reserved_as_local`, `non_argument_as_local`, `non_volatile_as_local`.

**Correctness hazard (decisive for risk-ordering):** locals are zero-init with **NO ctx-sync** at
boundaries. This title uses **SEH (77 scopes) + setjmp/longjmp** (manifest) pervasively:
- setjmp does `env = ctx` / `ctx = env` (`context.cpp:181-185`) — a stale snapshot if regs are
  locals (misses them);
- the SEH catch path restores `ctx.r1/lr/r13-31` directly (`function_graph.cpp:615-671`) — but code
  reads from locals → restore ineffective.
⇒ **GPR/FPR/VR-as-local (`non_volatile`/`non_argument`) is HIGH risk** here (Phase 6).
⇒ **CR-as-local / XER-as-local is LOWER risk** (volatile; not setjmp/SEH-preserved; generally
  write-before-read within a function) — the tractable Phases 3–4 win.
The detdiff window loads many image assets through the setjmp-based JPEG/TGA/PNG decoder dispatch,
so it directly exercises the riskiest path — good sensitivity for these changes.

**Revised approach:** implement Phases 3/4/6 by *enabling the dormant config flags*, each
detdiff-gated + measured (footprint + floor), instead of hand-rolling liveness. Fall back to
hand-rolled dead-flag elision (elide `emitRecordFormCompare` when CR0 is provably dead, keeping CR0
in ctx — no calling-convention change) if a flag fails the gate. Order by risk: xer/cr/ctr/reserved
first, then (gingerly) non_argument/non_volatile.

---

## PHASE 3+ — register/flag-as-local experiments (detdiff-gated)

### cr_as_local — REFUTED (correctness), but pivotal evidence
- Enabled `cr_as_local` in the manifest → regen. Verified: `ctx.cr` accesses **0** (all CR via
  16,283 function-local `PPCCRRegister crN{}`), clang DCE'd the dead CR0 updates.
- **`.text`: 23,361,720 → 19,950,608 B = −3,411,112 (−14.6%)** — by far the biggest lever; CR flags
  ARE the dominant bloat (confirms §2's ~224K eager CR0 updates, much dead).
- **detdiff = FAIL** (`session_failed_no_in_level`). Crash log: `SEH: unwind through sub_824499A0
  -> return to caller` → `Execution complete` during early init. CR-as-local **desyncs from ctx
  across the SEH unwind** (the catch restores `ctx.{r1,lr,r13-31}` directly; the title uses SEH on 77
  scopes). Auto-reverted per the gate. **The win is real but must be captured WITHOUT desyncing ctx
  → pivot to hand-rolled dead-CR0 elision (keeps CR in ctx, elides only provably-dead CR0 writes).**
- Corollary (same SEH mechanism): `non_volatile_as_local` (r14-31 local) will also break SEH;
  `xer/ctr/reserved/non_argument` are volatile/scratch NOT touched by the SEH catch → likely safe.

### xer_as_local + ctr_as_local + reserved_as_local — REFUTED too (same SEH/setjmp cause)
- Δ`.text` = −730,364 (−3.13%); flags verified (ctx.xer/ctr/reserved → 0). **detdiff = FAIL**, same
  symptom as cr (`SEH: unwind through sub_824499A0 -> Execution complete` in early init). So the
  failure is NOT CR-specific: **the codegen's register-as-local machinery is systemically incompatible
  with this title's setjmp `env=ctx`/`ctx=env` snapshot + SEH ctx-restore — it never syncs locals into
  ctx at those boundaries.** ⇒ **ALL `*_as_local` flags are a dead end here.** Reverted.

### CONCLUSION on AS_LOCAL: dead end (correctness). PIVOT → outline the flag boilerplate.
The footprint win is real (CR flags alone are 14.6% of `.text`) but cannot be captured by moving state
out of ctx (breaks setjmp/SEH). Capture it instead by **keeping flags in ctx but de-duplicating the
emitted boilerplate into a shared out-of-line copy** — identical semantics, no desync possible.

### outline CRRegister::compare (header-only, `[[gnu::noinline]]`) — _testing_
- `CRRegister::compare<T>` was `inline` → expanded at ~224K record-form/cmp sites. Marked
  `[[gnu::noinline]]` so the linker folds it to one copy/type and the sites become direct calls.
  Shrinks the per-frame I-cache working set with identical semantics. Safe by construction.

## 6.1 Ledger
| # | Codegen change | detdiff | Δ`.text` | ΔL1i-miss | base→cand floor p10 | Δ | Verdict | Kept? |
|---|---|---|---|---|---|---|---|---|
| P2 | base `__restrict` + accessors | | | | | | | |
| P3a | cr_as_local (clang DCE all CR) | **FAIL** (SEH unwind) | −3.41 MB (−14.6%) | — | — | — | refuted: correctness | NO |
| P4a | xer_as_local + ctr + reserved | **FAIL** (SEH unwind) | −0.73 MB (−3.13%) | — | — | — | refuted: correctness | NO |
| P3b | outline CRRegister::compare (`[[gnu::noinline]]`, CR in ctx) | pass(semantics) | **+0.17 MB (REGRESSION)** | — | — | — | refuted: compare too compact, call-ABI > inline | NO |
| P3c | dead-CR0 elision (per-fn no-cr0-read, CR in ctx) | _(not gated)_ | **−896 B (~0)** | — | — | — | negligible: clang already DCEs in-ctx dead stores | NO |
| P3d | cr_as_local + sync-locals↔ctx-at-setjmp | **build FAIL** | — | — | — | — | impossible: REX_CONFIG_CR_AS_LOCAL removes ctx.cr0-7 → nothing to sync | NO |
| P2 | base `__restrict` (header-only) | **pass** | −80 B (≈0) | — | — | — | neutral: ctx already `__restrict` | NO |
| P6 | non_volatile/non_argument as local | _predicted FAIL_ | (−large) | | | | same mechanism: removes ctx.r14-31 the SEH catch + setjmp need | (untested) |
| P5 | accessor/idiom compaction | _deferred_ | | | | | Phase 1: REX_LOAD/STORE already compact (MOVBE) | |
