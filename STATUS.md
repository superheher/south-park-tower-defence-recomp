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

## Ground truth on the lag (maintainer, 2026-06-09 — overrides ALL prior framings)

- The lag is there **from the very first seconds of control on level 1, first
  wave** — not only in late heavy waves.
- **Menu/frontend: no lag. Game field: lag.** That delta is the thing to
  explain and fix.
- Prior framings — "only heavy combat" and "scales with entity count" — are
  **wrong** per the maintainer's direct observation. He describes the behavior
  as **"throttling"-like**, not mere fps dips.
- Measure with a screen **video recording** (ffmpeg x11grab) + `[pacing-diag]`
  log on a NATURAL level-1 start. The host is fully dedicated to this project —
  recording/profiling is always OK.

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

1. **Measure first, fix nothing:** reproduce the maintainer-visible lag on a
   natural level-1 start; record video + pacing log + one whole-process perf
   window during visible lag. Characterize: constant-low vs stutter vs
   degrading-over-time; menu-vs-field delta; first seconds vs later waves.
2. Pick the lever by data: guest↔CP overlap (likely) and/or draw-batching
   step 1.
3. Re-measure against the success bar; final check = the maintainer plays and
   confirms by feel.

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
