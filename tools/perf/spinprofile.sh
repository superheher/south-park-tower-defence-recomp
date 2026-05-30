#!/usr/bin/env bash
# Identify the busy-wait: profile the Main(guest-sim) thread WHILE PAUSED. Paused == ~pure spin
# (99% of running insn-rate with zero sim), so the hot functions here ARE the busy-wait loop.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"; PD=/tmp/sp/spin.data
exec >/tmp/spinprofile.txt 2>&1
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 2
"$ROOT/tools/gamectl.sh" play >/tmp/spin_play.log 2>&1
PID=$(pgrep -x south_park_td); [ -z "$PID" ] && { echo BOOT_FAILED; exit 0; }
MTID=$(ps -L -o tid,comm -p "$PID" 2>/dev/null | awk '/Main/{print $1}' | head -1)
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
echo "Main_tid=$MTID  PID=$PID"
sleep 3
"$ROOT/tools/gamectl.sh" press 0010 0.4   # START -> pause
sleep 2
echo "=== profiling MAIN THREAD while PAUSED (pure spin), 15s, call-graph ==="
perf record -e cycles:u --call-graph lbr -F 4000 -t "$MTID" -o "$PD" -- sleep 15 2>/dev/null
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo "=== top self-time functions in the PAUSED spin ==="
perf report -i "$PD" --stdio --percent-limit 0.8 --no-children 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -25
echo "=== call graph (who calls whom) — top chains ==="
perf report -i "$PD" --stdio --percent-limit 3 2>/dev/null | sed -n '/# Samples/,/^# /p' | grep -E '^\s+[0-9]+\.|--|^\s+\|' | head -60
echo SPINPROFILE_DONE
