#!/usr/bin/env bash
# measure_baseline.sh [combat_warmup] -- one-shot footprint+floor+mechanism snapshot
# of the CURRENTLY STAGED south_park_td, for the CODEGEN-SIZE report's "before".
# Launches the game (detached play + log-poll, robust to play's hanging tail),
# warms up combat, then runs profile.sh + floor.sh + catch_dip.sh.
set -u
ROOT="${REX_GAME_ROOT:-/home/h/src/recomp/rexglue-recomps/south-park-recomp}"
GAMECTL="$ROOT/tools/gamectl.sh"; GAME_DIR="$ROOT/out/build/linux-amd64-release"
LOG="$GAME_DIR/run.log"; WARMUP="${1:-75}"
trap '"$GAMECTL" kill >/dev/null 2>&1; pkill -f "gamectl.sh play" 2>/dev/null' EXIT

echo "=== .text footprint (staged) ==="
size "$GAME_DIR/south_park_td" | sed 's/^/  /'
md5sum "$GAME_DIR/south_park_td" "$GAME_DIR/librexruntime.so" | sed 's/^/  /'

"$GAMECTL" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null; sleep 1
( "$GAMECTL" play >/tmp/sp/measure_play.log 2>&1 ) &
echo "  [measure] booting+nav to Stan's House..."
got=0
for i in $(seq 1 60); do
  grep -q 'camp_diagram' "$LOG" 2>/dev/null && { got=1; break; }
  sleep 2
done
[ "$got" = 1 ] || { echo "  [measure] FAILED to reach camp_diagram"; exit 1; }
for i in $(seq 1 25); do grep -q 'Stans_House.lua' "$LOG" 2>/dev/null && break; sleep 2; done
echo "  [measure] in level; warming up combat ${WARMUP}s (let waves build)..."
sleep "$WARMUP"

echo "=== profile.sh 12 (micro-arch) ==="
"$ROOT/tools/perf/profile.sh" 12 2>&1
echo "=== floor.sh 120 (combat floor) ==="
"$ROOT/tools/perf/floor.sh" 120 2>/dev/null | tail -1
echo "=== catch_dip.sh 27 3 (confirm CPU-bound / GPU idle) ==="
"$ROOT/tools/perf/catch_dip.sh" 27 3 2>&1 | tail -8
echo "=== done ==="
