#!/usr/bin/env bash
# slack_probe.sh -- measure per-heavy-frame "quantization slack": time the CP sits BLOCKED in the
# WAIT_REG_MEM vblank/EOP fence CV-wait (the [slack-diag] line, diag .so only), vs total frame Δt.
# Large fence-wait while the GPU is idle == removable quantization slack == real floor headroom.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
OUT=/tmp/slack_probe.txt
HOLD="${1:-30}"
: > "$OUT"; say(){ printf '%s\n' "$*" >>"$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
rm -f /dev/shm/xenia_memory_* 2>/dev/null
say "== slack_probe so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) hold=${HOLD}s =="
"$ROOT/tools/gamectl.sh" play >/tmp/slack_play.log 2>&1
grep -q 'IN LEVEL' /tmp/slack_play.log || { say PLAY_FAILED; tail -5 /tmp/slack_play.log >>"$OUT"; echo SLACK_DONE>>"$OUT"; exit 0; }
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
# Escalate into combat (the pattern that reliably reached the 15fps dip), then wait for the dip, then hold.
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
say "dip at fps=$(fps); holding combat hot ${HOLD}s while measuring slack..."
END=$(( $(date +%s) + HOLD ))
while [ "$(date +%s)" -lt "$END" ]; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.0; done
say "fps_end=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
say ""
say "=== pacing + quant + slack over the run (focus on the low-fps windows) ==="
grep -E 'pacing-diag|quant-diag|slack-diag' "$LOG" 2>/dev/null | grep -oE '[0-9]+:[0-9]+:[0-9]+.*(swaps .*|dt_ms hist.*|avg_dt=.*)' | tail -45 >>"$OUT"
echo SLACK_DONE >>"$OUT"
