#!/usr/bin/env bash
# Autonomous test harness for the Linux build of south_park_td.
#
# Quick start (one command, boot -> live first-level gameplay, ~60-70 s, hands-off):
#   tools/gamectl.sh play
#   tools/gamectl.sh bench 20      # then profile the framerate
#
# Screenshots: capture the game's XWayland window via ImageMagick `import`
#   (launch the game with SDL_VIDEODRIVER=x11 so an X11 window exists).
# Input: focus-independent, by writing the REX_INPUT_FILE (live_input.txt)
#   that the runtime re-reads every poll. Format: "<btnhex> [lx] [ly]"
#   masks: 0010=START 1000=A 2000=B 0020=BACK 0001/2/4/8=DPAD U/D/L/R
#   sticks: lx/ly in [-32768,32767] (lx<0 left, lx>0 right, ly>0 up, ly<0 down)
#
# Requires: xdotool, ImageMagick (import), bc. See docs/RUN-linux.md and
#   knowledge-base/titles/south-park-lgtdp/68-autonomous-boot-to-gameplay.md.
set -u
export DISPLAY="${DISPLAY:-:0}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_ROOT="${REX_GAME_ROOT:-$(cd "$HERE/.." && pwd)}"          # south-park-recomp
GAME_DIR="${REX_GAME_DIR:-$GAME_ROOT/out/build/linux-amd64-release}"
GAME_DATA="${REX_GAME_DATA:-$GAME_ROOT/private/extracted}"
USER_DATA="${REX_USER_DATA:-$GAME_ROOT/private/userdata}"
LIVE="$GAME_DIR/live_input.txt"
LOG="$GAME_DIR/run.log"
SHOTDIR="${REX_SHOT_DIR:-/tmp/sp}"
mkdir -p "$SHOTDIR"

wid()      { timeout 4 xdotool search --classname south_park_td 2>/dev/null | tail -1; }  # timeout: X can glitch; never hang play
fps_line() { grep 'pacing-diag' "$LOG" 2>/dev/null | tail -1 | grep -oE 'swaps [0-9.]+/s loading=(true|false)'; }
press()    { printf '%s 0 0' "${1:?mask}" >"$LIVE"; sleep "${2:-0.4}"; printf '0 0 0' >"$LIVE"; }

kill_all() {            # exact-name match only -- never `pkill -f south_park_td` (matches this shell!)
  pkill -x south_park_td 2>/dev/null
  for _ in $(seq 1 10); do pgrep -x south_park_td >/dev/null || break; sleep 0.3; done
  pgrep -x south_park_td >/dev/null && { pkill -9 -x south_park_td 2>/dev/null; sleep 0.5; }
  # Reclaim leaked guest-RAM shm: the game maps a 4.5G xenia_memory_* per launch and does
  # NOT remove it on crash/kill. They accumulate in /dev/shm (16G tmpfs); once full, the
  # next launch SIGBUSes faulting in guest RAM. Safe here: no instance is alive at this point.
  rm -f /dev/shm/xenia_memory_* 2>/dev/null
  return 0
}

launch_detached() {     # one fresh instance, fully detached so it survives this script exiting
  printf '0 0 0' >"$LIVE"; rm -f "$LOG"
  ( cd "$GAME_DIR" && setsid env SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. REX_INPUT_FILE="$LIVE" \
      ./south_park_td \
        --game_data_root="$GAME_DATA" --user_data_root="$USER_DATA" \
        --license_mask=1 --mnk_mode=true --always_win=true \
        --window_width=960 --window_height=540 \
        --log_file=run.log --log_level=info >/dev/null 2>&1 & )
}

# Launch and wait for the first present; retry the intermittent boot-deadlock (see KB doc 60).
# A successful warm boot reaches the first [pacing-diag] in ~4 s; a hung boot never does.
boot_until_title() {
  local a i
  for a in 1 2 3 4; do
    echo "  [boot] attempt $a: launching..." >&2
    kill_all; launch_detached
    for i in $(seq 1 75); do            # up to 15 s for the first [pacing-diag]
      grep -q 'pacing-diag' "$LOG" 2>/dev/null && { echo "  [boot] title up" >&2; sleep 1; return 0; }
      sleep 0.2
    done
    echo "  [boot] hung (no present in 15 s) -> retry" >&2
  done
  echo "  [boot] FAILED after 4 attempts" >&2; return 1
}

