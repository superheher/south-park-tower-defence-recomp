#!/usr/bin/env bash
# measure45.sh <prefix> <pin01> -- measure3 + per-core freq sampling; optional
# pinning of hot game threads to separate physical cores after level entry.
# Usage: measure45.sh m4 0   (no pin)   /   measure45.sh m5 1   (pin)
set -u
PFX="${1:?prefix}"; PIN="${2:-0}"
SP=/tmp/sp; mkdir -p "$SP"
TL=$SP/${PFX}_timeline.txt; : >"$TL"
mark() { echo "$(date +%s.%N) $1" >>"$TL"; }
GC=/home/h/src/recomp/gamectl.sh
GAME_DIR=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
LOG=$GAME_DIR/run.log
LIVE=$GAME_DIR/live_input.txt
export DISPLAY=:0

mark script_start
"$GC" play >"$SP/${PFX}_play.out" 2>&1 &

ok=""
for _ in $(seq 1 450); do
  grep -q 'title up' "$SP/${PFX}_play.out" 2>/dev/null && { ok=1; break; }
  grep -q 'boot failed' "$SP/${PFX}_play.out" 2>/dev/null && break
  sleep 0.2
done
[ -z "$ok" ] && { mark abort_no_title; exit 1; }
mark title_up
sleep 0.5
PID=$(pgrep -x south_park_td | head -1)

# Per-core frequency sampler + current core of hot threads, 2 Hz, while game lives.
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

ID=$(xdotool search --classname south_park_td 2>/dev/null | tail -1)
[ -z "$ID" ] && { mark abort_no_wid; kill "$SAMPLER_JOB" 2>/dev/null; exit 1; }
ffmpeg -hide_banner -loglevel warning -f x11grab -window_id "$ID" -framerate 60 \
  -i :0.0 -t 150 -c:v libopenh264 -b:v 12M -pix_fmt yuv420p \
  -y "$SP/${PFX}_capture.mp4" >"$SP/${PFX}_ffmpeg.out" 2>&1 &
FF_PID=$!
mark ffmpeg_start

ok=""
for _ in $(seq 1 500); do
  grep -q 'IN LEVEL' "$SP/${PFX}_play.out" 2>/dev/null && { ok=1; break; }
  grep -qE 'boot failed|could not enter' "$SP/${PFX}_play.out" 2>/dev/null && break
  sleep 0.2
done
[ -z "$ok" ] && { mark abort_no_level; kill "$FF_PID" "$SAMPLER_JOB" 2>/dev/null; exit 1; }
mark in_level

if [ "$PIN" = "1" ]; then
  # Pin hot threads to separate physical cores (HT siblings are N,N+1 pairs
  # on this host: core0=cpu0+1, core1=cpu2+3, ...). Unprivileged on own proc.
  for t in /proc/$PID/task/*; do
    c=$(cat "$t/comm" 2>/dev/null); tid=${t##*/}
    case "$c" in
      'GPU Commands'*) taskset -cp 2 "$tid" >/dev/null 2>&1 ;;
      'Main XThread'*) taskset -cp 4 "$tid" >/dev/null 2>&1 ;;
      'Audio Worker'*) taskset -cp 6 "$tid" >/dev/null 2>&1 ;;
      'GPU VSync'*) taskset -cp 8 "$tid" >/dev/null 2>&1 ;;
    esac
  done
  mark pinned
fi

import -window "$ID" "$SP/${PFX}_level_t0.png" 2>/dev/null
(
  for _ in $(seq 1 45); do
    printf '0 26000 0' >"$LIVE"; sleep 0.9
    printf '0 0 26000' >"$LIVE"; sleep 0.9
    printf '0 -26000 0' >"$LIVE"; sleep 0.9
    printf '0 0 -26000' >"$LIVE"; sleep 0.9
  done
  printf '0 0 0' >"$LIVE"
) &
MOVE_JOB=$!
mark move_start

wait "$FF_PID" 2>/dev/null
mark video_end
import -window "$ID" "$SP/${PFX}_level_end.png" 2>/dev/null
cp "$LOG" "$SP/${PFX}_run.log" 2>/dev/null
kill "$MOVE_JOB" 2>/dev/null
printf '0 0 0' >"$LIVE"
"$GC" kill >/dev/null 2>&1
kill "$SAMPLER_JOB" 2>/dev/null
mark done
echo "DONE $PFX"
