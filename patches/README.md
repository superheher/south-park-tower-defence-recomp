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

Both detailed in `knowledge-base/titles/south-park-lgtdp/30-boot-log.md`.
