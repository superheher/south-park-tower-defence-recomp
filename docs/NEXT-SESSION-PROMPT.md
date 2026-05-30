# South Park recomp — next-session LAUNCH PROMPT: the floor is CP TRANSLATION throughput, cut it

> **THE FLOOR IS THE GPU COMMAND-PROCESSOR'S SINGLE-THREADED PM4→VULKAN TRANSLATION, NOT spin/CPU.**
> Proven 2026-05-30 by per-frame instrumentation (`[frame-diag]`): in a heavy combat dip the CP burns
> **~29 ms/frame translating the command stream** (≈72 % of the heavy-frame wall-clock); present is
> ~3 ms, frame-sync wait ~8 ms, the GPU HW is 17–26 % IDLE (starved by the serial CP). **READ FIRST:**
> `docs/FLOOR-CP-TRANSLATION-REPORT.md` + memory `sp_floor_cp_translation`.
> **BIG LESSON: do NOT do block-on-signal / de-spin / codegen for the floor.** Two MORE thesis-killers
> landed this session: de-spinning BOTH busy-wait layers (CP `WAIT_REG_MEM` → CV block; guest Main
> `sub_821B9270` 34 %→9.6 % via a vblank-park) is **floor-NEUTRAL** (it only raised avg/ceiling). The
> floor only moved when we cut real translation work (skip-unchanged-constants: p10 +0.3, min +2).

## THE TASK — cut heavy-frame CP translation (~29 ms → <22 ms gets 30 fps; keep-bar p10 > +1.0, ≥5 heavy reps)
The CP is `librexruntime.so` runtime code (NOT recompiled guest code — a fresh, untouched surface). Levers,
easiest→hardest:
1. **Extend redundant-write elision.** `vulkan/command_processor.cpp:WriteRegister` already skips
   unchanged float/bool/loop shader constants (the proof-of-concept, +0.3 p10/+2 min). The title re-sets
   redundant state per draw. Safely skip unchanged writes for the OTHER pure-state registers — but FIRST
   enumerate the side-effect set that must NOT be skipped: scratch (`SCRATCH_REG0..7`), `DC_LUT_*`, gamma,
   `COHER_STATUS_HOST`, the fetch constants (`SHADER_CONSTANT_FETCH_*` → texture-cache/vertex-residency
   invalidation), and anything in `CommandProcessor::WriteRegister`'s special switch. **Verify rendering
   with MID-COMBAT screenshots** (`gamectl shot`), not just the determinism gate (the gate is behavioral,
   it won't catch a subtle constant glitch).
2. **Devirtualize / bulk the register path.** `WriteRegister` is `virtual`, called per-register in loops
   (`RestoreRegisters`, TYPE0 ranges). Bulk-store the range + batch the dirty-mark once.
3. **Draw batching / instancing** for the many small sprites — the diffuse remainder of the 29 ms is draw
   issue (pipeline lookup + descriptor sets + command-buffer recording, `radv_UpdateDescriptorSets` etc.).
4. **(Hard) parallelize CP translation** — the GPU is idle, starved by the serial CP thread.
**Re-instrument first:** re-add the `[frame-diag]` split (it was removed; see the report) AND add a
per-register-index write-count + unchanged-count histogram to TARGET the highest-redundancy registers
before optimizing blindly.

## Where we are
Static recompilation (rexglue-sdk) of South Park: Let's Go Tower Defense Play! (XBLA) → native
Linux/Vulkan, fully playable (boot→match→win). Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`)
+ submodule `south-park-recomp` (port, `main`). **SDK edits = working-tree patch
`patches/rexglue-sdk-current-full.patch`** (regenerate with
`git -C ../third_party/rexglue-sdk diff > patches/rexglue-sdk-current-full.patch` before committing).
Identity `superheher <heh@vivaldi.net>`, on `main`, **NO Co-Authored-By trailer**, **commit, do NOT push**
unless asked. Host: i9-8950HK (6c/12t), governor=performance, sudo `<redacted>`, disposable bench.

**Current working binary (kept this session — de-spun + GetLoggerRaw cache + constant-skip, gate PASS,
render-verified):** exe `848f191c` (`south_park_td.cpfence6`) + `librexruntime.so` `f1d86a27`
(`librexruntime.so.cpfence6`). The PRE-this-session base (for A/B) is `bothfix`: exe `d4b0f50b`
(`south_park_td.bothfix`) + `.so` `605ce3ee` (`librexruntime.so.bothfix`). Floor p10 ≈ 15.

## Validation discipline (mandatory — the constant-skip touches the render path)
- `tools/perf/detdiff.sh gate <label> 40` must be `status=pass` (behavioral fingerprint).
- **MID-COMBAT screenshots** (`gamectl play` then `gamectl press 1000`/`shot`) — the gate will NOT catch a
  wrong-constant rendering glitch; eyeball the sprites/colors/transforms.
- Floor A/B: `tools/perf/ab_both.sh 90 5 base <bothfix exe> <bothfix so> cand <new exe> <new so>` (swaps
  BOTH binaries; a translation change is in the .so, but keep the both-swap harness for consistency).
  **Keep-bar: median p10 > +1.0 swaps/s, ≥5 heavy reps.**
- `tools/perf/regen_build.sh full` after an init_h.inja/header change (new exe); for a pure SDK `.cpp`
  edit, `cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --target install` then copy
  `out/install/linux-amd64/lib64/librexruntime.so` into the port build dir (~18–30 s, exe unchanged).
- HOST GOTCHAS: (a) the harness BLOCKS a literal `sleep` token in a Bash command string → put waits in a
  SCRIPT FILE; (b) the game is reaped when its launching shell ends, but `gamectl play` uses `setsid` so
  it survives; (c) ONE game instance only; (d) `gamectl kill` + `rm -f /dev/shm/xenia_memory_*` cleans the leak.

## DEAD ENDS — do NOT retry for the floor (all measured neutral)
- **De-spin / block-on-signal** (THIS session): CP `WAIT_REG_MEM`→CV block AND guest Main→vblank-park —
  both floor-NEUTRAL (avg/ceiling win only). The floor is not CPU-contention.
- **Codegen / µop / layout / flags / mcmodel / PGO / BOLT / ICF / outliner / ThinLTO / GPR-as-local**
  (6 prior sessions) — all floor-neutral; they profiled a spin or the wrong target. See
  `docs/FLOOR-FRONTEND-REBASELINE-REPORT.md` + `docs/FLOOR-OVEREXEC-REPORT.md` (both now superseded for
  the floor by the CP-translation finding).

## Tools (tools/perf/)
NEW: `ab_both.sh` (both-exe+so floor A/B). The `[frame-diag]` XE_SWAP split + a WAIT_REG_MEM address diag
were used then removed — re-add per the report when re-instrumenting. Also: `pausetest.sh`,
`spinprofile.sh`, `heavydip.sh`/`gpucp3.sh` (per-thread/per-comm dip profiling), `detdiff.sh`, `ab.sh`,
`floor.sh`, `regen_build.sh`.
