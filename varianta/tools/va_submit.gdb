# cont.76 prod-oracle counterpart: walk the FULL prod-discovered per-frame submit chain on VARIANT A.
# prod chain (measured): xstart -> sub_82249638 -> sub_82249678 -> sub_8214FFD0 -> sub_8212DBA0
#   -> sub_821C7F08 (per-frame GPU submit) -> { sub_821C6D58->sub_821C6C80->sub_821C6600 ring-kick ;
#                                               sub_821C73D8 completion-setup writes device+10900 }
# Count each node on variant A's stable-base boot (attract). The node where the count drops to 0 is the
# divergence: that is where variant A's per-frame submit fails to run (the A<->B "submit never runs", k.cpp:1361).
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_va_submit.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out/sp_td_varianta
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out
set environment REX_NOTOKEN=1
set environment REX_CSLEAK=1
set environment REX_CPCOMPLETE=1
set environment REX_MOVIE_EOF=30
set args /home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted/default.xex

set $n4ffd0 = 0
set $n2dba0 = 0
set $n7f08 = 0
set $n6d58 = 0
set $n6600 = 0
set $n73d8 = 0
set $n450fd0 = 0
set $ncc5d0 = 0
set $ncbdc8 = 0

# --- steady-state per-frame submit thread (prod: sub_82450FD0 -> sub_821CC5D0 -> sub_821CC310 -> sub_821CBDC8 -> kick) ---
break sub_82450FD0
commands
  silent
  set $n450fd0 = $n450fd0 + 1
  if $n450fd0 == 1
    printf "\n[VA RENDER-THREAD ENTRY] sub_82450FD0 HIT (first):\n"
    bt 5
  end
  continue
end
break sub_821CC5D0
commands
  silent
  set $ncc5d0 = $ncc5d0 + 1
  if $ncc5d0 == 1
    printf "\n[VA PER-FRAME SUBMIT LOOP] sub_821CC5D0 HIT (first):\n"
    bt 6
  end
  if $ncc5d0 % 120 == 0
    printf "[va-submitloop count=%d]\n", $ncc5d0
  end
  continue
end
break sub_821CBDC8
commands
  silent
  set $ncbdc8 = $ncbdc8 + 1
  continue
end

# sub_8214FFD0 = main-thread frontend tick (prod: leads to the GPU-submit branch). Known reached (diverge S3).
break sub_8214FFD0
commands
  silent
  set $n4ffd0 = $n4ffd0 + 1
  if $n4ffd0 == 1
    printf "\n[VA S3] sub_8214FFD0 HIT (first):\n"
    bt 6
  end
  continue
end
# sub_8212DBA0 = the GPU-submit branch prod takes from sub_8214FFD0. DOES variant A take it?
break sub_8212DBA0
commands
  silent
  set $n2dba0 = $n2dba0 + 1
  if $n2dba0 == 1
    printf "\n[VA GPU-SUBMIT BRANCH] sub_8212DBA0 HIT (first):\n"
    bt 8
  end
  continue
end
# sub_821C7F08 = the per-frame GPU submit (calls ring-kick + completion-setup).
break sub_821C7F08
commands
  silent
  set $n7f08 = $n7f08 + 1
  if $n7f08 == 1
    printf "\n[VA PER-FRAME SUBMIT] sub_821C7F08 HIT (first):\n"
    bt 10
  end
  continue
end
break sub_821C6D58
commands
  silent
  set $n6d58 = $n6d58 + 1
  continue
end
break sub_821C6600
commands
  silent
  set $n6600 = $n6600 + 1
  if $n6600 == 1
    printf "\n[VA RING-KICK] sub_821C6600 HIT (first):\n"
    bt 10
  end
  if $n6600 % 120 == 0
    printf "[va-kick count=%d]\n", $n6600
  end
  continue
end
break sub_821C73D8
commands
  silent
  set $n73d8 = $n73d8 + 1
  if $n73d8 == 1
    printf "\n[VA COMPLETION-SETUP] sub_821C73D8 HIT (first):\n"
    bt 10
  end
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== variant A submit-chain watch armed, running boot ===\n"
run
printf "\n=== variant A exited ===\n"
printf "INIT CHAIN:  sub_8214FFD0=%d  sub_8212DBA0=%d  sub_821C7F08=%d  sub_821C6D58=%d  sub_821C73D8(comp)=%d\n", $n4ffd0, $n2dba0, $n7f08, $n6d58, $n73d8
printf "STEADY CHAIN: sub_82450FD0(rthread)=%d  sub_821CC5D0(submitloop)=%d  sub_821CBDC8=%d  sub_821C6600(KICK)=%d\n", $n450fd0, $ncc5d0, $ncbdc8, $n6600
