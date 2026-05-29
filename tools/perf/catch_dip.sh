#!/usr/bin/env bash
# catch_dip.sh [fps_threshold] [n_dips] -- catch heavy-frame dips and snapshot GPU
# busy% + per-thread CPU at that instant. Use to (re)confirm dips are CPU-bound
# (GPU idle) and which thread gates, after any change.
set -u
GAME_DIR="${REX_GAME_DIR:-/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release}"
LOG="$GAME_DIR/run.log"; PID=$(pgrep -x south_park_td) || { echo "no game"; exit 1; }
THRESH="${1:-27}"; NEED="${2:-3}"; caught=0; last=""
radeontop -d /tmp/rt_dip.log -i 1 -t 600 >/dev/null 2>&1 & RT=$!
end=$(( $(date +%s) + 150 ))
while [ "$(date +%s)" -lt "$end" ] && [ "$caught" -lt "$NEED" ]; do
  line="$(grep pacing-diag "$LOG" 2>/dev/null|tail -1)"; sig="${line##*pacing-diag}"
  if [ -n "$line" ] && [ "$sig" != "$last" ]; then
    last="$sig"; v="$(printf '%s' "$line"|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+')"; vi="${v%.*}"
    if [ -n "$vi" ] && [ "$vi" -lt "$THRESH" ]; then
      caught=$((caught+1))
      echo "=== DIP #$caught: ${v} swaps/s ==="
      echo "  GPU: $(tail -1 /tmp/rt_dip.log 2>/dev/null|grep -oE 'gpu [0-9.]+%')"
      top -H -b -n1 -p "$PID" 2>/dev/null|awk 'NR>7 && $9+0>15{printf "  %5.1f%%  %s\n",$9,$12}'
    fi
  fi; sleep 0.4
done
kill $RT 2>/dev/null; echo "caught $caught dips (GPU% LOW + a CPU thread ~100% => CPU-bound, as expected)"
