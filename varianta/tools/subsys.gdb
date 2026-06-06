set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_subsys.txt
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

# Entry: print args for EVERY call to the factory (find the r7=0x827FD56C = subsystem-slot creation)
break __imp__sub_8248F4C8
commands
  silent
  printf "\n=== sub_8248F4C8 ENTRY r3=%u r4=0x%x r5=0x%x r6=0x%x r7(out-slot)=0x%x ===\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32
  continue
end

# Trace the 4 gating sub-call results, ONLY for the 0x827FD56C creation (r23 preserves the out-slot)
break ppc_recomp.69.cpp:20141 if ctx.r23.u32 == 0x827FD56C
commands
  silent
  printf "  [1 sub_82497720] status r3=%d %s\n", ctx.r3.s32, (ctx.r3.s32<0 ? "<-- FAIL (<0)" : "ok")
  continue
end
break ppc_recomp.69.cpp:20159 if ctx.r23.u32 == 0x827FD56C
commands
  silent
  printf "  [2 sub_824898C0 OBJECT-CREATE] r3=0x%x %s\n", ctx.r3.u32, (ctx.r3.u32==0 ? "<-- FAIL (null)" : "ok")
  continue
end
break ppc_recomp.69.cpp:20171 if ctx.r23.u32 == 0x827FD56C
commands
  silent
  printf "  [3 sub_8244E5C0 ->obj+16] r3=0x%x %s\n", ctx.r3.u32, (ctx.r3.u32==0 ? "<-- FAIL (null)" : "ok")
  continue
end
break ppc_recomp.69.cpp:20204 if ctx.r23.u32 == 0x827FD56C
commands
  silent
  printf "  [4 sub_82493F98 INIT] status r3=%d %s\n", ctx.r3.s32, (ctx.r3.s32<0 ? "<-- FAIL (<0)" : "ok")
  continue
end
break ppc_recomp.69.cpp:20245 if ctx.r23.u32 == 0x827FD56C
commands
  silent
  printf "  [STORE -> *0x827FD56C] value r31=0x%x %s\n", ctx.r31.u32, (ctx.r31.u32==0 ? "<-- NULL subsystem (gate stuck)" : "<-- NON-NULL!")
  continue
end

run
