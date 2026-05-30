#!/usr/bin/env bash
# load_probe.sh -- during a sustained heavy combat dip, answer two decisive questions
# WITHOUT turbostat/sudo:
#   Q1 (throttle?):   are the cores at max all-core turbo, or clocked down (power/thermal)?
#   Q2 (saturated?):  how many threads are actually hot (>5% cpu)? serial floor -> few hot
#                     threads -> freeing the audio spin just idles a core (neutral). saturated
#                     -> freeing it can give the CP thread clock/core budget (could help floor).
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"; LOG="$GAME_DIR/run.log"
OUT=/tmp/load_probe.txt
: > "$OUT"
say(){ printf '%s\n' "$*" >> "$OUT"; }
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; sleep 2
say "== load_probe exe=$(md5sum "$GAME_DIR/south_park_td"|cut -c1-12) so=$(md5sum "$GAME_DIR/librexruntime.so"|cut -c1-12) =="
ncores=$(nproc); say "cores=$ncores  max_turbo=$(( $(cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq)/1000 ))MHz"
freqs(){ for c in $(seq 0 $((ncores-1))); do printf '%d ' "$(( $(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null)/1000 ))"; done; }
temps(){ for z in /sys/class/thermal/thermal_zone*/temp; do printf '%d ' "$(( $(cat "$z" 2>/dev/null)/1000 ))"; done; }
say "-- IDLE (no game) --"
say "  freqMHz: $(freqs)"
say "  tempC:   $(temps)"
"$ROOT/tools/gamectl.sh" play >/tmp/load_play.log 2>&1
grep -q 'IN LEVEL' /tmp/load_play.log || { say "PLAY_FAILED"; tail -5 /tmp/load_play.log >>"$OUT"; echo LOAD_PROBE_DONE >>"$OUT"; exit 0; }
PID=$(pgrep -x south_park_td)
fps(){ grep pacing-diag "$LOG"|tail -1|grep -oE 'swaps [0-9.]+'|grep -oE '[0-9.]+'; }
for i in 1 2 3 4 5 6; do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.2; done
say "waiting for heavy dip (fps<24)..."
for i in $(seq 1 120); do f=$(fps); [ -n "$f" ] && awk "BEGIN{exit !($f<24)}" && break; sleep 0.5; done
say "-- IN-DIP (fps=$(fps)) keeping combat hot, 5 samples over ~12s --"
( for i in $(seq 1 8); do "$ROOT/tools/gamectl.sh" press 1000 0.3; sleep 1.4; done ) &
for s in 1 2 3 4 5; do
  say "  [s$s fps=$(fps)] freqMHz: $(freqs)"
  say "       tempC: $(temps)"
  sleep 1.6
done
wait
say "-- per-thread CPU%% during dip (ps -L, top 16) --"
ps -L -p "$PID" -o lwp,pcpu,comm --no-headers 2>/dev/null | sort -k2 -rn | head -16 | sed 's/^/    /' >> "$OUT"
say "-- hot-thread summary --"
ps -L -p "$PID" -o pcpu,comm --no-headers 2>/dev/null | awk -v n="$ncores" '
  {tot+=$1; if($1>5)hot++; if($1>20)vhot++}
  END{printf "    sum_pcpu=%.0f%%  threads>5%%=%d  threads>20%%=%d  (of %d logical cores)\n",tot,hot,vhot,n}' >> "$OUT"
say "-- whole-machine load avg --"; say "    $(cat /proc/loadavg)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo LOAD_PROBE_DONE >> "$OUT"
