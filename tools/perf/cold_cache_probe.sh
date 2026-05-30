#!/usr/bin/env bash
# cold_cache_probe.sh -- quantify the TRANSIENT first-encounter stutter: clear the persistent shader/pipeline
# cache, boot+drive combat, and log WHAT compiles WHEN (pipeline creates by thread + timestamp) vs the fps dips.
# Distinguishes sync (on the CP/render thread = a frame hitch) from async (bg compile threads = no hitch).
# Reversible: backs up and RESTORES the warm cache afterward.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
CACHE="$ROOT/private/userdata/cache/shaders/shareable"
BK=/tmp/sp/warmcache_bk
OUT=/tmp/cold_cache_probe.txt
HOLD="${1:-25}"
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
mkdir -p "$BK"
say "== cold_cache_probe so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) hold=${HOLD}s =="
say "warm cache before clear:"; ls -la "$CACHE" 2>/dev/null | grep 58410931 | sed 's/^/  /' >>"$OUT"
# back up + clear the persistent shader/pipeline cache (COLD)
cp -f "$CACHE"/58410931.* "$BK"/ 2>/dev/null
rm -f "$CACHE"/58410931.* 2>/dev/null
say "cleared cache (cold). Booting + driving combat..."
"$ROOT/tools/gamectl.sh" play >/tmp/cold_play.log 2>&1
if ! grep -q 'IN LEVEL' /tmp/cold_play.log; then
  say PLAY_FAILED; tail -6 /tmp/cold_play.log >>"$OUT"
  cp -f "$BK"/58410931.* "$CACHE"/ 2>/dev/null; say "restored warm cache"; echo COLD_DONE>>"$OUT"; exit 0
fi
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
# drive into combat and keep it hot for HOLD seconds
END=$(( $(date +%s) + HOLD ))
while [ "$(date +%s)" -lt "$END" ]; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 0.9; done
say "fps_end=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
# restore the warm cache so the user's next run is warm again
cp -f "$BK"/58410931.* "$CACHE"/ 2>/dev/null; say "restored warm cache: $(ls "$CACHE"/58410931.* 2>/dev/null | wc -l) files"
say ""
say "=== pipeline CREATES: count + which threads (sync CP-thread vs async bg pool) ==="
grep 'Creating graphics pipeline' "$LOG" 2>/dev/null | grep -oE '\[t[0-9]+\]' | sort | uniq -c | sort -rn | head >>"$OUT"
say "total pipeline creates this run: $(grep -c 'Creating graphics pipeline' "$LOG" 2>/dev/null)"
say "=== pipeline-create TIMELINE (count per second) vs the dips ==="
grep 'Creating graphics pipeline' "$LOG" 2>/dev/null | grep -oE '^\[[0-9-]+ [0-9]+:[0-9]+:[0-9]+' | sort | uniq -c | tail -30 >>"$OUT"
say "=== 'Created N pipelines from Vulkan storage' lines ==="
grep 'from Vulkan storage' "$LOG" 2>/dev/null | tail -6 >>"$OUT"
say "=== pacing-diag timeline (find dips that coincide with compile bursts) ==="
grep 'pacing-diag' "$LOG" 2>/dev/null | grep -oE '[0-9]+:[0-9]+:[0-9]+.*swaps [0-9.]+/s' | tail -30 >>"$OUT"
echo COLD_DONE >>"$OUT"
