# Night-2026-06-09 measurement protocol (archived from /tmp/sp — that copy is volatile)

The canonical regression checks for ANY perf change (see STATUS.md):

- `measure45.sh <prefix> <pin01>` — natural level-1 run: pacing-diag v2.1
  timeline + per-core frequency sampling (the layer-B / capacity view).
- `measure7.sh <prefix> <spin01>` — cold-soak entry: boot, idle 35 s at
  campaign select, then enter the level (the layer-A / frequency-trap view;
  it is measure6 + `--freq_keeper`).
- `measure1/2/3.sh` — earlier iterations kept for provenance of the night
  m1–m6 artifacts; `m10_mainprof.sh` — guest-Main perf profile helper.

All scripts write artifacts to /tmp/sp/<prefix>_*. They drive the game via
gamectl.sh conventions (REX_INPUT_FILE protocol) and read `[pacing-diag]`
v2.1 fields: `iv a:b:c:d:e` (swap-interval histogram in vblank units),
`xlat/wrm/swp` (CP frame-slot split), `draws`, `vb/vbburst`.
Read them as: grid-pure iv columns at LOW draws = pacing regression;
mixed iv at 1600+ draws = the known capacity peak.

Rules: clean /dev/shm/xenia_memory_* first; hands off the host during runs
(background load warms clocks and corrupts layer-A comparisons); bound every
wait (the scripts already do).
