# cont.101b: disambiguate sub_822045E0's output — is it 4 VERTS or a 4x4 MATRIX? Break AFTER all 4 vector stores
# (line 21993, near the blr) and dump the 64 bytes at the dest (r3 captured at the store). Also dump all 4 written
# vectors (v8,v9,v13,v0) as floats. A [*,*,*,*]x4 with a [0,0,0,1] row => matrix; screen-space xy pairs => verts.
set pagination off
set confirm off
set width 0
set logging file /tmp/gdb_va_vertdest2.txt
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
# break at the last store region to read all 4 written vectors
break ppc_recomp.22.cpp:21991
commands
  silent
  set $n = $n + 1
  printf "\n[VA vd2 #%d] r3=0x%08X | v8=[%f %f %f %f] v9=[%f %f %f %f]\n", $n, ctx.r3.u32, ctx.v8.f32[0],ctx.v8.f32[1],ctx.v8.f32[2],ctx.v8.f32[3], ctx.v9.f32[0],ctx.v9.f32[1],ctx.v9.f32[2],ctx.v9.f32[3]
  printf "          v13=[%f %f %f %f] v0=[%f %f %f %f]\n", ctx.v13.f32[0],ctx.v13.f32[1],ctx.v13.f32[2],ctx.v13.f32[3], ctx.v0.f32[0],ctx.v0.f32[1],ctx.v0.f32[2],ctx.v0.f32[3]
  if $n >= 4
    disable 1
  end
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== va_vertdest2 armed, running ===\n"
run
printf "\n=== va_vertdest2 done (hits=%d) ===\n", $n
