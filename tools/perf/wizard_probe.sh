#!/usr/bin/env bash
# Probe for "exotic lever" feasibility: byte-swap strategy (movbe vs bswap),
# machine-outliner usage, current -O flags, idiom duplication in .text.
cd /home/h/src/recomp/rexglue-recomps || exit 1
SDK=third_party/rexglue-sdk
GEN=south-park-recomp/generated/default
BIN=south-park-recomp/out/build/linux-amd64-release/south_park_td

echo "=================== 1. BYTE-SWAP STRATEGY (the #1 idiom) ==================="
echo "--- REX_LOAD / REX_STORE macro definitions (how every guest mem access swaps) ---"
grep -rn "define REX_LOAD_U32\|define REX_STORE_U32\|define REX_LOAD_U64\|define REX_LOAD_U16" $SDK/include 2>/dev/null | head
echo "--- do the macros use __builtin_bswap / byteswap / std::byteswap? ---"
grep -rn "REX_LOAD_U32\|REX_STORE_U32" $SDK/include 2>/dev/null | grep -iE "bswap|byteswap|swap" | head
echo "--- actual movbe vs bswap instruction counts in the shipped binary ---"
if [ -f "$BIN" ]; then
  objdump -d --no-show-raw-insn "$BIN" 2>/dev/null > /tmp/disas.txt
  echo "  .text disas lines: $(wc -l < /tmp/disas.txt)"
  echo "  movbe count: $(grep -cwE 'movbe' /tmp/disas.txt)"
  echo "  bswap count: $(grep -cwE 'bswap' /tmp/disas.txt)"
  echo "  mov (all):   $(grep -cwE 'mov' /tmp/disas.txt)"
  echo "  call count:  $(grep -cwE 'call' /tmp/disas.txt)"
else
  echo "  BIN not found at $BIN"
fi

echo "=================== 2. CURRENT OPTIMIZATION FLAGS ==================="
echo "--- port CMakePresets opt flags ---"
grep -niE "O2|O3|Os|Oz|outline|FLAGS|RELEASE" south-park-recomp/CMakePresets.json 2>/dev/null | head -20
echo "--- machine outliner anywhere set? ---"
grep -rniE "outline|enable-machine-outliner|moutline" south-park-recomp/CMakeLists.txt south-park-recomp/CMakePresets.json $SDK/CMakeLists.txt 2>/dev/null | head
echo "--- what -O level did the actual build use (from CMakeCache)? ---"
grep -iE "CMAKE_CXX_FLAGS_RELEASE|CMAKE_BUILD_TYPE|CMAKE_CXX_FLAGS:" south-park-recomp/out/build/linux-amd64-release/CMakeCache.txt 2>/dev/null | head

echo "=================== 3. IDIOM DUPLICATION (outliner potential) ==================="
echo "--- how the codegen emits a 32-bit load (template) ---"
grep -rn "REX_LOAD_U32\|loadU32\|emitLoad\|EmitLoad" $SDK/src/codegen 2>/dev/null | head
echo "--- fpscr.disableFlushMode frequency (a repeated boilerplate line) in generated ---"
echo "  disableFlushMode occurrences: $(grep -roh 'disableFlushMode' $GEN 2>/dev/null | wc -l)"
echo "  REX_STORE occurrences:        $(grep -roh 'REX_STORE' $GEN 2>/dev/null | wc -l)"
echo "  REX_LOAD occurrences:         $(grep -roh 'REX_LOAD' $GEN 2>/dev/null | wc -l)"
echo "  total generated .cpp lines:   $(cat $GEN/*.cpp 2>/dev/null | wc -l)"
echo "  number of recompiled funcs (PPC_FUNC):  $(grep -rh 'PPC_FUNC' $GEN/*.cpp 2>/dev/null | grep -c 'sub_')"

echo "=================== 4. CPU exotic-knob availability ==================="
echo "--- MOVBE / CAT(RDT) / prefetcher MSR support ---"
grep -oE "movbe|rdt_a|cat_l3|cqm" /proc/cpuinfo | sort -u | tr '\n' ' '; echo
echo "--- resctrl (Intel CAT) mounted/available? ---"
ls /sys/fs/resctrl 2>/dev/null && echo "  resctrl AVAILABLE" || echo "  resctrl not mounted"
echo "--- msr module (for prefetcher 0x1A4 tweaks) ---"
ls /dev/cpu/0/msr 2>/dev/null && echo "  /dev/cpu/0/msr present" || echo "  msr dev absent (modprobe msr)"
echo "WIZARD_PROBE_DONE"
