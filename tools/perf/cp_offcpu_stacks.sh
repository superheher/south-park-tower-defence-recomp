#!/usr/bin/env bash
# cp_offcpu_stacks.sh -- WHERE does the CP ("GPU Commands") thread block during a heavy dip?
# cp_offcpu.sh proved the CP is only ~50% on-cpu (blocks constantly). This answers the DISCRIMINATOR:
#   (a) blocked in vkWaitForFences / AwaitAll  => a GPU-sync the CP imposes  => a real chain-shortening lever EXISTS
#   (b) blocked on an empty ring / cond_wait   => waiting for the guest to produce commands => guest serialization, NO CP-side lever
# Method: perf record sched:sched_switch with DWARF call-graphs, restricted to the CP tid, over a sustained
# combat dip; perf report aggregates the switch-OUT (block) stacks by frequency. No code change.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
OUT=/tmp/cp_offcpu_stacks.txt; PD=/tmp/sp/offcpu_stacks.data
SECS="${1:-6}"
mkdir -p /tmp/sp
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== cp_offcpu_stacks exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) secs=$SECS =="
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid; echo 0 > /proc/sys/kernel/kptr_restrict; echo 1 > /proc/sys/kernel/sched_schedstats' 2>/dev/null
"$ROOT/tools/gamectl.sh" play >/tmp/offcpu_play.log 2>&1
grep -q 'IN LEVEL' /tmp/offcpu_play.log || { say PLAY_FAILED; tail -5 /tmp/offcpu_play.log >>"$OUT"; echo CP_OFFCPU_STACKS_DONE>>"$OUT"; exit 0; }
PID=$(pgrep -x south_park_td)
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
TID=$(ps -L -p "$PID" -o lwp,comm --no-headers 2>/dev/null | awk '/GPU Commands/{print $1; exit}')
say "dip at fps=$(fps); CP tid=$TID pid=$PID"
[ -z "$TID" ] && { say "NO CP TID"; echo CP_OFFCPU_STACKS_DONE>>"$OUT"; exit 0; }
# keep combat hot during capture
( for i in $(seq 1 40); do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.0; done ) &
DRIVER=$!
echo "${SUDO_PASS:-}" | sudo -S perf record -e sched:sched_switch --call-graph dwarf,8192 -t "$TID" -o "$PD" -- sleep "$SECS" 2>/tmp/offcpu_rec.log
kill "$DRIVER" 2>/dev/null
say "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
say ""
say "=== TOP CP block stacks (sched_switch-out call-graphs, ranked by frequency of blocking) ==="
echo "${SUDO_PASS:-}" | sudo -S perf report -i "$PD" --stdio --no-children -g graph,0.5,caller 2>/dev/null \
  | grep -vE '^#' | head -120 >>"$OUT"
say ""
say "=== flat: which functions appear at the leaf of a block (where it actually went to sleep) ==="
echo "${SUDO_PASS:-}" | sudo -S perf report -i "$PD" --stdio --no-children 2>/dev/null \
  | grep -vE '^#|^$' | head -40 >>"$OUT"
echo CP_OFFCPU_STACKS_DONE >>"$OUT"
