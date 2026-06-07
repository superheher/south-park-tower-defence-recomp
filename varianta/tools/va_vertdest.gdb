# cont.101 (variant A half): sub_822045E0 writes 4 transformed verts (vmaddfp world*proj) via stvlx/stvrx to
# ea=r3 (stride r9=16). r3 is NOT modified entry->store, so r3 = the destination VB. Capture r3 + r9 + the
# first transformed vert (v8) ONE-SHOT (self-disabling). Compare r3 vs the cont.22 fetch slot-0 target 0xA2000000.
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_va_vertdest.txt
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
break ppc_recomp.22.cpp:21961
commands
  silent
  set $n = $n + 1
  printf "\n[VA vertdest #%d] r3(dest VB)=0x%08X  r9(stride)=%d  v8.f32=[%f %f %f %f]\n", $n, ctx.r3.u32, ctx.r9.u32, ctx.v8.f32[0], ctx.v8.f32[1], ctx.v8.f32[2], ctx.v8.f32[3]
  if $n >= 6
    disable 1
  end
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== va_vertdest armed, running ===\n"
run
printf "\n=== va_vertdest done (hits=%d) ===\n", $n
