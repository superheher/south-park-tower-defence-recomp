#!/usr/bin/env bash
# drawbatch_probe.sh -- decides draw-batching FEASIBILITY: draws vs pipeline-changes vs pixel-texture-
# changes per heavy-combat frame (diag .so emits [drawbatch-diag]). tex_changes~=pipe_changes => same-
# pipeline runs share textures => trivially batchable; tex_changes~=draws => distinct textures per draw =>
# batching needs texture arrays (hard).
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
OUT=/tmp/drawbatch_probe.txt; HOLD="${1:-16}"
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== drawbatch_probe so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) hold=${HOLD}s =="
"$ROOT/tools/gamectl.sh" play >/tmp/db_play.log 2>&1
grep -q 'IN LEVEL' /tmp/db_play.log || { say PLAY_FAILED; tail -5 /tmp/db_play.log >>"$OUT"; echo DB_DONE>>"$OUT"; exit 0; }
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
say "dip at fps=$(fps); holding ${HOLD}s..."
END=$(( $(date +%s) + HOLD ))
while [ "$(date +%s)" -lt "$END" ]; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.0; done
say "fps_end=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
say ""
say "=== draw-batch feasibility (pacing + drawbatch over the run) ==="
grep -E 'pacing-diag|drawbatch-diag' "$LOG" 2>/dev/null | grep -oE '[0-9]+:[0-9]+:[0-9]+.*(swaps [0-9.]+/s|draws/frame.*)' | tail -40 >>"$OUT"
echo DB_DONE >>"$OUT"
