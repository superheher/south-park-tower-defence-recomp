#!/usr/bin/env bash
# floor.sh [seconds] -- robust combat-framerate "floor" sampler.
#
# WHY: combat fps is non-stationary (swings 20->50 with wave density), so AVG is
# noisy. The reproducible signal is the FLOOR (min / p10) = the heaviest frames.
# This sampler ALSO guards the two ways earlier measurements got fooled:
#   1. stale log  -> verifies the pacing-diag line COUNT is advancing (not just
#      that a process exists). A hung/closed game keeps the last line forever.
#   2. all-light window -> flags if no heavy frames were seen (min too high),
#      which would make a floor comparison invalid.
#
# Output (last line, parseable):
#   FLOOR n=<N> min=<m> p10=<p> avg=<a> max=<M> heavy=<yes|no> status=<ok|stale|dead|nodata>
set -u
GAME_DIR="${REX_GAME_DIR:-/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release}"
LOG="$GAME_DIR/run.log"
SEC="${1:-90}"
HEAVY_THRESH="${HEAVY_THRESH:-30}"   # a window is "heavy" if it dipped below this

pgrep -x south_park_td >/dev/null || { echo "FLOOR n=0 status=dead"; exit 2; }
c0=$(grep -c pacing-diag "$LOG" 2>/dev/null); sleep 2
c1=$(grep -c pacing-diag "$LOG" 2>/dev/null)
[ "${c1:-0}" -gt "${c0:-0}" ] || { echo "FLOOR n=0 status=stale"; exit 3; }

end=$(( $(date +%s) + SEC )); seen=""; vals=()
while [ "$(date +%s)" -lt "$end" ]; do
  line="$(grep pacing-diag "$LOG" 2>/dev/null | tail -1)"
  sig="${line##*pacing-diag}"
  if [ -n "$line" ] && [ "$sig" != "$seen" ]; then
    seen="$sig"
    v="$(printf '%s' "$line" | grep -oE 'swaps [0-9.]+' | grep -oE '[0-9.]+')"
    [ -n "$v" ] && { vals+=("$v"); printf '  %s\n' "$v" >&2; }
  fi
  sleep 0.4
done
n=${#vals[@]}
[ "$n" -eq 0 ] && { echo "FLOOR n=0 status=nodata"; exit 4; }
printf '%s\n' "${vals[@]}" | awk -v n="$n" -v ht="$HEAVY_THRESH" '
  {a[NR]=$1; s+=$1; if(min==""||$1<min)min=$1; if($1>max)max=$1}
  END{ c=asort(a); p10=a[int(c*0.1)+1];
       printf "FLOOR n=%d min=%.1f p10=%.1f avg=%.1f max=%.1f heavy=%s status=ok\n",
              n, min, p10, s/n, max, (min<ht?"yes":"no") }'