# From a fresh title, drive into live first-level gameplay. See KB doc 68 for the why.
#  Phase 1: alternate A/START until CAMPAIGN LEVEL SELECT renders (its 'camp_diagram' asset
#           appears in the log -- a reliable checkpoint). Covers the unskippable ~30 s intro,
#           the title (START-only), the lobby, and the mode/game menus without classifying screens.
#  Phase 2: press ONLY A (select Stan's House -> confirm -> start -> skip the "LEVEL 1" card +
#           the in-level dialogue). No START here, so nothing pauses/backs-out; extra A's just
#           land harmlessly in-match (always_win keeps the base invincible).
# NB: do NOT key off [pacing-diag] loading=true -- it only flips on a shader COMPILE, which is
#     cached away on a warm machine, so the first level loads with loading=false throughout.
nav_to_level() {
  echo "  [nav] START (leave title)" >&2
  press 0010 0.4; sleep 1.6
  local i b
  for i in $(seq 1 24); do
    grep -q 'camp_diagram' "$LOG" 2>/dev/null && break
    if [ $((i % 2)) -eq 1 ]; then b=1000; else b=0010; fi
    press "$b" 0.4; sleep 1.3
  done
  grep -q 'camp_diagram' "$LOG" 2>/dev/null || { echo "  [nav] never reached campaign select" >&2; return 1; }
  echo "  [nav] campaign select reached -> confirm level + skip intro (pure A)" >&2
  for i in $(seq 1 13); do press 1000 0.4; sleep 1.3; done
  return 0
}

case "${1:-}" in
  play)
    if [ "${2:-}" = "--keep" ] && [ -n "$(wid)" ]; then echo "[play] reusing running instance"; exit 0; fi
    boot_until_title || { echo "[play] boot failed"; exit 1; }
    if nav_to_level; then
      sleep 2; id=$(wid); out="$SHOTDIR/play_result.png"
      [ -n "$id" ] && timeout 6 import -window "$id" "$out" 2>/dev/null  # timeout: screenshot is non-essential, must not hang play
      echo "[play] IN LEVEL (Stan's House). $(fps_line)"
      echo "[play] window=$id  log=$LOG  verify-shot=$out"
      echo "[play] profile with: $0 bench 20"
    else
      echo "[play] reached title but could not enter a level (see $LOG)"; exit 1
    fi ;;

  bench)                # sample swaps/s from [pacing-diag] for N seconds (default 20)
    sec="${2:-20}"; [ -z "$(wid)" ] && { echo "no game window; run '$0 play' first"; exit 1; }
    echo "[bench] sampling ${sec}s of [pacing-diag]..."
    end=$(( $(date +%s) + sec )); min=9999; max=0; sum=0; n=0; stalls=0; last=""
    while [ "$(date +%s)" -lt "$end" ]; do
      line="$(grep 'pacing-diag' "$LOG" 2>/dev/null | tail -1)"
      if [ -n "$line" ] && [ "$line" != "$last" ]; then
        last="$line"; echo "$line" | grep -q 'loading=true' && stalls=$((stalls+1))
        v="$(echo "$line" | grep -oE 'swaps [0-9.]+' | grep -oE '[0-9.]+')"
        if [ -n "$v" ]; then
          n=$((n+1)); sum="$(echo "$sum + $v" | bc -l)"
          awk "BEGIN{exit !($v < $min)}" && min="$v"
          awk "BEGIN{exit !($v > $max)}" && max="$v"
          echo "$line" | grep -oE 'swaps.*'
        fi
      fi
      sleep 0.5
    done
    if [ "$n" -gt 0 ]; then
      echo "[bench] n=$n  min=${min}  avg=$(echo "scale=1; $sum/$n" | bc -l)  max=${max} swaps/s  load-stalls=$stalls"
    else echo "[bench] no [pacing-diag] samples (is the game running?)"; fi ;;

  boot) boot_until_title && echo "[boot] ready: $(fps_line)" ;;
  kill) kill_all; echo "killed (instances now: $(pgrep -cx south_park_td))" ;;
  fps)  fps_line ;;
  wid)  wid ;;
  shot)
    id=$(wid); [ -z "$id" ] && { echo "no game window" >&2; exit 1; }
    out="$SHOTDIR/${2:-shot}.png"; import -window "$id" "$out" 2>/dev/null && echo "$out" ;;
  press)   press "${2:?mask}" "${3:-0.4}" ;;
  move)    printf '0 %s %s' "${2:?lx}" "${3:?ly}" >"$LIVE"; sleep "${4:-0.6}"; printf '0 0 0' >"$LIVE" ;;
  set)     printf '%s %s %s' "${2:?mask}" "${3:-0}" "${4:-0}" >"$LIVE" ;;
  release) printf '0 0 0' >"$LIVE" ;;
  *) cat >&2 <<'EOF'
usage: gamectl.sh <command>
  play [--keep]        boot + drive into live first-level gameplay (~60-70s, hands-off)
  bench [sec]          sample framerate (swaps/s) from [pacing-diag]: min/avg/max + load-stalls
  boot                 launch + wait for the title (retries the intermittent boot-hang)
  kill                 kill every south_park_td instance (exact-name match)
  fps                  print the latest swaps/s + loading flag
  shot <name>          capture the game window -> $REX_SHOT_DIR/<name>.png (default /tmp/sp)
  press <mask> [s] | move <lx> <ly> [s] | set <mask> <lx> <ly> | release | wid
  masks: 0010=START 1000=A 2000=B 0020=BACK 0001/2/4/8=DPAD U/D/L/R ; sticks: [-32768,32767]
  env: REX_GAME_DIR REX_GAME_DATA REX_USER_DATA REX_SHOT_DIR
EOF
   exit 2 ;;
esac
