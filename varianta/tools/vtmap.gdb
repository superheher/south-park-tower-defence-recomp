# cont.98: sub_821F8E60 makes 10 vtable/indirect calls (ppc_recomp.21). cont.97 named vt+120=Lock(sub_822052B0),
# vt+124=Unlock(sub_822052F8, no PM4). The actual DrawPrimitive->DRAW_INDX must be one of the OTHER calls.
# Capture each call's method guest-addr (ctx.ctr) + first-arg (ctx.r3) ONE-SHOT (self-disabling -> 1 hit each,
# NO throttle per cont.96). Then name each via the recompiled code to find the draw-emit.
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_vtmap.txt
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

# helper macro pattern: break at each indirect-call line, print ctx.ctr (method) + ctx.r3 (arg0), self-disable
break ppc_recomp.21.cpp:22373
commands
  silent
  printf "\n[vt+24  @22373] method=0x%08X r3=0x%08X\n", ctx.ctr.u32, ctx.r3.u32
  disable 1
  continue
end
break ppc_recomp.21.cpp:22391
commands
  silent
  printf "[vt+20  @22391] method=0x%08X r3=0x%08X\n", ctx.ctr.u32, ctx.r3.u32
  disable 2
  continue
end
break ppc_recomp.21.cpp:22436
commands
  silent
  printf "[dev+17704 @22436] method=0x%08X r3=0x%08X\n", ctx.ctr.u32, ctx.r3.u32
  disable 3
  continue
end
break ppc_recomp.21.cpp:22454
commands
  silent
  printf "[vt+60  @22454 SetTex] method=0x%08X r3=0x%08X r4=0x%08X\n", ctx.ctr.u32, ctx.r3.u32, ctx.r4.u32
  disable 4
  continue
end
break ppc_recomp.21.cpp:22528
commands
  silent
  printf "[vt+64  @22528] method=0x%08X r3=0x%08X r4=0x%08X\n", ctx.ctr.u32, ctx.r3.u32, ctx.r4.u32
  disable 5
  continue
end
break ppc_recomp.21.cpp:22547
commands
  silent
  printf "[vt+100 @22547] method=0x%08X r3=0x%08X r4=0x%08X\n", ctx.ctr.u32, ctx.r3.u32, ctx.r4.u32
  disable 6
  continue
end
break ppc_recomp.21.cpp:22558
commands
  silent
  printf "[vt+200 @22558 getVB] method=0x%08X r3=0x%08X\n", ctx.ctr.u32, ctx.r3.u32
  disable 7
  continue
end
break ppc_recomp.21.cpp:22623
commands
  silent
  printf "[vt+60  @22623 SetTex0] method=0x%08X r3=0x%08X r4=0x%08X\n", ctx.ctr.u32, ctx.r3.u32, ctx.r4.u32
  disable 8
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== vtmap armed (one-shot x8), running ===\n"
run
printf "\n=== vtmap done ===\n"
