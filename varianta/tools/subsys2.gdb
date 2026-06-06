set pagination off
set confirm off
set width 0
set logging file /tmp/gdb_subsys2.txt
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

# mark our subsystem-init path
break __imp__sub_82493F98
commands
  silent
  set $inpath = 1
  printf "\n==== sub_82493F98 (subsystem INIT) entered — tracing sub_824C70A0 ====\n"
  continue
end

# sub_824C70A0 sub-call results + decision points (scoped to our path)
break ppc_recomp.75.cpp:15227 if $inpath==1
commands
  silent
  printf "[alloc sub_82448090] r31=0x%x %s\n", ctx.r31.u32, (ctx.r31.u32==0?"FAIL->OOM":"ok")
  continue
end
break ppc_recomp.75.cpp:15277 if $inpath==1
commands
  silent
  printf "[singleton 0x82819D90 old] r10=0x%x %s\n", ctx.r10.u32, (ctx.r10.u32!=0?"!=0 ->E_FAIL(site1 already-init)":"0 ok->init")
  continue
end
break ppc_recomp.75.cpp:15329 if $inpath==1
commands
  silent
  printf "[sub_824C6040] r3=%d (0x%x) %s\n", ctx.r3.s32, ctx.r3.u32, (ctx.r3.s32<0?"<0 ->exit":"ok")
  continue
end
break ppc_recomp.75.cpp:15352 if $inpath==1
commands
  silent
  printf "[sub_824C9500 loop] r3=%d (0x%x) %s\n", ctx.r3.s32, ctx.r3.u32, (ctx.r3.s32<0?"<0 ->exit":"ok")
  continue
end
break ppc_recomp.75.cpp:15411 if $inpath==1
commands
  silent
  printf "[sub_824CAFE8 loop] r3=%d (0x%x) %s\n", ctx.r3.s32, ctx.r3.u32, (ctx.r3.s32<0?"<0 ->exit":"ok")
  continue
end
break ppc_recomp.75.cpp:15440 if $inpath==1
commands
  silent
  printf "[sub_8244D358 NtCreateEvent-ish] r3=0x%x %s\n", ctx.r3.u32, (ctx.r3.u32==0?"==0 ->E_FAIL(site2)":"ok")
  continue
end
break ppc_recomp.75.cpp:15461 if $inpath==1
commands
  silent
  printf "[XamNotifyCreateListener] r3=0x%x %s\n", ctx.r3.u32, (ctx.r3.u32==0?"==0 ->E_FAIL":"ok")
  continue
end
break ppc_recomp.75.cpp:15495 if $inpath==1
commands
  silent
  printf "[reached post-listener: *(r24+8) check / sub_824505E8 area]\n"
  continue
end
break ppc_recomp.75.cpp:15502 if $inpath==1
commands
  silent
  printf "[sub_824505E8] r3=0x%x %s\n", ctx.r3.u32, (ctx.r3.u32==0?"==0 ->E_FAIL(site2b)":"ok")
  continue
end
break ppc_recomp.75.cpp:15515 if $inpath==1
commands
  silent
  printf "[sub_82448D78] r6=0x%x\n", ctx.r6.u32
  continue
end
# the final return of sub_824C70A0
break ppc_recomp.75.cpp:15557 if $inpath==1
commands
  silent
  printf ">>> sub_824C70A0 RETURN r29(status)=%d (0x%x) r31(obj)=0x%x <<<\n", ctx.r29.s32, ctx.r29.u32, ctx.r31.u32
  set $inpath = 0
  continue
end

run
