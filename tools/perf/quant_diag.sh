#!/usr/bin/env bash
# quant_diag.sh -- collect the per-frame Δt histogram ([quant-diag]) over a sustained heavy combat dip,
# to test whether the floor is vblank-QUANTIZED (frame periods cluster at 16.6ms multiples) or continuous.
# Requires the diag .so (adds [quant-diag] hist lines next to [pacing-diag]).
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
OUT=/tmp/quant_diag.txt
HOLD="${1:-18}"
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== quant_diag exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) hold=${HOLD}s =="
"$ROOT/tools/gamectl.sh" play >/tmp/quant_play.log 2>&1
grep -q 'IN LEVEL' /tmp/quant_play.log || { say PLAY_FAILED; tail -5 /tmp/quant_play.log >>"$OUT"; echo QUANT_DIAG_DONE>>"$OUT"; exit 0; }
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
# escalate into combat and force a heavy dip
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
say "dip at fps=$(fps); holding combat hot ${HOLD}s..."
# keep combat hot during the histogram window
END=$(( $(date +%s) + HOLD ))
while [ "$(date +%s)" -lt "$END" ]; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.0; done
say "fps_end=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
say ""
say "=== [pacing-diag] + [quant-diag] over the run (look for the low-fps windows) ==="
grep -E 'pacing-diag|quant-diag' "$LOG" 2>/dev/null | tail -40 >>"$OUT"
echo QUANT_DIAG_DONE >>"$OUT"
