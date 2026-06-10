# Next session — size the level-3+ peaks, then pick ONE capacity lever

Read `STATUS.md` first — single source of truth. Maintainer play-verified the
night fixes on 2026-06-10 (levels 1–2, gamepad): big improvement, start-of-
field throttling gone (the "game feels sped up" = removed 2.3–2.5× dilation;
explained in STATUS Ground truth). Residual: level-2 peak waves (1900–2250
draws/swap) re-grid to a clean 30.0 for ~35 s = exact 2× slow-mo. Checkpoint
anchors exist: sdk branch `checkpoint/2026-06-10-playable`, repo tag
`checkpoint-2026-06-10-playable`.

## First: size the target (cheap, do before any lever work)

Bench the biggest wave of levels 3+ (natural play protocol, pacing-diag v2.1):
peak draws/swap per level decides the lever. Level 2 alone needs ~15% lower
per-draw translate cost (~8.4 → ≤ ~7 µs/draw); if later levels push 3000+,
batching alone won't reach — plan for pipelining + swap-at-ring-arrival.
Coordinate with the maintainer or run overnight — no GUI sessions while he may
be at the machine.

## The two open levers (pick ONE, by the measured ceiling)

- Texture-array draw batching (translate-cost cut, XL):
  `docs/DRAW-BATCHING-STEP1-RECIPE.md`, `docs/DRAW-GROUP-BREAKDOWN.md`.
- True guest↔CP pipelining + swap-at-ring-arrival fence semantics (removes the
  2-vblank re-grid even when translate >16.7 ms; deep, multi-session).
- NOT levers: vsync=false (sim runs at render speed = fast-forward),
  codegen/spin/elision classes (CLOSED, `docs/archive/FLOOR-*`).

## Gates for ANY change

`tools/perf/ab_both.sh 40 3` (keep-bar median p10 +1.0) + render screenshots +
the cold-soak protocol (`/tmp/sp/measure7.sh`); detdiff when the translation
path changes. Grid-locks in iv (pure 2/3 columns) at LOW draws = regression.

## Hygiene

- `rm -f /dev/shm/xenia_memory_*` before runs (leak → SIGBUS); gamectl
  `kill_all` now reclaims them.
- Hands off the host during measurement runs (background load warms clocks and
  corrupts layer-A comparisons).
- Commit per step (author "Claude Code"); patches via the worktree recipe in
  `patches/README.md`; verify the series byte-identical before committing.
