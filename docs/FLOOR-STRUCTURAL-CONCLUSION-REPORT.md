# South Park recomp — the heavy-combat floor is STRUCTURAL (the game's own fixed-60Hz cost)

> **Bottom line (measured this session, multiple independent probes + a user-watched on-machine test):**
> the 15–20 fps heavy-combat "floor" is **the title's own per-frame CPU cost at a fixed 60 Hz timestep**,
> in a serial *guest-logic → render → guest-waits* round-trip. The GPU is idle, the emulator runtime is
> <1% of the guest thread, and the combat clock is **vblank-coupled** (proven: at 120 Hz the combat ran
> ~2× faster). No host-side lever moves it without **speeding up the game**, and the per-frame work
> exceeds the 16.6 ms budget by more than any feasible micro-optimization can recover. This is the honest
> end of the floor campaign for this title on this hardware (i9-8950HK + RX 460 / RADV).

This supersedes the "next levers" framing in `sp_floor_latency_bound` / `NEXT-SESSION-PROMPT` (overlap
CP/GPU, draw-batch): those were reasonable hypotheses, but the measurements below show they cannot cross a
vblank quantization boundary and the round-trip cannot be overlapped (the guest is single-threaded
fixed-timestep and waits for its own render).

## Two distinct lag regimes (do not conflate)

A user observation this session forced a split that the campaign had been conflating:

1. **Transient first-encounter stutter** — the first time new content appears, its guest shaders translate
   (Xenos→SPIR-V, `.xsh`), Vulkan pipelines build, textures upload. With `async_shader_compilation=true`
   (default) a frame that needs an un-built pipeline is **skipped** (`vulkan_async_skip_incomplete_frames`,
   `vulkan/command_processor.cpp:2340`) → the screen *freezes* until ready. On a **cold** machine
   (cold `~/.cache/mesa_shader_cache` ISA cache) those builds are 10–100 ms each → multi-second freezes;
   they warm out and don't recur. `store_shaders=true` persists translations/pipelines to
   `private/userdata/cache/shaders/shareable/58410931.{xsh,fbo.vk.xpso}`, and at boot the storage-load
   **pre-builds all stored pipelines** on parallel "Vulkan Pipelines" threads (blocking the load,
   `vulkan/pipeline_cache.cpp:~678`). So a warm cache → zero mid-combat compiles. This regime is real but
   self-warming; the only shippable win is fresh-install pre-warming (the machine-specific Mesa ISA cache
   cannot be shipped).

   ⚠ **Correction (caught by the user):** an earlier claim that "today's smoothness = the cache warmed
   during my runs" was WRONG. The Mesa ISA cache had been accumulating since **2026-04-23** (warm for a
   month, incl. all prior play days), so cache-warming cannot explain a day-to-day change. The day-to-day
   difference is more likely that **the binary itself changed** (5+ rebuilds in one day, each altering
   CPU/timing). Unverified; the honest test is old-build vs current-build, same scripted run.

2. **Sustained vsync-quantized combat floor** — the 15–20 fps heavy combat. The subject of this report.

## The measurements (all reproducible; tools in `tools/perf/`)

