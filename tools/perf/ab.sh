#!/usr/bin/env bash
# ab.sh -- controlled, interleaved A/B of two-or-more build variants by the FLOOR.
#
# Combat load drifts, so we (a) interleave variants every rep (A B A B ...) to
# cancel slow drift, (b) take REPS reps and report the MEDIAN p10 per variant,
# (c) relaunch fresh per measurement (a .so/binary swap requires a relaunch),
# (d) KILL before staging (overwriting an mmap'd .so/exe under a live process
#     SIGBUSes it), (e) discard windows that never went heavy (heavy=no).
#
# Usage:
#   TARGET=<live file that gets swapped> tools/perf/ab.sh <SECS> <REPS> \
#       <labelA> <srcA> <labelB> <srcB> [<labelC> <srcC> ...]
#   e.g. TARGET=$GAME/librexruntime.so ab.sh 90 2 base good.so cand candidate.so
#        TARGET=$GAME/south_park_td    ab.sh 90 2 base sp.baseline bolt sp.bolt
set -u
ROOT="${REX_GAME_ROOT:-/home/h/src/recomp/rexglue-recomps/south-park-recomp}"
GAMECTL="$ROOT/tools/gamectl.sh"; FLOOR="$ROOT/tools/perf/floor.sh"
RESLOG="${RESLOG:-$ROOT/tools/perf/results.log}"
TARGET="${TARGET:?set TARGET=<live file to swap>}"
SECS="${1:?secs}"; REPS="${2:?reps}"; shift 2
labels=(); srcs=()
while [ "$#" -ge 2 ]; do labels+=("$1"); srcs+=("$2"); shift 2; done
declare -A scores
echo "## A/B $(date '+%F %T')  TARGET=$(basename "$TARGET")  SECS=$SECS REPS=$REPS  variants=${labels[*]}" | tee -a "$RESLOG"
for r in $(seq 1 "$REPS"); do
  for i in "${!labels[@]}"; do
    L="${labels[$i]}"; S="${srcs[$i]}"
    [ -f "$S" ] || { echo "  MISSING src $S for $L -- skip" | tee -a "$RESLOG"; continue; }
    "$GAMECTL" kill >/dev/null 2>&1; sleep 1
    cp -f "$S" "$TARGET"
    md5=$(md5sum "$TARGET" | cut -c1-12)
    "$GAMECTL" play >/tmp/ab_play.log 2>&1
    if ! grep -q 'IN LEVEL' /tmp/ab_play.log; then
      echo "  [$L r$r md5=$md5] PLAY-FAILED (boot/nav) -- skip" | tee -a "$RESLOG"; continue
    fi
    sleep 3
    out="$("$FLOOR" "$SECS" 2>/dev/null | tail -1)"
    echo "  [$L r$r md5=$md5] $out" | tee -a "$RESLOG"
    p10="$(printf '%s' "$out" | grep -oE 'p10=[0-9.]+' | cut -d= -f2)"
    heavy="$(printf '%s' "$out" | grep -oE 'heavy=[a-z]+' | cut -d= -f2)"
    [ "$heavy" = "yes" ] && [ -n "$p10" ] && scores[$L]+="$p10 "
  done
done
echo "## medians (p10, heavy windows only):" | tee -a "$RESLOG"
for L in "${labels[@]}"; do
  m=$(printf '%s\n' ${scores[$L]:-} | awk 'NF{a[NR]=$1} END{if(NR==0){print "n/a";exit} c=asort(a); print a[int((c+1)/2)]}')
  echo "   $L: median_p10=$m   (samples: ${scores[$L]:-none})" | tee -a "$RESLOG"
done
echo "" | tee -a "$RESLOG"
