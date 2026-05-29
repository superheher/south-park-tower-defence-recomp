#!/usr/bin/env bash
# topdown.sh [label] -- settle the floor's bottleneck CLASS: is the Main/guest-sim thread
# front-end bound, back-end-throughput bound, or MEMORY-LATENCY bound (dependent load chains)?
#
# The prior session proved front-end levers don't move the floor; the uops triage showed the
# Main thread runs at ~2.0 uops/cyc (of 4) => NOT throughput-saturated. If most stall cycles are
# CYCLE_ACTIVITY.STALLS_*_MISS (waiting on L1/L2/L3/DRAM loads), the floor is latency-bound and the
# lever is reducing/reordering loads (e.g. dropping `volatile` for CSE+MLP), NOT cutting uops.
#
# Boots into combat, waits for a HEAVY wave (swaps < HEAVY), then runs a long perf-stat window so
# multiple heavy waves are captured. Boot+measure in ONE process (a launching shell that ends reaps
# the game); all waits live in THIS file.
#   bash tools/perf/topdown.sh [label]
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"
LABEL="${1:-cand}"
OUT="${OUT:-/tmp/topdown_${LABEL}.txt}"; exec >"$OUT" 2>&1
LOG="$GAME_DIR/run.log"

"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null
sleep 2
echo "== topdown  label=$LABEL  $(date '+%F %T') =="
echo "  exe md5=$(md5sum "$GAME_DIR/south_park_td" | cut -c1-12)"
"$ROOT/tools/gamectl.sh" play >/tmp/topdown_play.log 2>&1
PID=$(pgrep -x south_park_td)
echo "PID=$PID  $(tail -1 /tmp/topdown_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; tail -8 /tmp/topdown_play.log; exit 0; }
MTID=$(ps -L -o tid,comm -p "$PID" 2>/dev/null | awk '/Main/{print $1}' | head -1)
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
curfps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
echo "Main_tid=$MTID  fps_before=$(curfps)"

# wait up to 40s for a heavy wave (swaps below HEAVY) so the window starts in combat load
HEAVY=28
for i in $(seq 1 80); do
  f=$(curfps); [ -n "$f" ] && awk "BEGIN{exit !($f < $HEAVY)}" && break
  sleep 0.5
done
echo "  heavy wave detected at fps=$(curfps); starting counters"

TGT=(-p "$PID"); [ -n "$MTID" ] && TGT=(-t "$MTID")
echo "  --- counter target: ${TGT[*]} ---"

# group1: TMA level-1 (frontend/bad-spec/retiring/backend) -- Skylake-client raw events
echo "  --- group1 TMA L1 (18s) ---"
perf stat "${TGT[@]}" -e \
cpu-cycles,uops_retired.retire_slots,uops_issued.any,idq_uops_not_delivered.core,int_misc.recovery_cycles \
  -- sleep 18 2>/tmp/td_g1.txt; cat /tmp/td_g1.txt
# group2: memory-latency stall decomposition
echo "  --- group2 memory stalls (18s) ---"
perf stat "${TGT[@]}" -e \
cpu-cycles,cycle_activity.stalls_total,cycle_activity.stalls_mem_any,cycle_activity.stalls_l1d_miss,cycle_activity.stalls_l2_miss,cycle_activity.stalls_l3_miss \
  -- sleep 18 2>/tmp/td_g2.txt; cat /tmp/td_g2.txt
# group3: where do loads hit? (retirement)
echo "  --- group3 load hit/miss (16s) ---"
perf stat "${TGT[@]}" -e \
cpu-cycles,mem_load_retired.l1_hit,mem_load_retired.l1_miss,mem_load_retired.l2_hit,mem_load_retired.l2_miss,mem_load_retired.l3_hit,mem_load_retired.l3_miss \
  -- sleep 16 2>/tmp/td_g3.txt; cat /tmp/td_g3.txt
# group4: port pressure (are we execution-port bound?)
echo "  --- group4 exec ports (12s) ---"
perf stat "${TGT[@]}" -e \
cpu-cycles,uops_executed.core,uops_executed.stall_cycles,exe_activity.bound_on_stores,exe_activity.1_ports_util,exe_activity.2_ports_util \
  -- sleep 12 2>/tmp/td_g4.txt; cat /tmp/td_g4.txt
echo "fps_after=$(curfps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1

