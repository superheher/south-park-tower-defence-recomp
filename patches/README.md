# patches/ — granular local patch series for third_party/rexglue-sdk

`rexglue-sdk` is a third-party submodule pinned at upstream `e8ce24f` (Release v0.8.0).
These are **local bring-up + perf fixes** found while porting *South Park: Let's Go Tower
Defense Play!* to native Linux/Vulkan. They are kept as a **granular `git format-patch`
series** (not pushed upstream, not committed into the submodule) so the super-repo gitlink
stays on the upstream commit and every fix is individually reviewable / revertible.

> **2026-05-30:** the old single rolling `rexglue-sdk-current-full.patch` blob (~3.3k lines,
> all fixes mixed together, and it silently dropped the untracked `netplay.{h,cpp}`) was
> **split into the granular series below**. The series is byte-for-byte equivalent to the old
> working tree *plus* the previously-missing netplay files (verified: `git am` onto a pristine
> `e8ce24f` reproduces the exact build).

## Apply (after `git submodule update`)

```bash
cd third_party/rexglue-sdk
git am ../../south-park-recomp/patches/0*.patch      # applies the whole series in order
#   …or, to keep the submodule on a detached gitlink with a dirty tree (the build only needs
#   the changes applied, not committed):  for p in ../../south-park-recomp/patches/0*.patch; do git apply "$p"; done
cmake --build out/build/linux-amd64 --target install # rebuild + reinstall the SDK
```

## The series

| # | Patch | What |
|---|-------|------|
| 01 | `build-mcmodel-medium…` | Recomp built `-mcmodel=large` → every guest call was an indirect `movabs;call *reg` (BTB-thrash). Flip to `medium` (direct calls 1→85k, exec mispredicts −79%, .text −4.9%). + rexglue cmake helper tweaks. |
| 02 | `codegen-implement-lmw-as-local…` | `build_lmw` (load-multiple, was `REX_UNIMPLEMENTED` in epilogues; `lmw` 119→0); the as-local/context decouple (registers as per-fn C++ locals synced to ctx only at setjmp/SEH boundaries, −16% .text, gate-pass); `db16cyc` spin-idiom → `REX_SPIN_BACKOFF` (the guest spin no longer pegs a core). |
| 03 | `platform-SEH-forced-unwind…` | POSIX `SEH_CATCH_ALL` `catch(...)` swallowed glibc's `pthread_exit` forced-unwind → `FATAL: exception not rethrown` abort; narrowed to `catch(SehException&)`. + `seh_raise_guest_unwind()` POSIX impl (the missing link symbol) + first-cut SEH recovery. |
| 04 | `core-timer-queue-blocking…` | `TimerQueue::TimerThreadMain` was **16.6% of all CPU cycles** busy-spinning in disruptorplus `spin_wait_strategy` → `blocking_wait_strategy` (+ fix a latent arg-order bug in `blocking_wait_strategy.hpp`). |
| 05 | `graphics-command-processor-boot-present-fence…` | The boot present/vsync + WAIT_REG_MEM fence-deadlock fixes: `RefreshVblankFence` (push live `counter_` to the guest's vblank fence each vblank), periodic ring read-ptr write-back, WAIT_REG_MEM fast-poll + escape (the 1 ms-sleep-per-poll throttle that froze the intro). |
| 06 | `graphics-vulkan-texture-shared-memory…` | Vulkan pipeline-cache / texture-cache / shared-memory bring-up fixes. |
| 07 | `audio-SDL-driver…` | SDL audio driver bring-up fixes. |
| 08 | `input-analog-left-stick…` | `REX_INPUT_FILE` parses an optional left-stick suffix `"<btnhex> <lx> <ly>"` so the autonomous harness (`tools/gamectl.sh`) can move the player, not just press buttons. |
| 09 | `kernel-xam-save-user-net-msg-input…` | xam save/user-profile/messaging/input-mapping + the **paused, cvar-gated netplay** code (incl. `netplay.{h,cpp}` — these were missing from the old blob). |
| 10 | `kernel-xboxkrnl-debug-io-rtl…` | xboxkrnl debug / io / rtl fixes. |
| 11 | `system-loader-r1-stack-headroom…` | Initial guest `r1` stack headroom (no-prologue XEX thunk reads above `r1`); `REX_ENTRY_OVERRIDE` env var; boot-continuation re-enter + stack-zero; STFS direct-mount (`--game_data_root` can be a single STFS package). |
| 12 | `perf-floor-GPU-fence-block-on-signal…` | **This session.** CP `WAIT_REG_MEM` blocks on a vblank `condition_variable` (instead of the 8000× `sched_yield` fast-poll); guest Main spin parks on the vblank signal (`REX_SPIN_BACKOFF` → exported `rex::thread::VblankBackoffWait()`/`NotifyVblank()`, `sub_821B9270` 34%→9.6%); `WriteRegister` caches `GetLoggerRaw` (was ~3% of CP cycles). All floor-neutral but de-spun + avg/ceiling up. |
| 13 | `perf-floor-skip-unchanged…shader-constant…` | **This session.** Skip unchanged float/bool/loop shader-constant register writes (pure GPU state) — the first real combat-floor lever (heavy-frame min +2, p10 +0.3): cuts redundant PM4→Vulkan translation. See `../docs/FLOOR-CP-TRANSLATION-REPORT.md`. |

## Adding / regenerating patches (keep it granular!)

- **New fix** → its own new numbered patch. Stage just that fix's hunks and:
  `git -C third_party/rexglue-sdk diff -- <files> > south-park-recomp/patches/00NN-<feature>.patch`
  (for files shared with an existing patch, stage only the new hunks with `git add -p`).
- **Re-split everything** (if it drifts) → rebuild the series on a throwaway branch and
  `git format-patch e8ce24f`:
  ```bash
  git -C third_party/rexglue-sdk worktree add --detach /tmp/wt e8ce24f
  # apply existing series, make per-feature commits, then:
  git -C /tmp/wt format-patch e8ce24f -o south-park-recomp/patches/
  ```
- **Capture untracked files** (e.g. netplay): `git add` them on the branch before `format-patch`
  (a plain `git diff` does NOT include untracked files — that was the old blob's bug).
- **Always verify**: `git am` the series onto a pristine `e8ce24f` worktree and byte-compare to
  the live SDK tree before committing.
