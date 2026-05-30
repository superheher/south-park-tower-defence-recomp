#!/usr/bin/env bash
# turbo_probe.sh -- during a sustained heavy combat dip, measure whether the CPU is
# turbo/power/thermal throttled (which would make freeing the audio-worker's ~25%
# spin able to raise the CP thread's clock) or already at max all-core turbo (which
# would make audio de-spin floor-neutral, like the prior Main/timer/CP de-spins).
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
exec >/tmp/turbo_probe.txt 2>&1
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
echo "== turbo_probe exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) =="
echo "-- idle baseline (3s turbostat, no game) --"
echo "${SUDO_PASS:-}" | sudo -S turbostat --quiet --show Core,CPU,Bzy_MHz,PkgWatt,CorWatt,PkgTmp,CoreTmp --interval 1 sleep 3 2>/dev/null | tail -20
"$ROOT/tools/gamectl.sh" play >/tmp/turbo_play.log 2>&1
grep -q 'IN LEVEL' /tmp/turbo_play.log || { echo PLAY_FAILED; tail -5 /tmp/turbo_play.log; exit 0; }
PID=$(pgrep -x south_park_td)
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
echo "waiting for heavy dip (fps<22)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
echo "dip at fps=$(fps)"
echo "-- IN-DIP turbostat (10s, package summary lines) --"
( for i in 1 2 3 4 5; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.8; done ) &   # keep combat hot
echo "${SUDO_PASS:-}" | sudo -S turbostat --quiet --show Core,CPU,Bzy_MHz,Busy%,PkgWatt,CorWatt,PkgTmp --interval 2 sleep 10 2>/dev/null
wait
echo "fps_in_dip=$(fps)"
echo "-- per-thread CPU% (ps -L, top consumers) during/after dip --"
ps -L -p "$PID" -o lwp,pcpu,comm --no-headers 2>/dev/null | sort -k2 -rn | head -16
echo "-- active-thread count (pcpu>5) --"
ps -L -p "$PID" -o pcpu,comm --no-headers 2>/dev/null | awk '$1>5{n++} END{print n" threads >5% (cores=12)"}'
echo "-- RAPL power-limit throttle flags (MSR_CORE_PERF_LIMIT_REASONS not parsed; use PkgWatt vs TDP=45W) --"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo TURBO_PROBE_DONE
