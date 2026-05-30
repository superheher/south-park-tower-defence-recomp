#!/usr/bin/env bash
# cp_offcpu.sh -- the CP ("GPU Commands") thread is only ~40-80% busy during a heavy dip, NOT pegged.
# So the floor may be LATENCY-bound (CP blocked waiting) rather than CP-compute-throughput-bound.
# This probe answers: during a sustained dip, what fraction of wall-time is the CP thread ON vs OFF cpu,
# and when OFF, what is it blocked on? Uses perf sched + off-cpu wakeup stacks (no code change).
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
OUT=/tmp/cp_offcpu.txt; PD=/tmp/sp/offcpu.data
mkdir -p /tmp/sp
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
say "== cp_offcpu exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) =="
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid; echo 0 > /proc/sys/kernel/kptr_restrict' 2>/dev/null
"$ROOT/tools/gamectl.sh" play >/tmp/offcpu_play.log 2>&1
grep -q 'IN LEVEL' /tmp/offcpu_play.log || { say PLAY_FAILED; tail -5 /tmp/offcpu_play.log >>"$OUT"; echo CP_OFFCPU_DONE>>"$OUT"; exit 0; }
PID=$(pgrep -x south_park_td)
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
TID=$(ps -L -p "$PID" -o lwp,comm --no-headers 2>/dev/null | awk '/GPU Commands/{print $1; exit}')
say "dip at fps=$(fps); CP tid=$TID"
# keep combat hot during capture
( for i in $(seq 1 8); do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.4; done ) &
# perf sched: per-thread on-cpu residency + scheduling latency over ~10s
echo "${SUDO_PASS:-}" | sudo -S perf sched record -o "$PD" -- sleep 10 2>/dev/null
wait
say "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
say "=== per-thread sched summary (runtime/wait, sort by runtime); look for GPU Commands ON-cpu fraction ==="
echo "${SUDO_PASS:-}" | sudo -S perf sched latency -i "$PD" --sort runtime 2>/dev/null | grep -iE 'Task|GPU Commands|Audio Worker|Main XThread|VSync|XMA|south_park|---' | head -25
say ""
say "=== total trace window + CP on-cpu time (perf sched timehist aggregate for the CP tid) ==="
echo "${SUDO_PASS:-}" | sudo -S perf sched timehist -i "$PD" 2>/dev/null | awk -v tid="$TID" '
  $0 ~ ("\\["tid"\\]") || $3==tid {n++; rt+=$NF} END{printf "  CP samples=%d sum_runtime_ms~=%.1f\n", n, rt}' 2>/dev/null
echo CP_OFFCPU_DONE >>"$OUT"