python3 - <<'PY'
import re
def val(path,ev):
    try: txt=open(path).read()
    except: return 0.0
    for line in txt.splitlines():
        m=re.match(r'\s*([\d,]+)\s+'+re.escape(ev)+r'(\s|$)',line)
        if m: return float(m.group(1).replace(',',''))
    return 0.0
g1,g2,g3,g4='/tmp/td_g1.txt','/tmp/td_g2.txt','/tmp/td_g3.txt','/tmp/td_g4.txt'
cyc=val(g1,'cpu-cycles'); slots=4*cyc
ret=val(g1,'uops_retired.retire_slots'); iss=val(g1,'uops_issued.any')
notdel=val(g1,'idq_uops_not_delivered.core'); recov=val(g1,'int_misc.recovery_cycles')
print("\n========== TMA LEVEL-1 (share of 4*cycles issue slots) ==========")
if cyc>0:
    fe=notdel/slots
    retiring=ret/slots
    badspec=(iss-ret+4*recov)/slots
    backend=1-fe-retiring-badspec
    print(f"  cycles={cyc/1e9:.2f}B  slots(4*cyc)={slots/1e9:.2f}B")
    print(f"  Frontend Bound   = {100*fe:5.1f} %")
    print(f"  Bad Speculation  = {100*badspec:5.1f} %")
    print(f"  Retiring         = {100*retiring:5.1f} %")
    print(f"  *** Backend Bound = {100*backend:5.1f} % ***")
c2=val(g2,'cpu-cycles')
st=val(g2,'cycle_activity.stalls_total'); mem=val(g2,'cycle_activity.stalls_mem_any')
l1=val(g2,'cycle_activity.stalls_l1d_miss'); l2=val(g2,'cycle_activity.stalls_l2_miss'); l3=val(g2,'cycle_activity.stalls_l3_miss')
print("\n========== STALL-CYCLE DECOMPOSITION (% of cycles) ==========")
if c2>0:
    print(f"  stalls_total    = {100*st/c2:5.1f} %  (no-uop-exec cycles)")
    print(f"  stalls_mem_any  = {100*mem/c2:5.1f} %  *** load/store wait — the latency signal ***")
    print(f"   .. L1D miss    = {100*l1/c2:5.1f} %  (waiting past L1)")
    print(f"   .. L2  miss    = {100*l2/c2:5.1f} %  (waiting past L2)")
    print(f"   .. L3  miss    = {100*l3/c2:5.1f} %  (waiting on DRAM)")
print("\n========== LOAD HIT/MISS DISTRIBUTION ==========")
h1=val(g3,'mem_load_retired.l1_hit'); m1=val(g3,'mem_load_retired.l1_miss')
h2=val(g3,'mem_load_retired.l2_hit'); m2=val(g3,'mem_load_retired.l2_miss')
h3=val(g3,'mem_load_retired.l3_hit'); m3=val(g3,'mem_load_retired.l3_miss')
tot=h1+m1
if tot>0:
    print(f"  retired loads ~ {tot/1e9:.2f}B   L1hit={100*h1/tot:.1f}% L1miss={100*m1/tot:.1f}%")
    print(f"  of L1 misses: L2hit={h2/1e6:.0f}M L2miss={m2/1e6:.0f}M L3hit={h3/1e6:.0f}M L3miss(DRAM)={m3/1e6:.0f}M")
c4=val(g4,'cpu-cycles'); exe=val(g4,'uops_executed.core'); estall=val(g4,'uops_executed.stall_cycles')
bos=val(g4,'exe_activity.bound_on_stores'); p1=val(g4,'exe_activity.1_ports_util'); p2=val(g4,'exe_activity.2_ports_util')
print("\n========== EXECUTION / PORT PRESSURE ==========")
if c4>0:
    print(f"  uops_executed/cyc = {exe/c4:.2f} (of 4 ports)  exec_stall_cyc={100*estall/c4:.1f}%")
    print(f"  bound_on_stores={100*bos/c4:.1f}%  1-port-util={100*p1/c4:.1f}%  2-port-util={100*p2/c4:.1f}%")
print("\nINTERPRETATION: high Backend%% + high stalls_mem_any => MEMORY-LATENCY bound (lever=reduce/reorder")
print("loads, drop volatile for CSE+MLP). high Retiring or uops_executed/cyc~3-4 => throughput bound (lever=cut uops).")
PY
echo TOPDOWN_DONE
