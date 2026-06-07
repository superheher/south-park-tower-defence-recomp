# cont.97: sub_821F8E60 (per-draw render, CONFIRMED entered in cont.96) does Lock(vtable+120) -> fill(sub_8242BF10)
# -> Unlock/submit(vtable+124, bctrl @ ppc_recomp.21:22605). cont.55: "the submit runs but emits no PM4 DRAW_INDX".
# QUESTION: what GUEST ADDRESS is in r30's vtable+124 (the submit method)? Is it a real recompiled fn or a
# variant-A stub? Capture ONE-SHOT (self-disable after first hit -> services 1 hit, NO throttle, per cont.96).
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_submit_probe.txt
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

# capture the Lock method (vtable+120) guest addr -- one-shot
break ppc_recomp.21.cpp:22580
commands
  silent
  printf "\n[LOCK   vtable+120] r3(obj)=0x%08X  method-guest=0x%08X\n", ctx.r3.u32, ctx.ctr.u32
  disable 1
  continue
end

# capture the Unlock/SUBMIT method (vtable+124) guest addr + the object + its vtable base -- one-shot
break ppc_recomp.21.cpp:22605
commands
  silent
  set $vt = ctx.r30.u32
  printf "\n[SUBMIT vtable+124] r30(VB/draw obj)=0x%08X  submit-method-guest=0x%08X\n", $vt, ctx.ctr.u32
  # read r30's vtable base (BE-stored guest mem at host base 0x7ffef7400000 + guest)
  set $vbase_be = *(unsigned int*)(0x7ffef7400000 + $vt)
  set $vbase = (($vbase_be & 0xff) << 24) | (($vbase_be & 0xff00) << 8) | (($vbase_be >> 8) & 0xff00) | (($vbase_be >> 24) & 0xff)
  printf "          r30 vtable-base=0x%08X\n", $vbase
  disable 2
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== submit-probe armed (one-shot), running ===\n"
run
printf "\n=== submit-probe done ===\n"
