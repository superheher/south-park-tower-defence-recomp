set pagination off
set confirm off
set width 0
set logging file /tmp/gdb_vtbl15.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out/sp_td_varianta
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out
set environment REX_NOTOKEN=1
set environment REX_CSLEAK=1
set environment REX_CPCOMPLETE=1
set environment REX_MOVIE_EOF=30
set args /home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted/default.xex
set $hits = 0
break ppc_recomp.21.cpp:22452
commands
  silent
  printf "[vtbl15] #%d renderer=0x%x target(vtable[15])=0x%x arg r4=0x%x\n", $hits, ctx.r31.u32, ctx.ctr.u32, ctx.r4.u32
  set $hits = $hits + 1
  if $hits >= 5
    disable 1
  end
  continue
end
run
