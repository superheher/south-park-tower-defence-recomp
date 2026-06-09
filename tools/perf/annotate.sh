#!/usr/bin/env bash
# annotate.sh [func] -- instruction-level cycle attribution for the hottest recompiled function,
# to STOP GUESSING where the reducible host work is (ctx-reg memory traffic vs guest loads vs
# CR-flag compares vs the (ctx,base) ABI shuffle vs the calls). Boots into combat, perf-records
# cycles, prints the per-instruction hot lines of <func> (default the profile's #1, sub_821B9270).
# Boot+record in ONE process; waits live in this file.   bash tools/perf/annotate.sh [func]
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"
FUNC="${1:-__imp__sub_821B9270}"
OUT="${OUT:-/tmp/annotate_${FUNC}.txt}"; exec >"$OUT" 2>&1
LOG="$GAME_DIR/run.log"; PD=/tmp/sp/annot.data

"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 2
echo "== annotate $FUNC  $(date '+%F %T')  exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) =="
"$ROOT/tools/gamectl.sh" play >/tmp/annot_play.log 2>&1
PID=$(pgrep -x south_park_td); echo "PID=$PID $(tail -1 /tmp/annot_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; exit 0; }
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
curfps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
# wait for heavy combat
for i in $(seq 1 80); do f=$(curfps); [ -n "$f" ] && awk "BEGIN{exit !($f<28)}" && break; sleep 0.5; done
echo "heavy at fps=$(curfps); recording 20s"
perf record -e cycles:u -F 4000 -p "$PID" -o "$PD" -- sleep 20 2>/dev/null
echo "fps_after=$(curfps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo "== top self-time functions =="
perf report -i "$PD" --stdio --percent-limit 1 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -20
echo "== per-instruction annotation of $FUNC (hot lines) =="
perf annotate -i "$PD" --stdio -d "$GAME_DIR/south_park_td" "$FUNC" 2>/dev/null \
  | awk '{ if ($1 ~ /[0-9]+\.[0-9]+/ && $1+0 >= 0.5) print "  HOT>"$0; else print $0 }' \
  | grep -E 'HOT>|:$|<__imp' | head -80
echo ANNOTATE_DONE
