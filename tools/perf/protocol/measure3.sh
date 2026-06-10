#!/usr/bin/env bash
# measure3.sh -- Task-1 canonical run: video from TITLE (menu baseline) through
# level entry + ~85 s of field, window-drawable capture (-window_id), cursor
# circling on the field, two perf windows. All waits bounded.
set -u
SP=/tmp/sp; mkdir -p "$SP"
TL=$SP/m3_timeline.txt; : >"$TL"
mark() { echo "$(date +%s.%N) $1" >>"$TL"; }
GC=/home/h/src/recomp/gamectl.sh
GAME_DIR=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
LOG=$GAME_DIR/run.log
LIVE=$GAME_DIR/live_input.txt
export DISPLAY=:0

mark script_start
"$GC" play >"$SP/m3_play.out" 2>&1 &

ok=""
for _ in $(seq 1 450); do
  grep -q 'title up' "$SP/m3_play.out" 2>/dev/null && { ok=1; break; }
  grep -q 'boot failed' "$SP/m3_play.out" 2>/dev/null && break
  sleep 0.2
done
[ -z "$ok" ] && { mark abort_no_title; exit 1; }
mark title_up
sleep 0.5

ID=$(xdotool search --classname south_park_td 2>/dev/null | tail -1)
[ -z "$ID" ] && { mark abort_no_wid; exit 1; }
mark "wid $ID"
ffmpeg -hide_banner -loglevel warning -f x11grab -window_id "$ID" -framerate 60 \
  -i :0.0 -t 150 -c:v libopenh264 -b:v 12M -pix_fmt yuv420p \
  -y "$SP/m3_capture.mp4" >"$SP/m3_ffmpeg.out" 2>&1 &
FF_PID=$!
mark ffmpeg_start

ok=""
for _ in $(seq 1 500); do
  grep -q 'IN LEVEL' "$SP/m3_play.out" 2>/dev/null && { ok=1; break; }
  grep -qE 'boot failed|could not enter' "$SP/m3_play.out" 2>/dev/null && break
  sleep 0.2
done
[ -z "$ok" ] && { mark abort_no_level; kill "$FF_PID" 2>/dev/null; exit 1; }
mark in_level
PID=$(pgrep -x south_park_td | head -1)
import -window "$ID" "$SP/m3_level_t0.png" 2>/dev/null

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

mark perf1_start
perf record -F 499 -g -p "$PID" -o "$SP/m3_perf_first.data" -- sleep 15 >/dev/null 2>&1
mark perf1_end
import -window "$ID" "$SP/m3_level_t20.png" 2>/dev/null

sleep 25
mark perf2_start
perf record -F 499 -g -p "$PID" -o "$SP/m3_perf_later.data" -- sleep 15 >/dev/null 2>&1
mark perf2_end
import -window "$ID" "$SP/m3_level_t60.png" 2>/dev/null

wait "$FF_PID" 2>/dev/null
mark video_end
import -window "$ID" "$SP/m3_level_end.png" 2>/dev/null
cp "$LOG" "$SP/m3_run.log" 2>/dev/null
kill "$MOVE_JOB" 2>/dev/null
printf '0 0 0' >"$LIVE"
mark done
echo "DONE"
