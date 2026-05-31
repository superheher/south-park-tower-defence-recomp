#!/usr/bin/env bash
# shot_stub.sh <hexaddr|""> <tag> -- boot, drive to active combat, screenshot, kill.
# With a hex addr, that pass is stubbed (REX_HLE_STUB) so the shot shows what it rendered.
set -u
STUB="${1:-}"; TAG="${2:-baseline}"
[ -n "$STUB" ] && export REX_HLE_STUB="$STUB"
GC=/home/h/src/recomp/gamectl.sh
LOG=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/run.log
OUT=/tmp/shot_${TAG}.txt; : > "$OUT"
say(){ printf '%s\n' "$*" >>"$OUT"; }
export DISPLAY="${DISPLAY:-:0}"
# Capture via gamectl shot (the method that produced clean 960x540 color shots at baseline). Log the
# resulting dimensions/size so a bad capture (1-bit / tiny) is visible without opening the file.
robshot(){
  local out=/tmp/sp/$1.png
  "$GC" shot "$1" >/dev/null 2>&1
  say "shot $1: $(file "$out" 2>/dev/null | grep -oE '[0-9]+ x [0-9]+|1-bit') $(stat -c%s "$out" 2>/dev/null)B"
}
"$GC" kill >/dev/null 2>&1; sleep 2; rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== shot tag=$TAG stub=${STUB:-none} =="
"$GC" play >/tmp/shot_play_${TAG}.log 2>&1
fps(){ grep pacing-diag "$LOG" 2>/dev/null|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
# Fail-fast: a stub that breaks all rendering yields no window after the boot retries -> CRITICAL.
if [ -z "$("$GC" wid)" ]; then
  say "CRITICAL: no window (stub ${STUB:-none} breaks boot/rendering)"
  say "SHOT_DONE tag=$TAG"
  "$GC" kill >/dev/null 2>&1 || true
  exit 0
fi
grep -q 'IN LEVEL' /tmp/shot_play_${TAG}.log || say "PLAY(soft): $(tail -2 /tmp/shot_play_${TAG}.log|tr '\n' '|')"
# place towers + advance waves to get active combat with sprites/HUD on screen
for d in 0004 0008 0001 0002 0004 0008; do
  "$GC" press "$d" 0.3 >/dev/null 2>&1; sleep 0.4
  "$GC" press 1000 0.3 >/dev/null 2>&1; sleep 0.5
  "$GC" press 1000 0.3 >/dev/null 2>&1; sleep 0.5
done
for i in $(seq 1 24); do
  "$GC" press 1000 0.3 >/dev/null 2>&1; sleep 0.7
  "$GC" press 0008 0.2 >/dev/null 2>&1; "$GC" press 1000 0.2 >/dev/null 2>&1; sleep 0.6
done
say "fps=$(fps)"
robshot "${TAG}_a"; sleep 2.5
for i in 1 2 3 4 5 6; do "$GC" press 1000 0.3 >/dev/null 2>&1; sleep 0.9; done
say "fps2=$(fps)"
robshot "${TAG}_b"
say "SHOT_DONE tag=$TAG"
"$GC" kill >/dev/null 2>&1 || true
