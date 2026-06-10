#!/usr/bin/env bash
# measure1.sh -- Task-1 natural-load lag measurement (NO fixes).
# Orchestrates: play -> x11grab video (menu nav + ~2 min field) -> stick circling
# from level entry (visible cursor-smoothness probe) -> two perf windows
# (first seconds of control, +45 s) -> screenshots -> pacing log copy.
# All waits bounded. Emits epoch-stamped timeline to /tmp/sp/m1_timeline.txt.
set -u
SP=/tmp/sp; mkdir -p "$SP"
TL=$SP/m1_timeline.txt; : >"$TL"
mark() { echo "$(date +%s.%N) $1" >>"$TL"; }
GC=/home/h/src/recomp/gamectl.sh
GAME_DIR=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
LOG=$GAME_DIR/run.log
LIVE=$GAME_DIR/live_input.txt
export DISPLAY=:0

mark script_start
"$GC" play >"$SP/m1_play.out" 2>&1 &
PLAY_JOB=$!

# Wait for a successful boot (bounded 90 s; play retries internally up to 4x).
ok=""
for _ in $(seq 1 450); do
  grep -q 'title up' "$SP/m1_play.out" 2>/dev/null && { ok=1; break; }
  grep -q 'boot failed' "$SP/m1_play.out" 2>/dev/null && break
  sleep 0.2
done
[ -z "$ok" ] && { mark abort_no_title; exit 1; }
mark title_up
sleep 1

ID=$(xdotool search --classname south_park_td 2>/dev/null | tail -1)
[ -z "$ID" ] && { mark abort_no_wid; exit 1; }
eval "$(xdotool getwindowgeometry --shell "$ID")" # X Y WIDTH HEIGHT
mark "geom ${WIDTH}x${HEIGHT}+${X}+${Y}"
ffmpeg -hide_banner -loglevel warning -f x11grab -framerate 60 \
  -video_size "${WIDTH}x${HEIGHT}" -i ":0.0+${X},${Y}" \
  -t 165 -c:v libx264 -preset ultrafast -crf 18 -pix_fmt yuv420p \
  -y "$SP/m1_capture.mp4" >"$SP/m1_ffmpeg.out" 2>&1 &
FF_PID=$!
mark ffmpeg_start

# Wait for level entry (bounded 100 s).
ok=""
for _ in $(seq 1 500); do
  grep -q 'IN LEVEL' "$SP/m1_play.out" 2>/dev/null && { ok=1; break; }
  grep -qE 'boot failed|could not enter' "$SP/m1_play.out" 2>/dev/null && break
  sleep 0.2
done
[ -z "$ok" ] && { mark abort_no_level; kill "$FF_PID" 2>/dev/null; exit 1; }
mark in_level
PID=$(pgrep -x south_park_td | head -1)
import -window "$ID" "$SP/m1_level_t0.png" 2>/dev/null

# Slow stick circling for the rest of the capture: moves the cursor every
# frame -- the on-video probe for the maintainer's "throttling" feel.
(
  for _ in $(seq 1 30); do
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
perf record -F 499 -g -p "$PID" -o "$SP/m1_perf_first.data" -- sleep 15 >/dev/null 2>&1
mark perf1_end
import -window "$ID" "$SP/m1_level_t20.png" 2>/dev/null

sleep 25
mark perf2_start
perf record -F 499 -g -p "$PID" -o "$SP/m1_perf_later.data" -- sleep 15 >/dev/null 2>&1
mark perf2_end
import -window "$ID" "$SP/m1_level_t60.png" 2>/dev/null

wait "$FF_PID" 2>/dev/null
mark video_end
import -window "$ID" "$SP/m1_level_end.png" 2>/dev/null
cp "$LOG" "$SP/m1_run.log" 2>/dev/null
kill "$MOVE_JOB" 2>/dev/null
printf '0 0 0' >"$LIVE"
mark done
echo "DONE"
