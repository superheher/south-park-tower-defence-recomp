# Next session — SDK track: characterize, then kill the field lag

Read `STATUS.md` (repo root) first — it is the single source of truth.
Course: SDK track ACTIVE; `varianta/` FROZEN (do not touch).

## Task 1 — measure the real lag (no fixes yet)

1. Boot via `/home/h/src/recomp/gamectl.sh play` (bounded, auto-retries).
2. On a NATURAL level-1 start, from the moment control is given, capture
   simultaneously:
   - video: `ffmpeg -f x11grab` of the game window (`gamectl.sh wid`), 60–90 s;
   - `[pacing-diag]`: `gamectl.sh bench 60` in parallel;
   - one whole-process perf profile window during visible lag (`tools/perf/`).
3. Characterize the lag: constant-low vs periodic stutter vs degrading; the
   menu-vs-field delta; first seconds vs later waves. NOTE the maintainer's
   ground truth (STATUS): deep lag starts in the FIRST seconds of field
   control — if measurements disagree, trust the video + report the conflict.
4. Overwrite STATUS.md "Next steps" with findings; keep artifacts in /tmp/sp.

## Task 2 — one lever, chosen by Task-1 data

- Serial latency dominant → spike the guest↔CP OVERLAP (STATUS, untried #1).
- A single draw-group dominant → `docs/DRAW-BATCHING-STEP1-RECIPE.md`.
- Gate every change: `tools/perf/detdiff` + `ab_both.sh` (keep-bar median p10
  +1.0) + screenshots for render correctness.

## Hard rules

- Levers in `docs/archive/FLOOR-*` are CLOSED — do not retry them.
- Bound all GUI waits. If blocked on a user decision → STATUS.md `BLOCKED` +
  stop cleanly. No Telegram. No busy-work while waiting.
- Commit each logical step (author Claude Code). Overwrite THIS file at session
  end with the next concrete task (keep it ≤ 3 KB).
