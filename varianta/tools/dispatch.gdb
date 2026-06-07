# cont.96: sub_821F8E60 (per-draw render) is never entered (cont.95). Its ONLY callers are inside
# sub_821F8488 (ppc_recomp.21:21874 + 22228) — a per-draw render LOOP (iterates r17, stride16, bound 6400).
# So either (a) sub_821F8488 is never reached, or (b) it IS reached but the draw-list is empty / an early
# branch skips the loop body. Break the dispatcher + the callee under the full chain; capture thread + bt +
# the loop registers to bisect (a) vs (b).
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_dispatch.txt
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

set $disp = 0
set $callee = 0

# sub_821F8488 = the per-draw render dispatcher (the ONLY caller of sub_821F8E60).
break sub_821F8488
commands
  silent
  set $disp = $disp + 1
  if $disp <= 4
    printf "\n===== sub_821F8488 (render dispatcher) hit #%d  thread=%d  r3=0x%08X r4=0x%08X =====\n", $disp, $_thread, ctx.r3.u32, ctx.r4.u32
    bt 10
  end
  continue
end

# sub_821F8E60 = the per-draw render (should be 0 per cont.95 — confirm + catch if it fires now).
break sub_821F8E60
commands
  silent
  set $callee = $callee + 1
  if $callee <= 2
    printf "\n===== sub_821F8E60 (per-draw render) hit #%d  thread=%d =====\n", $callee, $_thread
    bt 6
  end
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== dispatch watch armed (full chain), running ===\n"
run
printf "\n=== exited: dispatcher(821F8488)=%d  callee(821F8E60)=%d ===\n", $disp, $callee
