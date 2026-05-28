#!/usr/bin/env bash
# Autonomous test harness for the Linux build of south_park_td.
#
# Screenshots: capture the game's XWayland window via ImageMagick `import`
#   (launch the game with SDL_VIDEODRIVER=x11 so an X11 window exists).
# Input: focus-independent, by writing the REX_INPUT_FILE (live_input.txt)
#   that the runtime re-reads every poll. Format: "<btnhex> [lx] [ly]"
#   masks: 0010=START 1000=A 2000=B 0020=BACK 0001/2/4/8=DPAD U/D/L/R
#   sticks: lx/ly in [-32768,32767] (lx<0 left, lx>0 right, ly>0 up, ly<0 down)
#
# Requires: xdotool, ImageMagick (import). See docs/RUN-linux.md.
set -u
export DISPLAY="${DISPLAY:-:0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GAME_DIR="${REX_GAME_DIR:-$HERE/../out/build/linux-amd64-release}"
LIVE="$GAME_DIR/live_input.txt"
SHOTDIR="${REX_SHOT_DIR:-/tmp/sp}"
mkdir -p "$SHOTDIR"

wid() { xdotool search --classname south_park_td 2>/dev/null | tail -1; }

case "${1:-}" in
  wid) wid ;;
  shot)
    id=$(wid); [ -z "$id" ] && { echo "no game window" >&2; exit 1; }
    out="$SHOTDIR/${2:-shot}.png"
    import -window "$id" "$out" 2>/dev/null && echo "$out" ;;
  press) printf '%s 0 0' "${2:?mask}" >"$LIVE"; sleep "${3:-0.4}"; printf '0 0 0' >"$LIVE" ;;
  move)  printf '0 %s %s' "${2:?lx}" "${3:?ly}" >"$LIVE"; sleep "${4:-0.6}"; printf '0 0 0' >"$LIVE" ;;
  set)   printf '%s %s %s' "${2:?mask}" "${3:-0}" "${4:-0}" >"$LIVE" ;;
  release) printf '0 0 0' >"$LIVE" ;;
  *) cat >&2 <<'EOF'
usage: gamectl.sh {shot <name>|press <mask> [s]|move <lx> <ly> [s]|set <mask> <lx> <ly>|release|wid}
  masks: 0010=START 1000=A 2000=B 0020=BACK 0001/2/4/8=DPAD U/D/L/R
  sticks: lx/ly in [-32768,32767]
  env: REX_GAME_DIR (build dir holding live_input.txt), REX_SHOT_DIR (default /tmp/sp)
EOF
   exit 2 ;;
esac
