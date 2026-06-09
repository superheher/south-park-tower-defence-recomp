#!/usr/bin/env bash
# Are guest worker threads STARVED by the Main thread's never-yielding spin? Sample per-thread %CPU
# during combat. If Main:XThread is ~100% (spinning) and the real sim/worker XThreads get little CPU,
# the spin is starving them -> that's the floor mechanism.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
LOG="$ROOT/out/build/linux-amd64-release/run.log"
exec >/tmp/threads.txt 2>&1
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 2
"$ROOT/tools/gamectl.sh" play >/tmp/threads_play.log 2>&1
PID=$(pgrep -x south_park_td); [ -z "$PID" ] && { echo BOOT_FAILED; exit 0; }
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
sleep 4
echo "PID=$PID  fps=$(fps)  total_threads=$(ls /proc/$PID/task|wc -l)"
echo "=== per-thread %CPU (top -H, 3 snapshots 2s apart), busy threads only ==="
for i in 1 2 3; do
  echo "--- snapshot $i  fps=$(fps) ---"
  top -H -b -n1 -p "$PID" 2>/dev/null | awk 'NR>7 && $9+0>2.0 {printf "  %6s %-16s %5s%%CPU\n",$1,$NF,$9}' | head -20
  sleep 2
done
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo THREADS_DONE
