# South Park recomp — next-session LAUNCH PROMPT: the floor is SERIAL RENDER-PIPELINE LATENCY (cores are free)

> **THE FLOOR IS THE SERIAL PER-FRAME RENDER-PIPELINE ROUND-TRIP LATENCY** (guest-sim → CP-translate →
> GPU-exec → present), NOT CPU throughput / core contention / thermal / saturation. A `perf sched` probe
> this session PROVED it: the CP thread's average scheduling delay is **5 µs** (cores are free), it is only
> **~50% on-cpu**, and it context-switches **~14k×/s** (runs in tiny bursts, blocks constantly). This is why
> **every CPU-side lever for 7 sessions has been floor-neutral** — none shorten the dependency chain.
> **READ FIRST:** `docs/FLOOR-AUDIO-DESPIN-REPORT.md` (§1 is the decisive measurement) and memory
> `sp_floor_latency_bound`. Prior framing (`FLOOR-CP-ELISION-REPORT.md` / `sp_floor_cp_elision`: "CP
> translation **throughput**") is now *refined* by this — the CP is not CPU-bound, it's latency-bound.

## Where we are
Static recompilation (rexglue-sdk) of South Park: Let's Go Tower Defense Play! (XBLA) → native
Linux/Vulkan, fully playable (boot→match→win). Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`)
+ submodule `south-park-recomp` (port, `main`). **SDK edits = granular patch series in
`patches/0*.patch`** (the submodule gitlink stays on upstream `e8ce24f`; `git am` the series to apply —
see `patches/README.md`). Identity `superheher <heh@vivaldi.net>`, on `main`, **NO Co-Authored-By
trailer**, **commit, do NOT push** unless asked. Host: i9-8950HK (6c/12t), governor=performance, sudo
`<redacted>`, disposable bench. **NOT thermally throttled under combat** (measured: cores hold ~4.2GHz all-core
through the dip; `load_probe.sh`).

**Current working binary (kept this session — audiofix, gate PASS, render-verified):** exe `848f191c`
(`south_park_td.cpfence6`, UNCHANGED) + `librexruntime.so` `8d2bf92e` (`librexruntime.so.audiofix` = patch
0015 audio de-spin on top of elis1). Prior was **elis1** (`.so` `5276a282`). The keep-bar reference base is
**bothfix**: exe `d4b0f50b` + `.so` `605ce3ee`. Floor p10 ≈ 15 (bothfix) → ≈ 17-18 (elis1/audiofix).

## THE TASK — shorten the per-frame serial dependency chain (the ONLY thing that can move the floor)
CPU-side cuts are floor-neutral here (the cores are free; see §1 of the audio-despin report). Attack the
ROUND-TRIP LATENCY, hardest-but-only-real first:
1. **Overlap CP-translate with GPU-exec (biggest headroom).** The GPU HW is 17-26% idle, starved by the
   serial CP burst. Pipeline the next frame's PM4→Vulkan translation against the current frame's GPU
   execution (double-buffer the command stream / submit earlier / cut the present→next-frame stall). This
   attacks the dependency chain directly. Non-trivial but it is the lever with real headroom.
2. **Shorten the per-heavy-frame CP critical path** — draw batching/instancing for the many small
   tower-defense sprites. After the audio de-spin, `radv_UpdateDescriptorSets` rose to ~7.5% of the CP
   thread; coalescing identical-state draws cuts pipeline lookups + descriptor updates + cmd-buffer records
   per frame, shortening the CP segment of the chain.
3. **Cut sync-wait LATENCY on the guest→CP→present handshakes** — the *latency* of the round-trip, not the
   CPU spent spinning (de-spinning is already done and is floor-neutral).
**Measure the chain, not CPU%.** Use `cp_offcpu.sh` (perf sched) to see CP/GPU/present on-cpu vs blocked
fractions and the per-stage wait, and the per-frame `[frame-diag]` split (translate/sync/present). A lever
only matters if it shrinks the per-frame round-trip.

## DEAD ENDS — do NOT retry for the floor (all measured neutral; the cores are FREE)
- **Audio de-spin** (this session, patch 0015): removed ~20% whole-proc of Audio Worker mutex-spin → libc
  DSO 22%→10%, gate-pass, render-correct — **floor-neutral** (kept as a CPU/power efficiency win, NOT a
  floor lever). Confirmed the latency-bound model: freeing a spin just idles a spare core.
- **Per-register-write micro-opt is EXHAUSTED** (prior): generic unchanged-write elision (~90% of writes)
  + the TYPE0 dispatch-skip — the body-elision was the last worthwhile bite.
- **De-spin / block-on-signal** (prior): CP `WAIT_REG_MEM`→CV block, guest Main→vblank-park — floor-neutral.
- **Codegen / µop / layout / flags / mcmodel / PGO / BOLT / ICF / outliner / ThinLTO / GPR-as-local**
  (6 prior sessions) — all floor-neutral. See `FLOOR-FRONTEND-REBASELINE-REPORT.md` + `FLOOR-OVEREXEC-REPORT.md`.
- **Thermal / CPU-saturation** (this session): REFUTED on-machine — not throttled, 11 of 12 cores idle.
- A bulk-constant `memcmp`-skip in `WriteRegistersFromMem` is a valid *throughput* micro-opt but is expected
  floor-neutral too (cuts CP CPU, not chain length) — only worth it as efficiency, not for the floor.
**Only chain-shortening (overlap/batch/cut-latency) can move the floor.**

## Validation discipline (mandatory)
- `tools/perf/detdiff.sh gate <label> 40` must be `status=pass`.
- **MID-COMBAT screenshots** (`tools/perf/smoke_shot.sh <tag>` does play→escalate→3 shots) — the gate will
  NOT catch a wrong-state rendering glitch; eyeball sprites/colors/transforms.
- Floor A/B: `tools/perf/ab_both.sh 75 6 base <bothfix exe> <bothfix so> cand <new exe> <new so>` (swaps
  BOTH binaries). Keep-bar: median p10 > +1.0 swaps/s vs bothfix, ≥5 heavy reps. The floor is **noisy
  run-to-run (~±2)** — interleave + ≥6 reps; bothfix is the stable ~15.0 anchor.
- `.so`-only change: `cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --target install` then
  `cp out/install/linux-amd64/lib64/librexruntime.so` into the port build dir (~20s, exe unchanged).
  `tools/perf/regen_build.sh full` only if an init_h.inja/header change (new exe).
- HOST GOTCHAS: (a) the harness BLOCKS a literal `sleep` token in a Bash string → put waits in a SCRIPT
  FILE (`tools/perf/wait_for.sh <file> <marker> <timeout>`); (b) `gamectl play` uses `setsid`; (c) ONE game
  instance only — run ONE `ab_both.sh` at a time and NEVER launch a 2nd while one is swapping binaries
  (they collide and corrupt both); (d) `gamectl kill` + `rm -f /dev/shm/xenia_memory_*` cleans the shm leak.
- ⚠ HARNESS: an early Bash command that exits non-zero CANCELS every later tool call in the same message.
  Run benchmark/commit steps ONE per message and end each Bash with `; true`. Also `pkill -f ab_both.sh`
  matches your OWN shell — build the pattern non-contiguously (`pat='ab''_both.sh'`) or use `gamectl kill`.

## Tools (tools/perf/)
`cp_offcpu.sh` (perf-sched on/off-cpu + sched-delay — THE decisive latency probe), `load_probe.sh`
(per-core MHz + hot-thread count under dip), `turbo_probe.sh` (turbostat power/freq), `heavydip.sh`
(per-thread/per-comm dip profile), `ab_both.sh` (both-exe+so A/B), `detdiff.sh` (gate), `smoke_shot.sh`,
`floor.sh`, `regen_build.sh`. This session's artifacts: `audio_thermal_findings_2026-05-30.txt` (raw
measurements + the two refuted hypotheses), `heavydip_elis1_baseline.txt`, `ab_audiofix.txt`.
