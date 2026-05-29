# South Park recomp — codegen footprint-reduction plan (raise the combat floor toward native)

**You are a fresh session. Execute top-to-bottom, autonomously, no questions to the user.
Close it for a *correct + faster* result, not a checkmark.** This is a **recompiler-codegen
engineering** project, not post-link tuning. Project memory is auto-loaded — the anchors are
[[sp_combat_perf_frontend_bound]] (the floor is I-cache **capacity**-bound; post-link layout/size
tools are all floor-neutral — *do not re-try them*) and [[sp_devshm_xenia_memory_leak]].

> **Read first:** `docs/PERF-OVERNIGHT-REPORT.md` + `docs/PERF-OVERNIGHT-LOG.md` (the prior session).
> They *proved* the floor can't be moved by ICF/BOLT/PGO/PGO∘BOLT/-Os/.so-micro-opts/.so-BOLT — all
> floor-neutral. The lever that's left, and that this plan pursues, is the only one that attacks the
> proven root cause: **shrink the absolute hot-code footprint at the codegen level so more of the
> per-frame working set fits in cache.** PGO is still worth keeping for avg/max (orthogonal).

---

## 0. Prime directive & success criteria

**Goal:** reduce the recompiled hot `.text` footprint (port `.text` is **22.3 MB** ≫ 12 MB L3) by
changing *how the recompiler emits code*, to raise the **combat floor** (median p10 swaps/s, Stan's
House) toward what native PPC achieved on real hardware — **without changing observable game
behaviour**.

**A codegen change is KEPT only if ALL hold:**
1. **Correctness preserved** — passes the **determinism-diff gate** (§6): behaviour-equivalent to the
   known-good baseline on a fixed scripted run (no new errors/asserts/NaNs, same milestone sequence,
   same checkpoint state within the measured nondeterminism floor). **This gate is mandatory and
   ranks above fps** — a faster-but-wrong recomp is a failure. Divergence ⇒ auto-revert.
2. **Floor improves** — median p10 **> +1.0** swaps/s in the interleaved A/B (`tools/perf/ab.sh`),
   OR (acceptable secondary) a clear **code-size / L1i-miss reduction with no floor regression**
   (footprint wins compound across phases even when one alone is within fps noise — track both).
3. **Boots & GPU-idle** — `gamectl play` reaches `IN LEVEL`; `catch_dip` still shows GPU idle / CPU
   gating (we must not become GPU-bound).
Otherwise **revert** and log refuted/neutral.

**Cumulative:** each phase builds on the current best-known-good *codegen state*. Track it (§7).
**Definition of done:** the best floor achievable by footprint reduction, reported honestly with the
code-size + L1i-miss mechanism (Δ`.text`, Δicache-miss, ΔDSB:MITE), and a clear statement of how
close to "fits in cache" we got. Getting *partway* is an acceptable, honest outcome — the working
set may still exceed L3 after tractable wins; report the residual.

---

## 1. Authority & safety (full autonomy within these rails)

**You MAY freely:** edit the SDK codegen, regenerate, rebuild SDK + port + .so as many times as
needed; relaunch the game; `sudo` (password `<redacted>`); run long build/measure jobs in background
(`run_in_background`, poll the output file). The host is a disposable bench ([[host_test_bench]]).

**Hard rails (violating these wastes the project):**
1. **Correctness gate is non-negotiable.** Run the §6 determinism-diff after *every* codegen change
   before trusting any fps number. A change that diverges is reverted regardless of its speed.
