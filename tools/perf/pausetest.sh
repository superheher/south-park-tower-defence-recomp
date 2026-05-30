#!/usr/bin/env bash
# Decisive over-execution test: Main(guest-sim) thread instruction RATE while RUNNING vs PAUSED.
# A correctly-emulated game does ~nothing while paused (sim stopped). If the Main thread keeps
# retiring billions of insn/s while paused, the guest is BUSY-WAITING (over-execution) -> that, not
# the CPU, is the floor. Boot+measure in ONE process; waits in this file.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
exec >/tmp/pausetest.txt 2>&1
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 2
"$ROOT/tools/gamectl.sh" play >/tmp/pause_play.log 2>&1
PID=$(pgrep -x south_park_td); [ -z "$PID" ] && { echo BOOT_FAILED; exit 0; }
MTID=$(ps -L -o tid,comm -p "$PID" 2>/dev/null | awk '/Main/{print $1}' | head -1)
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
echo "Main_tid=$MTID"
sleep 3
echo "=== RUNNING (in-level, unpaused) ==="
echo "  fps=$(fps)"
perf stat -t "$MTID" -e instructions:u,cycles:u -- sleep 8 2>/tmp/pt_run.txt
grep -E 'instructions:u|cycles:u' /tmp/pt_run.txt

echo "=== pressing START to PAUSE ==="
"$ROOT/tools/gamectl.sh" press 0010 0.4
sleep 2
echo "  fps_after_pause=$(fps)  (a paused game should still PRESENT the static frame ~steadily)"
echo "=== PAUSED ==="
perf stat -t "$MTID" -e instructions:u,cycles:u -- sleep 8 2>/tmp/pt_pause.txt
grep -E 'instructions:u|cycles:u' /tmp/pt_pause.txt
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1

python3 - <<'PY'
import re
def v(p,ev):
    for l in open(p):
        m=re.match(r'\s*([\d,]+)\s+'+re.escape(ev),l)
        if m: return float(m.group(1).replace(',',''))
    return 0.0
ri=v('/tmp/pt_run.txt','instructions:u'); rc=v('/tmp/pt_run.txt','cycles:u')
pi=v('/tmp/pt_pause.txt','instructions:u'); pc=v('/tmp/pt_pause.txt','cycles:u')
print("\n=== Main-thread instruction RATE: running vs paused (8s each) ===")
print(f"  RUNNING: {ri/1e9:.2f}B insn  ({ri/8/1e9:.2f}B/s)   {rc/8/1e9:.2f}B cyc/s")
print(f"  PAUSED : {pi/1e9:.2f}B insn  ({pi/8/1e9:.2f}B/s)   {pc/8/1e9:.2f}B cyc/s")
if ri>0:
    print(f"  paused/running insn ratio = {100*pi/ri:.0f}%")
    if pi/ri > 0.4:
        print("  >>> PAUSED still burns most of the work => the guest BUSY-WAITS / over-executes.")
        print("  >>> The floor is OVER-EXECUTION, not CPU speed. Fix the spin, not the codegen.")
    else:
        print("  >>> Paused drops to near-idle => combat work is REAL (scales with entities).")
PY
echo PAUSETEST_DONE
