# cont.77 prod-oracle: the producer/consumer render-work queue in the WORKING build.
# PRODUCER sub_821CC7A0 (enqueues a work item r3); CONSUMER sub_821CC310 (dequeues item=*(r3), calls *(item+16)).
# Goal: (a) HOW is the producer invoked? (bt -> is it the gfx-interrupt sub_821C7170 / *(B+0x10) callback?)
#       (b) the work-item vtable *(item) + draw-handler *(item+16) the consumer issues.
# Guest r3 = *(uint32_t*)&ctx (PPCContext r3 @ offset 0); guest mem host = 0x100000000 + guest (BE-stored -> bswap).
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_prod_pc.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/south_park_td
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment LD_LIBRARY_PATH=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment DISPLAY=:0
set environment SDL_VIDEODRIVER=x11
set args --game_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted --user_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/userdata --license_mask=1 --mnk_mode=true --always_win=true --window_width=960 --window_height=540 --log_file=run_gdb.log --log_level=info

set $prod = 0
set $cons = 0

# PRODUCER raw entry (rdi = &ctx)
break *0x6f2f20
commands
  silent
  set $prod = $prod + 1
  if $prod <= 4
    set $item = *(unsigned int*)$rdi
    printf "\n[PROD-PRODUCER #%d] sub_821CC7A0 r3(work-item)=0x%08X\n", $prod, $item
    if $item > 0x1000
      set $vr = *(unsigned int*)(0x100000000 + $item)
      set $vt = (($vr & 0xff) << 24) | (($vr & 0xff00) << 8) | (($vr >> 8) & 0xff00) | (($vr >> 24) & 0xff)
      set $hr = *(unsigned int*)(0x100000000 + $item + 16)
      set $h = (($hr & 0xff) << 24) | (($hr & 0xff00) << 8) | (($hr >> 8) & 0xff00) | (($hr >> 24) & 0xff)
      printf "    vtable*(item)=0x%08X  draw-handler*(item+16)=0x%08X\n", $vt, $h
    end
    bt 16
    printf "[end PROD-PRODUCER #%d]\n", $prod
  end
  continue
end

# CONSUMER raw entry (rdi = &ctx); item = *(r3), handler = *(item+16)
break *0x6f1ef0
commands
  silent
  set $cons = $cons + 1
  if $cons <= 4
    set $r3 = *(unsigned int*)$rdi
    printf "\n[PROD-CONSUMER #%d] sub_821CC310 r3(queue)=0x%08X\n", $cons, $r3
    if $r3 > 0x1000
      set $ir = *(unsigned int*)(0x100000000 + $r3)
      set $it = (($ir & 0xff) << 24) | (($ir & 0xff00) << 8) | (($ir >> 8) & 0xff00) | (($ir >> 24) & 0xff)
      printf "    item=*(r3)=0x%08X\n", $it
      if $it > 0x1000
        set $hr2 = *(unsigned int*)(0x100000000 + $it + 16)
        set $h2 = (($hr2 & 0xff) << 24) | (($hr2 & 0xff00) << 8) | (($hr2 >> 8) & 0xff00) | (($hr2 >> 24) & 0xff)
        printf "    draw-handler*(item+16)=0x%08X\n", $h2
      end
    end
    bt 12
    printf "[end PROD-CONSUMER #%d]\n", $cons
  end
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== prod producer/consumer oracle armed, running ===\n"
run
printf "\n=== prod exited: producer=%d consumer=%d ===\n", $prod, $cons
