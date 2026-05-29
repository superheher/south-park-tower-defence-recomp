#!/usr/bin/env bash
# floor_rootcause.sh -- decisively separate the candidate floor causes that the
# prior "I-cache capacity wall" conclusion never disentangled:
#   (1) Is the hot thread CPU-BOUND during dips, or WAITING (pacing/sync)?   -> task-clock utilization
#   (2) Is the front-end stall CAPACITY (icache/itlb) or MISPREDICT (resteers)/DSB? -> TMA L3 split
#   (3) Do the HOTTEST functions even fit L1i/L2?                            -> nm sizes of top symbols
#   (4) Is the GPU idle at the dip (=> not GPU-bound)?                       -> radeontop
#   (5) How much work per frame vs a native budget?                         -> insn/frame
# Boot + measure in ONE process so the game stays alive. sleeps live in this file (not a cmd string).
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"
OUT="${OUT:-/tmp/floor_rootcause.txt}"; exec >"$OUT" 2>&1

"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null
sleep 1
echo "== boot =="
"$ROOT/tools/gamectl.sh" play >/tmp/fr_play.log 2>&1
PID=$(pgrep -x south_park_td)
echo "PID=$PID  $(tail -1 /tmp/fr_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; tail -8 /tmp/fr_play.log; exit 0; }
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
MAIN=$(ps -T -p "$PID" -o tid,comm | awk '$2=="Main"{print $1;exit}')
GPU=$(ps -T -p "$PID" -o tid,comm | awk '$2=="GPU"{print $1;exit}')
echo "Main_tid=$MAIN GPU_tid=$GPU"
fps(){ grep pacing-diag "$GAME_DIR/run.log"|tail -1|grep -oE 'swaps [0-9.]+/s'; }
echo "fps_before=$(fps)"

echo
echo "############ (1) MAIN THREAD: CPU-BOUND or WAITING? (task-clock utilization, 15s) ############"
echo "  if 'CPUs utilized' ~1.0 -> Main is pegged (CPU/cache bound). if <<1.0 -> Main WAITS (pacing/sync)."
perf stat -t "$MAIN" -- sleep 15 2>&1 | grep -iE 'task-clock|CPUs utilized|context-switches|cpu-migrations'
echo "  --- per-thread run-state snapshot (R=run R+=runnable, S/D=sleeping/wait) over 3s ---"
for i in 1 2 3 4 5 6; do ps -L -p "$PID" -o tid,comm,stat 2>/dev/null | awk 'NR>1 && ($2=="Main"||$2=="GPU"){printf "%s:%s ",$2,$3}'; echo " fps=$(fps)"; sleep 0.5; done

echo
echo "############ (2) FRONT-END: CAPACITY (icache/itlb) vs MISPREDICT (resteers) vs DSB ############"
echo "  --- TMA L3 fetch-latency breakdown (whole proc, 12s) ---"
perf stat -M tma_icache_misses,tma_itlb_misses,tma_branch_resteers,tma_dsb_switches,tma_lcp,tma_ms_switches \
  -p "$PID" -- sleep 12 2>&1 | grep -iE 'tma_(icache|itlb|branch_resteers|dsb_switches|lcp|ms_switches)'
echo "  --- raw branch + icache to size the two (8s) ---"
perf stat -p "$PID" -e instructions,branches,branch-misses,L1-icache-load-misses,iTLB-load-misses \
  -- sleep 8 2>&1 | grep -iE 'instructions|branch|icache|iTLB'
echo "  --- residual BTB-resteer events (the layer-2 floor wall; full per-1e9i isolation in resteer.sh) ---"
echo "    FRONTEND_RETIRED.BRANCH_RESTEER does NOT exist on Coffee Lake; BACLEARS.ANY is the BTB-resteer proxy."
echo "    (DSB_MISS is NOT subtracted from BACLEARS -- orthogonal uop-cache capacity, ~2.5x scale; see resteer.sh.)"
perf stat -p "$PID" -e instructions,BACLEARS.ANY,INT_MISC.CLEAR_RESTEER_CYCLES,MACHINE_CLEARS.COUNT,ICACHE_64B.IFTAG_MISS,br_misp_retired.near_call \
  -- sleep 8 2>&1 | grep -iE 'instructions|BACLEARS|CLEAR_RESTEER|MACHINE_CLEARS|IFTAG|near_call'

echo
echo "############ (3) DO THE HOTTEST FUNCTIONS FIT L1i(32K)/L2(256K)? ############"
echo "  L1i=32768 B/core, L2=262144 B/core. Sizes of the profile's top self-time symbols:"
for sym in sub_821B9270 sub_8244CE40 sub_821C6E58 __savegprlr_29 __restgprlr_29; do
  line=$(grep -E "(^|[^0-9a-zA-Z_])(__imp__)?${sym}\b" /tmp/m0_syms.txt 2>/dev/null | head -1)
  if [ -n "$line" ]; then
    sz=$((16#$(echo "$line" | awk '{print $2}')))
    printf "    %-18s = %d B (%.1f KB)\n" "$sym" "$sz" "$(echo "scale=1;$sz/1024"|bc)"
  else
    printf "    %-18s = (not found in nm dump)\n" "$sym"
  fi
done
echo "  --- sum of top-20 self-time functions' .text (do they collectively bust L1i/L2?) ---"

echo
echo "############ (4) GPU BUSY% during the window (idle => not GPU-bound) ############"
( timeout 10 radeontop -d - -l 8 2>/dev/null | grep -oE 'gpu [0-9.]+%' | sort -t' ' -k2 -n | tail -4 ) || echo "  (radeontop n/a)"

echo
echo "############ (5) WORK PER FRAME (whole proc) ############"
read INSN < <(perf stat -p "$PID" -e instructions -- sleep 6 2>&1 | grep -iE 'instructions' | grep -oE '^[ 0-9,]+' | tr -d ', ')
SW=$(fps | grep -oE '[0-9.]+')
echo "  instructions in 6s = ${INSN:-?};  current swaps/s = ${SW:-?}"
echo "  (native Xbox360 budget ~2-3B useful insn/s/core; recompiler expands ~4-7x/guest-insn)"
echo "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo FLOOR_ROOTCAUSE_DONE
