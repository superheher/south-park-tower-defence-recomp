#!/usr/bin/env bash
# resteer.sh -- P0 instrument: isolate the residual BTB-capacity resteer wall so every
# floor lever (P1 .so-medium, P2 leaf-inline, P3 PGO) gets a *measured* resteer go/no-go,
# not a noisy fps guess. The expensive misprediction-FLUSH half is already fixed by
# -mcmodel=medium (br_misp_retired.near_call -79%); what remains is FRONT-END RE-STEERS
# (BACLEARS) gated by BTB *capacity* over ~157k direct branch sites. We separate that
# capacity component from the cache/iTLB/DSB front-end misses by subtraction:
#
#     residual_BTB_resteers ~= BACLEARS.ANY - (FE_L1I_MISS + FE_ITLB_MISS + FE_DSB_MISS)
#
# FRONTEND_RETIRED.BRANCH_RESTEER does NOT exist on Coffee Lake i9-8950HK; these are the
# confirmed-working fallbacks. The FRONTEND_RETIRED.* PEBS events share one slot, so they
# get their own group; everything is normalized PER 1e9 INSTRUCTIONS (load-invariant --
# combat load drifts, so raw counts are not comparable run-to-run, but per-insn rates are).
#
# Boot + measure in ONE process so the game stays alive (a launching shell that ends reaps
# the game). All waits live in THIS file (the harness blocks a bare `sleep` token in a
# command string). Run:  bash tools/perf/resteer.sh [label]
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"
LABEL="${1:-cand}"
OUT="${OUT:-/tmp/resteer_${LABEL}.txt}"; exec >"$OUT" 2>&1

pkill -f floor_rootcause.sh 2>/dev/null; pkill -f branch_breakdown.sh 2>/dev/null
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null
sleep 2
echo "== resteer isolation  label=$LABEL  $(date '+%F %T') =="
echo "  exe md5=$(md5sum "$GAME_DIR/south_park_td" | cut -c1-12)  so md5=$(md5sum "$GAME_DIR/librexruntime.so" | cut -c1-12)"
"$ROOT/tools/gamectl.sh" play >/tmp/resteer_play.log 2>&1
PID=$(pgrep -x south_park_td)
echo "PID=$PID  $(tail -1 /tmp/resteer_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; tail -8 /tmp/resteer_play.log; exit 0; }
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
fps(){ grep pacing-diag "$GAME_DIR/run.log"|tail -1|grep -oE 'swaps [0-9.]+/s'; }
echo "fps_before=$(fps)"
sleep 3   # let it settle into the level

G1=/tmp/resteer_g1.txt; G2=/tmp/resteer_g2.txt; G3=/tmp/resteer_g3.txt
echo "  --- group1 (14s): resteers / machine-clears / iftag ---"
perf stat -p "$PID" -e \
instructions,cpu-cycles,BACLEARS.ANY,INT_MISC.CLEAR_RESTEER_CYCLES,MACHINE_CLEARS.COUNT,ICACHE_64B.IFTAG_MISS,branches,branch-misses \
  -- sleep 14 2>"$G1"; cat "$G1"
echo "  --- group2 (12s): FRONTEND_RETIRED.* (own PEBS slot, multiplexed) ---"
perf stat -p "$PID" -e \
instructions,FRONTEND_RETIRED.L1I_MISS,FRONTEND_RETIRED.ITLB_MISS,FRONTEND_RETIRED.DSB_MISS \
  -- sleep 12 2>"$G2"; cat "$G2"
echo "  --- group3 (12s): br_misp / br_inst by type ---"
perf stat -p "$PID" -e \
instructions,br_inst_retired.all_branches,br_inst_retired.near_call,br_misp_retired.all_branches,br_misp_retired.near_call,br_misp_retired.conditional \
  -- sleep 12 2>"$G3"; cat "$G3"
echo "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1

# ---- parse + compute per-1e9-insn rates ----
val(){ grep -E "[[:space:]]$1([[:space:]]|\$)" "$2" 2>/dev/null | head -1 | awk '{gsub(/,/,"",$1); if($1+0>0) print $1; else print 0}'; }
python3 - "$G1" "$G2" "$G3" <<'PY'
import sys,re
def val(path,ev):
    try: txt=open(path).read()
    except: return 0.0
    for line in txt.splitlines():
        # perf line: "   1,234,567   EVENT.NAME   # ..."
        m=re.match(r'\s*([\d,]+)\s+'+re.escape(ev)+r'(\s|$)',line)
        if m: return float(m.group(1).replace(',',''))
    return 0.0
