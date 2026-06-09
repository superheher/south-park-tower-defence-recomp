#!/usr/bin/env bash
# uops.sh [label] -- P1 instrument: did a codegen/flag change cut host uops per frame?
#
# The combat floor is back-end/raw-work bound (docs/FLOOR-PGO-MEDIUM-REPORT.md), so the
# lever is reducing host uops retired per guest frame. This boots into combat and measures,
# for BOTH the whole process and the Main/guest-sim thread alone:
#   uops_retired.retire_slots, inst_retired.any, cpu-cycles  (-> uops/insn, uops/cycle, IPC)
#   idq.mite_uops / idq.dsb_uops                              (-> front-end uop source mix)
# plus the swaps/s during the window, so we can express uops PER SWAP (per frame) -- the
# load-invariant "work per frame" number the floor ultimately reflects.
#
# Boot + measure in ONE process (a launching shell that ends reaps the game); all waits live
# in THIS file (the harness blocks a bare `sleep` token in a command string).
#   bash tools/perf/uops.sh [label]
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"
LABEL="${1:-cand}"
OUT="${OUT:-/tmp/uops_${LABEL}.txt}"; exec >"$OUT" 2>&1
LOG="$GAME_DIR/run.log"

"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null
sleep 2
echo "== uops  label=$LABEL  $(date '+%F %T') =="
echo "  exe md5=$(md5sum "$GAME_DIR/south_park_td" | cut -c1-12)  so md5=$(md5sum "$GAME_DIR/librexruntime.so" | cut -c1-12)"
echo "  exe .text=$(size "$GAME_DIR/south_park_td" 2>/dev/null | awk 'NR==2{print $1}')"
"$ROOT/tools/gamectl.sh" play >/tmp/uops_play.log 2>&1
PID=$(pgrep -x south_park_td)
echo "PID=$PID  $(tail -1 /tmp/uops_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; tail -8 /tmp/uops_play.log; exit 0; }
# Main thread = the guest-sim thread (comm "Main:XThread"); pick the busiest such tid.
MTID=$(ps -L -o tid,comm -p "$PID" 2>/dev/null | awk '/Main/{print $1}' | head -1)
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+/s'; }
swaps_count(){ grep -c pacing-diag "$LOG"; }
echo "fps_before=$(fps)  Main_tid=$MTID"
sleep 4   # settle into the level / heavy combat

EV=uops_retired.retire_slots,inst_retired.any,cpu-cycles,idq.mite_uops,idq.dsb_uops,idq.ms_uops
WIN=15
echo "  --- WHOLE PROCESS ($WIN s) ---"
s0=$(swaps_count)
perf stat -p "$PID" -e "$EV" -- sleep "$WIN" 2>/tmp/uops_proc.txt
s1=$(swaps_count)
cat /tmp/uops_proc.txt
echo "  proc_swaps_in_window=$((s1-s0))"
if [ -n "$MTID" ]; then
  echo "  --- MAIN THREAD tid=$MTID ($WIN s) ---"
  m0=$(swaps_count)
  perf stat -t "$MTID" -e "$EV" -- sleep "$WIN" 2>/tmp/uops_main.txt
  m1=$(swaps_count)
  cat /tmp/uops_main.txt
  echo "  main_swaps_in_window=$((m1-m0))"
fi
echo "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1

python3 - /tmp/uops_proc.txt /tmp/uops_main.txt "$((s1-s0))" "$WIN" <<'PY'
import sys,re
def val(path,ev):
    try: txt=open(path).read()
    except: return 0.0
    for line in txt.splitlines():
        m=re.match(r'\s*([\d,]+)\s+'+re.escape(ev)+r'(\s|$)',line)
        if m: return float(m.group(1).replace(',',''))
    return 0.0
def report(tag,path):
    uops=val(path,'uops_retired.retire_slots'); insn=val(path,'inst_retired.any')
    cyc=val(path,'cpu-cycles'); mite=val(path,'idq.mite_uops'); dsb=val(path,'idq.dsb_uops'); ms=val(path,'idq.ms_uops')
    if insn==0: print(f"  [{tag}] no data"); return
    print(f"  [{tag}] uops={uops/1e9:.2f}B insn={insn/1e9:.2f}B cyc={cyc/1e9:.2f}B")
    print(f"  [{tag}] uops/insn={uops/insn:.3f}  IPC={insn/cyc if cyc else 0:.3f}  uops/cyc={uops/cyc if cyc else 0:.3f}")
    tot=mite+dsb+ms
    if tot>0: print(f"  [{tag}] uop-source: MITE={100*mite/tot:.0f}% DSB={100*dsb/tot:.0f}% MS={100*ms/tot:.0f}%  (mite={mite/1e9:.2f}B dsb={dsb/1e9:.2f}B)")
    return uops,insn,cyc
swaps=float(sys.argv[3]); win=float(sys.argv[4])
print("\n========== UOPS SUMMARY ==========")
r=report('proc',sys.argv[1])
report('main',sys.argv[2])
if r and swaps>0:
    uops,insn,cyc=r
    print(f"  swaps_in_window={swaps:.0f} over {win:.0f}s ({swaps/win:.1f}/s)")
    print(f"  *** proc uops/swap = {uops/swaps/1e6:.2f}M   insn/swap = {insn/swaps/1e6:.2f}M *** (work per frame; LOWER is the P1 win)")
PY
echo UOPS_DONE
