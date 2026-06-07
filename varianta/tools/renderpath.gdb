# cont.95: with pillar A (REX_EWDRAIN+FRAMEEXEC) + timer (REX_TIMER) + subsys (REX_SUBSYS) all live,
# the subsys worker runs sub_8242BF10 (VB-fill) but the ring still plateaus at 373 (no content DRAW_INDX).
# QUESTION: has the cascade reached the cont.55 render-submit STUB (sub_821F8E60 Lock->memcpy->Unlock,
# where the Unlock's D3D GPU-submit is stubbed and emits no DRAW_INDX), or does it stop earlier at the
# cont.34/54 sub_821C6F50 (per-frame GPU-command building)? Break the three render-path nodes, capture
# WHICH THREAD + the full caller chain at each, count hits.
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_renderpath.txt
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

set $fill = 0
set $submit = 0
set $spin = 0

# sub_8242BF10 = the VB-fill memcpy we SEE running (cont.64-65). Capture its caller chain once.
break sub_8242BF10
commands
  silent
  set $fill = $fill + 1
  if $fill <= 3
    printf "\n===== sub_8242BF10 (VB-fill) hit #%d  thread=%d =====\n", $fill, $_thread
    bt 10
  end
  continue
end

# sub_821F8E60 = the cont.55 D3D dynamic-VB text-renderer (Lock -> sub_8242BF10 -> Unlock[vtable+124]).
# If THIS is reached, the cascade has walked to the render-submit; the Unlock submit is the stub.
break sub_821F8E60
commands
  silent
  set $submit = $submit + 1
  if $submit <= 3
    printf "\n===== sub_821F8E60 (D3D VB Lock/Unlock render-submit) hit #%d  thread=%d =====\n", $submit, $_thread
    bt 8
  end
  continue
end

# sub_821C6F50 = cont.34/54 per-frame GPU-command building (reg 0xC0003C00). Is it hit, and on what thread?
break sub_821C6F50
commands
  silent
  set $spin = $spin + 1
  if $spin <= 2
    printf "\n===== sub_821C6F50 (per-frame GPU-cmd build) hit #%d  thread=%d =====\n", $spin, $_thread
    bt 8
  end
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== renderpath watch armed (full chain), running ===\n"
run
printf "\n=== exited: fill(8242BF10)=%d  submit(821F8E60)=%d  spin(821C6F50)=%d ===\n", $fill, $submit, $spin