**A. CP off-cpu blocking stacks** (`cp_offcpu_stacks.sh`, perf `sched:sched_switch` + dwarf, the "GPU
Commands" thread, heavy dip). The CP is ~50% on-cpu; its *blocked* time is:
- **~32 %** in `ExecutePacketType3_WAIT_REG_MEM → pthread_cond_clockwait` — the guest-inserted GPU/vblank
  fence wait (the patch-0012 block-on-signal CV, `command_processor.cpp:1316-1340`).
- **~5–9 %** in `IssueDraw → TextureCache::RequestTextures` / `SharedMemory::RequestRange(s) →
  pthread_mutex_lock` — the single global `rex::thread::global_critical_region` (recursive), where the CP
  contends with the **guest thread's synchronous memory-write-watch callbacks** (glyph/texture writes).

**B. Per-frame Δt histogram** (`quant_diag.sh`, diag build). In a dip the frame period is **hard-quantized
to vblank multiples**: frames cluster at 44–55 ms (3 vbl = 20 fps) and 55–70 ms (4 vbl = 15 fps); the
18–44 ms buckets are **near-empty**. The "44 fps" pacing-diag windows are time-mixes of 16.6 ms (60 fps)
and 66 ms (15 fps) frames, not steady 45. ⇒ **any work cut that does not cross a 16.6 ms boundary is
invisible** — this alone explains why 7 prior sessions of CPU/codegen levers were floor-neutral.

**C. GPU utilization** (`gpu_busy_probe.sh`, AMD `gpu_busy_percent`). In the dip: **p50 = 3 %**, 68 % of
samples < 20 %, avg 24 %, brief bursts to 95–100 %. ⇒ **NOT GPU-throughput-bound**; draw-batch / GPU-work
cuts cannot move the floor.

**D. Per-frame fence-wait split** (`slack_probe.sh`, diag build). As a frame gets heavier (avg_dt
16.7 → 35 ms) the CP fence-wait *drops* from ~10 ms (56 %, legit 60 Hz throttle on light frames) to
**~4–7 ms (12–19 %)** and is bounded ≤ 1 vblank. ⇒ a heavy frame is **work-dominated, not
slack-dominated** — there is no large removable "waiting" slack on the CP.

**E. Guest (Main XThread) profile** (`guest_profile.sh`, heavy dip). The decisive one:
- **92 % of the guest thread is the recompiled GAME code** (the exe); only **0.9 %** is librexruntime
  (emulator runtime). ⇒ the cost is the *game*, not emulator overhead to optimize away.
- **50.9 % of the entire guest thread is one function, `sub_821B9270`** (recomp.5.cpp:60004) — a guest
  **spin-WAIT** (a 32× `REX_SPIN_BACKOFF` delay loop + a queue-check + a 5000-tick-timeout poll). It is the
  guest waiting for its frame's render round-trip. Already de-spun to PAUSE + `VblankBackoffWait` by a
  prior session (floor-neutral). So a heavy frame = guest logic (~49 %) → submit → **wait for render
  (~51 %)**, serial.

**F. 120 Hz refresh test** (`REX_VIDEO_MODE_REFRESH_RATE=120`, user-watched — the one thing automation
can't judge). At 120 Hz: reached the level in 14 s (vs ~40), light content ~100 swaps/s (vs the 60 cap),
dip ~29 swaps/s (vs ~15). **The user confirmed by eye: combat ran ~2× faster, while menus/splashes ran at
normal speed.** ⇒ **the combat clock is frame/vblank-coupled fixed-timestep** (menus are wall-clock-paced).
The relative floor is unchanged (15/60 ≈ 29/120 ≈ 24 % of target). This *proves* any change that lets the
guest proceed faster **speeds up the game** — so the vsync/quantization lever is unsafe, and the floor is
the game's own per-frame work.

## Why no feasible lever moves it (the arithmetic)

A heavy 2-vblank frame ≈ 33 ms = guest-logic (~16 ms) + render (~17 ms), serial; the GPU is idle, so
"render" is CP translate + handshake (CPU), not GPU. To cross **one** vblank boundary you must cut **≥16 ms**:
- 33 ms → < 16.6 ms (60 fps) needs render → ~0 (impossible; the CP must translate the draws).
- 66 ms (deep dip) → < 50 ms (20 fps) needs ~16 ms off; halving the render (~34→17) *might* reach it, but
  draw-batching is a large, risky change and the GPU-idle data says the bottleneck is CP-serial + the
  round-trip, not GPU.
- The guest logic and render **cannot be overlapped**: the guest is single-threaded fixed-timestep and
  waits (sub_821B9270) for its own render before the next frame.

Every available lever (codegen / µop / layout / mcmodel / PGO / BOLT / ICF / outliner / ThinLTO /
GPR-as-local / de-spin / block-on-signal / write-elision — 7 sessions) saves **< 16 ms** and so cannot
cross a boundary; the off-cpu/guest data shows the residual is the game's own serial work. **Confirmed
structural.**

**Where the CP render (~17 ms) actually goes** (`cp_oncpu_profile.sh`, CP "GPU Commands" thread on-cpu in
a heavy dip; DSO = 79 % librexruntime + 13 % RADV + 7 % libc) — the map for any future CP-reduction session:
- **~23 % register writes** — `VulkanCommandProcessor::WriteRegister` 13.8 % + base `WriteRegister` 2.5 % +
  `ExecutePacketType0` 6.7 %. patch-0014 already skips *unchanged* writes; the residual is changed-value
  writes through the big register switch (optimizable but render-affecting → risky).
- **~7 % descriptor/binding** — `UpdateBindings` 2.5 %, `radv_UpdateDescriptorSets` 3.1 %,
  `radv_bind_descriptor_sets` 0.7 %, `WriteTransientTextureBindings` 0.7 % — the draw-batch target.
- **~3 % render-target/pipeline state** — `RenderTargetCache::Update`, `GetCurrentStateDescription`.
- **~3.2 % pure waste — the GPU trace writer**: `TraceWriter::WritePacketStart`/`WritePacketEnd` are called
  per PM4 packet even in normal play. They early-out on `if (!file_) return;` but were **non-inlined**, so the
  per-packet call overhead alone cost ~3.2 % of CP on-cpu for nothing. **Fixed this session (patch 0016):**
  moved inline into `trace_writer.h` — first `WritePacketStart`/`WritePacketEnd` (the measured 3.2 %), then
  extended to all the hot per-buffer / per-memory-op trace hooks (same pattern) for completeness, so the
  null-check folds to a load+branch with no call/ret. Byte-identical behaviour; `.so`-only; **detdiff gate=
  pass/equivalent (both the packet-only `.so` `1dd98fdb` and the full set `1a3f6076`), mid-combat render
  pixel-correct**. **Validated by re-profile** (`cp_oncpu_profile.sh`): `WritePacketStart`/
  `WritePacketEnd` dropped **out of the CP on-cpu top-30 entirely** (were 3.2%, now absent — folded into
  callers). **Floor A/B** (`ab_both.sh 60 6`, run twice — the packet-only `.so` and the full `.so` `1a3f6076`):
  baseline median p10 **19.3** vs patch-0016 **19.8** both times (+0.5, inside the ±2 noise → floor-neutral,
  no regression; patch-0016 ran ≥ baseline in 5/6 reps, a consistent tiny positive lean). Kept as a CPU
  efficiency win (same rationale as the
  audio de-spin). The remaining CP hotspots (register writes ~23 %, descriptors ~7 %) are genuine work, each
  saving < 4 ms even if fully eliminated, so none is a floor lever (and reducing them is render-affecting/risky).

## Empirical "long-shot" attempt this session

Per the user's request to try the game-logic/wait path directly, one latency experiment was run:
`VblankBackoffWait` re-check granularity **2 ms → 200 µs** (cut the guest render-wait overshoot; `.so`-only,
correctness-neutral — only changes how often a parked spin re-checks its unchanged condition).
- Candidate `.so` `f41ad176` (audiofix + 200 µs) vs kept audiofix `8d2bf92e`, same exe `848f191c`.
- **Result: FLOOR-NEUTRAL** (interleaved `ab_both.sh 60 6`, heavy-window median p10): kept **19.3**
  (19.3/19.8/19.3/19.8/18.7) vs vbwait200 **19.8** (21.5/16.9/19.3/19.8/20.0/20.2). +0.5 is inside the
  ±2 run-to-run noise (keep-bar is +1.0). **Reverted** (neutral, and 200 µs re-checks cost ~10× more CV
  wakeups than 2 ms — a slight efficiency loss for no gain). This empirically confirms the guest's
  render-wait is gated by actual frame-completion timing (vblank-coupled), not by re-check granularity —
  consistent with the structural conclusion.

## Honest recommendation
The heavy-combat floor is a hard limit for this title on this hardware. Constructive remaining work:
1. **Fresh-install smoothness** (transient regime): ship/верify a pre-warmed pipeline cache so a first
   playthrough doesn't compile on the fly (real win for new users; the local machine is already warm).
2. **Stop chasing the combat floor** — it is the game's own fixed-60Hz cost, not an emulator inefficiency.

## New tools (tools/perf/)
`cp_offcpu_stacks.sh` (CP block stacks), `quant_diag.sh` (per-frame Δt histogram — diag build),
`gpu_busy_probe.sh` (GPU utilization in the dip), `cold_cache_probe.sh` (transient-stutter / cold-cache),
`slack_probe.sh` (per-frame fence-wait split — diag build), `guest_profile.sh` (guest thread DSO + function
breakdown). The diag builds add `[quant-diag]`/`[slack-diag]` log lines; the instrumentation was reverted
from the committed source.
