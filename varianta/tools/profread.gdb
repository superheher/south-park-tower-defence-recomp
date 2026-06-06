set pagination off
set confirm off
set width 0
set logging file /tmp/gdb_profread.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out/sp_td_varianta
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out
set environment REX_NOTOKEN=1
set environment REX_CSLEAK=1
set environment REX_CPCOMPLETE=1
set environment REX_MOVIE_EOF=30
set environment REX_SKIPINTRO=1
set args /home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted/default.xex
set $inpath = 0
break __imp__sub_82493F98
commands
  silent
  set $inpath = 1
  continue
end
# args at the XamUserReadProfileSettings call in the subsystem-init path
break __imp__XamUserReadProfileSettings if $inpath==1
commands
  silent
  printf "\n[XamUserReadProfileSettings args] title=0x%x user=%d cnt=%d setcnt=%d bufSizePtr=0x%x bufPtr=0x%x overlapped=0x%x\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r7.u32, ctx.r9.u32, ctx.r10.u32, ctx.r11.u32
  continue
end
# the return as seen by the caller (sub_824C9500), expects 122
break ppc_recomp.75.cpp:20911 if $inpath==1
commands
  silent
  printf "[XamUserReadProfileSettings RET] r3=0x%x (%d)  caller-expects 122 -> %s\n", ctx.r3.u32, ctx.r3.u32, (ctx.r3.u32==122?"PASS":"E_FAIL")
  continue
end
run
