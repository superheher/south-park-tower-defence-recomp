set pagination off
set confirm off
set width 0
set print address on
set logging file /tmp/gdb_vatest.txt
set logging overwrite on
set logging enabled on
file /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out/sp_td_varianta
set cwd /home/h/src/recomp/rexglue-recomps/south-park-recomp/varianta/runtime/out
set environment REX_NOTOKEN=1
set environment REX_CSLEAK=1
set environment REX_CPCOMPLETE=1
set environment REX_MOVIE_EOF=30
set environment REX_SKIPINTRO=1
set args /home/h/src/recomp/rexglue-recomps/south-park-recomp/private/extracted/default.xex

# Confirm WHICH XexLoadImage body runs (kernel.cpp = my fix, import_stubs.gen.cpp = old weak stub)
break __imp__XexLoadImage
commands
  silent
  printf "\n>>>> HIT __imp__XexLoadImage <<<<\n"
  frame 0
  printf ">>>> (above frame shows the source file: kernel.cpp = FIX active) <<<<\n"
  continue
end

break __imp___xstart
run
set $b = (unsigned long long)base
printf "\n=== base=0x%llx, watching 0x828183A0 for writes ===\n", $b
watch *(unsigned int*)($b + 0x828183A0)
commands
  silent
  printf "\n@@@@ WROTE 0x828183A0 -> 0x%08x [LE-raw] @@@@\n", *(unsigned int*)($b + 0x828183A0)
  bt 4
  continue
end
delete 2
continue
