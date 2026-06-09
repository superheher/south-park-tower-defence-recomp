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

- **Layer A — CPU-frequency trap (the "first seconds" lock).** Host runs
  intel_pstate ACTIVE+HWP, governor=powersave, **EPP=power**, 0.8–2.9 GHz.
  Idling in menus sags clocks to ~1.0 GHz; field entry then costs >33 ms/frame
  → clean 2–3-vblank lock (26–30 swaps) from the first seconds; the locked
  chain keeps per-core util low (threads migrate) so HWP never ramps —
  self-sustaining. Proven causal BOTH ways (m6a/m6b cold-soak runs: 35 s idle
  at campaign select → enter): no aid = 26–30 lock @1.0–1.2 GHz; +1-core
  nice-19 spinner = same scene 60.0 @2.9 GHz. Menu immune (≈20–140 draws/swap
  fits 16.7 ms even at 1 GHz) — exactly the maintainer's menu-vs-field delta.
- **Layer B — chain capacity (heavy waves).** At ~1500–1800 draws/swap the
  serial guest→CP chain exceeds 16.7 ms at FULL 2.9 GHz → clean 30.0 lock
  (iv 0:60:0:0:0). CP translate ≈10 µs/draw; "entity count" was wrong only as
  the *sole* cause — draws/swap scales with waves and sets WHERE layer B bites.
- Disproven: vblank delivery bursts (vbburst 0 always), guest double-submission
  (draws/swap mode-invariant), host limiter (sleeps ≈0 in locks).

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
- **Untried levers with real headroom:**
  1. **Pipeline overlap** — let the CP translate frame N while the guest sims
     frame N+1 (double-buffer guest GPU state + fence rework). The shipped
     "block-on-signal" only de-spun the wait; overlap itself was never tried.
  2. **Draw-call batching via texture arrays** (~816 draws/frame; measured ~7×
     run-length headroom): `docs/DRAW-BATCHING-STEP1-RECIPE.md`,
     `docs/DRAW-GROUP-BREAKDOWN.md`.
  3. Audio `WaitMultiple` targeted-wake rewrite (efficiency only; the naive
     shared-CV version thundering-herds — see memory).
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

## Next steps (in order)

1. ~~Measure first~~ DONE (see Ground truth; pacing-diag v2 = patch 0017).
2. **Layer A fix.** (a) Runtime: cvar-gated freq-keeper (1-core nice-19 spinner
   thread in south_park_td while rendering) — implement, verify with the
   m6 cold-soak protocol, ship as patch 0018. (b) **MAINTAINER MORNING
   ONE-LINER (root, permanent, replaces the spinner):**
   `echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference`
   (or governor=performance / EPP=balance_performance persisted via tuned).
3. **Layer B lever: guest↔CP pipeline overlap** (untried #1) — target: heavy
   waves 30 → ≥45–50 swaps (success bar: dips to ~50 are acceptable).
   Draw-batching/texture-arrays stays the fallback if overlap alone misses.
4. Gate every change (detdiff + ab_both keep-bar + screenshots), re-measure
   with the m6 cold-soak + heavy-wave protocol; final check = maintainer feel.

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
