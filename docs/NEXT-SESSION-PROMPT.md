# South Park recomp — next-session LAUNCH PROMPT: the floor is CP translation THROUGHPUT; the per-register-write levers are exhausted

> **THE FLOOR IS THE GPU COMMAND-PROCESSOR'S SINGLE-THREADED PM4→VULKAN TRANSLATION THROUGHPUT.**
> This session closed the **per-register-write** lever class. Generic redundant-write elision (skip every
> unchanged-value write to a pure-state register, ~90% of ALL per-register writes) is now in and clears the
> +1.0 keep-bar vs bothfix. A follow-on virtual-dispatch skip was floor-neutral. **READ FIRST:**
> `docs/FLOOR-CP-ELISION-REPORT.md` (+ `FLOOR-CP-TRANSLATION-REPORT.md`) and memory `sp_floor_cp_elision`.

## Where we are
Static recompilation (rexglue-sdk) of South Park: Let's Go Tower Defense Play! (XBLA) → native
Linux/Vulkan, fully playable (boot→match→win). Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`)
+ submodule `south-park-recomp` (port, `main`). **SDK edits = granular patch series in
`patches/0*.patch`** (the submodule gitlink stays on upstream `e8ce24f`; `git am` the series to apply —
see `patches/README.md`). Identity `superheher <heh@vivaldi.net>`, on `main`, **NO Co-Authored-By
trailer**, **commit, do NOT push** unless asked. Host: i9-8950HK (6c/12t), governor=performance, sudo
`<redacted>`, disposable bench.

**Current working binary (kept this session — elis1, gate PASS, render-verified, +1.0 keep-bar cleared):**
exe `848f191c` (`south_park_td.cpfence6`, UNCHANGED — the elision is .so-only) + `librexruntime.so`
`5276a282` (`librexruntime.so.elision1`). The +1.0 keep-bar reference base is **bothfix**: exe `d4b0f50b`
(`south_park_td.bothfix`) + `.so` `605ce3ee` (`librexruntime.so.bothfix`). Floor p10 ≈ 15 (base) → ≈ 17-18
(elis1).

## THE TASK — cut heavy-frame CP translation THROUGHPUT further (the per-register-write lever is DONE)
The new heavydip CP-thread profile (`tools/perf/heavydip_cpfence6_2026-05-30.txt`) showed the dip is
dominated by the register-write dispatch — that is now ~90% elided. **Re-profile the CURRENT elis1 binary
first** (`heavydip.sh`) to see the NEW hot set, then attack the remaining translate work, easiest→hardest:
1. **Bulk-constant path** (`VulkanCommandProcessor::WriteRegistersFromMem`). The big float/fetch ranges go
   through `memory::copy_and_swap` + a single dirty-mark — already efficient, but check whether the title
   sends large UNCHANGED constant ranges that could be memcmp-skipped before the copy + dirty-mark.
2. **Draw batching / instancing** for the many small sprites (tower-defense = many similar units). The
   diffuse remainder of translate is draw issue — pipeline lookup + descriptor sets (`radv_UpdateDescriptorSets`
   was ~1.35%) + command-buffer recording. Coalesce identical-state draws.
3. **(Hard) parallelize the CP translation.** The GPU HW is 17-26% idle, starved by the serial CP thread.
   This is the only lever with headroom to actually break the throughput floor.

## Validation discipline (mandatory)
- `tools/perf/detdiff.sh gate <label> 40` must be `status=pass`.
- **MID-COMBAT screenshots** (`gamectl play`, escalate with `press 1000`, then `shot`) — the gate will NOT
  catch a wrong-state rendering glitch; eyeball sprites/colors/transforms. (`tools/perf/smoke_shot.sh <tag>` does
  play→escalate→3 shots.)
- Floor A/B: `tools/perf/ab_both.sh 90 6 base <bothfix exe> <bothfix so> cand <new exe> <new so>` (swaps
  BOTH binaries). **Keep-bar: median p10 > +1.0 swaps/s vs bothfix, ≥5 heavy reps.** NOTE the floor is
  noisy run-to-run (~±2 on the de-spun binaries; base is the stable ~15.0 anchor) — interleave + ≥6 reps.
- `.so`-only change: `cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --target install` then
  `cp out/install/linux-amd64/lib64/librexruntime.so` into the port build dir (~20s, exe unchanged).
  `tools/perf/regen_build.sh full` only if an init_h.inja/header change (new exe).
- HOST GOTCHAS: (a) the harness BLOCKS a literal `sleep` token in a Bash string → put waits in a SCRIPT
  FILE (`tools/perf/wait_for.sh <file> <marker> <timeout>` polls a log); (b) `gamectl play` uses `setsid` so the
  game survives the launching shell; (c) ONE game instance only; (d) `gamectl kill` + `rm -f
  /dev/shm/xenia_memory_*` cleans the shm leak. **NEVER build/launch the game while an A/B is swapping
  binaries.**

## DEAD ENDS — do NOT retry for the floor (all measured neutral)
- **Per-register-write micro-opt is EXHAUSTED** (this session): generic unchanged-write elision (~90% of
  writes) clears the bar but its *marginal over the constant-skip* is +0.2 (noise); the TYPE0 virtual-
  dispatch skip on top is strictly floor-neutral (removing ~millions/sec of indirect calls didn't move it
  → the floor is NOT branch-prediction-bound here). The body-elision was the last worthwhile bite.
- **De-spin / block-on-signal** (prior): CP `WAIT_REG_MEM`→CV block AND guest Main→vblank-park — both
  floor-neutral (avg/ceiling win only). Not CPU-contention.
- **Codegen / µop / layout / flags / mcmodel / PGO / BOLT / ICF / outliner / ThinLTO / GPR-as-local**
  (6 prior sessions) — all floor-neutral. See `FLOOR-FRONTEND-REBASELINE-REPORT.md` + `FLOOR-OVEREXEC-REPORT.md`.
**The floor is CP serial-translation THROUGHPUT. Only cutting real translation work, or parallelizing it,
can move it — and CPU-side cuts have low floor-elasticity (this session: −4% whole-proc CPU for +0.2 p10).**

## Tools (tools/perf/)
`heavydip.sh` (per-thread/per-comm dip profile — the decisive measurement), `ab_both.sh` (both-exe+so A/B),
`detdiff.sh` (gate), `floor.sh`, `regen_build.sh`. This session's artifacts: `reghist_2026-05-30.txt`
(per-register write/unchanged histogram — re-add the `[reg-hist]` diag at the top of
`VulkanCommandProcessor::WriteRegister` to regenerate), `heavydip_cpfence6_2026-05-30.txt`,
`ab_elision{1,2}_2026-05-30.txt`.
