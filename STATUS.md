# STATUS — single source of truth

> Rewrite this file in place; do NOT let it grow. History lives in git; closed
> investigations live in `docs/archive/` and `varianta/archive/`.
> Last update: 2026-06-09 (course-reset session).

## Goal

*South Park: Let's Go Tower Defense Play!* playable on THIS Linux host without
in-match lag. Success bar (maintainer, 2026-06-09): kill the deep lag on the
game field; dips to ~50 fps are imperceptible and acceptable; a locked 60 is
NOT required.

## Course (decided by the maintainer, 2026-06-09)

- **ACTIVE: SDK track (rexglue).** The build is fully playable end-to-end
  (boot → menu → match → win → save → continue, audio + input, Linux/Vulkan).
  The only open problem is in-match performance.
- **FROZEN: variant A** (`varianta/` — from-scratch XenonRecomp + own runtime +
  own renderer). Research asset; do NOT iterate on it. Journals/plans:
  `varianta/archive/`. Verdict at freeze: renders boot splashes + reconstructed
  menu text with its own engine; blocked on the A↔B wall (needs a real GPU
  pipeline); est. 4–6 weeks to playable — not the goal path.
- **No Telegram.** Never message the maintainer via Telegram tools. When blocked
  on a user decision: record it under BLOCKED below, finish cleanly, stop. No
  busy-work iterations while waiting.

## Ground truth on the lag (maintainer 2026-06-09 + night measurement — ROOT-CAUSED, two layers)

Maintainer: lag from the FIRST seconds of field control; menu fine; feels like
"throttling". Night runs m1–m6 (video + pacing-diag v2 + perf + per-core freq
sampling, artifacts /tmp/sp) explain all of it. The lag = exact vblank-divisor
cadence locks (60/30/20/12 swaps/s) + game-time dilation (sim drops to 60/k;
measured 2.5 s per countdown-second = slow-mo ⇒ the "throttling" feel). Layers:

- **Layer A — CPU-frequency trap (the "first seconds" lock).** intel_pstate
  ACTIVE+HWP, governor=powersave, **EPP=power** (0.8–2.9 GHz): menu idling sags
  clocks to ~1 GHz; field entry then costs >33 ms/frame → 26–30 lock from the
  first seconds; the locked chain keeps per-core util low (migrations) so HWP
  never ramps — self-sustaining. Causal BOTH ways (m6a/m6b 35 s cold-soak →
  enter): no aid = 26–30 @1.0–1.2 GHz; +1-core nice-19 spinner = 60.0 @2.9 GHz.
  Menu immune (≈20–140 draws/swap fits 16.7 ms even at 1 GHz).
- **Layer B — chain capacity (heavy waves).** ~1500–1800 draws/swap exceeds
  16.7 ms at FULL clock (translate ≈10 µs/draw) → clean 30.0 lock pre-fix.
- Disproven: vblank bursts (vbburst 0), guest double-submission (draws/swap
  mode-invariant), host limiter (sleeps ≈0 in locks).

## What's known (validated — do not redo)

- Mechanism (measured under natural load, 2026-05-31): single-threaded CP
  PM4→Vulkan translation ≈43.7% of process; guest Main spends half its time
  waiting on its own render (`__imp__sub_821B9270` 13.3%); GPU ~3% busy; cores
  free. Serial chain per frame: guest-sim → CP-translate → GPU → present.
  Latency-bound, not CPU-saturated.
- **24 perf levers tried and closed** (PGO, BOLT, ICF, mcmodel, -Os, outliner,
  ThinLTO, *_as_local, de-spins, write-elision, GPU-offload, order-safe draw
  batching, …): `docs/archive/FLOOR-*`, memory `sp_floor_*`. Kept wins:
  `-mcmodel=medium` (floor +0.65), de-spin/elision efficiency (avg +2.4).
  **Do NOT retry closed levers.**
