#!/usr/bin/env bash
# affinity_test.sh [rounds] -- is the combat floor capped by HYPERTHREAD CONTENTION rather than
# codegen? The Main/guest-sim thread is the floor bottleneck, but TimerThreadMain (~17% CPU) and the
# Audio worker (~11% in mutex spin) also burn cores. On this 6c/12t i9 (HT siblings 0/6,1/7,...,5/11),
# if the scheduler co-locates Main with a spinning helper on one physical core, the helper steals
# Main's execution slots -> the floor is contention-capped and NO codegen lever can move it.
#
# Test: one boot, then interleave floor windows with Main UNPINNED (OS default, may share a core) vs
# Main ISOLATED (pinned to cpu0 with its HT sibling cpu6 vacated -- every other game thread forced to
# cpus 1-5,7-11). Affinity changes live (no relaunch), so load drift cancels across interleaved rounds.
#   bash tools/perf/affinity_test.sh [rounds]
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
ROUNDS="${1:-4}"; SECS="${SECS:-60}"
OUT="${OUT:-/tmp/affinity_test.txt}"; exec >"$OUT" 2>&1

"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 2
echo "== affinity_test  $(date '+%F %T')  exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) =="
"$ROOT/tools/gamectl.sh" play >/tmp/aff_play.log 2>&1
PID=$(pgrep -x south_park_td); echo "PID=$PID $(tail -1 /tmp/aff_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; exit 0; }
MTID=$(ps -L -o tid,comm -p "$PID" 2>/dev/null | awk '/Main/{print $1}' | head -1)
echo "Main_tid=$MTID  threads=$(ls /proc/$PID/task | wc -l)"

pin_isolated() {   # NO-HT: every thread restricted to the 6 distinct physical cores (one lane each)
  for t in /proc/$PID/task/*; do tid=$(basename "$t"); taskset -pc 0-5 "$tid" >/dev/null 2>&1; done
}
pin_default() {    # everything -> all 12 logical CPUs (OS default, HT lanes available)
  for t in /proc/$PID/task/*; do tid=$(basename "$t"); taskset -pc 0-11 "$tid" >/dev/null 2>&1; done
}
measure() {        # echo the FLOOR p10 over SECS
  "$ROOT/tools/perf/floor.sh" "$SECS" 2>/dev/null | tail -1
}

echo "  settling..."; sleep 5
declare -A acc
for r in $(seq 1 "$ROUNDS"); do
  for mode in default isolated; do
    [ "$mode" = default ] && pin_default || pin_isolated
    sleep 2
    out="$(measure)"
    p10="$(printf '%s' "$out" | grep -oE 'p10=[0-9.]+' | cut -d= -f2)"
    heavy="$(printf '%s' "$out" | grep -oE 'heavy=[a-z]+' | cut -d= -f2)"
    echo "  [r$r $mode] $out"
    [ "$heavy" = yes ] && [ -n "$p10" ] && acc[$mode]+="$p10 "
  done
done
pin_default
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo "== medians (heavy p10) =="
for mode in default isolated; do
  m=$(printf '%s\n' ${acc[$mode]:-} | awk 'NF{a[NR]=$1} END{if(NR==0){print "n/a";exit} c=asort(a); print a[int((c+1)/2)]}')
  echo "  $mode: median_p10=$m  samples: ${acc[$mode]:-none}"
done
echo AFFINITY_DONE
