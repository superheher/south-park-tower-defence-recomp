# cont.101 (prod half): capture prod's sub_822045E0 destination r3 (guest reg at *(uint32*)rdi, BE->bswap) at
# ENTRY (r3 not modified entry->store). Compare prod's r3 to variant A's r3: same addr => the draw fetches the
# wrong slot; different => a variant-A address divergence. one-shot-ish (disable after a few hits).
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_prod_vertdest.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/south_park_td
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment LD_LIBRARY_PATH=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment DISPLAY=:0
set environment SDL_VIDEODRIVER=x11
set args --game_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted --user_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/userdata --license_mask=1 --mnk_mode=true --always_win=true --window_width=960 --window_height=540 --log_file=run_gdb.log --log_level=info

set $n = 0
break sub_822045E0
commands
  silent
  set $n = $n + 1
  set $r3be = *(unsigned int*)$rdi
  set $r3 = (($r3be & 0xff) << 24) | (($r3be & 0xff00) << 8) | (($r3be >> 8) & 0xff00) | (($r3be >> 24) & 0xff)
  printf "\n[PROD vertdest #%d] r3(dest VB)=0x%08X\n", $n, $r3
  if $n >= 6
    disable 1
  end
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== prod_vertdest armed, running ===\n"
run
printf "\n=== prod_vertdest done (hits=%d) ===\n", $n
