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
| `0002-rexglue-mainthread-r3-entry-sentinel.patch` | `PrepareModuleLaunch` launched the main thread with `start_context = 0` (→ guest `r3 = 0`). XDK CRT entry thunks double as the thread trampoline and only run process init when `r3 == -1` (`xstart: cmpwi r3,-1; bne <epilogue>`), so init was skipped. Pass `0xFFFFFFFF`. |

Both detailed in `knowledge-base/titles/south-park-lgtdp/30-boot-log.md`.
