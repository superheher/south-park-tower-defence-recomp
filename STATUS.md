# STATUS — single source of truth

> Rewrite this file in place; do NOT let it grow. History lives in git; closed
> investigations live in `docs/archive/` and `varianta/archive/`.
> Last update: 2026-06-10 (maintainer playtest + checkpoint session).

## Goal

*South Park: Let's Go Tower Defense Play!* playable on THIS Linux host without
in-match lag. Success bar (maintainer, 2026-06-09): kill the deep lag on the
game field; dips to ~50 fps are imperceptible and acceptable; a locked 60 is
NOT required. 2026-06-10: maintainer confirms a big improvement and wants the
residual peak slow-mo finished ("feels almost there — polish it").

## Course (decided by the maintainer, 2026-06-09)

- **ACTIVE: SDK track (rexglue).** Fully playable end-to-end; the only open
  problem is the residual peak-load slow-mo (below).
- **FROZEN: variant A** (`varianta/` — from-scratch track). Research asset; do
  NOT iterate. Verdict at freeze: splashes + menu text with own engine; A↔B
  wall (needs a real GPU pipeline); est. 4–6 weeks to playable — not the path.
- **No Telegram.** Never message the maintainer via Telegram tools. Blocked on
  a user decision → record under BLOCKED, finish cleanly, stop. No busy-work.

## Ground truth on the lag (root-caused 2026-06-09; play-verified 2026-06-10)

Two layers (night runs m1–m12, /tmp/sp; playtest levels 1–2 + its run.log):

- **Layer A — CPU-frequency trap: FIXED.** powersave/EPP=power: menu idle
  sagged clocks to ~1 GHz; field entry then cost >33 ms/frame → 26–30 cadence
  lock from the first seconds, self-sustaining = 2.3–2.5× game-time dilation.
  Fix: EPP=performance persisted (systemd `cpu-epp-performance.service`,
  enabled) + `--freq_keeper` in gamectl. Cold-entry now 60.0 @2.9 GHz (m12).
- **Layer B — chain capacity: REMAINS at the heaviest waves.** Serial per
  frame: guest sim → single-thread CP PM4→Vulkan translate (~8.4 µs/draw
  post-elision). When translate alone overruns the 16.7 ms budget, the title's
  in-stream vblank-fence wait (WAIT_REG_MEM) re-quantizes swaps to a clean
  2-vblank grid: exact 30.0 — and sim (1 tick/frame) = exact 2× slow-mo.
- **Playtest telemetry (2026-06-10 run.log):** session avg 54.2 swaps/s, max
  60.0, min 30.0 (old 20/12 grids gone). Episodes: ~12 s @43–55 (1600–1840
  draws/swap, mixed 1–2-vblank) and ~35 s @30.0 clean grid (level-2 peak
  waves, 1900–2250 draws/swap). Felt as "game slows down, sound + gamepad stay
  normal" — correct: audio/input threads are off the render chain.
- **Perception note (maintainer asked "is it sped up?"):** NO. Pre-fix the
  field START ran 2.3–2.5× slow-mo and was learned as "normal"; native pace
  now reads as "on steroids". Cap is structural: XE_SWAP vsync=true limiter
  paces swaps to 60 Hz, sim ticks ≤1/frame ⇒ ≤1× native; measured ceiling is
  exactly 60.0 in every window; 0018/0019 do not touch game-time.
- Disproven earlier: vblank bursts, guest double-submission, host limiter.

## What's known (validated — do not redo)

- Mechanism (2026-05-31): CP translate single-threaded ≈43.7% of process;
  guest Main waits half its time on own render; GPU ~3%; cores free.
  Latency-bound, not CPU-saturated.
- **24 perf levers tried and closed** (PGO, BOLT, ICF, mcmodel, -Os, outliner,
  ThinLTO, *_as_local, de-spins, write-elision, GPU-offload, order-safe draw
  batching, …): `docs/archive/FLOOR-*`, memory `sp_floor_*`. Kept: mcmodel=
  medium, de-spin/elision. **Do NOT retry closed levers.**
