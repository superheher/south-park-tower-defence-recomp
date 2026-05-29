# South Park recomp — next-session runbook: push the combat floor past the BTB-capacity wall

> This runbook supersedes the previous one. The **`-mcmodel=large` indirect-call storm is FIXED and
> shipped** (see "Where we are"). The remaining levers below were each **read-code investigated AND
> adversarially verified** this session (2026-05-29) — the verdicts (incl. two myth-busts and one big
> new finding) are baked into the priorities. Read `docs/FLOOR-MCMODEL-MEDIUM-REPORT.md` and
> `docs/FLOOR-GPR-ASLOCAL-GONOGO.md §8` first, plus memory `sp_mcmodel_floor_fix`.

## Where we are
Static recompilation (rexglue-sdk) of South Park: Let's Go Tower Defense Play! (XBLA) → native
Linux/Vulkan, fully playable (boot→match→win). Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`)
+ submodule `south-park-recomp` (port, `main`). SDK edits live as the working-tree patch
`patches/rexglue-sdk-current-full.patch`. Commit identity `superheher <heh@vivaldi.net>`, on `main`,
**NO Co-Authored-By trailer** (the user removed them); prior sessions **commit, do NOT push** unless
asked. Host: i9-8950HK (6c/12t, ~12MB L3, 256KB L2/core, Coffee Lake **~4K-entry BTB**, 16-entry RSB),
sudo password `<redacted>`, disposable test bench.

**Best-known-good = the medium-model build:** `south_park_td` md5 `bef1b65c`, `.text` 18,653,832,
`librexruntime.so` md5 `1996b550` (unchanged). Phase C fallback staged as `south_park_td.phaseC_final`
(md5 `dc32b4e1`). Commits: port `986d0c5`, super `3b89092` (committed, **not pushed**).

## The corrected diagnosis (settled, measured)
The combat floor is **branch-prediction-bound**, and it has TWO layers:
1. **Misprediction FLUSHES (FIXED):** the recomp exe was built `-mcmodel=large`, so every guest
   call/branch was `movabs $imm64; call *reg` / `jmp *reg` — ~20k+ indirect sites swamping the ~4K BTB.
   `near_call` was **70 %** of mispredicts @ 65 % rate (NOT "conditional 69 %" — that earlier split was
   wrong; conditional is 2.3 %). Fix = `-mcmodel=large → medium` (recomp exe only). Result: direct calls
   **1 → 85,220**; execution mispredicts **−79 %**; floor `ab.sh` 8-rep median p10 **14.35 → 15.0
   (+0.65)**; avg +0.6; `.text` −4.9 %; gate-pass.
2. **Front-end RESTEERS (residual, the new wall):** BACLEARS fell only **39.3 % → 32.8 %**. Even a
   *direct* branch needs a BTB entry to be predicted; ~**157k** direct call/jmp sites still
   over-subscribe the ~4K BTB → cold/evicted-entry resteers. This is a genuine **predictor-capacity**
   wall (a desktop CPU with a bigger BTB will floor higher — the user's "it's an i9, absurd" intuition).
   The floor is +0.65, **UNDER the +1.0 keep-bar**. Layer 2 is what the levers below attack.

## Goal
Move the combat-floor p10 further. **Keep-bar (per lever): detdiff gate PASSES, it boots, and median
p10 improves > 1.0 swaps/s on `ab.sh` (re-confirm with the resteer counters — do not trust fps alone).**
Levers are ordered by verified confidence × payoff. **Measure each; do not assume.**

---

## P0 — Make the resteer wall MEASURABLE (cheap, do first; no game-code change)
Every lever below must be judged on the *resteer* counter, not just fps. But the obvious event is
missing on this uarch: **`FRONTEND_RETIRED.BRANCH_RESTEER` does NOT exist on Coffee Lake i9-8950HK**
(verified: `perf stat -e FRONTEND_RETIRED.BRANCH_RESTEER` → "Unable to find event"). Use the confirmed
fallbacks and a subtraction:
- Working events: `BACLEARS.ANY` (total FE resteers), `INT_MISC.CLEAR_RESTEER_CYCLES`,
  `MACHINE_CLEARS.COUNT`, `FRONTEND_RETIRED.{L1I_MISS,ITLB_MISS,DSB_MISS}`, `ICACHE_64B.IFTAG_MISS`,
  `br_misp_retired.{near_call,conditional,all_branches}`.
- **`residual_BTB_resteers ≈ BACLEARS.ANY − (cache/iTLB/DSB FE misses)`** isolates the BTB-capacity
  component. Add these to `tools/perf/floor_rootcause.sh` (it already has the TMA L3 split + a place
  for raw events around lines 39–43). This makes each P1–P3 go/no-go a measured BTB-resteer delta, not
  a noisy fps guess.

## P1 — Apply `-mcmodel=medium` to `librexruntime.so` (highest-confidence win)
**The exact same pathology is still live in the runtime `.so`, untouched.** Verified: `librexruntime.so`
inherits the global `-mcmodel=large` (`third_party/rexglue-sdk/CMakeLists.txt:~93`) and has
**221,677 `movabs` + 142,767 indirect calls** (34× the exe's residual). The `.so` is ~30 % of
whole-process self-time and owns the CP-thread hot funcs (`CommandProcessor::WriteRegister`,
`TimerQueue::TimerThreadMain` ~8.7 %). This is the largest untapped indirect-call population.
- **Change:** give the `rexruntime` target `-mcmodel=medium`. It does **not** call
  `rexglue_apply_target_settings` — add `target_compile_options(rexruntime PRIVATE -mcmodel=medium)`
  in `third_party/rexglue-sdk/src/system/CMakeLists.txt` (after the target is defined, ~line 102), OR
  flip the global `CMakeLists.txt:93` `large→medium` (broader; also hits other SDK libs).
- **CAVEAT (don't over-expect):** the `.so` is **PIC** (shared lib, `CMAKE_POSITION_INDEPENDENT_CODE
  ON`). Medium fixes **intra-`.so`** calls (PC-relative direct) but cross-module calls still go via
  GOT/PLT. So the win is on the `.so`'s *internal* call graph (which is most of the 142k). Consider
  also `-fvisibility=hidden` / `-Bsymbolic` to de-PLT internal calls — measure separately.
- **Risk/process:** rebuilding the `.so` **changes its md5** (the pinned `1996b550` BKG) → it needs its
  OWN detdiff gate + the full `regen_build.sh full` path. Stage the current `.so` as a fallback first.
- **Measure:** `regen_build.sh full` → `detdiff.sh gate so_medium 40` (must pass) →
  `TARGET=$GAME/librexruntime.so ab.sh 90 5 base <good.so> cand <new.so>` (note TARGET is the **.so**
  here, not the exe) → re-run the P0 counters (expect CP-thread/avg win; possibly floor via the
  secondary CP gate). **Expected: strong avg/CP win; floor uncertain (CP is the secondary gate).**

## P2 — Selective leaf-inlining to cut the hot branch-SITE count (THE floor lever)
This is the only lever that directly attacks the **residual BTB-capacity wall** (fewer static branch
sites → fewer BTB entries needed → fewer cold/evicted resteers, and shallower call depth).
- **MYTH BUSTED (verified twice, independently):** the per-function `__attribute__((weak,noinline))`
  (`REX_WEAK_FUNC`, `include/rex/ppc/context.h:52`) is **NOT** required for dispatch, SEH, or overrides.
  Dispatch uses symbol *names* (`registrar->SetFunction(addr, sub_X)` resolved through the weak alias to
  the out-of-line `__imp__sub_X`); SEH uses C++ try/catch + entry register-snapshots (`function_graph.cpp`)
  independent of inlining; overrides need only `weak`. **Dropping `noinline` does not break any of them**
  (the out-of-line `__imp__` + weak alias still exist for the table). So inlining is genuinely feasible.
- **How (don't just blanket-remove noinline — that risks code-size/compile-time blowup across 47 huge
  TUs):** make the codegen mark an **inline-eligible** subset — tiny **leaf** functions (no `bl`/`bctr`/
  `bctrl`/`blrl`, no setjmp, no SEH/mid-asm-hook, body < ~64 B like `sub_8244CE40`=31 B) — as inlinable
  (emit without `noinline`, or as an `inline`/`always_inline` header body the recompiler can pull into
  direct-call sites) while KEEPING the out-of-line `__imp__` + weak alias for the dispatch table and
  overrides. Codegen lives in `src/codegen/` (`DEFINE_REX_FUNC` in `resources/templates/codegen/
  init_h.inja:69`; the leaf/terminator classification already exists in `function_scanner.cpp` /
  `function_graph.cpp`). Alternatively evaluate **ThinLTO** across the recomp TUs (no `-flto` today) to
  let the linker inline hot leaves globally.
- **Risk:** `.text` blowup + compile-time (start with the smallest leaf subset and a size threshold;
  measure `.text` each step). Correctness: keep the out-of-line copy; gate hard (the detdiff gate caught
  an injected `add→sub` before).
- **Measure:** `regen_build.sh full` → `detdiff.sh gate leaf_inline 40` → `ab.sh 90 5` floor +
  P0 resteer counters (the metric to move is **BACLEARS / residual-BTB-resteers down**, not just fps).
  **Expected: the most likely floor mover, but real engineering effort + size risk.**

## P3 — Re-test PGO on the medium build (compose with P2; likely avg-leaning)
Prior PGO was floor-neutral, but **on the old large-model tree** (indirect calls → PGO branch-weights
couldn't help target prediction). Now the front-end is flush-light but resteer/BTB-bound, so PGO's
hot/cold code layout + branch ordering *might* improve code locality enough to cut cold-BTB resteers.
- The "noinline caps PGO" worry is **also busted** (see P2) — PGO can compose with the inline-eligible
  subset for real cross-call layout wins.
- **Procedure** (from `docs/PERF-OVERNIGHT-LOG.md`): build instrumented → collect a combat profile by
  **gdb-dumping `__llvm_profile_write_file()` from the live PID** (the game has no clean exit) → merge →
  build optimized → gate → A/B floor+avg + P0 counters. Use the `*-pgo-*` presets.
- **Expected: avg win likely; floor only if locality reduces resteers — measure, don't assume.**

## DEAD ENDS — verified this session, **do NOT re-try**
- **Widen indirect-dispatch / jump-table detection:** CONFIRMED dead. Only 134 of 8,632 `bctr` are
  detectable switches (1.6 %); the backward scan stops at `bl`/`bctrl` terminators
  (`function_scanner.cpp:1411`), so widening `backward_scan_limit` is < 1 % payoff. The residual ~24k
  indirect calls are genuine VTable dispatch (runtime targets, not statically knowable).
- **`-mcmodel=small` / drop `-Wl,--no-relax`:** CONFIRMED safe but **~nil payoff**. Small model is safe
  (17.82 MB, non-PIE, no 64-bit relocs) but the 6,558 residual `movabs` are hardcoded **constants**
  (guest mask `0xffffffff7df00000` ×2346, FP/bit patterns ×4212), not data refs — small can't remove
  them. `--no-relax` has zero effect (no PLT32/GOTPCRELX relocs to relax). Only worth it as a trivial
  size tidy, not for the floor.

## Discipline (mandatory)
- **detdiff gate ranks ABOVE fps:** `tools/perf/detdiff.sh gate <label> 40` → must be `status=pass`
  (it has teeth — caught an injected `add→sub`). Never trust fps on a gate-fail.
- **Floor A/B (interleaved, median p10, heavy windows):** for an **exe** change,
  `TARGET=$GAME/south_park_td tools/perf/ab.sh 90 5 base south_park_td.phaseC_final cand <new>`; for a
  **.so** change, `TARGET=$GAME/librexruntime.so ... base <good.so> cand <new.so>`. Use ≥5 reps (the
  floor noise is ~±10 %; the medium win was a clean but small +0.65, so small deltas need reps). Always
  re-confirm with the P0 resteer counters (`branch_breakdown.sh` / `floor_rootcause.sh`).
- **Rebuild:** `tools/perf/regen_build.sh full` after a codegen/flag change (it rebuilds+installs the
  SDK, regenerates, fixes labels, configures+builds the port). The cmake/flag change must reach the SDK
  *install* dir, which `full` handles.
- **HOST GOTCHAS (cost hours):** (a) the harness **BLOCKS the literal token `sleep` in a Bash command
  string** → put waits in a **script file** and run the file; (b) the game is **reaped when its
  launching shell ends** → boot+profile must be ONE command/script; (c) **only ONE game instance**
  (shared `live_input.txt`); (d) `gamectl.sh kill` auto-cleans the 4.5 GB `/dev/shm` leak; (e)
  `branch_breakdown.sh`/`floor_rootcause.sh`/`cache_levels.sh` are **non-executable** — run with an
  explicit `bash tools/perf/<x>.sh`.

## Success / STOP
- **SUCCESS** = a gate-passing change with median p10 **+ > 1.0** AND a measured BACLEARS/resteer drop
  → commit (no co-author trailer), update `FLOOR-MCMODEL-MEDIUM-REPORT.md` + memory; ask before pushing.
- **Partial win** (e.g. another clean but < +1.0 floor gain, or an avg/CP win from P1) is still worth
  committing per the "commit successful work" rule — just report the honest numbers.
- **STOP** if P1+P2+P3 each fail to drop resteers or move the floor: write the honest report (what was
  tried, measured resteer/fps deltas, why) and recommend **confirming the BTB-capacity hypothesis on a
  desktop CPU with a larger BTB** (run the same `ab.sh`/`branch_breakdown.sh` on a Zen4/recent Core and
  compare p10 — if it floors materially higher with the *same* binary, the residual wall is hardware
  predictor capacity, not codegen, and the floor is done on this host).

## Separate, independent issue (do NOT conflate with the floor)
The **"sinusoid"** (sim speed oscillates) is a **pacing** bug (guest paces on present/GPU progress,
`++counter_` per swap in `command_processor.cpp` `XE_SWAP`, IMMEDIATE present). NOT the fps floor.
Several fixes tried + reverted (memory `rexglue_recomps_port`). Only touch if explicitly asked.

**Read first:** `docs/FLOOR-MCMODEL-MEDIUM-REPORT.md`, `docs/FLOOR-GPR-ASLOCAL-GONOGO.md §8`, memory
`sp_mcmodel_floor_fix` + `sp_gpr_aslocal_nogo` + `sp_combat_perf_frontend_bound` + `rexglue_recomps_port`.
Tools: `tools/perf/{branch_breakdown,floor_rootcause,detdiff,ab,regen_build}.sh`.
</content>
