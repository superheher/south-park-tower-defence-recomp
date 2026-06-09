#!/usr/bin/env bash
# cp_profile.sh -- boot to Stan's House combat and LBR-profile the live process to
# resolve WHERE the CPU time goes (self-time per symbol, per-thread Main vs CP/GPU),
# specifically to test the dispatcher/interpreter-overhead hypothesis vs the
# recompiled-code (front-end) hypothesis. One self-contained run (boot + profile)
# so the game stays alive for the whole capture. Writes a parseable report to $OUT.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
cd "$ROOT" || exit 1
OUT="${OUT:-/tmp/prof_final.txt}"; exec >"$OUT" 2>&1
SECS="${SECS:-22}"
GAME_DIR="$ROOT/out/build/linux-amd64-release"

"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null
sleep 1
echo "== boot =="
"$ROOT/tools/gamectl.sh" play >/tmp/cp_play.log 2>&1
PID=$(pgrep -x south_park_td)
echo "PID=$PID  $(tail -1 /tmp/cp_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; tail -8 /tmp/cp_play.log; exit 0; }

echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid; echo 0 > /proc/sys/kernel/kptr_restrict' 2>/dev/null
T=$(ps -T -p "$PID" -o tid,comm | awk '$2=="Main"{print $1;exit}')
G=$(ps -T -p "$PID" -o tid,comm | awk '$2=="GPU"{print $1;exit}')
echo "Main_tid=$T GPU_tid=$G fps_before=$(grep pacing-diag "$GAME_DIR/run.log"|tail -1|grep -oE 'swaps [0-9.]+/s')"
echo "== threads (tid comm %cpu) =="
ps -T -p "$PID" -o tid,comm,pcpu --sort=-pcpu | head -14

echo "== perf record --call-graph lbr -F 2000 ${SECS}s =="
perf record --call-graph lbr -F 2000 -p "$PID" -o /tmp/g.data -- sleep "$SECS" 2>&1 | tail -1
echo "fps_after=$(grep pacing-diag "$GAME_DIR/run.log"|tail -1|grep -oE 'swaps [0-9.]+/s')"

echo "================ OVERALL self-time (no children) >=0.6% ================"
perf report -i /tmp/g.data --stdio --no-children -g none --sort dso,symbol --percent-limit 0.6 2>/dev/null | grep -vE '^#|^$' | head -40
echo "================ dispatch/interp/Execute/lookup/trampoline/longjmp symbols ================"
perf report -i /tmp/g.data --stdio --no-children -g none --sort symbol 2>/dev/null | grep -iE 'dispatch|interp|Execute|lookup|recompil|trampolin|longjmp|setjmp|MmGet|TranslateV|host_to_guest|guest_to_host' | head -15
echo "================ MAIN thread (guest sim) >=1% ================"
perf report -i /tmp/g.data --stdio --no-children -g none --tid="$T" --sort dso,symbol --percent-limit 1 2>/dev/null | grep -vE '^#|^$' | head -22
echo "================ GPU/CP thread >=1% ================"
perf report -i /tmp/g.data --stdio --no-children -g none --tid="$G" --sort dso,symbol --percent-limit 1 2>/dev/null | grep -vE '^#|^$' | head -22
echo "================ DSO share — whole proc ================"
perf report -i /tmp/g.data --stdio --no-children -g none --sort dso 2>/dev/null | grep -vE '^#|^$' | head -8
echo PROFILE_DONE_OK
