#!/usr/bin/env bash
# gpu_busy_probe.sh -- is the heavy combat dip GPU-bound or CP/guest-serialization-bound?
# Samples AMD gpu_busy_percent at ~20Hz during a sustained 15fps dip and reports the distribution.
# If GPU busy averages high (>70%) the floor is GPU-throughput; if low (<40%) it is serial latency/quantization.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
OUT=/tmp/gpu_busy_probe.txt
GBP=/sys/class/drm/card3/device/gpu_busy_percent
HOLD="${1:-16}"
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== gpu_busy_probe so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) hold=${HOLD}s gbp=$GBP =="
"$ROOT/tools/gamectl.sh" play >/tmp/gbp_play.log 2>&1
grep -q 'IN LEVEL' /tmp/gbp_play.log || { say PLAY_FAILED; tail -5 /tmp/gbp_play.log >>"$OUT"; echo GBP_DONE>>"$OUT"; exit 0; }
PID=$(pgrep -x south_park_td)
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
say "dip at fps=$(fps); sampling gpu_busy ~20Hz for ${HOLD}s while holding combat hot..."
# keep combat hot during the sampling window
( for i in $(seq 1 60); do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 0.8; done ) &
DRIVER=$!
SAMP=/tmp/sp/gbp_samples.txt; : > "$SAMP"
END=$(( $(date +%s) + HOLD ))
n=0
while [ "$(date +%s)" -lt "$END" ]; do
  v=$(cat "$GBP" 2>/dev/null); [ -n "$v" ] && { echo "$v" >>"$SAMP"; n=$((n+1)); }
  sleep 0.05
done
kill "$DRIVER" 2>/dev/null
say "fps_end=$(fps)  samples=$n"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
say ""
say "=== gpu_busy_percent distribution over the dip ==="
awk '{a[NR]=$1; s+=$1; if($1>mx||NR==1)mx=$1; if($1<mn||NR==1)mn=$1}
  END{ if(NR==0){print "no samples"; exit}
    n=asort(a);
    printf "  n=%d  min=%d  avg=%.1f  max=%d  p50=%d  p90=%d\n", NR, mn, s/NR, mx, a[int(NR*0.5)], a[int(NR*0.9)];
    b0=b1=b2=b3=b4=0; for(i=1;i<=NR;i++){v=a[i]; if(v<20)b0++; else if(v<40)b1++; else if(v<60)b2++; else if(v<80)b3++; else b4++}
    printf "  hist  <20:%d 20-40:%d 40-60:%d 60-80:%d 80-100:%d\n", b0,b1,b2,b3,b4 }' "$SAMP" >>"$OUT"
say ""
say "=== [pacing-diag]+[quant-diag] during sampling ==="
grep -E 'pacing-diag|quant-diag' "$LOG" 2>/dev/null | tail -12 >>"$OUT"
echo GBP_DONE >>"$OUT"
