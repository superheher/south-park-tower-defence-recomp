#!/usr/bin/env bash
# passprobe.sh -- drive heavy combat and collect [hle-passprobe] from the app probe
# (REX_HLE_PASSPROBE in the south_park_td exe). Ranks the frame-render passes by CP-kick
# count = the dominant draw emitter (variant-B native-render hook). App-only build.
set -u
export REX_HLE_PASSPROBE=1
GC=/home/h/src/recomp/gamectl.sh
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
LOG=$ROOT/out/build/linux-amd64-release/run.log
OUT=/tmp/passprobe.txt; : > "$OUT"
say(){ printf '%s\n' "$*" >>"$OUT"; }
HOLD="${1:-45}"

"$GC" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== passprobe exe=$(md5sum "$ROOT/out/build/linux-amd64-release/south_park_td"|cut -c1-12) hold=${HOLD}s =="
"$GC" play >/tmp/pp_play.log 2>&1
grep -q 'IN LEVEL' /tmp/pp_play.log || say "PLAY(soft): $(tail -3 /tmp/pp_play.log | tr '\n' '|')"
fps(){ grep pacing-diag "$LOG" 2>/dev/null|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
# Place towers across the map (DPAD move cursor + A place) to maximise entities/projectiles,
# then advance waves. 0001/2/4/8 = DPAD U/D/L/R, 1000 = A.
say "placing towers (DPAD+A)..."
for d in 0004 0008 0001 0002 0004 0008 0001 0002; do
  "$GC" press "$d" 0.3 >/dev/null 2>&1; sleep 0.4
  "$GC" press 1000 0.3 >/dev/null 2>&1; sleep 0.5
  "$GC" press 1000 0.3 >/dev/null 2>&1; sleep 0.5
done
say "tower phase fps=$(fps); advancing waves, holding ${HOLD}s (entities accumulate)..."
END=$(( $(date +%s) + HOLD )); minf=99
while [ "$(date +%s)" -lt "$END" ]; do
  "$GC" press 1000 0.3 >/dev/null 2>&1; sleep 0.7
  # occasionally re-place towers to keep load high
  "$GC" press 0004 0.2 >/dev/null 2>&1; "$GC" press 1000 0.2 >/dev/null 2>&1; sleep 0.6
  f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<$minf)}" && minf="$f"
done
say "fps_end=$(fps) fps_min=$minf"
say ""
say "=== [hle-passprobe] (last 12) ==="
grep -E '\[hle-passprobe\]' "$LOG" 2>/dev/null | tail -12 >>"$OUT"
say "=== [drawgroup]/[pacing-diag] sanity (last 4) ==="
grep -E '\[drawgroup\]|pacing-diag' "$LOG" 2>/dev/null | tail -4 >>"$OUT"
say "PP_DONE"
"$GC" kill >/dev/null 2>&1 || true
