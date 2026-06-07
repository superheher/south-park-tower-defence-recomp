# cont.100 prod-oracle: prod renders the menu so its batch FLUSH fires + emits a real DRAW_INDX. The flush is
# the SAME guest fn in prod and variant A (recompiled from the same code). Break the render chain + the cont.99
# orchestrator's flush candidates BY NAME (prod has full symbols), one-shot/self-disabling (no throttle), deep bt.
set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_prod_flush.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release/south_park_td
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment LD_LIBRARY_PATH=/home/h/src/recomp/rexglue-recomps/south-park-recomp/out/build/linux-amd64-release
set environment DISPLAY=:0
set environment SDL_VIDEODRIVER=x11
set args --game_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted --user_data_root=/home/h/src/recomp/rexglue-recomps/south-park-recomp/private/userdata --license_mask=1 --mnk_mode=true --always_win=true --window_width=960 --window_height=540 --log_file=run_gdb.log --log_level=info

# (a) the per-primitive fill — confirm prod's render chain == variant A's. one-shot deep bt.
break sub_821F8E60
commands
  silent
  printf "\n===== PROD sub_821F8E60 (per-prim fill) — DEEP BT =====\n"
  bt 24
  disable 1
  continue
end

# (b) the cont.99 flush candidates from the orchestrator sub_8222D808. one-shot bt each.
break sub_821FB340
commands
  silent
  printf "\n===== PROD sub_821FB340 (flush cand #1, after fill) r3=0x%08X =====\n", $rsi
  bt 12
  disable 2
  continue
end
break sub_8221FA08
commands
  silent
  printf "\n===== PROD sub_8221FA08 (flush cand #2) =====\n"
  bt 12
  disable 3
  continue
end
break sub_822045E0
commands
  silent
  printf "\n===== PROD sub_822045E0 (flush cand #3) =====\n"
  bt 12
  disable 4
  continue
end

handle SIGSEGV nostop noprint pass
printf "\n=== prod_flush armed (one-shot x4), running ===\n"
run
printf "\n=== prod_flush done ===\n"
