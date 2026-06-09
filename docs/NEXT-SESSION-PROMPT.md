# Next session — verify by feel, then (only if needed) the deep capacity lever

Read `STATUS.md` first — single source of truth. Night 2026-06-09→10 shipped
patches 0017 (pacing-diag v2.1), 0018 (`freq_keeper`), 0019 (eager swap-fence
push). The throttling (cadence locks + slow-motion) is dead in the measured
protocol; heaviest ~15–20 s wave peaks still dip to 38–51 honest fps.

## First: maintainer plays by feel (the real success check)

EPP=performance is already applied AND persisted (systemd unit
`cpu-epp-performance.service`, enabled 2026-06-09 night). Expected feel:
menus 60, field 60 from the first seconds (even after idling in menus), no
slow-mo; only brief honest dips (~41–56 for ~14 s) at the biggest wave peak.
`--freq_keeper=true` remains in gamectl as belt-and-suspenders.

## If the maintainer still feels lag

- Reproduce with the night protocol before touching anything:
  `/tmp/sp/measure45.sh mX 0` (natural run) and `/tmp/sp/measure7.sh mX 0`
  (cold-soak entry); read `[pacing-diag]` v2.1 fields
  (`iv a:b:c:d:e | xlat | wrm | swp | draws | vb`). Grid-locks (iv pure 2/3
  columns) = regression; mixed iv at high draws = the known capacity peak.
- Capacity levers left (pick ONE, by data): true guest↔CP pipelining
  (double-buffer guest GPU state; report swap at ring-arrival; deep,
  multi-session) or texture-array draw batching
  (`docs/DRAW-BATCHING-STEP1-RECIPE.md`, XL). Codegen/spin/elision classes
  stay CLOSED (`docs/archive/FLOOR-*`).

## Gates for ANY change

`tools/perf/ab_both.sh 40 3` (keep-bar median p10 +1.0) + render screenshots +
the cold-soak protocol; detdiff only when the translation path changes.

## Hygiene

- `rm -f /dev/shm/xenia_memory_*` before runs (leak → SIGBUS).
- Keep hands off the host during measurement runs (background CPU load warms
  clocks and corrupts layer-A comparisons).
- Commit per step (author "Claude Code"), patches via the worktree recipe in
  `patches/README.md`; verify the series byte-identical before committing.
