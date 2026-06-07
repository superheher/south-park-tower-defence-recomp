# cont.76 prod-oracle: per-frame SUBMIT path reachability + call-chain.
# Breaks the ring-kick (sub_821C6600) and completion-setup (sub_821C73D8, writes device+10900)
# in the WORKING prod build. Captures the call chain at first hit and a steady-state hit, plus a
# rough per-frame frequency. Compare against va_submit.gdb (variant A) to locate the A<->B divergence.
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_prod_submit.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/south_park_td
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment LD_LIBRARY_PATH=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment DISPLAY=:0
set environment SDL_VIDEODRIVER=x11
set args --game_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted --user_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/userdata --license_mask=1 --mnk_mode=true --always_win=true --window_width=960 --window_height=540 --log_file=run_gdb.log --log_level=info

set $kick = 0
set $comp = 0

break sub_821C6600
commands
  silent
  set $kick = $kick + 1
  if $kick == 1
    printf "\n[KICK #1] sub_821C6600 ring-kick (first/init):\n"
    bt 16
    printf "[end KICK #1]\n"
  end
  if $kick == 60
    printf "\n[KICK #60] sub_821C6600 ring-kick (steady-state attract):\n"
    bt 16
    printf "[end KICK #60]\n"
  end
  if $kick % 60 == 0
    printf "[kick count=%d]\n", $kick
  end
  continue
end

break sub_821C73D8
commands
  silent
  set $comp = $comp + 1
  if $comp == 1
    printf "\n[COMP #1] sub_821C73D8 completion-setup writes device+10900 (first):\n"
    bt 16
    printf "[end COMP #1]\n"
  end
  if $comp == 60
    printf "\n[COMP #60] sub_821C73D8 completion-setup (steady-state):\n"
    bt 16
    printf "[end COMP #60]\n"
  end
  if $comp % 60 == 0
    printf "[comp count=%d]\n", $comp
  end
  continue
end

# prod traps GPU-register MMIO via mprotect+SIGSEGV (GraphicsSystem::ReadRegister); pass it to prod's
# own handler so prod runs to steady-state attract instead of gdb halting on the first MMIO access.
handle SIGSEGV nostop noprint pass
printf "\n=== prod submit-path watch armed (kick=sub_821C6600 comp=sub_821C73D8), running ===\n"
run
printf "\n=== prod exited: kick=%d comp=%d ===\n", $kick, $comp
