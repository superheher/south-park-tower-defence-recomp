#!/usr/bin/env bash
# wizard_build.sh -- cheap .text projection for the untried COMPILE-LEVEL levers,
# stacked on the current generated/default (already Phase C, -16.06%):
#   (A) -march=native  -> lets clang fuse 264k load+bswap pairs into movbe + denser ISA
#   (B) LLVM Machine Outliner (always) -> dedupes repeated idiom sequences across funcs
# Port-only rebuild into a SEPARATE build dir (no codegen regen needed -- generated
# sources are unchanged; we only change the compile flags). Reports .text vs Phase C.
# NO game run here -- this is the go/no-go projection. If .text drops a lot, THEN gate+floor.
set -u
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
SDK_INSTALL=/home/h/src/recomp/rexglue-recomps/third_party/rexglue-sdk/out/install/linux-amd64
OUT="${OUT:-/tmp/wizard_build.txt}"; exec >"$OUT" 2>&1
cd "$ROOT" || exit 1
J="$(nproc)"

# clang-20 alias (preset names clang-20; Fedora has clang 21)
if ! command -v clang-20 >/dev/null 2>&1; then
  TB="$(mktemp -d "${TMPDIR:-/tmp}/rexglue-toolbin.XXXXXX")"
  ln -sf "$(command -v clang)"   "$TB/clang-20"
  ln -sf "$(command -v clang++)" "$TB/clang++-20"
  export PATH="$TB:$PATH"
fi

PHASEC_TEXT=$(size out/build/linux-amd64-release/south_park_td 2>/dev/null | awk 'NR==2{print $1}')
echo "Phase C .text (baseline-for-this-test) = ${PHASEC_TEXT:-unknown}"
echo "clang: $(clang --version | head -1)"

run_variant() {
  local name="$1" flags="$2"
  local bd="out/build/linux-amd64-$name"
  echo "===================================================================="
  echo "== VARIANT: $name   flags: $flags"
  echo "===================================================================="
  rm -rf "$bd"
  local t0=$(date +%s)
  if ! cmake -S . -B "$bd" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_PREFIX_PATH="$SDK_INSTALL" \
        -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG $flags" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG $flags" >/tmp/wiz_cfg_$name.log 2>&1; then
    echo "  CONFIGURE FAILED -- tail:"; tail -15 /tmp/wiz_cfg_$name.log; return 1
  fi
  if ! cmake --build "$bd" --parallel "$J" >/tmp/wiz_bld_$name.log 2>&1; then
    echo "  BUILD FAILED -- tail:"; tail -25 /tmp/wiz_bld_$name.log; return 1
  fi
  local t1=$(date +%s)
  local TX=$(size "$bd/south_park_td" 2>/dev/null | awk 'NR==2{print $1}')
  local MD=$(md5sum "$bd/south_park_td" 2>/dev/null | cut -c1-12)
  local MOVBE BSWAP
  objdump -d --no-show-raw-insn "$bd/south_park_td" 2>/dev/null > /tmp/wiz_dis_$name.txt
  MOVBE=$(grep -cwE 'movbe' /tmp/wiz_dis_$name.txt)
  BSWAP=$(grep -cwE 'bswap' /tmp/wiz_dis_$name.txt)
  echo "  BUILD OK in $((t1-t0))s  md5=$MD"
  echo "  .text = $TX   (Phase C $PHASEC_TEXT)"
  if [ -n "$TX" ] && [ -n "$PHASEC_TEXT" ]; then
    awk -v a="$PHASEC_TEXT" -v b="$TX" 'BEGIN{printf "  delta vs Phase C = %+d B (%+.2f%%);  vs 12MB L3 = %.2fx\n", b-a, (b-a)*100.0/a, b/12582912.0}'
  fi
  echo "  movbe=$MOVBE  bswap=$BSWAP"
}

# A: just -march=native (movbe fusion + denser ISA)
run_variant "wiz-native"   "-march=native"
# B: -march=native + machine outliner (the dedup pass)
run_variant "wiz-outline"  "-march=native -mllvm -enable-machine-outliner=always -mllvm -outliner-leaf-descendants"
echo "WIZARD_BUILD_DONE"
