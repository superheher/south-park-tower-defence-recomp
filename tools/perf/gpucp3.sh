#!/usr/bin/env bash
# Whole-proc record during a heavy dip, then break down BY THREAD-COMM. Find the GPU CommandProcessor
# thread's DSO (librexruntime user-parse vs libvulkan vs kernel) + top functions. user+kernel cycles.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"; PD=/tmp/sp/gpucp3.data
exec >/tmp/gpucp3.txt 2>&1
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 2
echo "== gpucp3  exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) =="
"$ROOT/tools/gamectl.sh" play >/tmp/gpucp3_play.log 2>&1
PID=$(pgrep -x south_park_td); [ -z "$PID" ] && { echo BOOT_FAILED; exit 0; }
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
echo "=== all thread comms (identify the GPU CP thread) ==="; ps -L -o tid,comm -p "$PID" 2>/dev/null | awk '{print $2}' | sort | uniq -c | sort -rn | head
for i in $(seq 1 10); do "$ROOT/tools/gamectl.sh" press 1000 0.25; sleep 0.9; done
echo "waiting for dip..."; for i in $(seq 1 90); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
echo "fps=$(fps); recording WHOLE PROC 16s (user+kernel)"
perf record -e cycles --call-graph lbr -F 2500 -p "$PID" -o "$PD" -- sleep 16 2>/dev/null
echo "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo "=== per-thread(comm) total cycle share ==="
perf report -i "$PD" --stdio --sort comm --no-children 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -12
echo "=== GPU CommandProcessor thread: DSO breakdown ==="
perf report -i "$PD" --stdio --sort comm,dso --no-children 2>/dev/null | grep -iE 'command|gpu' | head -8
echo "=== GPU CommandProcessor thread: top functions ==="
perf report -i "$PD" --stdio --sort comm,symbol --no-children 2>/dev/null | grep -iE 'command|gpu' | head -22
echo GPUCP3_DONE
