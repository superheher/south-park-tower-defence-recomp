set pagination off
set confirm off
set width 0
set logging file /tmp/gdb_vbcheck.txt
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
# At each textured-draw SetTexture (real handle), dump the cont.47 dynamic VB (0xA022FFF0) as floats —
# is it clean per-draw geometry (screen/authoring coords + UVs) we could compose, or soup?
break __imp__sub_821BEC00 if ctx.r5.u32 >= 0x06000000 && ctx.r5.u32 < 0x07000000
commands
  silent
  set $b = (unsigned long long)base
  printf "\n[vb] #%d texH=0x%x | VB@0xA022FFF0 floats:\n", $hits, ctx.r5.u32
  x/24f $b + 0xA022FFF0
  set $hits = $hits + 1
  if $hits >= 4
    disable 1
  end
  continue
end
run
