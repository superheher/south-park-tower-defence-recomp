# South Park recomp — overnight autonomous framerate plan

**You are a fresh session. Execute this top-to-bottom, fully autonomously, no
questions to the user. Close it for a *good result*, not a checkmark.** When
done, write the final report (§9). Keep going until the queue is exhausted or the
global time budget (§8) is hit. If something is ambiguous, pick the most
defensible option, log it, and proceed — never stop to ask.

Project memory is auto-loaded; the key fact is [[sp_combat_perf_frontend_bound]].
This doc supersedes/expands it with the exact runbook.

---

## 0. Prime directive & success criteria

**Goal:** raise the *combat* framerate floor on Stan's House toward 60 swaps/s,
without regressing boot→menu→level.

- **Metric = the FLOOR** (`p10`/`min` swaps/s over a long combat window), NOT
  average. Combat is non-stationary (fps swings 20→50 with wave density); avg is
  noise. Use `tools/perf/floor.sh` and the interleaved A/B in `tools/perf/ab.sh`.
- **A change is KEPT only if ALL hold:** (1) median `p10` improves by **> 1.0
  swaps/s** vs the interleaved baseline in the same A/B run; (2) boot→menu→level
  still works (the A/B's `gamectl play` prints `IN LEVEL`, else PLAY-FAILED);
  (3) GPU stays idle (`catch_dip.sh` still shows GPU% low — we must not shift the
  bottleneck onto the GPU and call it a "win"). Otherwise **revert** and log it
  as refuted/neutral.
- **Cumulative best config:** each phase builds on the current best-known-good
  binary/.so. Track it explicitly (§7).
- **Definition of done (per the user's brief):** stable ~60 in combat, **or as
  close as possible** — this is a CPU-front-end / code-size–bound recomp (proven
  below), so "as close as possible" is an acceptable, honest outcome. Report the
  best achieved floor and the cumulative config.

---

## 1. Authority & safety (you have full autonomy — within these rails)

**You MAY, freely and without asking:** rebuild the SDK and the port as many
times as needed; relaunch the game any number of times; install packages with
`sudo` (password `<redacted>`); run long `perf`/build/measurement jobs (use
`run_in_background: true` and poll the output file); change CPU/GPU host knobs.
The host is a disposable bench (see [[host_test_bench]]).

**Hard rails (violating these wastes the whole night):**
1. **Never break boot→menu→level.** After every new binary/.so, the A/B's
   `gamectl play` verifies it reaches combat. If a variant fails to boot or load,
   **auto-revert** to last-known-good and log "refuted: regressed boot/load".
2. **Do not touch the frame limiter / pacing / loading path.** The drops are NOT
   a pacing problem (proven §2). Editing `ExecutePacketType3_XE_SWAP`'s limiter,
   sleeping the CP thread, or gating per-swap has broken boot/load 3+ times
   historically. Leave it alone.
3. **One instance. Kill with `pkill -x south_park_td` only** (never `-f` — matches
   your shell). `gamectl.sh kill` does this.
4. **KILL before staging a binary/.so.** Overwriting an mmap'd `librexruntime.so`
   or `south_park_td` under a live process SIGBUSes it. `ab.sh` already does
   kill→cp→play in that order; follow the same pattern in any ad-hoc staging.
5. **Always back up before overwriting.** Keep `south_park_td.baseline` and the
   known-good `librexruntime.so`. Never leave a broken artifact staged.
6. **SDK source edits stay working-tree only** (do NOT `git commit` the
   `third_party/rexglue-sdk` submodule). They're already a working-tree patch.
7. **Verify the log is ADVANCING before trusting any fps number** (a hung/closed
   game freezes its last `pacing-diag` line). `floor.sh` checks this and returns
   `status=stale|dead` — treat those as invalid, relaunch, retry.
8. **Restore host knobs at the very end** is optional (bench is disposable) but
   record their final state in the report.

---

## 2. What we already know (don't re-derive — it's solid)

Measured this evening, high confidence:

- **NOT GPU-bound.** At every fps≈20 dip the GPU engine is ~18–22% busy and
  already at max clock (sclk 1004MHz). The two hot threads are the guest sim
  (`Main`, 99%) and the GPU command-processor (`GPU`/CP, ~90%). GPU has ~80%
  headroom.
- **The bottleneck is the CPU FRONT-END (instruction fetch), not memory/data.**
  Intel Topdown: frontend-bound **44.8%** overall — `Main` **48.6%** (backend/
  memory only **4.4%**!), CP **52.4%**. Bad-speculation 7.5%, retiring ~28%,
  IPC≈**0.79**. Supporting counters: **L1-icache-misses ≈421M** (≈2× dcache),
  **branch-miss ≈11%**. The port's **`.text` is 22.3 MB** — larger than the whole
  12 MB L3. That is the root cause: the hot recompiled-PPC code thrashes L1i/L2,
  and indirect guest branches mispredict.
- **Ruled out (don't redo except the cheap re-confirm in P6):**
  - CPU clock/turbo: floor ~20 at *both* 2.9 and 4.2 GHz (frontend-latency is
    largely clock-independent). Turbo gives only a marginal edge.
  - Thread pinning: made it **worse** (floor 15.4 vs 19.7) — scheduler beats it.
  - TLB / huge pages: dTLB miss **0.055%** (non-issue); shmem THP huge pages
    don't even map on the `MAP_FIXED` shm views. Dead end.
  - Cross-core contention / false sharing: `perf c2c` = 358 local HITM over 12s,
    0 remote. Not a factor.
- **Implication:** the only real lever is **reducing/relaying-out hot
  instruction footprint** — code layout (BOLT), profile-guided optimization
  (PGO), identical-code folding (ICF), size flags. The dominant cost (`Main`,
  48.6% FE) lives in the **port binary** (the codegen'd guest), so port-level
  work matters most; the CP (`.so`) is secondary but cheap to iterate.

**Baseline to beat (current staged build = GetRegisterInfo `.so`):** combat
floor `p10 ≈ 14–16`, `min ≈ 13–14`, `avg ≈ 28–30`, `max ≈ 50` (heavier-wave
session) — or `p10 ≈ 20, avg ≈ 32` in a lighter session. **Re-measure your own
baseline in P0** (waves differ run-to-run); always compare within one interleaved
A/B, never against these absolute numbers.

---

## 3. Paths, tooling, measurement protocol

```
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME=$ROOT/out/build/linux-amd64-release         # port binary + librexruntime.so live here
SDK=/home/h/src/recomp/rexglue-recomps/third_party/rexglue-sdk
SDK_BUILD=$SDK/out/build/linux-amd64             # incremental .so build dir (exists)
SDK_INSTALL=$SDK/out/install/linux-amd64         # lib64/librexruntime.so installs here
GEN=$ROOT/generated/default                      # 50 codegen'd guest .cpp (95M)
```
- Game control: `tools/gamectl.sh {play|bench|kill|fps|shot}` (boot retries +
  drives into Stan's House combat in ~60–70s; `play` prints `IN LEVEL` on success).
- Perf tooling (already built tonight, in `tools/perf/`): `floor.sh [secs]`,
  `profile.sh [secs]`, `ab.sh`, `catch_dip.sh`. `ab.sh` appends to
  `tools/perf/results.log`.
- Linker is **ld.lld** (so `-Wl,--icf=all`, `-Wl,--emit-relocs` are available).
- Compiler is `clang-20` (→ clang 21.1.8). Release preset adds `-O3 -DNDEBUG`,
  no LTO/ICF/PGO currently.

**Measurement protocol (use this for every comparison):**
- A/B via `ab.sh` with **SECS=90 REPS=2** (bump REPS=3 if a result is within
  ~1.5 of baseline and you need to break a tie). It interleaves variants per rep
  to cancel drift, relaunches fresh each time, discards non-heavy windows, and
  reports **median p10 per variant**.
- Before/after a *big* change, also snapshot `profile.sh` (topdown + counters) so
  the report can show *why* it helped (e.g., L1i-miss ↓, frontend% ↓).
- **Do NOT run builds during a measurement window** — a parallel build steals
  cores and corrupts fps. Build, *then* measure. (Background a build, wait for it
  to finish, then start the A/B.)

---

## 4. Build / stage procedures (exact)

### 4a. SDK / librexruntime.so (CP-thread changes) — cheap (~20s)
```
# edit under $SDK/src ... then:
cmake --build $SDK_BUILD --config Release --target install --parallel
# stage (KILL FIRST):
tools/gamectl.sh kill; cp -f $SDK_INSTALL/lib64/librexruntime.so $GAME/librexruntime.so
```
First time, snapshot the current good one: `cp $GAME/librexruntime.so $GAME/librexruntime.so.good`.

### 4b. Port relink only (ICF / --emit-relocs) — cheap (objects exist, just links)
Reconfigure the existing build dir with extra linker flags, then build (relinks):
```
cmake $GAME -DCMAKE_EXE_LINKER_FLAGS="-Wl,--icf=all"        # or "-Wl,--emit-relocs -Wl,-q"
cmake --build $GAME --parallel                              # relink (fast)
```
Back up the binary first: `cp $GAME/south_park_td $GAME/south_park_td.baseline` (once).
Reset linker flags by reconfiguring with `-DCMAKE_EXE_LINKER_FLAGS=""`.

### 4c. Port recompile (size/PGO flags) — heavier (recompiles ≤56 TUs incl. 50 huge codegen .cpp; minutes)
Use a SEPARATE build dir so the baseline build stays intact (preset binaryDir is
fixed, so configure manually mirroring the preset):
```
cmake -S $ROOT -B $ROOT/out/build/linux-amd64-EXP -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
  -DCMAKE_PREFIX_PATH=$SDK_INSTALL \
  -DCMAKE_CXX_FLAGS="<your experiment flags>" -DCMAKE_EXE_LINKER_FLAGS="<...>"
cmake --build $ROOT/out/build/linux-amd64-EXP --parallel
# the binary is $ROOT/out/build/linux-amd64-EXP/south_park_td ; stage it like 4b.
```
(`CMAKE_PREFIX_PATH=$SDK_INSTALL` is REQUIRED so the port finds the SDK — see
`tools/build-linux.sh` step 6.)

### 4d. Port codegen rebuild — only if you ever change codegen; NOT needed for this plan.
`tools/build-linux.sh` does the full flow. Avoid unless a hypothesis demands it.

### Staging a port binary for A/B
`ab.sh` swaps whatever `TARGET` points at. For port experiments:
`TARGET=$GAME/south_park_td tools/perf/ab.sh 90 2 base $GAME/south_park_td.baseline cand <path-to-candidate>`

---

## 5. THE EXPERIMENT QUEUE (execute in order; each: GOAL / DO / DECIDE / BUDGET)

> After each step, append a line to `tools/perf/results.log` and a narrative
> entry to `docs/PERF-OVERNIGHT-LOG.md` (create it). Update the ledger (§6).

### PHASE 0 — Setup & re-baseline  [MANDATORY FIRST, ~20 min]
- **DO:**
  1. Host: `sudo` set governor=performance, `no_turbo=0`, `perf_event_paranoid=-1`,
     `kptr_restrict=0`. Confirm `radeontop` & `perf` present (install if not:
     `sudo dnf install -y radeontop perf`).
  2. **Install BOLT + profdata now** (needed in P2/P4):
     `sudo dnf install -y llvm-bolt llvm` → gives `llvm-bolt`, `perf2bolt`,
     `llvm-profdata`. Verify each is on PATH; if `perf2bolt` is missing, it may be
     `llvm-bolt --aggregate-only` mode or in `/usr/lib/llvm*/bin` — locate it.
  3. Back up known-good: `cp $GAME/librexruntime.so $GAME/librexruntime.so.good`;
     `cp $GAME/south_park_td $GAME/south_park_td.baseline`.
  4. `gamectl play` → combat. Run `tools/perf/profile.sh 12` and
     `tools/perf/floor.sh 90` → record as **BASELINE**. Run `catch_dip.sh 27 3`
     once → confirm GPU-idle/CPU-bound still holds.
- **DECIDE:** if `profile.sh` front-end L2 split says **fetch_bandwidth** (DSB/
  MITE) dominates rather than **fetch_latency** (icache), still proceed — BOLT/
  PGO/ICF all help both; just note it to weight P5 (alignment) higher.
- **BUDGET:** 20 min.

### PHASE 1 — ICF relink (port)  [cheap, do first; also preps relocs for BOLT] [~45 min]
- **GOAL:** fold identical functions (codegen'd PPC has many) → shrink the 22.3 MB
  `.text` → less I-cache pressure.
- **DO:** relink per §4b with `-Wl,--icf=all`. Record `.text` via
  `size $GAME/south_park_td` (expect shrink). A/B: base vs icf (TARGET=binary).
- **DECIDE:** keep if floor `p10` improves >1.0 AND boots. If `--icf=all` breaks
  boot/behaviour (function-pointer identity), retry `-Wl,--icf=safe`. If neither
  helps, revert flags, log neutral. **If kept, this binary becomes the new
  baseline for P2.**
- **BUDGET:** 45 min.

### PHASE 2 — BOLT post-link layout (FLAGSHIP, targets the exact bottleneck) [~2.5 h]
- **GOAL:** reorder the 22.3 MB `.text` by a real combat profile so hot funcs/
  blocks cluster → big drop in I-cache/iTLB misses + branch resteers. No recompile.
- **DO:**
  1. Relink the (best-so-far) port binary WITH relocations (BOLT needs them):
     §4b with `-DCMAKE_EXE_LINKER_FLAGS="-Wl,--emit-relocs -Wl,-q"` (keep `--icf`
     too if P1 was kept). Back up this relinked binary.
  2. Collect a combat profile with LBR branch records:
     `gamectl play` (reach combat), then
     `perf record -e cycles:u -j any,u -o /tmp/sp/bolt.perf -p $(pgrep -x south_park_td) -- sleep 120`
     — make sure heavy waves occur during the 120s (watch fps; re-record if it was
     all-light). LBR exists on this Coffee Lake CPU.
  3. `perf2bolt -p /tmp/sp/bolt.perf -o /tmp/sp/bolt.fdata $GAME/south_park_td`
     (use the EXACT binary that was running). If perf2bolt complains about missing
     relocs → the relink in step 1 didn't take; fix and re-collect.
  4. `llvm-bolt $GAME/south_park_td -o /tmp/sp/sp.bolt -data=/tmp/sp/bolt.fdata \
        -reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions \
        -split-all-cold -icf=1 -dyno-stats` (note the printed dyno-stats
     improvement estimate).
  5. A/B: `TARGET=$GAME/south_park_td ab.sh 90 2 base <relinked-baseline> bolt /tmp/sp/sp.bolt`.
  6. Re-run `profile.sh` on the BOLT binary → expect frontend% and L1i-miss down.
- **DECIDE:** keep if floor improves & boots & GPU idle. If `llvm-bolt` errors on
  some funcs, add `-skip-funcs=<regex>` or drop `-split-all-cold`; retry. If
  perf2bolt/LBR unavailable, fall back to `-pa`/no-LBR aggregation
  (`perf record -e cycles:u` without `-j`, then `perf2bolt`), lower quality but
  works. **If kept, BOLT binary = new best baseline.**
- **BUDGET:** 2.5 h. If blocked >1h on tooling, log partial and jump to P3, return
  later if time.

### PHASE 3 — CP `.so` micro-opts (fast iterate; independent of port) [~1.5 h]
Each: edit `$SDK/src/graphics/...`, build .so (§4a, ~20s), A/B
(TARGET=`$GAME/librexruntime.so`, base=`librexruntime.so.good`, cand=new). Keep
wins; they stack. The CP is 52% front-end-bound, so shrinking its per-command hot
code helps.
- **3a. Hot/cold split `CommandProcessor::WriteRegister`** (command_processor.cpp
  ~L438): the gamma/DC_LUT/COHER/scratch cases are RARE but bloat the function
  that runs on *every* register write. Move them to a `[[gnu::cold]] __attribute__
  ((noinline))` `WriteRegisterSlow(index,value)` and keep the hot path tiny
  (bounds + volatile store + the already-gated debug check; call slow-path only
  for the rare index ranges). Preserve exact behaviour.
- **3b. Trace-writer in hot path:** check whether `trace_writer_.Write*` does real
  work in normal play (it's for trace capture). If it's not a no-op, gate the
  hot-path trace calls (WAIT_REG_MEM `WriteMemoryRead`, etc.) behind a
  `trace_writer_.is_open()`-style check.
- **3c. Branch hints / attrs:** `[[likely]]`/`[[unlikely]]` and `[[gnu::hot]]` on
  `ExecutePacket`/`ExecutePacketType3`/`ExecutePacketType0`/`WriteRegister` and the
  common packet cases.
- **3d. `ExecutePacketType3` dispatch:** verify it compiles to a jump table (not a
  comparison chain); if a chain, reorder cases by observed frequency (instrument
  briefly or infer from PM4 docs: DRAW_INDX, SET_CONSTANT, REG_RMW, WAIT_REG_MEM
  are common).
- **DECIDE:** keep each win; revert neutrals. **The cumulative .so** becomes the
  new `librexruntime.so.good`. NB: this stacks with port wins (orthogonal threads).
- **BUDGET:** 1.5 h.

### PHASE 4 — PGO on the port (+ PGO∘BOLT combo: gold standard) [~3.5 h]
- **GOAL:** profile-guided layout + inlining + block ordering across the whole
  guest code — the highest-ceiling front-end fix. Then BOLT the PGO binary (PGO
  feeds inlining/codegen, BOLT perfects final layout; they compose).
- **DO (instrumented PGO; clang-only, no extra tools beyond llvm-profdata):**
  1. Configure gen build (§4c, separate dir `-pgo-gen`) with
     `-DCMAKE_CXX_FLAGS="-fprofile-generate=/tmp/sp/pgo"`
     `-DCMAKE_EXE_LINKER_FLAGS="-fprofile-generate=/tmp/sp/pgo"`. Build (slow).
  2. Stage that binary (back up baseline first), `gamectl play`, play combat
     ~4 min (instrumented = slow fps, that's fine — we need counts over heavy
     waves; optionally drive a couple wave cycles). Then `gamectl kill` so
     profiles flush. `.profraw` files land in `/tmp/sp/pgo`.
  3. `llvm-profdata merge -o /tmp/sp/port.profdata /tmp/sp/pgo/*.profraw`.
  4. Configure use build (separate dir `-pgo-use`) with
     `-DCMAKE_CXX_FLAGS="-fprofile-use=/tmp/sp/port.profdata -Wno-profile-instr-out-of-date -Wno-backend-plugin"`. Build.
  5. A/B base vs pgo (TARGET=binary). Snapshot `profile.sh`.
  6. **Combo:** if PGO kept, BOLT the PGO binary (P2 steps 1–4 on the pgo-use
     binary, requires its own `--emit-relocs` relink + fresh combat profile). A/B
     pgo vs pgo+bolt; keep the best.
- **DECIDE:** keep best of {baseline, bolt, pgo, pgo+bolt}. If instrumented build
  fails/OOMs or profraw doesn't write, try AutoFDO instead (sampling): build
  normal+`-fdebug-info-for-profiling -funique-internal-linkage-names`, `perf
  record` combat, `create_llvm_prof` → `-fprofile-sample-use` (needs
  `create_llvm_prof`; build from the autofdo repo if dnf lacks it — only if time).
- **BUDGET:** 3.5 h. This is the deep-night job; background the builds.

### PHASE 5 — size/layout compile flags (stack onto best, if time) [~1 h]
On a separate build dir, try (A/B each vs current best port binary):
- `-Os` (or `-O2`) on the 50 codegen TUs only (smaller code → better I-cache);
  if CMake can't scope per-dir easily, try whole-binary `-O2` vs `-O3`.
- `-freorder-blocks-and-partition -ffunction-sections -fdata-sections`
  + `-Wl,--gc-sections` (verify gc-sections actually on; dead-strip).
- ThinLTO `-flto=thin` (may be slow/RAM-heavy on 95M of source; abort if it OOMs).
- **DECIDE:** keep only clear wins; these are lower-probability than BOLT/PGO.
- **BUDGET:** 1 h (skip if PGO+BOLT already near GPU/ceiling).

### PHASE 6 — hygiene & re-validate [~45 min]
- **6a. Timer spin → blocking** (low fps value, clean hygiene): in
  `$SDK/src/core/timer_queue.cpp`, the `dp::spin_wait_strategy` busy-spins (~8.5%
  of samples doing nothing). disruptorplus has `blocking_wait_strategy.hpp`. Swap
  the three `spin_wait_strategy` types → `blocking_wait_strategy` (+ include).
  Build .so. **CRITICAL: verify boot→menu→level still works** (timer feeds guest
  timing). A/B floor + confirm boot. Keep ONLY if no regression; else revert.
- **6b.** Re-run clean turbo A/B (floor, back-to-back) to quantify the marginal
  clock effect with the final binary. Re-run `catch_dip.sh` to CONFIRM GPU is
  still idle and a CPU thread still gates (we didn't become GPU-bound).
- **BUDGET:** 45 min.
- *(Do NOT attempt the Audio `WaitMultiple` poll rewrite — it's core threading
  used by boot/load; high regression risk, low fps value. Skip.)*

### PHASE 7 — Synthesize, stage best, report. (§9)

---

## 6. Bookkeeping — the hypothesis ledger (fill this in `PERF-OVERNIGHT-LOG.md`)

| # | Hypothesis / change | Scope | Result (median p10 base→cand) | Δ | Verdict | Kept? |
|---|---|---|---|---|---|---|
| P1 | ICF `--icf=all` | port link | | | confirmed/refuted/neutral | |
| P2 | BOLT layout | port post-link | | | | |
| 3a | WriteRegister hot/cold split | .so | | | | |
| 3b | trace-writer gating | .so | | | | |
| 3c | hot/cold attrs + branch hints | .so | | | | |
| 3d | Type3 dispatch ordering | .so | | | | |
| P4 | PGO | port recompile | | | | |
| P4 | PGO∘BOLT | port | | | | |
| P5 | size/layout flags | port | | | | |
| 6a | Timer spin→blocking | .so | | | | |
| 6b | turbo (re-confirm) | host | | | | |

Record absolute floor numbers AND the topdown frontend% before vs after the big
wins, so the report shows mechanism, not just fps.

---

## 7. State tracking & recovery

- **best_so_far** = the staged combo that currently holds the record. Maintain a
  note at the top of `PERF-OVERNIGHT-LOG.md`: which `.so` and which port binary
  are the current best, with their md5s and floor.
- **Fallbacks always on disk:** `$GAME/librexruntime.so.good` (GetRegisterInfo,
  known-good CP) and `$GAME/south_park_td.baseline` (known-good port). If anything
  is broken or uncertain, stage these two and you're back to a working game.
- If a build dir gets wedged, delete it and reconfigure (§4c). Don't fight it.
- If `gamectl play` flakes (intermittent boot deadlock — see KB doc 60), it
  already retries 4×; if still failing, `gamectl kill` and retry the play once
  more before declaring a variant PLAY-FAILED.

---

## 8. Global time budget & stop conditions

- Target ~8–10 h. Phase budgets sum to ~9.5 h. **If a phase overruns its budget,
  log current state and MOVE ON** — do not get stuck. The ordering front-loads the
  two highest-leverage port items (ICF, BOLT) right after baseline so they happen
  even on a short night; PGO is the deep-night job.
- **Stop early & jump to §9 report if** the combat floor reaches ~55–60 `p10`
  (effectively solved) OR `catch_dip.sh` shows the GPU has become the limiter
  (~100% busy) — at that point you've converted the CPU-bound problem into a
  GPU-bound one and further CPU work won't help (report it as success-to-ceiling).
- Always leave the **best-known-good** combo staged before finishing.

---

## 9. FINAL REPORT (write `docs/PERF-OVERNIGHT-REPORT.md`)

Must contain:
1. **TL;DR:** baseline floor → best floor achieved (median p10 + min + avg), the
   winning config (which port binary + which .so), and whether DoD met / why not.
2. **Per-experiment results table** (the §6 ledger, filled), with for each: the
   before→after floor, the Δ, verdict (confirmed/refuted/neutral/skipped + why),
   and kept-or-reverted.
3. **Mechanism evidence** for kept wins: topdown frontend% and L1i-miss /
   branch-miss / `.text` size before vs after; `llvm-bolt -dyno-stats` and PGO
   notes. Show that wins came from front-end relief (or explain if not).
4. **Final host + build state:** governor/turbo, which artifacts are staged
   (md5s), where backups are, any new working-tree SDK edits (and that the SDK
   patch file is now further out of date — note regenerate command).
5. **What's left / recommendations:** the realistic remaining ceiling, next ideas
   not done (e.g., AutoFDO, codegen-level branch reduction, rendering at higher
   res since GPU is idle), and whether 60 is reachable on this HW.
6. Update memory: revise [[sp_combat_perf_frontend_bound]] with the verdicts
   (which front-end fixes worked and by how much) so future sessions start from
   the answer, not the hypothesis.

Then send the user a concise summary (they'll read it in the morning).

---

### Appendix A — quick command cheat-sheet
```
# baseline snapshot
tools/gamectl.sh play && tools/perf/profile.sh 12 && tools/perf/floor.sh 90
# .so A/B
TARGET=$GAME/librexruntime.so tools/perf/ab.sh 90 2 base $GAME/librexruntime.so.good cand $SDK_INSTALL/lib64/librexruntime.so
# port A/B
TARGET=$GAME/south_park_td   tools/perf/ab.sh 90 2 base $GAME/south_park_td.baseline cand /path/to/candidate
# confirm still CPU-bound (GPU idle)
tools/perf/catch_dip.sh 27 3
```
### Appendix B — interpreting counters
- `tma_frontend_bound` high + `L1-icache-load-misses` high → fetch-latency (code
  too big / bad layout) → BOLT/PGO/ICF/-Os are the fixes. ← our case.
- `tma_backend_bound` high + LLC misses → data-memory (NOT our case: Main backend
  was 4.4%). If a change pushes backend up, you traded one stall for another.
- `tma_bad_speculation` high + `branch-misses` high → mispredicts; PGO/BOLT layout
  helps; deeper needs codegen changes.
- DSB≪MITE uops → running from legacy decoders → alignment/uop-cache (P5).
- IPC: baseline ≈0.79; rising IPC at equal work = win.
```
```
