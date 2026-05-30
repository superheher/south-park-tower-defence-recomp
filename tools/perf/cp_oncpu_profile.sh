#!/usr/bin/env bash
# cp_oncpu_profile.sh -- where does the CP ("GPU Commands") thread spend its ON-cpu (translation) time in
# a HEAVY combat dip? Groundwork for the one remaining floor lever (CP-translate / draw-batch reduction):
# shows whether the ~17ms render is a concentrated, safely-optimizable hotspot or irreducibly broad.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"; PD=/tmp/sp/cponcpu.data
OUT=/tmp/cp_oncpu_profile.txt; SECS="${1:-12}"
mkdir -p /tmp/sp
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== cp_oncpu_profile exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) secs=$SECS =="
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid; echo 0 > /proc/sys/kernel/kptr_restrict' 2>/dev/null
"$ROOT/tools/gamectl.sh" play >/tmp/cponcpu_play.log 2>&1
grep -q 'IN LEVEL' /tmp/cponcpu_play.log || { say PLAY_FAILED; tail -5 /tmp/cponcpu_play.log >>"$OUT"; echo CP_ONCPU_DONE>>"$OUT"; exit 0; }
PID=$(pgrep -x south_park_td)
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
TID=$(ps -L -p "$PID" -o lwp,comm --no-headers 2>/dev/null | awk '/GPU Commands/{print $1; exit}')
say "dip at fps=$(fps); CP tid=$TID"
[ -z "$TID" ] && { say "NO CP TID"; echo CP_ONCPU_DONE>>"$OUT"; exit 0; }
( for i in $(seq 1 40); do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.0; done ) &
DRIVER=$!
echo "${SUDO_PASS:-}" | sudo -S perf record -e cycles:u --call-graph lbr -F 4000 -t "$TID" -o "$PD" -- sleep "$SECS" 2>/tmp/cponcpu_rec.log
kill "$DRIVER" 2>/dev/null
say "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
say ""
say "=== CP thread ON-cpu: DSO split ==="
echo "${SUDO_PASS:-}" | sudo -S perf report -i "$PD" --stdio --sort dso --no-children 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -8 >>"$OUT"
say ""
say "=== CP thread ON-cpu: top 30 functions (self) — the translation hotspots ==="
echo "${SUDO_PASS:-}" | sudo -S perf report -i "$PD" --stdio --sort symbol --no-children 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -30 >>"$OUT"
echo CP_ONCPU_DONE >>"$OUT"