g1,g2,g3=sys.argv[1],sys.argv[2],sys.argv[3]
insn1=val(g1,'instructions'); insn2=val(g2,'instructions'); insn3=val(g3,'instructions')
baclears=val(g1,'BACLEARS.ANY'); clear_cyc=val(g1,'INT_MISC.CLEAR_RESTEER_CYCLES')
mclears=val(g1,'MACHINE_CLEARS.COUNT'); iftag=val(g1,'ICACHE_64B.IFTAG_MISS')
branches=val(g1,'branches'); bmiss=val(g1,'branch-misses')
fe_l1i=val(g2,'FRONTEND_RETIRED.L1I_MISS'); fe_itlb=val(g2,'FRONTEND_RETIRED.ITLB_MISS'); fe_dsb=val(g2,'FRONTEND_RETIRED.DSB_MISS')
mall=val(g3,'br_misp_retired.all_branches'); mcall=val(g3,'br_misp_retired.near_call'); mcond=val(g3,'br_misp_retired.conditional')
iall=val(g3,'br_inst_retired.all_branches'); icall=val(g3,'br_inst_retired.near_call')
def per(x,insn): return (x/insn*1e9) if insn>0 else 0.0
print("\n========== RESTEER SUMMARY (per 1e9 instructions; load-invariant) ==========")
print(f"  insn windows: g1={insn1/1e9:.2f}B g2={insn2/1e9:.2f}B g3={insn3/1e9:.2f}B")
baclears_per = per(baclears,insn1)
# NOTE: the runbook's literal `BACLEARS - (FE cache/itlb/dsb misses)` subtraction is DROPPED.
# It goes negative because FRONTEND_RETIRED.DSB_MISS (uop-cache/DSB capacity miss -> MITE refetch)
# is ORTHOGONAL to a BAClear (a branch-predictor re-steer) and ~2.5x its scale on this recomp.
# A DSB miss does not generate a BAClear, so subtracting it is a category error. BACLEARS.ANY
# itself is the BTB-resteer proxy (cold/evicted-BTB-entry re-steers); the FE misses are shown as
# orthogonal context (does DSB/uop-cache pressure move when we change branch-site count?).
print("  ---- PRIMARY SIGNALS (the go/no-go metrics) ----")
print(f"  BACLEARS.ANY            /1e9i = {baclears_per:12.0f}   *** BTB-resteer proxy (P2 lever target: DOWN) ***")
print(f"  INT_MISC.CLEAR_RESTEER  /1e9i = {per(clear_cyc,insn1):12.0f}   resteer STALL CYCLES (cost of all re-steers)")
print(f"  branch-misses           /1e9i = {per(bmiss,insn1):12.0f}   execution mispredict FLUSHES (P1 .so lever target: DOWN)")
print(f"  br_misp.near_call       /1e9i = {per(mcall,insn3):12.0f}   indirect-call mispredicts  rate={100*mcall/icall if icall else 0:5.1f}%")
print("  ---- ORTHOGONAL FRONT-END CONTEXT (uop-cache / fetch capacity; not resteers) ----")
print(f"  FE DSB_MISS             /1e9i = {per(fe_dsb,insn2):12.0f}   (uop-cache miss -> MITE; code-size capacity)")
print(f"  FE L1I_MISS             /1e9i = {per(fe_l1i,insn2):12.0f}")
print(f"  FE ITLB_MISS            /1e9i = {per(fe_itlb,insn2):12.0f}")
print(f"  ICACHE_64B.IFTAG_MISS   /1e9i = {per(iftag,insn1):12.0f}")
print(f"  MACHINE_CLEARS.COUNT    /1e9i = {per(mclears,insn1):12.0f}   (memory-ordering/SMC clears; should stay ~flat)")
print(f"  br_misp.all_branches    /1e9i = {per(mall,insn3):12.0f}")
print(f"  br_misp.conditional     /1e9i = {per(mcond,insn3):12.0f}   (data-dependent game logic; not codegen-fixable)")
print("  KEY  baclears_per=%.0f clear_cyc_per=%.0f bmiss_per=%.0f near_call_misp_per=%.0f dsb_miss_per=%.0f"
      % (baclears_per, per(clear_cyc,insn1), per(bmiss,insn1), per(mcall,insn3), per(fe_dsb,insn2)))
PY
echo RESTEER_DONE
