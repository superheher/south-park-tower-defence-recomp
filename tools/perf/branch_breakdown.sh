#!/usr/bin/env bash
# branch_breakdown.sh -- THE decisive follow-up: the floor is branch-mispredict bound
# (39% resteers, 12% mispredict rate), not i-cache. This splits the mispredicts by TYPE:
#   conditional (direct, data-dependent game logic -> harder to fix)
#   vs indirect+return (the recompiler's call/return dispatch -> fixable in codegen).
# Self-contained: cleanup, boot, measure, all in one process (sleeps live in the file).
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
GAME_DIR="$ROOT/out/build/linux-amd64-release"
OUT="${OUT:-/tmp/branch_breakdown.txt}"; exec >"$OUT" 2>&1

pkill -f floor_rootcause.sh 2>/dev/null
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1; pkill -f 'gamectl.sh play' 2>/dev/null
sleep 2
echo "== boot =="
"$ROOT/tools/gamectl.sh" play >/tmp/bb_play.log 2>&1
PID=$(pgrep -x south_park_td)
echo "PID=$PID  $(tail -1 /tmp/bb_play.log)"
[ -z "$PID" ] && { echo BOOT_FAILED; tail -8 /tmp/bb_play.log; exit 0; }
echo "${SUDO_PASS:-}" | sudo -S sh -c 'echo -1 > /proc/sys/kernel/perf_event_paranoid' 2>/dev/null
fps(){ grep pacing-diag "$GAME_DIR/run.log"|tail -1|grep -oE 'swaps [0-9.]+/s'; }
echo "fps_before=$(fps)"

echo
echo "############ BRANCH COUNTS by type (10s) ############"
perf stat -p "$PID" -e \
instructions,\
br_inst_retired.all_branches,\
br_inst_retired.conditional,\
br_inst_retired.near_call,\
br_inst_retired.near_return,\
br_inst_retired.near_taken \
  -- sleep 10 2>&1 | grep -iE 'instructions|br_inst|all_branches|conditional|near_call|near_return|near_taken'

echo
echo "############ BRANCH MISPREDICTS by type (10s) ############"
echo "  (indirect+return mispred ~= all_misp - conditional_misp - near_call_misp)"
perf stat -p "$PID" -e \
br_misp_retired.all_branches,\
br_misp_retired.conditional,\
br_misp_retired.near_call,\
br_misp_retired.near_taken \
  -- sleep 10 2>&1 | grep -iE 'br_misp|all_branches|conditional|near_call|near_taken'

echo
echo "############ try indirect-specific events (may be unsupported on this uarch) ############"
perf stat -p "$PID" -e \
br_inst_retired.indirect,\
br_misp_retired.indirect,\
br_misp_retired.ret,\
baclears.any \
  -- sleep 6 2>&1 | grep -iE 'indirect|ret|baclear|supported' || echo "  (none supported)"

echo
echo "############ how does a guest CALL / RETURN get emitted? (codegen evidence) ############"
echo "  --- indirect-call / return dispatch in the generated header ---"
sed -n '230,275p' "$GAME_DIR/../../../generated/default/south_park_td_init.h" 2>/dev/null | grep -nE 'LOOKUP_FUNC|indirect|ResolveIndirect|dispatch|blr|bctr|return' | head
echo "fps_after=$(fps)"
"$ROOT/tools/gamectl.sh" kill >/dev/null 2>&1
echo BRANCH_BREAKDOWN_DONE
