#!/usr/bin/env bash
# One-shot Linux build for the South Park recomp (SDK Vulkan backend).
#
#   tools/build-linux.sh [STFS_PACKAGE]
#
# Runs the whole flow from a fresh clone: submodules -> apply SDK patch ->
# build+install SDK -> (extract dump) -> codegen -> post-codegen fixup ->
# configure+build app -> stage runtime libs. Re-runnable (idempotent-ish).
#
# STFS_PACKAGE: your own dump's 58410931/000D0000/<hash> file. Needed only if
# private/extracted/default.xex isn't already present (bring your own dump).
#
# Prereqs (install once; see docs/RUN-linux.md): clang>=18, lld, cmake, ninja,
# python3, and dev libs (Vulkan, GTK3, X11/xcb, ALSA/Pulse/PipeWire). No sudo here.
set -euo pipefail

SP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"     # south-park-recomp
SUPER="$(cd "$SP_DIR/.." && pwd)"                              # super-repo
SDK="$SUPER/third_party/rexglue-sdk"
PATCH="$SP_DIR/patches/rexglue-sdk-current-full.patch"
PLAT="linux-amd64"

say() { printf '\n\033[1;36m== %s ==\033[0m\n' "$*"; }

command -v clang >/dev/null || { echo "ERROR: clang not found (need >=18)"; exit 1; }
# The app preset names clang-20; alias to whatever clang is installed (in a temp
# dir so a fresh checkout stays clean).
if ! command -v clang-20 >/dev/null 2>&1; then
  TOOLBIN="$(mktemp -d "${TMPDIR:-/tmp}/rexglue-toolbin.XXXXXX")"
  ln -sf "$(command -v clang)"   "$TOOLBIN/clang-20"
  ln -sf "$(command -v clang++)" "$TOOLBIN/clang++-20"
  export PATH="$TOOLBIN:$PATH"
fi
say "toolchain"; clang --version | head -1; cmake --version | head -1

say "1/7 submodules"
git -C "$SUPER" submodule update --init --recursive

say "2/7 apply SDK patch (only if working tree is pristine)"
if [ -z "$(git -C "$SDK" status --porcelain)" ]; then
  git -C "$SDK" apply "$PATCH"; echo "applied $PATCH"
else
  echo "SDK working tree dirty -> assuming patch already applied (skip)"
fi

say "3/7 build + install SDK ($PLAT, Release)"
( cd "$SDK" && cmake --preset "$PLAT" )
cmake --build "$SDK/out/build/$PLAT" --config Release --target install --parallel
SDK_INSTALL="$SDK/out/install/$PLAT"

say "4/7 extract dump (if needed)"
if [ ! -f "$SP_DIR/private/extracted/default.xex" ]; then
  [ "$#" -ge 1 ] && [ -f "$1" ] || {
    echo "ERROR: private/extracted/default.xex missing and no STFS package arg." >&2
    echo "  usage: tools/build-linux.sh <dump>/58410931/000D0000/<hash>" >&2; exit 1; }
  python3 "$SP_DIR/tools/stfs_extract.py" -o "$SP_DIR/private/extracted" "$1"
fi

say "5/7 codegen + post-codegen fixup"
( cd "$SP_DIR" && "$SDK_INSTALL/bin/rexglue" codegen south_park_td_manifest.toml )
python3 "$SP_DIR/tools/fix_recomp_labels.py" "$SP_DIR/generated/default"

say "6/7 configure + build app"
( cd "$SP_DIR" && cmake --preset "$PLAT-release" -DCMAKE_PREFIX_PATH="$SDK_INSTALL" )
cmake --build "$SP_DIR/out/build/$PLAT-release" --parallel

say "7/7 stage runtime libs next to the exe"
OUT="$SP_DIR/out/build/$PLAT-release"
cp -f "$SDK_INSTALL"/lib64/librexruntime.so "$SDK_INSTALL"/lib64/libTracyClient.so "$OUT"/
printf '0 0 0' > "$OUT/live_input.txt"

say "DONE"
echo "Binary: $OUT/south_park_td"
echo "Run (see docs/RUN-linux.md):"
echo "  cd $OUT && SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. REX_INPUT_FILE=\$PWD/live_input.txt \\"
echo "    ./south_park_td --game_data_root=$SP_DIR/private/extracted \\"
echo "      --user_data_root=$SP_DIR/private/userdata --license_mask=1 --mnk_mode=true"
