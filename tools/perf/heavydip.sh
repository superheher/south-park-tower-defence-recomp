#!/usr/bin/env bash
# Find the HEAVY-COMBAT floor bottleneck (with the Main-thread spin fixed). Boot, drive into combat,
# wait for a sustained heavy dip (fps<22), then perf-record the WHOLE process + report per-thread CPU
# and top functions/DSOs during the dip. Tells us which thread (GPU CP? sim? another spin?) gates the floor.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"; PD=/tmp/sp/dip.data
exec >/tmp/heavydip.txt 2>&1
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 2
echo "== heavydip  exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) =="
"$ROOT/tools/gamectl.sh" play >/tmp/dip_play.log 2>&1
PID=$(pgrep -x south_park_td); [ -z "$PID" ] && { echo BOOT_FAILED; exit 0; }
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
# press A a few times to spawn/escalate combat, then wait for a heavy dip
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
echo "waiting for heavy dip (fps<22)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<22)}" && break; sleep 0.5; done
echo "dip at fps=$(fps); recording whole-proc 16s + per-thread CPU"
( for s in 1 2 3 4; do echo "  [thr fps=$(fps)]"; top -H -b -n1 -p "$PID" 2>/dev/null | awk 'NR>7 && $9+0>3 {printf "    %-16s %5s%%\n",$NF,$9}' | head -8; sleep 3; done ) &
perf record -e cycles:u --call-graph lbr -F 2500 -p "$PID" -o "$PD" -- sleep 16 2>/dev/null
wait
echo "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo "=== top self-time (whole proc, by thread+function) during the dip ==="
perf report -i "$PD" --stdio --percent-limit 1.2 --sort overhead,comm,dso,symbol --no-children 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -30
echo "=== DSO share ==="
perf report -i "$PD" --stdio --sort overhead,dso --no-children 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -8
echo HEAVYDIP_DONE
