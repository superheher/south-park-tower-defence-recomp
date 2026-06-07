# cont.100 prod-oracle (variant A half): prod runs sub_822045E0 via a SEPARATE chain (root sub_82249678,
# via sub_822296C0->sub_82229D00->sub_8222A0F0->sub_8222A2A0) distinct from the fill chain. Does variant A's
# full-chain run reach this draw-path? Break the distinctive nodes one-shot (self-disabling, no throttle).
# If they FIRE -> variant A runs the same path (issue is downstream buffer/GPU). If NOT -> the gate is entry here.
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_va_drawchain.txt
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

break sub_822045E0
commands
  silent
  printf "\n===== VA sub_822045E0 (prod draw-path leaf) FIRES thread=%d =====\n", $_thread
  bt 14
  disable 1
  continue
end
break sub_8222A2A0
commands
  silent
  printf "\n===== VA sub_8222A2A0 (prod draw-path entry) FIRES thread=%d =====\n", $_thread
  bt 14
  disable 2
  continue
end
break sub_822296C0
commands
  silent
  printf "\n===== VA sub_822296C0 (prod draw-path, calls 045E0) FIRES thread=%d =====\n", $_thread
  bt 10
  disable 3
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== va_drawchain armed (one-shot x3), running ===\n"
run
printf "\n=== va_drawchain done ===\n"
