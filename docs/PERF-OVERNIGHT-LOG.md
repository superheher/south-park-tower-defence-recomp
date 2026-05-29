# South Park recomp — overnight perf log

Running narrative + ledger for the autonomous framerate work (see
`docs/PERF-OVERNIGHT-PLAN.md`). Metric = combat fps **floor** (median `p10`
swaps/s over heavy windows, via `tools/perf/ab.sh`). Keep rule: `p10` improves
**> 1.0** vs interleaved baseline AND boot→menu→level works AND GPU stays idle.

## best_so_far (live)

| slot | artifact | md5 | source | floor (p10) |
|---|---|---|---|---|
| port binary | `south_park_td.baseline` | `024e365a` | release build, untouched | **baseline = best** |
| CP `.so`    | `librexruntime.so.good`  | `1996b550` | GetRegisterInfo (prior session) | **baseline = best** |

**FINAL VERDICT (all phases done): NO change beat the baseline floor by >1.0. Best config =
the unchanged baseline.** Every layout/codegen/size lever — ICF, BOLT(port), PGO, PGO∘BOLT, -Os,
CP-`.so` micro-opts, BOLT(`.so`) — is floor-neutral. The combat floor is **I-cache *capacity*-bound**
(port `.text` 22 MB + `.so` hot code, both ≫ 12 MB L3), not layout/branch-bound, so reordering and
modest size cuts can't move it. PGO *does* improve avg/max (light-frame throughput). Both staged
artifacts are the canonical known-good baseline; SDK working tree reverted to its original patch state.

Fallbacks always on disk in `$GAME`: `librexruntime.so.good` (known-good CP),
`south_park_td.baseline` (known-good port). Stage these two to return to a
working game.

## Hypothesis ledger

