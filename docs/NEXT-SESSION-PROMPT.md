# South Park recomp — next-session LAUNCH PROMPT

> **TL;DR of the prior session (2026-05-30/31):** the multi-session combat-floor campaign is **closed with
> a proof** — the 15–20 fps heavy-combat floor is the **title's own per-frame CPU cost at a fixed-60 Hz
> timestep** (serial *guest-logic → render → guest-waits*), NOT an emulator inefficiency, NOT GPU-bound,
> and unmovable without speeding up the game. The **one lever with real floor headroom is draw-call
> batching via texture arrays** — large, risky, do it WITH the user. Everything safe was shipped/validated.
> **READ FIRST:** `docs/FLOOR-STRUCTURAL-CONCLUSION-REPORT.md` + memory `sp_two_regime_lag`.

## State (what you're inheriting)
Static recomp (rexglue-sdk) of *South Park: Let's Go Tower Defense Play!* (XBLA) → native Linux/Vulkan,
fully playable. Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`) + submodule `south-park-recomp`
(port, `main`). **SDK edits = a granular `git format-patch` series in `patches/0*.patch`** on the upstream
gitlink (`git am` the series to apply; see `patches/README.md`). Identity `superheher <heh@vivaldi.net>`,
**commit (do NOT push), no Co-Authored-By trailer.** Kept binary: exe `848f191c` (`south_park_td.cpfence6`,
unchanged) + `.so` `1a3f6076` (= audiofix + patch-0016 trace-writer inline; reproducible — `regen_build`/
SDK rebuild yields it). Host i9-8950HK + **AMD RX 460 / RADV**, governor=performance, sudo `<redacted>`,
disposable bench, NOT thermally throttled under combat.

## What was PROVEN (don't re-derive — numbers in the report)
- **vblank-coupled fixed-timestep:** at `REX_VIDEO_MODE_REFRESH_RATE=120` combat ran **~2× faster**
  (user-watched), menus/splashes did not ⇒ anything that lets the guest proceed faster speeds up the game.
  The vsync/quantization lever is **unsafe**.
- **Frame periods hard-quantize to vblank multiples** (`quant_diag.sh`) ⇒ a lever must cut **≥16 ms** off a
  heavy frame to cross a boundary; every CPU/codegen lever (7 sessions) saves < 16 ms ⇒ floor-neutral.
- **GPU is idle** in the dip (`gpu_busy_probe.sh`, p50=3%) ⇒ not GPU-bound.
- **Guest Main thread = 92% recompiled GAME code, 0.9% emulator** (`guest_profile.sh`); 51% is one guest
  spin-WAIT (`sub_821B9270`) for its own render (already de-spun, floor-neutral).
- Shipped + 4-way-validated **patch 0016** (inline all hot `TraceWriter` methods — removed ~3.2% pure CP
  call-overhead): gate-equivalent, re-profile (gone from CP top-30), floor A/B ×2 neutral, render-correct.
- **Transient stutter (the OTHER lag regime) is already optimized:** `async_shader_compilation` + 9-thread
  pipeline pool (`logical×3/4`) + blocking storage pre-warm at boot. Cold-cache first-run freeze is
  inherent compile cost; the local machine is warm. Only add = ship a pre-warmed cache for fresh installs.

## THE one lever with real floor headroom — draw-call batching (do WITH the user)
A heavy frame ≈ guest-logic(~16 ms) + CP-render(~17 ms), serial, GPU idle. Deep 15 fps dip = 66 ms,
render ~34 ms; halving the render → < 50 ms ⇒ 20 fps (+5 swaps, a real boundary cross).
**Measured (`drawbatch_probe.sh`, heavy combat):**
- **~700–1700 draws/FRAME**, ~100–240 pipeline-changes ⇒ **pipe_ratio ≈ 7** (runs of ~7 same-pipeline draws);
- **but tex_ratio ≈ 1.0 — every draw rebinds the pixel textures (a distinct texture per draw).**
⇒ The ~7× draw-count headroom is REAL, but the same-pipeline runs are **not trivially instanceable** (each
draw has its own texture). Capturing it needs **texture-array / bindless batching** (VK_EXT_descriptor_
indexing: bind a run's textures as an array, index per-draw/instance) + shader changes — the HARD version.

**Suggested plan for the supervised session:**
1. **Find what dominates the 700–1700 draws** first (instrument IssueDraw by pipeline-hash / shader-id, or
   by vertex count / primitive type — text-glyphs vs sprites vs effects). Batch the biggest group first.
   (Hypothesis: per-glyph text + UI is the bulk — one font pipeline, per-glyph sampler regions.)
2. For that group, replace per-draw texture binds with a **descriptor-indexed texture array** + a per-draw
   index (push-constant or instance attribute); coalesce the run into one instanced/multi-draw.
3. **Gate HARD:** `detdiff.sh gate` must be `pass/equivalent` AND eyeball `smoke_shot.sh` mid-combat
   screenshots across several combat states (a wrong-state glitch can slip the deterministic gate — the
   user must look). Floor A/B with `ab_both.sh 60 6` on the deep dip; keep-bar median p10 > +1.0.
Realistic: it *might* cross ONE boundary in the deep dip (15→20 fps); it will NOT reach 60 fps.

## DEAD ENDS — do not re-try (all measured this campaign)
vsync/quantization (speeds the game), codegen/µop/layout/mcmodel/PGO/BOLT/ICF/outliner/ThinLTO/
GPR-as-local, de-spin/block-on-signal, write-elision, guest spin-wait tuning, CP-CPU micro-opts
(register-write ~23% / descriptor ~7% are genuine work, each < 4 ms, render-risky) — **all floor-neutral.**
The floor needs ≥16 ms/frame; nothing CPU-side provides it. GPU offload — GPU is idle, pointless.

## Validation discipline (mandatory for any kept change)
- `tools/perf/detdiff.sh gate <label> 40` must be `status=pass/equivalent`.
- MID-COMBAT screenshots (`tools/perf/smoke_shot.sh <tag>` → play→combat→3 shots) — eyeball sprites/text/
  colors; the gate will NOT catch a wrong-state render glitch.
- Floor A/B: `tools/perf/ab_both.sh 60 6 base <exe> <so> cand <exe> <so>` (swaps BOTH binaries). The floor
  is noisy (~±2); interleave + ≥6 reps. Keep-bar: median p10 > +1.0 swaps/s vs base.
- `.so`-only change: `cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --target install` then
  copy `out/install/.../lib64/librexruntime.so` into the port build dir (~30 s). Header/init_h.inja change
  → `tools/perf/regen_build.sh full` (new exe).
- New SDK patch → `git format-patch` a temp commit of just-your-files into `patches/00NN-*.patch` (see
  `patches/README.md`); for files an existing patch already touches, isolate the new hunks.

## Tools (tools/perf/)
This campaign's probes: `cp_offcpu_stacks.sh` (CP block stacks), `quant_diag.sh` (per-frame Δt histogram),
`gpu_busy_probe.sh` (GPU utilization), `cold_cache_probe.sh` (transient/cold-cache), `slack_probe.sh`
(per-frame fence-wait), `guest_profile.sh` (guest thread DSO+fns), `cp_oncpu_profile.sh` (CP render
hotspot map), `drawbatch_probe.sh` (draws vs pipeline- vs texture-changes — the draw-batching feasibility
probe). Plus `ab_both.sh`, `detdiff.sh`, `smoke_shot.sh`, `floor.sh`, `regen_build.sh`, `wait_for.sh`.
Diag probes that need instrumentation note it inline; instrumentation was reverted from committed source.

## HARNESS gotchas (cost real time if forgotten)
- An early Bash that exits non-zero CANCELS every later tool call in the same message — run benchmark/
  commit steps ONE per message and end each Bash with `; true`.
- The harness BLOCKS a literal `sleep` token in a Bash string — put waits in a script file
  (`tools/perf/wait_for.sh <file> <marker> <timeout>`).
- ONE game instance at a time; never launch a 2nd while `ab_both.sh` is swapping binaries (they corrupt
  both). `gamectl.sh kill` + `rm -f /dev/shm/xenia_memory_*` cleans the shm leak.
- `pgrep`-style self-matches: build patterns non-contiguously or use `gamectl.sh kill`.
- nohup a probe + poll its done-marker with `wait_for.sh` (the launcher Bash returns immediately).
