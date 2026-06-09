#!/usr/bin/env bash
# cache_levels.sh -- the decisive test for the hybrid floor question.
# Breaks the combat I-fetch stall down BY CACHE LEVEL during heavy frames.
# Question: does the hot working set fit in L3 (=> any MB-scale compaction, incl.
# the AOT+interpreter hybrid, is floor-NEUTRAL because the real target is L2/DSB,
# unreachable) -- or does it overflow L3 to DRAM (=> a big absolute cut like the
# hybrid could still help the floor)?
#
# Key counters (Skylake/CoffeeLake i9-8950HK):
#   L1i:  L1-icache-load-misses                       (-> hits L2)
#   L2 code-miss to L3:   l2_rqsts.code_rd_miss        (demand+prefetch code reads missing L2)
#   L2 code hits:         l2_rqsts.code_rd_hit
#   L3 code miss to DRAM: longest_lat_cache.miss is data; for code use
#                         offcore / mem_load? Instead use L3 overall miss rate via
#                         LLC-load-misses + the icache->L2->L3 chain ratios.
#   Front-end split:      idq.dsb_uops vs idq.mite_uops (uop-cache vs legacy decode)
# Boot+profile in ONE process so the game stays alive for the capture.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"
OUT="${OUT:-/tmp/cache_levels.txt}"; exec >"$OUT" 2>&1
SECS="${SECS:-20}"

"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null
sleep 1
echo "== boot =="
"$ROOT/tools/gamectl.sh" play >/tmp/cl_play.log 2>&1
PID=$(pgrep -x south_park_td)
echo "PID=$PID  $(tail -1 /tmp/cl_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; tail -8 /tmp/cl_play.log; exit 0; }
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
echo "fps_before=$(grep pacing-diag "$GAME_DIR/run.log"|tail -1|grep -oE 'swaps [0-9.]+/s')"

echo
echo "================ CACHE-LEVEL BREAKDOWN (${SECS}s, whole process) ================"
# Group 1: the instruction-cache hierarchy chain
perf stat -p "$PID" -e \
cycles,instructions,\
L1-icache-load-misses,\
l2_rqsts.code_rd_hit,l2_rqsts.code_rd_miss,\
l2_rqsts.all_code_rd,\
iTLB-load-misses \
  -- sleep "$SECS" 2>&1 | grep -iE 'cycles|instructions|icache|code_rd|all_code|iTLB|insn per'

echo
echo "---- LLC (L3) overall + offcore code demand to DRAM ----"
# LLC-* are data-centric but give the L3->DRAM picture; add offcore_response code if available.
perf stat -p "$PID" -e \
LLC-loads,LLC-load-misses,LLC-stores,\
cache-references,cache-misses \
  -- sleep 8 2>&1 | grep -iE 'LLC|cache-ref|cache-mis'

echo
echo "---- front-end uop source (DSB uop-cache vs MITE legacy decode) ----"
perf stat -p "$PID" -e \
cpu/event=0x79,umask=0x08,name=idq_dsb_uops/,\
cpu/event=0x79,umask=0x24,name=idq_mite_uops/,\
cpu/event=0x79,umask=0x30,name=idq_ms_uops/ \
  -- sleep 6 2>&1 | grep -iE 'dsb|mite|ms_uops'

echo
echo "---- fetch-latency vs fetch-bandwidth (TMA L2 frontend) ----"
perf stat -M tma_fetch_latency,tma_fetch_bandwidth -p "$PID" -- sleep 6 2>&1 | grep -iE 'tma_fetch' || echo "  (tma metric unavailable)"

echo "fps_after=$(grep pacing-diag "$GAME_DIR/run.log"|tail -1|grep -oE 'swaps [0-9.]+/s')"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo CACHE_LEVELS_DONE
