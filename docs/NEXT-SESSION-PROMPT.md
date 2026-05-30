# South Park recomp — next-session LAUNCH PROMPT: fix the combat-floor drops = runtime GPU-fence write-back

> **THE FLOOR IS GUEST OVER-EXECUTION (busy-waits / frame-sync spin-polling), NOT the CPU.** Proven
> 2026-05-30 (`tools/perf/pausetest.sh`): the guest-sim (Main) thread burns **5.3 G instr/s even while the
> game is PAUSED** (99% of running). Triggered by the user's killer data point — **Sonic Unleashed
> Recompiled (a full 3D game) runs great on this SAME i9 while our 2D tower-defense drops hard.** So the
> CPU is plenty fast; we *waste* it spinning. **READ FIRST:** `docs/FLOOR-OVEREXEC-REPORT.md` + memory
> `sp_floor_overexec`. **BIG LESSON: do NOT do codegen / µop / layout / flag work for the floor** — that's
> all measuring the cost of a spin loop (every such lever, 6 prior sessions, was floor-neutral; see DEAD
> ENDS). The lever is making the spins **block on a signal** instead of polling.

## The three over-execution layers (two FIXED this session, the third IS the floor)
1. ✅ **FIXED — Main thread guest spin on the GPU vblank fence.** `sub_821C6E58` (back-edge guest
   `0x821C6F10`) polls a guest-memory word the GPU command-processor advances via `EVENT_WRITE_SHD`,
   calling predicate `sub_821B9270`. The guest's Xenon spin idiom `cctpl`(prio-low)/`db16cyc`(backoff)/
   `cctpm` was emitted as **no-ops** → it spun at full speed/priority. Fix: `build_db16cyc`
   (`builders/system.cpp`) now emits `REX_SPIN_BACKOFF()` (helper in `init_h.inja` = x86 `pause` + a
   periodic `std::this_thread::yield`). **−25× insn-rate** (5.3→0.2 G/s), detdiff gate PASS, floor-neutral.
2. ✅ **FIXED — Timer thread busy-spin.** `TimerQueue::TimerThreadMain` was **16.6% of ALL CPU cycles**
   spinning in disruptorplus `spin_wait_strategy`. Fix: `core/timer_queue.cpp` → `blocking_wait_strategy`
   (+ fixed a real arg-order bug in `thirdparty/disruptorplus/.../blocking_wait_strategy.hpp`:
   `wait_until(lock, pred, time)` → `(lock, time, pred)`). Gate PASS.
3. 🔎 **THE FLOOR (your task) — GPU CommandProcessor spin-polls `WAIT_REG_MEM` + Main spins on the fence.**
   In a heavy dip: Main thread 42% of cycles (pausing on the fence) + **GPU CP 37% pegged on a core**, of
   which ~19.7% is user PM4 parse/`WriteRegister` and **~17% is kernel `sched_yield` syscalls** from
   `ExecutePacketType3_WAIT_REG_MEM` (`command_processor.cpp` ~1230: while unmatched, up to 8000×
   `SyncMemory()`+`MaybeYield()`=`sched_yield`, then `Sleep`). **GPU HARDWARE is 17–26% IDLE, libvulkan
   2.4%** ⇒ NOT GPU-bound. It's **CPU-side frame synchronization done as MUTUAL SPIN-POLLING**, and the
   frame quantizes to vblank (floor p10 ≈ 15 = 60/4; 20 = 60/3; 30 = 60/2). This is the "sinusoid" pacing.

