#!/usr/bin/env bash
# profile.sh -- one-shot CPU micro-arch snapshot of the RUNNING game during combat.
# Captures: Topdown L1 (+ per-thread Main/CP), front-end L2 split (latency vs
# bandwidth), key counters (IPC, L1i/L1d miss, branch-miss, dTLB), and a GPU-busy
# sample. Use it for BEFORE/AFTER of any change and to (re)confirm the bottleneck.
# Requires perf_event_paranoid<=0 (set by lib: echo -1 > .../perf_event_paranoid).
set -u
PID=$(pgrep -x south_park_td) || { echo "no game running"; exit 1; }
GAME_DIR="${REX_GAME_DIR:-/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release}"
SEC="${1:-12}"
fps(){ grep pacing-diag "$GAME_DIR/run.log" 2>/dev/null|tail -1|grep -oE 'swaps [0-9.]+/s'; }
tid(){ ps -T -p "$PID" -o tid,comm 2>/dev/null|awk -v c="$1" '$2==c{print $1;exit}'; }

echo "=== profile @ $(date '+%H:%M:%S')  fps=$(fps)  PID=$PID ==="
echo "--- GPU busy% (want LOW => CPU-bound); should stay idle after any win ---"
( timeout $((SEC+2)) radeontop -d - -l $SEC 2>/dev/null | grep -oE 'gpu [0-9.]+%' | sort | tail -3 ) || echo "  (radeontop n/a)"

echo "--- Topdown L1 (whole process, ${SEC}s) ---"
perf stat -M TopdownL1 -p "$PID" -- sleep "$SEC" 2>&1 | grep -iE 'tma_(retiring|frontend|backend|bad)'

echo "--- Topdown L1 per-thread ---"
for c in Main GPU; do t=$(tid "$c"); [ -n "$t" ] && {
  echo "  [$c tid=$t]"; perf stat -M TopdownL1 -t "$t" -- sleep 6 2>&1 | grep -iE 'tma_(retiring|frontend|backend|bad)' | sed 's/^/    /'; }
done

echo "--- Front-end L2 split (latency=icache/itlb/resteer  vs  bandwidth=decode/DSB) ---"
perf stat -M tma_fetch_latency,tma_fetch_bandwidth -p "$PID" -- sleep 6 2>&1 | grep -iE 'tma_fetch' || \
  echo "  (metric names unavailable on this perf; rely on raw events below)"

echo "--- Raw frontend events (${SEC}s): IPC, I-cache, DSB/MITE, branch-mispred, dTLB ---"
perf stat -p "$PID" -e cycles,instructions,L1-icache-load-misses,iTLB-load-misses,\
branches,branch-misses,L1-dcache-load-misses,dTLB-load-misses \
  -- sleep "$SEC" 2>&1 | grep -iE 'cycles|instructions|icache|iTLB|branch|dcache|dTLB|insn per'
# DSB vs MITE (uop-cache hit) -- if MITE-heavy => decode/layout bound
perf stat -p "$PID" -e cpu/event=0x79,umask=0x08,name=idq_dsb_uops/,cpu/event=0x79,umask=0x24,name=idq_mite_uops/ \
  -- sleep 4 2>&1 | grep -iE 'dsb|mite' || true
echo "=== end profile (fps=$(fps)) ==="
