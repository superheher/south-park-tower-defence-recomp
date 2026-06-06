set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_oracle.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/south_park_td
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment LD_LIBRARY_PATH=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment DISPLAY=:0
set environment SDL_VIDEODRIVER=x11
set args --game_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted --user_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/userdata --license_mask=1 --mnk_mode=true --always_win=true --window_width=960 --window_height=540 --log_file=run_gdb.log --log_level=info

break rex::ReXApp::OnPreLaunchModule
run

printf "\n=== AT OnPreLaunchModule (guest module mapped, guest thread not yet run) ===\n"
printf "handler 0x828183A0 (host 0x1828183A0) = 0x%08x [LE-raw]\n", *(unsigned int*)0x1828183A0
printf "subsys  0x827FD56C (host 0x1827FD56C) = 0x%08x [LE-raw]\n", *(unsigned int*)0x1827FD56C
printf "hdlr2   0x827FD568 (host 0x1827FD568) = 0x%08x [LE-raw]\n", *(unsigned int*)0x1827FD568

watch *(unsigned int*)0x1828183A0
commands
  silent
  printf "\n@@@@@@@@@@ WROTE 0x828183A0 menu_handler -> 0x%08x [LE-raw] @@@@@@@@@@\n", *(unsigned int*)0x1828183A0
  bt 16
  printf "@@@@@@@@@@ end 0x828183A0 @@@@@@@@@@\n"
  continue
end

watch *(unsigned int*)0x1827FD56C
commands
  silent
  printf "\n%%%%%%%%%% WROTE 0x827FD56C subsys_ptr -> 0x%08x [LE-raw] %%%%%%%%%%\n", *(unsigned int*)0x1827FD56C
  bt 16
  printf "%%%%%%%%%% end 0x827FD56C %%%%%%%%%%\n"
  continue
end

printf "\n=== watchpoints armed, continuing ===\n"
continue