- vsync=false is NOT a lever: uncapped IMMEDIATE present makes the sim run at
  render speed (fast-forward on light scenes) — see the XE_SWAP limiter.
- Boot: intermittent first-present deadlock; gamectl bounds + retries it.
- south_park_td leaks a 4.5G `/dev/shm/xenia_memory_*` per launch; gamectl
  `kill_all` reclaims them. After manual launches clean /dev/shm by hand or a
  later boot SIGBUSes.

## Shipped & checkpointed

- Night 2026-06-09→10 (all gated): 0017 pacing-diag v2, 0018 freq_keeper,
  0019 eager swap-fence push. First-seconds throttling dead; stage-complete
  30→60; 60-ceiling ~1100→~1300 draws/swap (level-1); ab_both p10 +2.5.
- **Checkpoint 2026-06-10 (the play-verified build):** patch series 0001–0019
  byte-verified == live SDK tree (tree `9e8c411`). Rollback anchors: sdk
  branch `checkpoint/2026-06-10-playable` (`c38a35e`) and tag
  `checkpoint-2026-06-10-playable` in this repo. Live
  `/home/h/src/recomp/gamectl.sh` is now a symlink to `tools/gamectl.sh`
  (forks merged; `REX_LOG_LEVEL`/`REX_EXTRA_ARGS` kept).

## How to run / measure

- `/home/h/src/recomp/gamectl.sh play | bench [sec] | boot | shot <name> | kill`
- Canonical perf check: natural level play (NO wave-spam probes), bench during
  waves + the pacing-diag v2.1 line; regression protocol `/tmp/sp/measure*.sh`.
- A/B keep-bar: median p10 swaps +1.0 (`tools/perf/ab_both.sh`); determinism
  gate (`tools/perf/detdiff/`); eyeball screenshots for render correctness.
- Do NOT run GUI sessions when the maintainer may be at the machine (daytime);
  coordinate first or measure overnight.

## Next steps

1. ~~Maintainer plays by feel~~ **DONE 2026-06-10** — verdict in Ground truth.
   Course: finish the residual peak slow-mo.
2. **Size the target before picking the lever:** capture peak draws/swap on
   levels 3+ (bench each level's biggest wave). Level 2 alone needs ~15% lower
   per-draw translate cost (≤ ~7 µs/draw to fit 2250 draws in budget); later
   levels likely need more headroom — pick the lever for the measured ceiling.
3. The two open levers (unchanged): texture-array draw batching (translate-cost
   cut, XL — `docs/DRAW-BATCHING-STEP1-RECIPE.md`, `docs/DRAW-GROUP-BREAKDOWN.md`)
   and true guest↔CP pipelining + swap-at-ring-arrival fence semantics (removes
   the 2-vblank re-grid even when translate >16.7 ms; deep, multi-session).
   Codegen/spin/elision classes stay CLOSED.
4. Keep the cold-soak + heavy-wave protocol (`/tmp/sp/measure*.sh`, pacing-diag
   v2.1 fields) as the regression check for any future change.

## BLOCKED

(nothing — clear this section when resolving)

## Rules for working sessions

- Decision authority: success criteria above; technical forks are the agent's
  call — decide by data, record here, keep going. Stop only on a verified
  result or a hard external blocker.
- This file is the entry point; keep it ≤ ~120 lines; rewrite in place.
- `docs/NEXT-SESSION-PROMPT.md` ≤ 3 KB, OVERWRITTEN each session.
- Per-iteration journaling = git commit messages (+ ≤10-line update here).
  Detailed write-ups only for CLOSED investigations → `docs/archive/`.
- Bound every GUI wait (`timeout` + bail); never idle-wait a flaky boot.
- Verify by running; screenshots to /tmp/sp; never claim success unverified.
