set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_vawatch.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out/sp_td_varianta
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out
set environment REX_NOTOKEN=1
set environment REX_CSLEAK=1
set environment REX_CPCOMPLETE=1
set environment REX_MOVIE_EOF=30
set environment REX_SKIPINTRO=1
set environment REX_HANDLERGUARD=1
set args /home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted/default.xex

break __imp___xstart
run

set $b = (unsigned long long)base
printf "\n=== variant A base = 0x%llx ===\n", $b
printf "lookup name @guest 0x820687F8: '%s'\n", (char*)($b + 0x820687F8)
printf "handler 0x828183A0 init = 0x%08x [LE-raw]\n", *(unsigned int*)($b + 0x828183A0)
printf "subsys  0x827FD56C init = 0x%08x [LE-raw]\n", *(unsigned int*)($b + 0x827FD56C)

watch *(unsigned int*)($b + 0x828183A0)
commands
  silent
  printf "\n@@@@@@@@@@ WROTE 0x828183A0 menu_handler -> 0x%08x [LE-raw] @@@@@@@@@@\n", *(unsigned int*)($b + 0x828183A0)
  bt 8
  printf "@@@@@@@@@@ end @@@@@@@@@@\n"
  continue
end

watch *(unsigned int*)($b + 0x827FD56C)
commands
  silent
  printf "\n%%%%%%%%%% WROTE 0x827FD56C subsys_ptr -> 0x%08x [LE-raw] %%%%%%%%%%\n", *(unsigned int*)($b + 0x827FD56C)
  bt 8
  printf "%%%%%%%%%% end %%%%%%%%%%\n"
  continue
end

delete 1
printf "\n=== watchpoints armed (base-relative), continuing ===\n"
continue