2. **SDK source edits stay working-tree-only** — do NOT `git commit` the `third_party/rexglue-sdk`
   submodule (it's a working-tree patch). The codegen lives there; your edits join that patch.
3. **One game instance. Kill with `gamectl.sh kill` only** (it `pkill -x`'s and now reclaims the
   leaked `/dev/shm/xenia_memory_*` — keep that; the game leaks 4.5 GB of shm per launch and a full
   `/dev/shm` SIGBUSes the next launch — see [[sp_devshm_xenia_memory_leak]]).
4. **KILL before staging** any binary/.so (overwriting an mmap'd artifact under a live process
   SIGBUSes it). `ab.sh` already does kill→cp→play.
5. **Always back up before overwriting**; keep the known-good fallbacks (§7). Never leave a broken
   artifact staged. Before finishing, leave the best-known-good staged.
6. **Verify the log is ADVANCING before trusting fps** (`floor.sh` returns `stale|dead` — treat as
   invalid, relaunch, retry). `gamectl play` boot is intermittently flaky (KB doc 60) — it retries
   4×; on PLAY-FAILED, `gamectl kill` + retry once before declaring failure.
7. **Do NOT run builds during a measurement window** (steals cores, corrupts fps). Build, *then*
   measure.

---

## 2. What we already know (don't re-derive)

- **Floor is I-cache *capacity*-bound** (prior session, hard data): frontend-bound 45% (CP 52%),
  fetch-latency 32.6% ≫ bandwidth 14.5%, L1i-miss 614 M (3× dcache), IPC 0.78, DSB:MITE ≈ 17:83,
  GPU idle ~20% while Main + CP peg ~100% at floor dips. **Reordering/branch/≤4%-size cuts don't
  move it** (ICF/BOLT/PGO/PGO∘BOLT/-Os/.so-micro-opts/.so-BOLT all neutral). **Only a large
  *absolute* hot-`.text` reduction can** → this plan.
- **The blow-up is real & measured:** original `default.xex` ≈ 8.5 MB (whole exe; PPC code ~3–5 MB)
  → recomp `.text` **22.3 MB** (~4–7× expansion). The original ran this native code within Xenon's
  tiny caches at its design target; the recomp's bloat is what overflows the (larger!) PC cache.
- **Where the bloat is (empirical, 3.14 M lines of generated C++ across 50 TUs):**
  - **`ctx.<reg>` accesses ≈ 2.82 M (~0.9 per line)** — guest state round-trips through the context
    struct. **`ctx` is ALREADY `__restrict`** (`include/rex/ppc/context.h:50`:
    `#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)`), so the
    cheap noalias win is taken. The residual cost is mostly **cross-call spilling** (every guest `bl`
    passes `ctx&` → callee may touch any field → caller spills/reloads live guest regs around every
    call) — this is the deep lever (Phase 6). **`base` is NOT `__restrict`** — cheap lever (Phase 2).
  - **CR-flag updates ≈ 224 K** and **XER carry/overflow ≈ 163 K** — eager flag computation, much of
    it dead (computed then never consumed). Lazy emission (Phases 3–4) cuts these.
  - byte-swaps (1132) and memory translation (13) are **not** the bloat — handled cheaply by
    accessors (`REX_LOAD_U32`/`REX_STORE_U32`). (Re-confirm the accessor expansion in Phase 1.)
- **Codegen map** (`third_party/rexglue-sdk/src/codegen/`): per-instruction emitters in
  `builders/{arithmetic,comparison,logical,memory,control_flow,floating_point,vector,system,context}.cpp`;
  flags in `codegen_flags.{cpp,h}`; register/liveness in `phase_register.cpp` (24 KB — check what
  liveness it already computes); function/CFG in `function_graph.cpp`; emission in
  `codegen_writer.cpp`. Macros/types in `include/rex/ppc/context.h` (REX_FUNC, REX_LOAD/STORE).
- **Regeneration path** (`tools/build-linux.sh` steps 5–6): build+install SDK →
  `$SDK_INSTALL/bin/rexglue codegen south_park_td_manifest.toml` → `tools/fix_recomp_labels.py
  generated/default` → configure+build port (`cmake --preset linux-amd64-release
  -DCMAKE_PREFIX_PATH=$SDK_INSTALL`). **Header-only changes (REX_FUNC/REX_LOAD macros) need only a
  port rebuild — no regen** (cheap, ~1 min). **Emitter changes need a full regen** (rebuild the
  `rexglue` tool → re-run codegen → rebuild port; minutes). Order cheap-first.
- **Measurement & tooling** (`tools/perf/`, all built): `floor.sh`, `profile.sh`, `ab.sh` (interleaved,
  median p10, keep>+1.0), `catch_dip.sh`. `gamectl.sh {play|bench|kill}` (hardened: screenshot can't
  hang; auto-cleans shm). **The game has NO clean exit** (ignores SIGTERM/INT/WM_DELETE) — to dump an
  LLVM PGO profile, gdb-call `__llvm_profile_write_file()` on the live PID (see report §5).

---

## 3. Paths, tooling, measurement protocol

```
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME=$ROOT/out/build/linux-amd64-release          # staged port + .so + run.log
SDK=/home/h/src/recomp/rexglue-recomps/third_party/rexglue-sdk
SDK_BUILD=$SDK/out/build/linux-amd64 ; SDK_INSTALL=$SDK/out/install/linux-amd64
GEN=$ROOT/generated/default                        # 50 codegen'd .cpp (regenerated by `rexglue codegen`)
```
**Three metrics per change, in this priority:**
1. **Correctness** — §6 determinism-diff gate (pass/fail; auto-revert on fail).
2. **Footprint** — `size $GAME/south_park_td` (`.text`), and `profile.sh` L1i-miss / DSB:MITE /
   fetch-latency before→after. *These are the direct mechanism* — a real footprint cut is progress
   even if one phase's floor delta is within noise.
3. **Floor** — `ab.sh` interleaved, **SECS=90 REPS=3** (bump to 5 to break a tie near ±1.0; the floor
   is noisy ±2–3). Keep>+1.0.

Always: re-run `catch_dip.sh` after a kept win to confirm still GPU-idle / CPU-bound.

---

## 4. PHASE 0 — Build the correctness harness + re-baseline  [MANDATORY FIRST, ~60–90 min]

**This gate guards every later phase. Do it first and well.**
- **DO:**
  1. Host hygiene (from the report): governor=performance, no_turbo=0, perf_event_paranoid=-1,
     kptr_restrict=0. Back up known-good: `cp $GAME/south_park_td{,.baseline}`,
     `cp $GAME/librexruntime.so{,.good}`. Confirm baseline boots → combat.
  2. **Build the determinism-diff harness** (`tools/perf/detdiff.sh`), behaviour-equivalence — NOT
     bit-exact (a realtime multithreaded game is nondeterministic: frame timing, thread interleave,
     time-seeded RNG). Design:
     - Drive a **fixed scripted input** to a reproducible milestone (reuse `gamectl` nav → Stan's
       House → `always_win` match to a win/`camp_diagram`→win marker). Same script every run.
     - Capture a **semantic fingerprint** from `run.log`: the ordered sequence of state-transition /
       asset-load / kernel-call markers, plus **counts of `[error]`/`[warning]`/`assert`/`nan`/`inf`**
       and the win/level markers. (A wrong computation usually cascades into divergent control flow →
       a divergent marker sequence, a new warning/assert, or a NaN — this catches far more than a
       boot gate.)
     - **First, measure the nondeterminism floor:** run the baseline **twice**, diff the fingerprints.
       Whatever differs run-to-run (timestamps, fps, thread ids, benign ordering) defines the *noise
       mask*. The gate flags a candidate only on divergence **beyond** that mask (missing/extra
       milestone, new error/assert/NaN, different asset, crash, hang, wrong win-state).
     - **Optional stronger check (do if a stable region is findable):** at a fixed scripted checkpoint,
       hash a known timing-independent slice of guest state (e.g., a game-state struct in guest RAM at
       a fixed guest address) and compare. Only add if it's stable baseline-vs-baseline; else skip and
       note the limit.
     - Output a single verdict line: `DETDIFF status=pass|fail reason=...`.
  3. **Validate the gate itself:** baseline-vs-baseline ⇒ `pass`. Then, as a positive control,
     deliberately mutate one emitter to be wrong (e.g., flip an add to sub in `arithmetic.cpp`,
     regen, build) ⇒ the gate MUST report `fail`. If it can't catch an injected bug, strengthen it
     before proceeding. Revert the injected bug.
  4. **Re-baseline:** `profile.sh 12` + `floor.sh 90` → record BASELINE (floor p10, L1i-miss,
     DSB:MITE, `.text`). `catch_dip.sh` once.
- **DECIDE / BUDGET:** do not advance until the gate passes baseline-vs-baseline AND catches the
  injected bug. ~60–90 min. (If a stable state-hash proves infeasible, proceed with the
  marker+error+NaN fingerprint — log the limitation.)

---

## 5. THE QUEUE — conservative (semantics-preserving) first, then the deep lever

Each step: **GOAL / DO / DECIDE / BUDGET.** After each: append `tools/perf/results.log` + a narrative
to `docs/CODEGEN-SIZE-LOG.md` (create it); update the ledger (§6.1). **Gate every change with §6
detdiff before measuring fps.**

### PHASE 1 — Investigate the per-instruction x86 cost (no change yet)  [~1 h]
- **GOAL:** ground the later phases — the *source* pattern counts (§2) don't equal *x86* cost. Find
  what dominates the emitted machine code per guest instruction.
- **DO:** `objdump -d --disassemble=<sym>` a few **hot** funcs (use the prior session's perf/BOLT
  profile or a fresh `perf report` to pick them). For representative guest ops (load/store, add.,
  cmp, bl) count the x86 instructions emitted and classify: ctx load/store (spill), flag computation
  (CR/XER), memory accessor (does `REX_LOAD_U32` inline to one MOVBE or to base+mask+swap+branch?),
  call glue. Check **`phase_register.cpp`**: does it already compute GPR / **CR / XER liveness**?
  (That liveness is the enabler for lazy flags & register caching.)
- **DECIDE:** rank Phases 2–6 by measured x86 contribution; reorder/scope accordingly. Log the
  breakdown — it's the mechanism evidence for the report. **BUDGET:** 1 h.

### PHASE 2 — `base` `__restrict` + accessor review (CHEAP: header+rebuild, no regen)  [~45 min]
- **GOAL:** let clang prove guest-RAM stores don't alias each other / other state, so it caches more
  guest regs in host regs across loads/stores → fewer spills → smaller hot code.
- **DO:** in `include/rex/ppc/context.h`, add `__restrict` to the `base` param of `REX_FUNC`
  (and any guest-call thunk signature). Confirm soundness: `base` (guest shm RAM) and `ctx`
  (host struct) never overlap, and guest code never reaches the ctx via `base` → restrict is sound.
  Also inspect `REX_LOAD_U32`/`REX_STORE_U32` — if they emit more than a (MOVBE) load/store + the
  fixed base add, tighten them. **Port rebuild only** (no regen). detdiff → A/B → size/profile.
- **DECIDE:** keep if detdiff-pass AND (floor>+1.0 OR clear `.text`/L1i-miss drop, no regression).
  **BUDGET:** 45 min.

### PHASE 3 — Lazy CR-flag emission (codegen regen)  [~2 h]
- **GOAL:** stop computing CR (condition-register) bits that are never consumed (≈224 K eager
  updates). PPC `.`-form ops set CR0; many results are never read by a later branch.
- **DO:** using CR liveness (from `phase_register.cpp`; add it if absent — compute per-block which
  CR fields reach a consumer before being overwritten), gate flag emission in `codegen_flags.cpp` +
  the `.`-form paths in `builders/{arithmetic,logical,comparison}.cpp` so dead CR computation is
  elided; materialize CR only where consumed (or lazily from the operands at the consumer). Preserve
  exact CR semantics where live. **Full regen + rebuild.** detdiff (critical — flags are
  correctness-sensitive) → A/B → size/profile.
- **DECIDE:** keep if detdiff-pass AND footprint↓ (expect a real `.text`/L1i drop) with no floor
  regression; ideally floor↑. **BUDGET:** 2 h. If liveness is hard, scope to the obvious-dead cases
  first.

### PHASE 4 — Lazy XER carry/overflow (codegen regen)  [~1.5 h]
- **GOAL:** same for XER CA/OV (≈163 K). Many adds/subtracts compute carry/overflow never consumed.
- **DO:** mirror Phase 3 for XER in `codegen_flags.cpp` + arithmetic emitters; elide dead CA/OV.
  Keep carry chains correct (addc/adde/subfc sequences genuinely consume CA — don't break those).
  Regen + rebuild. detdiff → A/B → size/profile. **BUDGET:** 1.5 h.

### PHASE 5 — Memory-accessor / common-idiom compaction (cheap-ish)  [~1 h, if Phase 1 flagged it]
- **GOAL:** if Phase 1 found `REX_LOAD/STORE` or common idioms (e.g. load-byteswap, sign-extend,
  rlwinm masks) emitting bloated sequences, emit tighter forms (MOVBE, single-shift masks, host
  idioms). Header/emitter change as appropriate. **DECIDE:** keep clear footprint wins. **BUDGET:** 1 h.

### PHASE 6 — DEEP: cross-call register caching (codegen, high-risk/high-reward)  [~3 h, gated]
*Only after the conservative phases are banked and the budget remains.* This is the biggest lever
on the 2.82 M ctx round-trips but changes the internal calling convention.
- **GOAL:** keep hot guest GPRs in host registers / C++ locals across a function, syncing to `ctx`
  only at guest-call boundaries and function entry/exit — and, where the callee's clobber set is
  known, pass/preserve hot regs across `bl` without a full spill.
- **DO (incremental, each detdiff-gated):**
  1. **Within-function:** emit C++ locals for the live GPR set per function (mem2reg-friendly),
     load from `ctx` at entry, store back before each guest call and at return. (clang already does
     much of this via `__restrict ctx`; verify what's left — the win is across calls.)
  2. **Across calls:** define a register-cache discipline — at a `bl`, spill only regs live across
     the call (per `phase_register.cpp` liveness), not all 32. Requires per-call liveness; verify the
     ABI assumption that callees observe ctx only for their own live-in set.
  This is where subtle bugs hide → **lean hard on detdiff, soak longer, one sub-step at a time,
  revert aggressively.** Regen + rebuild each. A/B + size/profile.
- **DECIDE:** keep only with detdiff-pass + a clear footprint and/or floor win. If correctness gets
  shaky, **stop and revert** — bank the conservative wins; log Phase 6 as partial/deferred.
- **BUDGET:** 3 h. If blocked >1 h on correctness, revert and jump to §9.

### (Stacking) After kept phases, re-apply **PGO** on the new codegen and re-confirm avg/max (it was
the one prior win for averages; collect the profile via the gdb-dump method). Optional, end if time.

---

## 6. Correctness & bookkeeping

**Determinism-diff gate (the spine):** every codegen change → regen/rebuild → run `detdiff.sh`
baseline-vs-candidate. `fail` (divergence beyond the noise mask: new error/assert/NaN, missing/extra
milestone, different asset, crash, hang, wrong win, or — if the state-hash check is active — a state
mismatch) ⇒ **auto-revert that change, log "refuted: correctness".** Only on `pass` do you trust the
A/B fps. Re-validate the gate's sensitivity periodically (the injected-bug control).

### 6.1 Ledger (fill in `docs/CODEGEN-SIZE-LOG.md`)
| # | Codegen change | detdiff | Δ`.text` | ΔL1i-miss | base→cand floor p10 | Δ | Verdict | Kept? |
|---|---|---|---|---|---|---|---|---|
| P2 | base `__restrict` + accessors | | | | | | | |
| P3 | lazy CR flags | | | | | | | |
| P4 | lazy XER CA/OV | | | | | | | |
| P5 | accessor/idiom compaction | | | | | | | |
| P6 | cross-call register caching | | | | | | | |

Record `.text` and L1i-miss / DSB:MITE before→after the big wins — footprint reduction is the
mechanism; show the working set shrinking toward L3.

---

## 7. State tracking & recovery
- **best_so_far** = current best-known-good **codegen state** (which SDK edits are in) + the staged
  port/.so md5s + floor. Note it at the top of `docs/CODEGEN-SIZE-LOG.md`.
- **Fallbacks on disk:** `$GAME/south_park_td.baseline`, `$GAME/librexruntime.so.good`. Stage these
  two to return to a working game. Keep a copy of the *generated/* tree (or the SDK diff) for each
  kept codegen state so you can rebuild it.
- A wedged build dir → delete + reconfigure. If `gamectl play` flakes → kill + retry once.
- **SDK edits stay working-tree-only.** When done, note that `patches/rexglue-sdk-current-full.patch`
  is further behind (regenerate from the SDK dir with the project's usual
  `git diff > patches/rexglue-sdk-current-full.patch`).

## 8. Time budget & stop conditions
- Target ~10–12 h. Conservative phases (0–5) ~7 h; deep Phase 6 ~3 h. **If a phase overruns, bank
  state and MOVE ON.** Front-load Phase 0 (the gate) and Phases 2–4 (the tractable footprint wins).
- **Stop early & report if:** the floor reaches ~55–60 p10 (solved), OR `catch_dip` shows the GPU
  became the limiter (~100% — converted CPU-bound→GPU-bound = success-to-ceiling), OR correctness
  can't be held (revert to best-known-good, report the safe wins).
- Always leave the best-known-good combo staged + booting before finishing.

## 9. FINAL REPORT (`docs/CODEGEN-SIZE-REPORT.md`)
1. **TL;DR:** baseline `.text`/floor → best `.text`/floor; which codegen changes were kept; DoD
   (how close to "fits in cache" / native; honest residual).
2. **Ledger** (§6.1 filled): per change — detdiff verdict, Δ`.text`, ΔL1i-miss, floor Δ, kept/reverted.
3. **Mechanism:** `.text` and L1i-miss / DSB:MITE / fetch-latency before→after; show the working set
   shrinking and whether it changed the cache-fit story. Quantify the expansion-ratio improvement
   vs the ~4–7× baseline.
4. **Correctness:** how the determinism-diff gate was built, its sensitivity (injected-bug control),
   and that all kept changes passed it; note its limits (behaviour-equivalence, not bit-exact).
5. **Final host/build state, staged artifacts (md5s), SDK working-tree edits & patch-regen note.**
6. **What's left:** remaining footprint sources, whether native parity is reachable, next ideas
   (interpreter-reference bit-exact gate, more idiom compaction, profile-guided codegen, X3D
   big-L3 validation run).
7. **Update memory** [[sp_combat_perf_frontend_bound]] with which codegen levers moved the footprint
   and the floor, and by how much — so the *next* session starts from the answer.

Then send the user a concise summary.

---
### Appendix — cheat sheet
```
# regen after an EMITTER change (Phases 3,4,6): rebuild rexglue -> codegen -> fixup -> build port
cmake --build $SDK_BUILD --target install --parallel && \
  ( cd $ROOT && $SDK_INSTALL/bin/rexglue codegen south_park_td_manifest.toml ) && \
  python3 $ROOT/tools/fix_recomp_labels.py $GEN && \
  ( cd $ROOT && cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH=$SDK_INSTALL ) && \
  cmake --build $ROOT/out/build/linux-amd64-release --parallel
# HEADER-only change (Phases 2,5 macros): just rebuild the port (no regen)
cmake --build $ROOT/out/build/linux-amd64-release --parallel
# gate + measure
tools/perf/detdiff.sh                       # MUST pass before trusting fps
size $GAME/south_park_td                    # .text footprint
TARGET=$GAME/south_park_td tools/perf/ab.sh 90 3 base $GAME/south_park_td.baseline cand <candidate>
tools/perf/profile.sh 12 ; tools/perf/catch_dip.sh 27 3
```
```
```
