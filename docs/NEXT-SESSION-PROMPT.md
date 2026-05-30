# South Park recomp — next-session prompt: the heavy-combat floor is STRUCTURAL (do not re-chase blindly)

> **READ FIRST:** `docs/FLOOR-STRUCTURAL-CONCLUSION-REPORT.md` and memory `sp_two_regime_lag`.
> This session (2026-05-30/31) closed the floor campaign with a multi-probe, on-machine,
> user-corroborated conclusion: **the 15–20 fps heavy-combat floor is the title's OWN per-frame CPU
> cost at a fixed-60 Hz timestep**, in a serial *guest-logic → render → guest-waits* round-trip. It is
> NOT an emulator inefficiency, NOT GPU-bound, and cannot be moved without speeding up the game.

## What was proven (don't re-derive — see the report for the probes/numbers)
- The combat clock is **vblank-coupled fixed-timestep** — the decisive proof: at `REX_VIDEO_MODE_REFRESH_RATE=120`
  the **combat ran ~2× faster** (user-watched), while menus/splashes (wall-clock-paced) did not. So any
  lever that lets the guest proceed faster **speeds up the game** ⇒ the vsync/quantization lever is unsafe.
- Frame periods are **hard-quantized to vblank multiples** (`quant_diag.sh`): a lever must cut **≥16 ms**
  off a heavy frame to cross one boundary; every available CPU/codegen lever saves < 16 ms ⇒ floor-neutral
  (explains all 7 prior neutral sessions).
- The GPU is **idle** in the dip (`gpu_busy_probe.sh`, p50=3%) ⇒ not GPU-bound; draw-batching helps the
  CPU-side render, not the GPU.
- The guest "Main XThread" is **92% recompiled GAME code, 0.9% emulator** (`guest_profile.sh`); **51% is one
  guest spin-WAIT** (`sub_821B9270`) for its own render round-trip (already de-spun, floor-neutral).
- A latency A/B this session (`VblankBackoffWait` 2 ms→200 µs) was **floor-neutral** (kept p10 19.3 vs
  19.8) — the guest wait is gated by real frame-completion, not re-check granularity. Reverted.

## The ONLY remaining theoretical lever (and why it needs YOU present)
**Draw-call batching / CP-translate reduction.** A heavy 2-vbl frame ≈ guest-logic(~16 ms) + CP-render(~17 ms),
serial, GPU idle. In the **deep 15 fps dip** (4 vbl/66 ms, render ~34 ms), *halving* the CP render could
reach < 50 ms ⇒ 20 fps (+5 swaps — a real boundary cross). It is the one lever with headroom. BUT it is a
large, risky change (coalescing draws / cutting `radv_UpdateDescriptorSets` ~7.5% / descriptor-set re-bind
elision); a subtle bug corrupts combat rendering and the detdiff gate may not catch a wrong-state glitch.
**Do this only with the user available to eyeball mid-combat rendering**, and gate hard. Even then it only
*might* cross one boundary; it will not reach 60 fps.

## Constructive work that IS achievable
1. **Fresh-install transient smoothness** (the OTHER lag regime): on a cold machine the first playthrough
   compiles shaders/pipelines on the fly → frame-skip freezes (`vulkan_async_skip_incomplete_frames`).
   Ship/verify a pre-warmed pipeline cache so a fresh first playthrough is smooth. The local machine is
   already warm, so this benefits new installs, not the dev box.
2. Otherwise: **the floor is done.** Don't burn sessions on CPU/codegen micro-levers — proven floor-neutral.

## Environment / discipline (unchanged)
Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`) + submodule `south-park-recomp` (port, `main`);
SDK edits = `patches/0*.patch` on the upstream gitlink. Kept binary: exe `848f191c` (`south_park_td.cppfence6`,
unchanged) + `.so` `1dd98fdb` (audiofix + patch-0016 trace-writer inline; was `8d2bf92e`).
Identity `superheher <heh@vivaldi.net>`, commit (NOT push), no Co-Authored-By.
Host i9-8950HK + RX 460/RADV, governor=performance, sudo `<redacted>`, NOT thermally throttled. New tools in
`tools/perf/`: `cp_offcpu_stacks.sh`, `quant_diag.sh`, `gpu_busy_probe.sh`, `cold_cache_probe.sh`,
`slack_probe.sh`, `guest_profile.sh`. Harness: never batch tool calls after a Bash that can exit non-zero;
put `sleep` in a script file (`tools/perf/wait_for.sh`); ONE game instance at a time.
