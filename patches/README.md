# patches/ — local bring-up patches to third_party/rexglue-sdk

rexglue-sdk is a third-party submodule tracking upstream
(`github.com/rexglue/rexglue-sdk`). These are **small local fixes** found during
bring-up, kept as patch files (not pushed upstream, not committed into the
submodule) so the super-repo gitlink stays clean and the fixes are reproducible.

Apply after `git submodule update`, then rebuild + reinstall the SDK:

```powershell
git -C third_party/rexglue-sdk apply ../../south-park-recomp/patches/0001-rexglue-thread-r1-stack-headroom.patch
cmake --build --preset win-amd64-release --target install   # in third_party/rexglue-sdk
```

| Patch | Fixes |
|-------|-------|
| `0001-rexglue-thread-r1-stack-headroom.patch` | Initial guest `r1` was set to `stack_base`, which sits on the stack's `PAGE_NOACCESS` guard page. A no-prologue XEX entry thunk that reads its caller frame *above* `r1` (South Park's `xstart`: `addi r1,r1,112; lwz r12,-8(r1)`) faults. Reserve a small 16-byte-aligned initial frame below `stack_base`. |
| ~~`0002` (r3=-1)~~ **REVERTED** | Was a wrong hypothesis. Studying Xenia's `KernelState::LaunchModule` (`kernel_state.cc:276`) showed it launches the main thread with `start_context = 0` (guest `r3 = 0`) — **identical to rexglue's default**. rexglue's launch correctly mirrors Xenia (which runs 360 games); `r3 = -1` was an unjustified deviation, now reverted. (South Park's entry returns regardless of `r3`; it's a stub — see `30-boot-log.md`.) |
| `0003-rexglue-codegen-implement-lmw.patch` | Codegen had `build_stmw` (store-multiple, in prologues) but no `build_lmw` (load-multiple, in matching epilogues), so any function using the stmw/lmw non-volatile-GPR save-restore pair compiled its prologue but `throw`s `REX_UNIMPLEMENTED` in its epilogue. Adds `build_lmw` (load mirror of `build_stmw`) + dispatch entry. Verified: `Unimplemented: lmw` 119 → 0. |
| `0004-rexglue-entry-point-override-envvar.patch` | **Bring-up experiment.** South Park's XEX entry is a do-nothing stub (see `35-entry-forensics.md`); to empirically test starting at the real `mainCRTStartup`, this reads `REX_ENTRY_OVERRIDE=0x82xxxxxx` (env var) right after the loader caches `XEX_HEADER_ENTRY_POINT` and overrides `entry_point_`. No per-address rebuild needed. Used to brute-force candidate entries (`tools/find_zeroref_roots.py` + the brute-force scripts). General technique in `knowledge-base/general/80`. |
| `0009-rexglue-stfs-direct-mount.patch` | **Launcher M1.** Lets `--game_data_root` point at a single STFS package file (the user's own console dump) instead of a pre-extracted folder — no extraction step. `Runtime::SetupVfs` auto-detects file-vs-directory and mounts `StfsContainerDevice` for a file / `HostPathDevice` for a folder. `ReXApp::ConstructRuntime` accepts a file (relaxes `is_directory`) and skips the host `default.xex` pre-flight for packages (the VFS validates the in-package XEX at load). Generic + upstreamable; offline folder boot is byte-for-byte unchanged. Verified by running: boots the title from both an STFS package and an extracted folder. |

## Linux bring-up fixes (folded into the full patch)

The port was first brought up on Windows + D3D12; these additional fixes make the SDK
build and run on Linux via the Vulkan backend. All are small/additive and are folded
into `rexglue-sdk-current-full.patch`. Full build/run/automation guide:
`../docs/RUN-linux.md`.

| Area | Fix |
|------|-----|
| **SEH unwind (decisive)** | `include/rex/platform/exceptions.h`: POSIX `SEH_CATCH_ALL` was `catch(...)`, which swallowed glibc's `pthread_exit` forced-unwind when a guest thread exited → `__libc_fatal("FATAL: exception not rethrown")` abort. Changed to `catch(const ::rex::SehException&)` so forced unwinds propagate, matching the selective Windows `__except(seh_filter)`. |
| **Missing POSIX symbol** | `src/core/seh_posix.cpp`: add `seh_raise_guest_unwind()` (Linux counterpart of the seh_win.cpp impl; throws `SehException` so the nearest generated catch handles the guest RtlUnwind). Without it `librexruntime.so` had an undefined reference and the toolchain failed to link. |
| **Win32-ism in shared code** | `src/input/mnk/mnk_input_driver.cpp`: `GetTickCount64()` → `std::chrono::steady_clock` (in the env-gated REX_INJECT test path). |
| **Winsock in shared code** | `src/kernel/xam/xam_net.cpp`: add `using SOCKET = int` to the Linux include branch. `src/kernel/xam/netplay.cpp`: add an `#else` with `<arpa/inet.h>`/`<netinet/in.h>` so `htons`/`inet_addr` resolve (the `SOCKET`-using paths were already `_WIN32`-only). |
| **Analog test input** | `src/input/mnk/mnk_input_driver.cpp`: `REX_INPUT_FILE` now also parses an optional left-stick suffix `"<btnhex> <lx> <ly>"` and sets `gamepad.thumb_lx/ly`, so autonomous tests can move the player character (digital masks alone cannot). Paired with `tools/gamectl.sh`. |

**`rexglue-sdk-current-full.patch`** is the rolling, self-contained snapshot of the
*entire* rexglue-sdk working tree (all the bring-up fixes above plus the larger
boot/font/save/SEH/audio/locale fixes and the paused, cvar-gated netplay code incl. the
untracked `netplay.{h,cpp}`). Apply it to a pristine `rexglue-sdk` checkout to reproduce the
current build, then rebuild + reinstall the SDK. Regenerate with:
`git -C third_party/rexglue-sdk add -N include/rex/system/netplay.h src/kernel/xam/netplay.cpp && git -C third_party/rexglue-sdk diff > south-park-recomp/patches/rexglue-sdk-current-full.patch && git -C third_party/rexglue-sdk reset -q`

Both detailed in `knowledge-base/titles/south-park-lgtdp/30-boot-log.md`.
