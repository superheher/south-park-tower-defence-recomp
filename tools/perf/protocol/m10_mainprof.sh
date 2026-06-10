#!/usr/bin/env bash
set -u
GC=/home/h/src/recomp/gamectl.sh
SP=/tmp/sp
$GC play > $SP/m10_play.out 2>&1
grep -q 'IN LEVEL' $SP/m10_play.out || { echo NOLEVEL; exit 1; }
PID=$(pgrep -x south_park_td | head -1)
MAIN=$(ps -T -p $PID -o tid,comm | awk '$2=="Main" && $3=="XThread"{print $1; exit} {for(i=2;i<=NF;i++) if($i=="Main"){print $1; exit}}')
[ -z "$MAIN" ] && MAIN=$(ls /proc/$PID/task | while read t; do c=$(cat /proc/$PID/task/$t/comm); case "$c" in 'Main XThread'*) echo $t;; esac; done | head -1)
echo "PID=$PID MAIN=$MAIN"
sleep 68   # ride into the heaviest wave-2 band
grep 'pacing-diag' /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/run.log | tail -1
perf record -F 999 -g -t $MAIN -o $SP/m10_main.data -- sleep 12 >/dev/null 2>&1
grep 'pacing-diag' /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/run.log | tail -1
cp /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/run.log $SP/m10_run.log
$GC kill >/dev/null 2>&1
echo DONE
