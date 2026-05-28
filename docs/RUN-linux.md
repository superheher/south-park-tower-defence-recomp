# RUN-linux.md — building and running on Linux (Vulkan)

The port was originally brought up on Windows + D3D12. It now also builds and runs
**playable** on Linux via the SDK's Vulkan backend (verified on Fedora 43, Clang 21,
AMD Radeon / RADV: boot → menus → match → win, driven autonomously).

## Quick start (one command)

After installing the prerequisites below and cloning with submodules, the whole
build is one script (bring your own dump):

```bash
git clone --recursive <repo-url> && cd rexglue-recomps
south-park-recomp/tools/build-linux.sh "<dump>/58410931/000D0000/<hash>"
```

It applies the SDK patch, builds+installs the SDK, extracts the dump, runs codegen
+ fixup, builds the app, and stages the runtime libs. Re-runnable. The manual
steps below explain what it does. (`generated/rexglue.cmake` is committed, so no
`rexglue init` is needed.)

## Prerequisites (Fedora; adjust for your distro)

```bash
sudo dnf install -y clang lld cmake ninja-build autoconf unzip \
  gtk3-devel libX11-devel libxcb-devel vulkan-loader-devel vulkan-headers \
  alsa-lib-devel pulseaudio-libs-devel pipewire-devel libstdc++-devel
# harness extras: xdotool + ImageMagick (`import`)
sudo dnf install -y xdotool ImageMagick
```

Clang ≥ 18 is required (the SDK rejects GCC). The port's Linux preset names
`clang-20`; if your distro ships unversioned `clang`, symlink it:
`ln -s "$(command -v clang)" ~/bin/clang-20 && ln -s "$(command -v clang++)" ~/bin/clang++-20`.

## Build

```bash
# from the super-repo root
git submodule update --init --recursive

# apply the local SDK bring-up patch (Windows + Linux fixes)
git -C third_party/rexglue-sdk apply ../../south-park-recomp/patches/rexglue-sdk-current-full.patch

# build + install the SDK (Vulkan backend auto-selected on Linux)
cmake --preset linux-amd64 -S third_party/rexglue-sdk
cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --config Release --target install --parallel
SDK_INSTALL=$PWD/third_party/rexglue-sdk/out/install/linux-amd64

# extract the XEX + assets from your own dump (git-ignored)
python3 south-park-recomp/tools/stfs_extract.py -o south-park-recomp/private/extracted \
  "<dump>/58410931/000D0000/<hash>"

# codegen + post-codegen fixup
( cd south-park-recomp && "$SDK_INSTALL/bin/rexglue" codegen south_park_td_manifest.toml \
  && python3 tools/fix_recomp_labels.py generated/default )

# NOTE: generated/rexglue.cmake (SDK find + codegen target + rexglue_setup_target)
# is committed (the rest of generated/ is git-ignored), so no `rexglue init` step.

# configure + build the app
cmake --preset linux-amd64-release -S south-park-recomp -DCMAKE_PREFIX_PATH="$SDK_INSTALL"
cmake --build south-park-recomp/out/build/linux-amd64-release --parallel
```

Output: `south-park-recomp/out/build/linux-amd64-release/south_park_td` (links
`librexruntime.so`; copy it next to the exe or set `LD_LIBRARY_PATH`).

## Run

```bash
cd south-park-recomp/out/build/linux-amd64-release
cp "$SDK_INSTALL"/lib64/librexruntime.so "$SDK_INSTALL"/lib64/libTracyClient.so .
ROOT=../../..   # south-park-recomp
SDL_VIDEODRIVER=x11 LD_LIBRARY_PATH=. REX_INPUT_FILE=$PWD/live_input.txt \
  ./south_park_td \
    --game_data_root="$ROOT/private/extracted" \
    --user_data_root="$ROOT/private/userdata" \
    --license_mask=1 --mnk_mode=true \
    --window_width=960 --window_height=540 \
    --log_file=run.log --log_level=info
```

- `--license_mask=1` runs the full (non-trial) version.
- `--always_win=true` makes the base invincible (handy for automated runs).
- `SDL_VIDEODRIVER=x11` makes the window an XWayland window so the harness can screenshot it.
- First boot is slow (~1-3 min) translating shaders; subsequent boots ~45 s.

## Autonomous testing (`tools/gamectl.sh`)

Drives the game with no window focus needed. Set `REX_GAME_DIR` to the build dir
if it differs from the default.

```bash
tools/gamectl.sh shot title          # capture -> /tmp/sp/title.png
tools/gamectl.sh press 0010          # tap START
tools/gamectl.sh press 1000          # tap A
tools/gamectl.sh move -30000 0 1.5   # hold left stick left 1.5 s (move the player char)
```

Title → match nav: `press 0010` (start), `press 1000` (LOCAL GAME), `press 0010`
(lobby start), `press 1000` (CAMPAIGN), `press 0001` (up→CASUAL), `press 1000`,
`press 1000` (first level). With `--always_win` you then just wait out the waves
and `press 1000` (CONTINUE) between levels.

## Linux-specific notes

- The decisive runtime fix: POSIX `SEH_CATCH_ALL` was `catch(...)`, which swallowed
  glibc's `pthread_exit` forced-unwind (a guest thread exiting) and aborted with
  "exception not rethrown". It now catches only `rex::SehException`, matching the
  selective Windows `__except(seh_filter)`. See `patches/README.md`.
- `REX_INPUT_FILE` accepts an optional analog suffix: `"<btnhex> <lx> <ly>"`.
- Benign warnings: `Stans_House.lua` / `*\en-en\*` not-found (optional script +
  locale subdir; both have fallbacks) and intro `.wmv` movies (no WMV decoder).
