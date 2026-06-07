# cont.91: with pillar A solved (REX_EWDRAIN unfreezes the kick, ring flows 61->373), find the NEXT gate.
# Run variant A with REX_FRAMEEXEC+REX_EWDRAIN, let it reach the wptr=373 plateau, then dump an ALL-THREAD
# backtrace (break the fence-wait sub_821C6E58, which still fires per-frame) to see what the title now waits on.
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_plateau.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out/sp_td_varianta
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out
set environment REX_NOTOKEN=1
set environment REX_CSLEAK=1
set environment REX_CPCOMPLETE=1
set environment REX_MOVIE_EOF=30
set environment REX_FRAMEEXEC=1
set environment REX_EWDRAIN=1
set environment REX_SUBSYS=1
set environment REX_TIMER=1
set args /home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted/default.xex

set $n = 0
break sub_821C6E58
commands
  silent
  set $n = $n + 1
  if $n == 1500
    printf "\n========== PLATEAU SAMPLE A (fence-wait hit #1500) — ALL-THREAD BT ==========\n"
    thread apply all bt 7
    printf "========== end sample A ==========\n"
  end
  if $n == 5000
    printf "\n========== PLATEAU SAMPLE B (fence-wait hit #5000) — ALL-THREAD BT ==========\n"
    thread apply all bt 7
    printf "========== end sample B ==========\n"
  end
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== plateau watch armed (REX_EWDRAIN+REX_FRAMEEXEC), running ===\n"
run
printf "\n=== exited: fence-wait hits=%d ===\n", $n
