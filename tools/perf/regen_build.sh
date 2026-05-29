#!/usr/bin/env bash
# regen_build.sh [full|port] -- rebuild the recomp after a codegen/header change.
#   full : rebuild rexglue tool -> codegen (regen) -> fixup -> configure+build port
#          (use after an EMITTER change in src/codegen/* or a config/manifest change)
#   port : just rebuild the port (use after a HEADER-only change, e.g. context.h macros)
# Prints .text size + md5 of the resulting south_park_td. Exits non-zero on any failure.
# Does NOT stage over a running game -- caller must ensure no instance is live (build only).
set -euo pipefail
MODE="${1:-full}"
ROOT="${REX_GAME_ROOT:-/home/h/src/recomp/rexglue-recomps/south-park-recomp}"
SDK="$(cd "$ROOT/.." && pwd)/third_party/rexglue-sdk"
SDK_BUILD="$SDK/out/build/linux-amd64"
SDK_INSTALL="$SDK/out/install/linux-amd64"
GEN="$ROOT/generated/default"
PORT_BUILD="$ROOT/out/build/linux-amd64-release"
J="$(nproc)"

# the port preset names clang-20; alias to installed clang if absent (temp dir, no repo churn)
if ! command -v clang-20 >/dev/null 2>&1; then
  TB="$(mktemp -d "${TMPDIR:-/tmp}/rexglue-toolbin.XXXXXX")"
  ln -sf "$(command -v clang)"   "$TB/clang-20"
  ln -sf "$(command -v clang++)" "$TB/clang++-20"
  export PATH="$TB:$PATH"
fi

t0=$(date +%s)
if [ "$MODE" = "full" ]; then
  echo "== [build] 1/4 rebuild + install rexglue tool (incremental) =="
  cmake --build "$SDK_BUILD" --config Release --target install --parallel "$J"
  echo "== [build] 2/4 codegen (regen generated/default) =="
  ( cd "$ROOT" && "$SDK_INSTALL/bin/rexglue" codegen south_park_td_manifest.toml )
  echo "== [build] 3/4 fix_recomp_labels =="
  python3 "$ROOT/tools/fix_recomp_labels.py" "$GEN"
  echo "== [build] 4/4 configure + build port =="
  ( cd "$ROOT" && cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH="$SDK_INSTALL" >/dev/null )
  cmake --build "$PORT_BUILD" --parallel "$J"
  # The exe loads librexruntime.so via RUNPATH=$ORIGIN (the port build dir), but the port build has
  # NO rule to refresh that copy from the freshly-installed .so -- so an SDK/.so change (e.g. the
  # -mcmodel flip) would silently NOT reach the game. Copy the install .so (correct RUNPATH/SONAME)
  # into the port dir here. Caller guarantees no live game (build-only), so the mmap'd .so is free.
  INSTALL_SO=""
  for d in lib64 lib; do
    if [ -f "$SDK_INSTALL/$d/librexruntime.so" ]; then INSTALL_SO="$SDK_INSTALL/$d/librexruntime.so"; break; fi
  done
  if [ -n "$INSTALL_SO" ]; then
    cp -f "$INSTALL_SO" "$PORT_BUILD/librexruntime.so"
    echo "  [build] refreshed port librexruntime.so from $INSTALL_SO"
  fi
else
  echo "== [build] port-only rebuild =="
  cmake --build "$PORT_BUILD" --parallel "$J"
fi
t1=$(date +%s)

echo "== [build] DONE in $((t1-t0))s =="
size "$PORT_BUILD/south_park_td" | sed 's/^/  /'
echo "  md5 south_park_td: $(md5sum "$PORT_BUILD/south_park_td" | cut -c1-12)"
echo "  md5 librexruntime.so: $(md5sum "$PORT_BUILD/librexruntime.so" | cut -c1-12)"
