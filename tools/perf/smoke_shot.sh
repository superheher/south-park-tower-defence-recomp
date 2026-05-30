#!/usr/bin/env bash
# Play into combat with whatever .so is currently staged, capture mid-combat screenshots.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
GAMECTL="$ROOT/tools/gamectl.sh"
TAG="${1:-cand}"
exec >/tmp/smoke_${TAG}.txt 2>&1
"$GAMECTL" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
echo "== smoke $TAG  so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) =="
"$GAMECTL" play >/tmp/smoke_${TAG}_play.log 2>&1
grep -q 'IN LEVEL' /tmp/smoke_${TAG}_play.log || { echo PLAY_FAILED; tail -4 /tmp/smoke_${TAG}_play.log; exit 0; }
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
# escalate combat
for i in 1 2 3 4 5 6; do "$GAMECTL" press 1000 0.3; sleep 1.1; done
"$GAMECTL" shot "${TAG}_combat_1" >/dev/null 2>&1; echo "shot1 fps=$(fps)"
sleep 1.5; "$GAMECTL" press 1000 0.3; sleep 1.0
"$GAMECTL" shot "${TAG}_combat_2" >/dev/null 2>&1; echo "shot2 fps=$(fps)"
sleep 1.5; "$GAMECTL" press 1000 0.3; sleep 1.0
"$GAMECTL" shot "${TAG}_combat_3" >/dev/null 2>&1; echo "shot3 fps=$(fps)"
echo "=== errors/warns in log (last 15, excl pacing/reg) ==="
grep -iE 'error|assert|nan|inf|fatal|abort' "$LOG" 2>/dev/null | grep -viE 'pacing-diag|reg-hist' | tail -15
echo "=== pacing tail ==="; grep pacing-diag "$LOG" | tail -6
"$GAMECTL" kill >/dev/null 2>&1
echo SMOKE_DONE
