set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_diverge.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out/sp_td_varianta
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out
set environment REX_NOTOKEN=1
set environment REX_CSLEAK=1
set environment REX_CPCOMPLETE=1
set environment REX_MOVIE_EOF=30
set environment REX_SKIPINTRO=1
set environment REX_HANDLERGUARD=1
set args /home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted/default.xex

# Handler chain: xstart -> sub_82450350 -> sub_825906D0 -> sub_82425480(store)
break sub_82450350
commands
  silent
  printf "\n[H1] HIT sub_82450350 (handler chain depth1)\n"
  bt 3
  disable 1
  continue
end
break sub_825906D0
commands
  silent
  printf "\n[H2] HIT sub_825906D0 (handler chain depth2)\n"
  bt 3
  disable 2
  continue
end
break sub_82425480
commands
  silent
  printf "\n[H3] HIT sub_82425480 (HANDLER WRITER)\n"
  bt 4
  disable 3
  continue
end

# Subsys chain: xstart -> sub_82249638 -> sub_82249678 -> sub_8214FFD0 -> sub_82292B10 -> sub_8248F4C8(store)
break sub_82249638
commands
  silent
  printf "\n[S1] HIT sub_82249638 (subsys chain depth1)\n"
  disable 4
  continue
end
break sub_82249678
commands
  silent
  printf "\n[S2] HIT sub_82249678 (subsys chain depth2)\n"
  disable 5
  continue
end
break sub_8214FFD0
commands
  silent
  printf "\n[S3] HIT sub_8214FFD0 (subsys chain depth3 = loader re-request)\n"
  disable 6
  continue
end
break sub_82292B10
commands
  silent
  printf "\n[S4] HIT sub_82292B10 (subsys chain depth4 = frontend producer)\n"
  bt 4
  disable 7
  continue
end
break sub_8248F4C8
commands
  silent
  printf "\n[S5] HIT sub_8248F4C8 (SUBSYS WRITER)\n"
  bt 5
  disable 8
  continue
end

printf "\n=== breakpoints armed, running variant A boot ===\n"
run
printf "\n=== inferior exited / end ===\n"