- **Lever state after the 2026-06-09 night:** overlap step 1 (eager fence
  push) SHIPPED as patch 0019; remaining headroom = true pipelining
  (double-buffer guest GPU state — deep) and texture-array draw batching
  (~7× run-length headroom, XL): `docs/DRAW-BATCHING-STEP1-RECIPE.md`,
  `docs/DRAW-GROUP-BREAKDOWN.md`. Audio `WaitMultiple` targeted-wake rewrite
  is efficiency-only (naive shared-CV thundering-herds — see memory).
- Boot: intermittent first-present deadlock exists; `gamectl.sh` bounds and
  retries it. 2026-06-09 evening: boots attempt-1 interactively (the overnight
  "prod dead-locks here" was environmental, not a property of the build).

## How to run / measure

- `/home/h/src/recomp/gamectl.sh play | bench [sec] | boot | shot <name> | kill`
  (header comments document the input protocol and why `play` works as it does).
- Canonical perf check: natural level-1 play (NO wave-spam probes), bench during
  the first waves + video capture.
- A/B keep-bar: median p10 swaps +1.0 (`tools/perf/ab_both.sh`); determinism
  gate (`tools/perf/detdiff/`); eyeball screenshots for render correctness.

## Night 2026-06-09→10 RESULT (shipped: patches 0017, 0018, 0019 — all gated)

- **Throttling is dead** (cadence locks + slow-mo): cold-entry first seconds
  26.5–30.0 locked → **60.0** (m11 final-build check); stage-complete screen
  30.0 → 60.0; 60-capable ceiling ~1100 → ~1300 draws/swap; ab_both p10 +2.5.
- 0018 `freq_keeper` (layer A, on in gamectl/RUN-linux): kills the EPP trap.
- 0019 eager swap-fence push (layer B step 1): de-quantizes the guest loop.
- **Remaining gap:** heaviest ~15–20 s wave bursts (~1700+ draws/swap) dip to
  38–51 honest fps (no grids, no slow-mo). Serial: CP translate 14–16 ms +
  guest sim ~8 ms. Below the "~50 imperceptible" bar only at the very peak.

## Next steps

1. ~~Root EPP fix~~ **DONE by the agent (2026-06-09 23:47):** EPP=performance
   set on all cores AND persisted across reboots via systemd unit
   `cpu-epp-performance.service` (enabled). `--freq_keeper` stays on in
   gamectl as belt-and-suspenders; safe to drop later. Verified: m12
   cold-soak on this config — entry 60.0, clocks pinned 2.9 GHz.
2. **Maintainer plays by feel** (the success check). Expect: no throttling
   anywhere; only brief honest dips (41–56, ~14 s) at the biggest wave peak.
3. If the peak dips still bother: the ONLY remaining levers are true guest↔CP
   pipelining (double-buffer guest GPU state + report swap at ring-arrival —
   deep, multi-session) or translate-cost cuts via texture-array batching
   (`docs/DRAW-BATCHING-STEP1-RECIPE.md`, XL). Codegen/spin/elision lever
   classes remain CLOSED.
4. Keep the m6/m7 cold-soak + heavy-wave protocol (`/tmp/sp/measure*.sh`,
   pacing-diag v2.1 fields) as the regression check for any future change.

## BLOCKED

(nothing — clear this section when resolving)

## Rules for working sessions

- **Decision authority (maintainer, 2026-06-09):** success criteria are recorded
  above; technical forks (which lever, revert-or-persist) are the agent's call —
  decide by data, record here, keep going. Stop only on a verified result or a
  hard external blocker. Do not wait on the maintainer overnight.
- This file is the entry point; keep it ≤ ~120 lines; rewrite in place.
- `docs/NEXT-SESSION-PROMPT.md` ≤ 3 KB, OVERWRITTEN each session — never
  append-grown.
- Per-iteration journaling = git commit messages (+ at most a 10-line update
  here). No cumulative NIGHT-LOG-style files. Detailed write-ups only for
  CLOSED investigations → `docs/archive/`.
- Bound every GUI wait (`timeout` + bail); never idle-wait a flaky boot.
- Verify by running; screenshots to `/tmp/sp`; never claim success unverified.
