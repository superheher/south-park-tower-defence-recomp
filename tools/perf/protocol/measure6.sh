#!/usr/bin/env bash
# measure6.sh <prefix> <spin01> -- COLD-ENTRY causal test for the frequency layer.
# Boot, nav to campaign select, IDLE 35 s there (clocks sag to the EPP=power
# floor), then enter the level. spin01=1 starts a nice-19 single-core busy
# spinner just before entry (utilization aid); 0 = no aid.
set -u
PFX="${1:?prefix}"; SPIN="${2:-0}"
SP=/tmp/sp; mkdir -p "$SP"
TL=$SP/${PFX}_timeline.txt; : >"$TL"
mark() { echo "$(date +%s.%N) $1" >>"$TL"; }
GC=/home/h/src/recomp/gamectl.sh
GAME_DIR=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
LOG=$GAME_DIR/run.log
LIVE=$GAME_DIR/live_input.txt
export DISPLAY=:0
press() { printf '%s 0 0' "$1" >"$LIVE"; sleep 0.4; printf '0 0 0' >"$LIVE"; }

mark script_start
"$GC" kill >/dev/null 2>&1
rm -f /dev/shm/xenia_memory_* 2>/dev/null

# boot to title (bounded, 4 attempts) -- replicate gamectl boot_until_title
ok=""
for attempt in 1 2 3 4; do
  pkill -x south_park_td 2>/dev/null; sleep 1
  printf '0 0 0' >"$LIVE"; rm -f "$LOG"
  ( cd "$GAME_DIR" && setsid env SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. REX_INPUT_FILE="$LIVE" \
      ./south_park_td --game_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted \
      --user_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/userdata \
      --license_mask=1 --mnk_mode=true --always_win=true --window_width=960 --window_height=540 \
      --log_file=run.log --log_level=info >/dev/null 2>&1 & )
  for _ in $(seq 1 75); do
    grep -q 'pacing-diag' "$LOG" 2>/dev/null && { ok=1; break; }
    sleep 0.2
  done
  [ -n "$ok" ] && break
done
[ -z "$ok" ] && { mark abort_boot; exit 1; }
mark title_up
sleep 1
PID=$(pgrep -x south_park_td | head -1)

# freq sampler (same as measure45, CP-core via stat-after-comm parse)
(
  FREQ_LOG=$SP/${PFX}_freq.log; : >"$FREQ_LOG"
  declare -A TIDS; TIDS[seed]=0
  while kill -0 "$PID" 2>/dev/null; do
    if [ "${#TIDS[@]}" -lt 5 ]; then
      for t in /proc/$PID/task/*; do
        c=$(cat "$t/comm" 2>/dev/null)
        case "$c" in
          'GPU Commands'*) TIDS[cp]=${t##*/} ;;
          'Main XThread'*) TIDS[main]=${t##*/} ;;
          'Audio Worker'*) TIDS[aud]=${t##*/} ;;
          'GPU VSync'*) TIDS[vs]=${t##*/} ;;
        esac
      done
    fi
    f=$(cat /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_cur_freq 2>/dev/null | tr '\n' ' ')
    cores=""
    for k in cp main aud vs; do
      tid=${TIDS[$k]:-}
      [ -n "$tid" ] && cores="$cores $k=$(sed 's/^[^)]*) //' /proc/$PID/task/$tid/stat 2>/dev/null | awk '{print $37}')"
    done
    echo "$(date +%s.%N) $f|$cores" >>"$FREQ_LOG"
    sleep 0.5
  done
) &
SAMPLER_JOB=$!

# nav to campaign select (replicates gamectl nav phase 1)
press 0010; sleep 1.6
for i in $(seq 1 24); do
  grep -q 'camp_diagram' "$LOG" 2>/dev/null && break
  if [ $((i % 2)) -eq 1 ]; then b=1000; else b=0010; fi
  press "$b"; sleep 1.3
done
grep -q 'camp_diagram' "$LOG" 2>/dev/null || { mark abort_nav; kill "$SAMPLER_JOB" 2>/dev/null; exit 1; }
mark camp_select

# THE COLD SOAK: idle 35 s, clocks sag to the EPP floor
sleep 35
mark cold_soak_done

if [ "$SPIN" = "1" ]; then
  ( exec nice -n 19 bash -c 'while :; do :; done' ) &
  SPIN_PID=$!
  mark spinner_started
else
  SPIN_PID=""
fi

# enter level: pure-A presses (camp select -> difficulty -> level card -> intro)
for i in $(seq 1 13); do press 1000; sleep 1.3; done
mark in_level_nav_done

# capture the first 60 s of field
ID=$(xdotool search --classname south_park_td 2>/dev/null | tail -1)
[ -n "$ID" ] && import -window "$ID" "$SP/${PFX}_t0.png" 2>/dev/null
(
  for _ in $(seq 1 17); do
    printf '0 26000 0' >"$LIVE"; sleep 0.9
    printf '0 0 26000' >"$LIVE"; sleep 0.9
    printf '0 -26000 0' >"$LIVE"; sleep 0.9
    printf '0 0 -26000' >"$LIVE"; sleep 0.9
  done
  printf '0 0 0' >"$LIVE"
) &
MOVE_JOB=$!
sleep 62
mark field_observed
[ -n "$ID" ] && import -window "$ID" "$SP/${PFX}_t60.png" 2>/dev/null

kill "$MOVE_JOB" 2>/dev/null
printf '0 0 0' >"$LIVE"
[ -n "$SPIN_PID" ] && kill "$SPIN_PID" 2>/dev/null
cp "$LOG" "$SP/${PFX}_run.log" 2>/dev/null
"$GC" kill >/dev/null 2>&1
kill "$SAMPLER_JOB" 2>/dev/null
mark done
echo "DONE $PFX"