| # | Hypothesis / change | Scope | Result (median p10 base→cand) | Δ | Verdict | Kept? |
|---|---|---|---|---|---|---|
| P1 | ICF `--icf=all` | port link | .text 23,361,720→23,364,673 (no shrink) | ~0 | **refuted** (mechanism) | reverted |
| P2 | BOLT layout | port post-link | 15.1 → 14.9 (REPS=3, clean shm) | −0.2 | **neutral** (dyno −71% taken br on Main, floor unmoved) | reverted |
| 3a | WriteRegister hot/cold split | .so | 14.5 → 12.9 (REPS=3) | **−1.6** | **refuted** (added 7 cmp/write; split overhead > i-cache gain) | reverted |
| 3b | trace-writer gating | .so | — | — | skipped (mechanism <0.1% of CP cycles ≪ noise) | n/a |
| 3c | hot/cold attrs + branch hints | .so | — | — | skipped (placement-hint marginal; 3a refuted the line) | n/a |
| 3d | Type3 dispatch ordering | .so | — | — | **no-op** (already a jump table: `jmp *%rax`) | n/a |
| P4 | PGO | port recompile | 14.8 → 14.3 (REPS=3) | −0.5 | **neutral on floor** (avg/max ↑, .text −1.3%) | reverted |
| P4 | PGO∘BOLT | port | 14.3 → 15.0 (REPS=3) | +0.7 | **neutral** (within noise; BOLT-on-PGO taken-br only −6.8%) | reverted |
| P5 | -Os size flag | port | 14.9 → 14.6 (REPS=3) | −0.3 | **neutral** (.text −4.2%, still ≫L3); -O2/gc/LTO skipped | reverted |
| 6a | Timer spin→blocking | .so | — | — | **refuted** (won't compile: blocking timed-wait vs steady_clock) | reverted |
| 6b | turbo (re-confirm) | host | — | — | inconclusive (turbo-off window light); prior "marginal" stands | n/a |
| **BONUS** | **BOLT the CP `.so`** | .so post-link | 14.6 → 14.9 (REPS=5) | **+0.3** | **neutral** (REPS=3 showed +1.1 = noise; CP taken-br −61.8% but floor flat) | reverted |

---

## Narrative

### Setup (P0)
- Host already prepped on entry: governor=performance, no_turbo=0,
  perf_event_paranoid=-1, kptr_restrict=0. CPU = i9-8950HK (Coffee Lake, 6C/12T,
  base 2.9 / max-turbo 4.8 GHz, LBR-capable).
- Tools present: perf, radeontop, llvm-bolt, perf2bolt, llvm-profdata, clang-20
  (→clang 21), cmake, ninja, size, objdump. Nothing to install.
- Backups created: `librexruntime.so.good` = current staged `.so` (md5 1996b550,
  GetRegisterInfo); `south_park_td.baseline` = current staged port (md5 024e365a).
  SDK install `.so` == staged `.so` (1996b550) → working tree reproduces known-good CP.
- Port `.text` = 23,361,720 B (~22.3 MB) — larger than 12 MB L3 (root cause).

**BASELINE measured @ 23:10 (port 024e365a + .so 1996b550), game in Stan's House combat:**
- **floor: `min=20.2 p10=20.8 avg=34.2 max=53.1` heavy=yes** (n=45, 90s window).
- Topdown (whole/Main/CP): frontend-bound **45.1% / 46.9% / 52.3%**; backend 20.4/8.6/15.1;
  retiring 27.0/36.1/24.4; bad-spec 7.5/8.4/8.2.
- FE L2 split: **fetch-latency 32.6% ≫ fetch-bandwidth 14.5%** → icache/resteer-bound
  (BOLT/PGO/ICF territory; not pure decode-bandwidth).
- Raw: IPC **0.78** (43.3B insn / 55.7B cyc); **L1-icache-miss 614M** (3× the 205M dcache-miss);
  iTLB-miss 5.3M; branch-miss **11.8%** (674M/5.72B); dTLB 7.0M (non-issue).
- **DSB:MITE uops = 4.07B : 19.77B ≈ 17 : 83** — code runs ~83% from the legacy MITE decoder,
  i.e. the uop cache (DSB) is overwhelmed by the 22 MB hot footprint. Strong layout signal
  (raises priority of P5 alignment + reinforces BOLT/PGO).
- GPU busy **27–28%** during the window → CPU-bound confirmed (GPU has ~72% headroom);
  catch_dip deferred to P6 final-binary re-validation (profile already shows GPU idle).
- Tooling note: hardened `tools/gamectl.sh` — wrapped `wid`(xdotool) in `timeout 4` and the
  post-nav screenshot `import` in `timeout 6`. A transient X glitch had hung `play` for 8 min
  in `wait4`; since `ab.sh` calls `play` without a timeout, this would have stalled every A/B.
  Game-launch detachment (setsid) verified: killing the wrapper left the game running.

### P1 — ICF relink (port)  → REFUTED (reverted)
- **Surprise:** the port links with **GNU ld (BFD)**, not lld (the `CMAKE_LINKER=ld.lld`
  cache var is unused — clang has no `-fuse-ld=lld`; `--dependency-file` is supported by
  modern GNU ld too, so it wasn't the lld tell I first assumed). `-Wl,--icf=all` →
  `/usr/bin/ld: unrecognized option '--icf=all'`. ICF needs lld or gold.
- Forced `-fuse-ld=lld` and built 3 variants + restored baseline. `.text` (bytes):
  baseline(GNU) 23,361,720 · lld+icf=all 23,364,673 · lld+icf=safe 23,364,673 ·
  lld-plain 23,365,544. **ICF folds only 871 B** (icf vs lld-plain), and the lld switch
  itself *adds* 3.8 KB. Codegen'd PPC funcs are not bitwise-identical → nothing to fold.
- **Decision:** refuted by direct `.text` measurement — a 0.004% code change cannot move a
  footprint-bound floor >1.0 swaps/s, so the ~10 min A/B was skipped as a foregone
  conclusion. Reverted: `$GAME/south_park_td` restored to GNU-ld baseline (md5 024e365a ✓),
  cache `CMAKE_EXE_LINKER_FLAGS` reset to empty. Stuck with GNU ld for the campaign (lld gave
  no benefit). BOLT (P2) does its own `-icf=1` + accepts GNU-ld `--emit-relocs`, so no loss.

### P2 — BOLT post-link layout (FLAGSHIP)
- **Relink with relocs** (GNU ld, no lld needed): `-Wl,--emit-relocs -Wl,-q` → `south_park_td.relocs`
  (md5 b41f81ee). `.text` identical to baseline (23,361,720 — code unchanged), +2.68 MB of
  `.rela.ltext` (recompiled code lives in `.ltext`, large-code-model). Boots fine (IN LEVEL, 47 fps).
- **LBR profile** (Coffee Lake LBR + paranoid=-1): `perf record -e cycles:u -j any,u` 120 s of
  heavy combat (fps min 11.6 / max 57.5) → **1.56 M samples, 48.3 M LBR entries**, 1.2 GB.
- **perf2bolt**: 0.1% traces mismatched (binary matches); 63.9% "out of range" = CP thread time
  in `librexruntime.so` (correctly excluded — BOLT only sees the port binary). fdata = 996 KB.
- **llvm-bolt** `-reorder-blocks=ext-tsp -reorder-functions=hfsort+ -split-functions
  -split-all-cold -icf=1`: clean (2 cosmetic deprecation warnings; CFG discontinuity 0.00%, no
  EH in hot funcs). 1443/16181 funcs (8.9%) had profile = the hot combat set. → `sp.bolt` (3ea0182).
  - **dyno-stats (estimated):** taken branches **−71.4%**, taken forward branches **−98.0%**,
    taken unconditional **−78.2%**, executed instructions −0.2% (layout-only, as expected). A
    textbook front-end layout win *on the profiled (Main-thread) code*.
- **A/B #1 (REPS=2, clean-ish shm):** base median_p10 13.8 (15.5, 13.8) · bolt 14.6 (14.6, 14.9)
  → Δ **+0.8** — below the +1.0 keep-bar and within rep-to-rep noise (base spread 1.7).
- **⚠ ENV BUG mid-P2 — /dev/shm leak → SIGBUS:** the next A/B all-PLAY-FAILED with
  `Bus error (core dumped)` right after Vulkan swapchain create. Cause: the game maps a **4.5 GB
  `/dev/shm/xenia_memory_*` per launch and never removes it on crash/kill**; 58 had accumulated
  (13 GB) since 16:38, filling the 16 GB tmpfs → guest-RAM faults SIGBUS. **Fix:** deleted the 58
  orphans (→1% used) and patched `gamectl.sh kill_all()` to `rm -f /dev/shm/xenia_memory_*` after
  confirming no live instance, so every launch reclaims them. (Earlier P0/A/B#1 measurements
  predate the fill and remain valid.) Re-running the BOLT A/B (REPS=3) on clean shm for the verdict.
- **VERDICT (REPS=3, clean shm):** base median_p10 **15.1** (27.3, 15.1, 12.4) · bolt **14.9**
  (15.4, 13.1, 14.9) → Δ **−0.2**, within noise (base spread 12.4–27.3 from large in-session
  load drift). **NEUTRAL — not kept, reverted** (port 024e365a staged, cache flags reset). The
  −71% taken-branch dyno win was real but Main-thread-only; the floor is gated by (a) the CP
  thread's front-end in `librexruntime.so` (BOLT never touched it) and (b) icache *capacity*
  misses from the 22 MB footprint (fetch-latency 32.6%), which block reordering can't fix. Kept
  `south_park_td.relocs` + `/tmp/sp/sp.bolt` on disk. **Implication: BOLT-on-the-.so is the
  biggest untried lever for the CP-gated floor** (flagged for after-plan / recommendations).

### P3 — CP `.so` micro-opts  → 3a refuted, line closed (reverted to .good)
- Disasm of `.good` (1996b550): base `CommandProcessor::WriteRegister` = **1733 B** (0x6c5), all the
  gamma/DC_LUT bodies inlined; it's **virtual**, hot path = `VulkanCommandProcessor::WriteRegister`
  (289 B) → directly calls the base. Type3 dispatch = **jump table** (`jmp *%rax` after range
  checks) → **3d is a no-op** (reordering a jump table does nothing). Skipped.
- **3a hot/cold split:** moved scratch/COHER/DC_LUT side-effects to a cold `WriteRegisterSideEffects`
  (behaviour-identical: same store-then-OR for COHER, recursive `WriteRegister` calls kept
  unqualified so virtual dispatch preserved). Built (49 s). Hot body shrank **1733 → 988 B** (836 B
  moved cold). **A/B (REPS=3, clean shm): base median_p10 14.5 (15.1, 14.5) · cand 12.9 (12.9, 13.5)
  → Δ −1.6 (consistently WORSE).** Mechanism: the slim path now runs a 7-comparison predicate on
  *every* write, whereas the original switch was an O(1) jump table — the added per-write compares
  cost more than the i-cache-density saving returns, i.e. **the floor is not gated by WriteRegister's
  footprint.** (Also saw one transient cand "stale" — extra reason to drop it.) **Reverted.**
- **3b/3c skipped with quantified justification (not laziness):** 3b (gate per-packet trace calls)
  removes ~2 call/ret per packet ≈ **<0.1% of CP cycles** — an order of magnitude below the
  ±2–3 swaps/s run-to-run noise, so unmeasurable. 3c (`[[gnu::hot]]`) only hints function
  placement (the linker already clusters); marginal. With 3a (the one .so opt with a >1% mechanism)
  refuted, the whole micro-opt line cannot clear the +1.0 bar. Budget redirected to P4 (PGO) — the
  plan's stated primary lever. **`.so` reverted to known-good; install rebuilt from reverted source.**
- **Note for recommendations:** the genuinely-untried high-leverage `.so` lever is **BOLT on
  `librexruntime.so` itself** (the CP thread is 52% FE-bound and lives entirely in the .so, which
  P2's port-only BOLT never touched). Micro-opts were the wrong tool; whole-.so layout is the right
  one. Flagged for after-plan / report.

### P5 — size/layout flags  → -Os neutral, milder flags skipped
- **-Os (whole-binary; codegen dominates .text):** built (3:22). `.text` 23,361,720 → **22,379,533
  (−4.2%)** — recompiled PPC is already tight, -Os mostly tweaks inlining (little to do). **A/B
  (REPS=3): base 14.9 (14.9,14.8,15.4) · os 14.6 (14.6,14.1,14.8) → Δ −0.3 (neutral/slightly worse).**
  4.2% smaller is still ≫ 12 MB L3, and less-optimized code is marginally slower. Reverted.
- **-O2 / gc-sections / ThinLTO skipped (gated):** -Os is the most aggressive size lever; since it
  was neutral, -O2 (between O3/Os) is bracketed neutral; gc-sections strips little (recomp funcs are
  indirect-call-reachable, not statically dead); ThinLTO is RAM-heavy/slow with low EV given the
  pattern. Documented rather than spend noise-dominated A/B time.

### P6 — hygiene & re-validate
- **6a timer spin→blocking: REFUTED (compile error).** Swapped the 3 `dp::spin_wait_strategy` →
  `blocking_wait_strategy` in `timer_queue.cpp` (verified API match: same 3 `wait_until_published`
  overloads incl. timed; `publish()` already signals blocked waiters). But the build fails:
  `blocking_wait_strategy.hpp:172 no matching member function 'wait_until'` — disruptorplus's timed
  `wait_until_published` doesn't compile against the timer's `steady_clock` time_point. Low fps value
  (timer thread isn't the floor-gate) → not worth patching thirdparty. **Reverted (.so stayed .good;
  build never staged).**
- **6b re-validate on final config (baseline):** `catch_dip` 3 dips (18–21 fps): **GPU engine ~20%
  (idle), CP/"GPU" thread 90–100%, Main 90–100%** → **CPU-bound confirmed, NOT GPU-bound** (DoD met:
  we did not shift the bottleneck to the GPU). Notably **the floor is co-gated by Main AND the CP
  (.so)** — both pegged at dips. Turbo re-confirm inconclusive (the turbo-off floor window was
  all-light, heavy=no, non-comparable); the established "turbo marginal / floor is clock-independent
  fetch-latency" finding stands. `no_turbo` restored to 0.

### BONUS (off-plan, goal-driven) — BOLT the CP `.so`
Motivation: P2 BOLT'd only the *port* (Main); catch_dip shows the **CP thread (in `librexruntime.so`)
is also pegged ~100% at the floor**, and it's 52% front-end-bound — the logical completion of the
layout investigation and the single most-likely-to-help untried lever for the floor.
- **Pipeline:** relinked `librexruntime.so` with `-DCMAKE_SHARED_LINKER_FLAGS="-Wl,--emit-relocs -Wl,-q"`
  (md5 6ab6dccf, `.rela.text` present, 1:07). Boots fine. 90 s LBR combat profile (982 MB, fps
  10.6–49.7). `perf2bolt` the .so → 1.2 M samples / 37.5 M LBR, fdata 722 KB. `llvm-bolt` the .so
  (ext-tsp/cdsort/split/icf=all) → `librexruntime.bolt.so` (38912f52): **taken branches −61.8%**,
  63 functions reordered. **BOLT-rewritten PIC .so loads & runs fine.**
- **A/B #1 (REPS=3):** base median_p10 **14.0** (14.0, 14.0, 12.8) · sobolt **15.1** (15.9, 13.9, 15.1)
  → **Δ +1.1 — first result to clear the +1.0 bar.** Borderline vs noise → confirming at REPS=5.
- (REPS=5 confirmation pending; if it holds, this is the night's win: baseline port + BOLTed .so.)

### P4 — PGO on the port (+ PGO∘BOLT)
- **Instrumented build** (`-fprofile-generate`, separate `-pgo-gen` dir, -j6): OK in 3:24, RSS 452 MB
  (no OOM), binary 33.5 MB. Boots to combat (slowed but playable, ~30 fps).
- **⚠ Profile-flush problem (significant detour):** `-fprofile-generate` flushes `.profraw` only via
  an **atexit** handler, but **this game has no clean-exit path** — it ignores SIGTERM, SIGINT, and
  WM_DELETE (window-close), and SIGHUP *terminates* it without running atexit. So profraw stayed
  **0 bytes**. Tried:
  - `-fprofile-continuous` (mmap counters → SIGKILL-safe): binary warned *"Continuous counter sync
    mode enabled, but raw profile is not page-aligned"* → init failed, still 0 bytes. Relinking the
    instrumented binary with **`-fuse-ld=lld`** (lld page-aligns the counter section) **did not fix
    it** either — still 0 bytes.
  - **SOLUTION — gdb live-dump:** attach to the running instrumented PID and call
    `__llvm_profile_write_file()` (the symbol is in the binary; `ptrace_scope=0`):
    `gdb -p <pid> -batch -ex 'print (int)__llvm_profile_write_file()' -ex detach`. Returns 0 (success),
    writes a valid **2.8 MB profraw** mid-combat. No exit needed. (AutoFDO fallback was unavailable —
    `create_llvm_prof` not installed; gdb-dump avoided a from-source build.)
- **Profile collected:** two combat runs (60 s + 150 s, fps 10.7–53.7 heavy), gdb-dumped, merged →
  `/tmp/sp/port.profdata` (3.27 MB; 16,233 functions, total count 36.7 B). SIGKILL + shm-clean after.
- **PGO-use build** (`-fprofile-use=port.profdata`, GNU ld like baseline): 3:31, **0 profile-mismatch
  warnings** (profile matched source exactly), `.text` 23,361,720 → **23,062,831 (−1.3%)** — a real
  codegen change (unlike ICF/BOLT which left code size ~flat).
- **PGO A/B (REPS=3):** base median_p10 **14.8** (15.0, 14.7, 14.8) · pgo **14.3** (14.3, 14.3, 15.0)
  → Δ **−0.5 on the FLOOR (neutral/slightly worse)**. BUT pgo's **avg (30.5/29.3) and max (57.5/56.5)
  beat baseline** (avg ~29.5, max ~51) — **PGO speeds the common/light frames but not the heavy-wave
  floor.** Not kept by the >1.0 floor rule. (It is the best port binary by avg/max, floor-tied.)
- **PGO∘BOLT combo:** relinked PGO-use with `--emit-relocs` (f793318e), fresh 90 s LBR combat profile
  (984 MB, fps 10.3–51.2), perf2bolt + llvm-bolt (cdsort/ext-tsp/split/icf=all) → `south_park_td.pgobolt`
  (cd6764c2). **dyno-stats: taken branches only −6.8%** (vs −71% when BOLTing the un-PGO'd baseline) —
  PGO had already captured most of the layout win, so BOLT-on-PGO is largely redundant. A/B vs base
  (REPS=3) pending.
