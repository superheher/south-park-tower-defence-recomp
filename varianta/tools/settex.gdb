set pagination off
set confirm off
set width 0
set logging file /tmp/gdb_settex.txt
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
# device.vtable[8] call in sub_821FFB10 (the actual texture bind / inlined-D3D SetTexture)
break ppc_recomp.22.cpp:10283
commands
  silent
  printf "[settex] #%d device=0x%x setTexFn(vtable[8])=0x%x texHandle r5=0x%x\n", $hits, ctx.r3.u32, ctx.ctr.u32, ctx.r5.u32
  set $hits = $hits + 1
  if $hits >= 6
    disable 1
  end
  continue
end
run
