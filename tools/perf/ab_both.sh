#!/usr/bin/env bash
# ab_both.sh -- interleaved A/B of variants that differ in BOTH the exe and the .so.
# Same idea as ab.sh but swaps the exe + librexruntime.so together per rep.
#   ab_both.sh <SECS> <REPS> <labelA> <exeA> <soA> <labelB> <exeB> <soB> [<labelC> <exeC> <soC> ...]
set -u
ROOT="${REX_GAME_ROOT:-/home/h/src/recomp/rexglue-recomps/south-park-recomp}"
GAMECTL="$ROOT/tools/gamectl.sh"; FLOOR="$ROOT/tools/perf/floor.sh"
GAME_DIR="${REX_GAME_DIR:-$ROOT/out/build/linux-amd64-release}"
RESLOG="${RESLOG:-$ROOT/tools/perf/results.log}"
SECS="${1:?secs}"; REPS="${2:?reps}"; shift 2
labels=(); exes=(); sos=()
while [ "$#" -ge 3 ]; do labels+=("$1"); exes+=("$2"); sos+=("$3"); shift 3; done
declare -A scores
echo "## AB_BOTH $(date '+%F %T')  SECS=$SECS REPS=$REPS  variants=${labels[*]}" | tee -a "$RESLOG"
for r in $(seq 1 "$REPS"); do
  for i in "${!labels[@]}"; do
    L="${labels[$i]}"; E="${exes[$i]}"; S="${sos[$i]}"
    [ -f "$E" ] && [ -f "$S" ] || { echo "  MISSING $L ($E / $S) -- skip" | tee -a "$RESLOG"; continue; }
    "$GAMECTL" kill >/dev/null 2>&1; sleep 1
    cp -f "$E" "$GAME_DIR/south_park_td"
    cp -f "$S" "$GAME_DIR/librexruntime.so"
    em=$(md5sum "$GAME_DIR/south_park_td" | cut -c1-8); sm=$(md5sum "$GAME_DIR/librexruntime.so" | cut -c1-8)
    "$GAMECTL" play >/tmp/ab_both_play.log 2>&1
    if ! grep -q 'IN LEVEL' /tmp/ab_both_play.log; then
      echo "  [$L r$r exe=$em so=$sm] PLAY-FAILED -- skip" | tee -a "$RESLOG"; continue
    fi
    sleep 3
    out="$("$FLOOR" "$SECS" 2>/dev/null | tail -1)"
    echo "  [$L r$r exe=$em so=$sm] $out" | tee -a "$RESLOG"
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
