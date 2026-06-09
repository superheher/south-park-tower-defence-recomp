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
    --license_mask=1 --mnk_mode=true --freq_keeper=true \
    --window_width=960 --window_height=540 \
    --log_file=run.log --log_level=info
```

- `--license_mask=1` runs the full (non-trial) version.
- `--freq_keeper=true` holds the CPU package frequency up (1 low-priority
  spinner core). Needed on powersave/EPP=power hosts or gameplay enters
  2-3-vblank cadence locks from a cold (idle-menu) entry — see STATUS.md
  "Layer A". Root alternative (better, makes the flag unnecessary):
  `echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference`.
- `--always_win=true` makes the base invincible (handy for automated runs).
- `SDL_VIDEODRIVER=x11` makes the window an XWayland window so the harness can screenshot it.
- First boot is slow (~1-3 min) translating shaders; subsequent boots ~45 s.

## Autonomous testing (`tools/gamectl.sh`)

Drives the game with no window focus needed (writes the `REX_INPUT_FILE`).

### One command: boot straight into live gameplay

```bash
tools/gamectl.sh play        # kill stragglers, boot, drive into the first level
                             # (Stan's House), un-paused, ~60-70 s, hands-off
tools/gamectl.sh bench 20    # then profile: min/avg/max swaps/s from [pacing-diag]
```

`play` is the fast path for benchmarking/profiling: run it, walk away, ~1 min later
you are in live tower-defense gameplay. It handles the rough edges automatically
(see `knowledge-base/titles/south-park-lgtdp/68-autonomous-boot-to-gameplay.md`):

- **Boot intermittently hangs** before the first present (frame-1 vsync/fence deadlock,
  KB doc 60). `play` detects it (no `[pacing-diag]` within 15 s) and relaunches.
- **The title honours only START, and only on a *fresh* process.** A stale instance that
  has idled ~28 s loops its attract demo and goes input-dead, so `play` always starts fresh.
  Only **one** instance may run (all instances share `live_input.txt`).
- After START an **unskippable ~30 s intro** plays before the menu (no skip cvar exists).
- **Nav recipe:** alternate A/START until the CAMPAIGN LEVEL SELECT renders (its
  `camp_diagram` asset hits the log — a reliable checkpoint), then press **only A** to
  select Stan's House, confirm, start, and skip the level-intro dialogue into live play.
- `[pacing-diag] loading=true` is **not** a reliable level-entry signal — it only flips on a
  shader *compile*, which is cached away on a warm machine, so the first level loads with
  `loading=false`. The level loads silently; confirm via the on-screen HUD or the verify shot.
- In-gameplay framerate **oscillates ~25–60 swaps/s** under load — that is what `bench` measures.

Warm boot to the title is ~4 s (shaders cached); the first cold boot is ~1–3 min.

### Low-level building blocks

```bash
tools/gamectl.sh shot title          # capture -> /tmp/sp/title.png
tools/gamectl.sh press 0010          # tap START (1000=A, 2000=B, 0001=DPAD up ...)
tools/gamectl.sh move -30000 0 1.5   # hold left stick left 1.5 s (move the player char)
tools/gamectl.sh kill | boot | fps   # kill all instances / boot+wait / latest swaps-per-sec
```

Manual title → match nav (if not using `play`): `press 0010` (start), `press 1000`
(LOCAL GAME), `press 0010` (lobby), `press 1000` (CAMPAIGN), `press 0001` (up→CASUAL),
`press 1000`, `press 1000` (first level) — pressed *promptly* on a fresh boot (before the
attract timer). With `--always_win` you then wait out the waves and `press 1000` (CONTINUE)
between levels.

## Linux-specific notes

- The decisive runtime fix: POSIX `SEH_CATCH_ALL` was `catch(...)`, which swallowed
  glibc's `pthread_exit` forced-unwind (a guest thread exiting) and aborted with
  "exception not rethrown". It now catches only `rex::SehException`, matching the
  selective Windows `__except(seh_filter)`. See `patches/README.md`.
- `REX_INPUT_FILE` accepts an optional analog suffix: `"<btnhex> <lx> <ly>"`.
- Benign warnings: `Stans_House.lua` / `*\en-en\*` not-found (optional script +
  locale subdir; both have fallbacks) and intro `.wmv` movies (no WMV decoder).
