#!/usr/bin/env bash
# guest_profile.sh -- where does the GUEST "Main XThread" (simulation) thread spend its time in a HEAVY
# combat dip? DSO split (south_park_td exe = recompiled GAME code vs librexruntime.so = EMULATOR runtime)
# + top functions. If dominated by game code -> structural (game's own cost). If a big chunk is emulator
# overhead (memory write-watch / invalidation / translation) -> a real lever exists.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"; PD=/tmp/sp/guest.data
OUT=/tmp/guest_profile.txt; SECS="${1:-12}"
mkdir -p /tmp/sp
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== guest_profile exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) secs=$SECS =="
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid; echo 0 > /proc/sys/kernel/kptr_restrict' 2>/dev/null
"$ROOT/tools/gamectl.sh" play >/tmp/guest_play.log 2>&1
grep -q 'IN LEVEL' /tmp/guest_play.log || { say PLAY_FAILED; tail -5 /tmp/guest_play.log >>"$OUT"; echo GUEST_DONE>>"$OUT"; exit 0; }
PID=$(pgrep -x south_park_td)
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
# identify the guest simulation thread
TID=$(ps -L -p "$PID" -o lwp,comm --no-headers 2>/dev/null | awk '/Main XThread|Main|XThread/{print $1; exit}')
say "dip at fps=$(fps); guest tid=$TID pid=$PID"
say "all thread comms:"; ps -L -p "$PID" -o comm --no-headers 2>/dev/null | sort | uniq -c | sort -rn | head >>"$OUT"
[ -z "$TID" ] && { say "NO GUEST TID"; echo GUEST_DONE>>"$OUT"; exit 0; }
# keep combat hot during capture
( for i in $(seq 1 40); do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.0; done ) &
DRIVER=$!
echo "${SUDO_PASS:-}" | sudo -S perf record -e cycles:u --call-graph lbr -F 4000 -t "$TID" -o "$PD" -- sleep "$SECS" 2>/tmp/guest_rec.log
kill "$DRIVER" 2>/dev/null
say "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
say ""
say "=== GUEST thread: DSO split (exe=GAME code vs librexruntime=EMULATOR vs libs) ==="
echo "${SUDO_PASS:-}" | sudo -S perf report -i "$PD" --stdio --sort dso --no-children 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -10 >>"$OUT"
say ""
say "=== GUEST thread: top 30 functions (self) ==="
echo "${SUDO_PASS:-}" | sudo -S perf report -i "$PD" --stdio --sort symbol --no-children 2>/dev/null | grep -E '^\s+[0-9]+\.[0-9]+%' | head -30 >>"$OUT"
say ""
say "=== GUEST thread: emulator-overhead markers (watch/invalidate/translate/lock/MMIO)? ==="
echo "${SUDO_PASS:-}" | sudo -S perf report -i "$PD" --stdio --sort symbol --no-children 2>/dev/null \
  | grep -iE 'watch|invalidat|translate|TranslatePhysical|lock|mutex|MMIO|StoreMemory|LoadMemory|RequestRange|SharedMemory|memcpy|store_and_swap' | head -20 >>"$OUT"
echo GUEST_DONE >>"$OUT"