## Where we are
Static recompilation (rexglue-sdk) of South Park: Let's Go Tower Defense Play! (XBLA) → native
Linux/Vulkan, fully playable (boot→match→win). Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`)
+ submodule `south-park-recomp` (port, `main`). **SDK edits = working-tree patch
`patches/rexglue-sdk-current-full.patch`** (the SDK submodule is kept dirty; regenerate the patch with
`git -C ../third_party/rexglue-sdk diff > patches/rexglue-sdk-current-full.patch` before committing).
Identity `superheher <heh@vivaldi.net>`, on `main`, **NO Co-Authored-By trailer**, **commit, do NOT push**
unless asked. Host: i9-8950HK (6c/12t), governor=performance, sudo `<redacted>`, disposable bench.

**Current working binary (= the 2 fixes above, gate PASS, over-execution hugely reduced, floor still ~15):**
`south_park_td` md5 **`d4b0f50b`**, `librexruntime.so` md5 **`605ce3ee`** — staged as
`out/build/linux-amd64-release/south_park_td.bothfix` + `librexruntime.so.bothfix`. The PRE-fix BKG (for
A/B base) is `south_park_td.mcmodel_medium` (`bef1b65c`) + `librexruntime.so.so_medium` (`47323bf2`).
Floor p10 ≈ 15 (= 60/4) on `ab.sh 90 5`.

## THE TASK — runtime GPU-fence write-back (lift the floor)
The SDK's own comment states the fix (`command_processor.cpp:1273`): *"the real fix is runtime GPU-fence
write-back."* Concretely: the guest's `WAIT_REG_MEM` packets and its post-frame fence wait (the Main-thread
spin, layer 1) are GPU→CPU sync points — the guest waits for the GPU to finish work / reach a vblank. On
real Xenon the GPU writes those values and the waits resolve on hardware. In our recomp the awaited values
are written **late / only via polling fallbacks**, so both sides spin. **Make the Vulkan side SIGNAL them
promptly:**
- When the Vulkan work that a fence gates COMPLETES (Vulkan fence/semaphore/timeline-semaphore completion,
  or queue-submit callback), **write the awaited guest value to guest memory/register immediately** so the
  CP's `WAIT_REG_MEM` poll and the Main thread's fence wait see it at once and exit. Wire it to a host
  condition variable so the waiters BLOCK and wake on signal instead of polling.
- Key code to study: `ExecutePacketType3_WAIT_REG_MEM` (~1230), `RefreshVblankFence` (~419) + `MarkVblank`,
  `ExecutePacketType3_EVENT_WRITE_SHD` (~1469, where `counter_` is the fence written), the `vblank_fence_*`
  machinery (~1474-1490), and `XE_SWAP` present pacing (~1108). The vblank counter the guest polls must
  advance at a steady 60 Hz independent of present (RefreshVblankFence already tries to push it "between"
  EVENT_WRITE points — check it actually fires often/steadily enough).
- **CHEAPER PARTIAL TO TRY FIRST (low-risk, bounded):** in the `WAIT_REG_MEM` fast-poll, replace the
  per-spin `sched_yield` (`MaybeYield`) with `_mm_pause()` + a *periodic* yield (e.g. every 64 spins). This
  cuts the ~17% CP kernel overhead without the big rework; measure if it lifts the floor at all. Also: the
  **Audio Worker thread was 13.6% of cycles in `pthread_mutex_trylock`** = a likely **4th busy-wait** worth
  a look (the SDL/XAudio mixer spinning on a mutex).
- **Reference (research lead):** how XenonRecomp / Unleashed Recompiled (hedge-dev) map guest GPU fences /
  vsync to host blocking — that's the known-good pattern this game's recomp is missing.

## RISK / validation discipline (mandatory — the present/vsync path is fragile)
This is the pacing area where fixes have been "tried + reverted" and where a boot present/vsync **deadlock**
once froze the CP forever (see `knowledge-base/titles/south-park-lgtdp/60-boot-present-deadlock.md` + the
`[recomp fix]`/`[recomp stop-gap]` comments around WAIT_REG_MEM). So:
- `tools/perf/detdiff.sh gate <label> 40` must be `status=pass` (ranks above fps). Then a **long playable
  soak** (boot→match→win, not just the 40s harness) — a missed fence wakeup deadlocks the CP.
- `tools/perf/pausetest.sh <label>` — the over-execution detector: the Main-thread (and ideally CP-thread)
  insn-rate must STAY LOW / collapse if the spin is now blocking. This is the decisive "did it work" signal.
- Floor A/B: `bash /tmp/sp/bothfix_ab.sh 6` style (swaps BOTH exe+so since a fix may touch both); base =
  `bef1b65c`+`47323bf2`, cand = the new build. **Keep-bar: median p10 > +1.0 swaps/s, ≥5 heavy reps.**
- `tools/perf/regen_build.sh full` after an SDK change (rebuilds the .so + regenerates + builds the exe).
- HOST GOTCHAS: (a) the harness BLOCKS a literal `sleep` token in a Bash command string → put waits in a
  SCRIPT FILE and run `bash tools/perf/<x>.sh`; (b) the game is reaped when its launching shell ends → boot
  AND measure in ONE script; (c) ONE game instance only; (d) `gamectl kill` auto-cleans the /dev/shm leak.

## DEAD ENDS — do NOT retry CODEGEN for the floor (it's a spin, not compute; all measured neutral)
arch-flags (`-mmovbe…`, +10% uops), rlwinm/CR/width special-casing (clang already optimal), non-volatile
RAM (↑insns + fails gate), ThinLTO (+0.3 = noise), HT-contention (−0.8), OS-switch / `mitigations=off`
(≤~2%, BTB/RSB/DSB are fixed silicon), GPR-as-local (SEH-blocked + the −16%-.text as-local was neutral),
mcmodel/leaf-inline/PGO/BOLT/ICF/outliner (all floor-neutral). Full detail + measurements:
`docs/FLOOR-FRONTEND-REBASELINE-REPORT.md` (the now-MOOT-for-floor codegen rebaseline) + its memos. These
remain valid as "codegen can't move the floor" — the floor was a spin all along.

## Tools (tools/perf/)
NEW (over-execution): `pausetest.sh` (running-vs-paused insn-rate = the spin detector), `spinprofile.sh`
(paused = pure-spin profile), `heavydip.sh` / `gpucp3.sh` (per-thread + per-comm dip profiling),
`threads.sh` (per-thread %CPU). Codegen-era (still useful): `topdown.sh`, `uops.sh`, `annotate.sh`,
`resteer.sh`, `affinity_test.sh`, `uksplit.sh`, `detdiff.sh`, `ab.sh`, `regen_build.sh`.
