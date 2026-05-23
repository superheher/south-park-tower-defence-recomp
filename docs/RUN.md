# RUN.md — building, running, and continuing bring-up

> **Status (2026-05-23):** the recomp **boots and RENDERS the game** — it brings up the
> full rexglue runtime (D3D12/XMA/SDL/VFS/kernel), executes the recompiled guest code, and
> **draws the South Park town backdrop via D3D12** (verified by screenshot). It then
> presents black frames and **hangs** on a worker-thread SEH fault (see "Remaining
> blocker"). Earlier "not playable / stub entry / doesn't boot in Xenia" notes are
> **retracted** — the title boots to menus in Xenia and the recomp now renders.
> Full evidence: `knowledge-base/titles/south-park-lgtdp/35-entry-forensics.md`.

## Prerequisites
- Clang (built with 22.1.6), CMake, Ninja, Python 3 (`pip install pycryptodome capstone`).
- **rexglue-sdk built & installed with the local patches applied.** Patches live in
  `patches/` and are applied to the SDK working tree (NOT committed to the submodule):
  `0001` (r1 stack headroom), `0003` (lmw), `0004` (entry-override env),
  `0005` (boot-continuation reenter + stack zero), `0007` (SEH exception-recovery
  first-cut). `patches/rexglue-sdk-current-full.patch` is the **authoritative full diff**
  (apply this one to a clean SDK to reproduce the exact state, incl. the diagnostics).
  Re-apply after a submodule bump, then rebuild/install the SDK.
- Game assets in `private/extracted/` (extracted title tree incl. `default.xex`).
  **Never committed** (git-ignored). Extract with the fixed `tools/stfs_extract.py`.

## One-time asset setup: movie locale subdir
The game requests intro/level movies under `Media/Assets/Movies/en-en/` (a locale subdir
the flat extraction omits). Create it with hardlinks (no data duplication):
```powershell
$mov='<repo>\south-park-recomp\private\extracted\media\Assets\Movies'
New-Item -ItemType Directory -Force "$mov\en-en" | Out-Null
Get-ChildItem $mov -Filter *.wmv -File | ForEach-Object {
  $l="$mov\en-en\$($_.Name)"; if(-not(Test-Path $l)){ New-Item -ItemType HardLink -Path $l -Target $_.FullName | Out-Null } }
```
(The movies are WMV3+WMA2, which the runtime can't decode, but the file must *open* or the
intro path stalls. The intro is not the hang — see below.)

## Build
```powershell
# SDK (after applying patches) — from third_party/rexglue-sdk:
cmake --build out/build/win-amd64 --target install
# then copy the fresh runtime next to the app exe:
copy ..\..\..\third_party\rexglue-sdk\out\install\win-amd64\bin\rexruntime.dll `
     out\build\win-amd64-relwithdebinfo\rexruntime.dll
# App:
cmake --build --preset win-amd64-relwithdebinfo
```
Produces `out/build/win-amd64-relwithdebinfo/south_park_td.exe` (+ `rexruntime.dll`).
After **codegen** (`rexglue -f codegen`) always run `python tools/fix_recomp_labels.py`.

## Run
```powershell
cd <repo>\south-park-recomp
out\build\win-amd64-relwithdebinfo\south_park_td.exe `
  --game_data_root=<repo>\south-park-recomp\private\extracted
```
- `--game_data_root` is **required**.
- `--log_level=debug` (or `trace`) for verbose kernel/APU logging.
- `--mnk_mode=true` makes the keyboard a virtual controller (Escape=Start, Space=A,
  default OFF) — needed once the boot reaches an interactive screen.
- Needs an **interactive desktop** (D3D12 window). Logs: `out\build\...\logs\south_park_td_NNN.log`.
- **Expected today:** runtime init → loads `game:\default.xex` → shaders/pipelines →
  **renders the town backdrop (~40s)** → goes black and hangs (no crash). Exit codes:
  `0xC0000005` = AV, `0xC0000409` = fail-fast.

## Diagnostics harness (what works for a live hang)
- **All-thread guest stacks (no hang):** attach cdb to the running process —
  `cdb -p <pid> -c "~*k 24; qd"` (qd = detach, leave it running). Guest functions
  symbolize as `south_park_td!__imp__sub_XXXXXXXX` (RelWithDebInfo). The old "cdb hangs"
  only applied to *launching* under cdb.
- **Screenshot the live D3D12 window:** get `Process.MainWindowHandle`, `SetForegroundWindow`,
  `GetWindowRect`, then `Graphics.CopyFromScreen` (PrintWindow returns black on flip-model
  swapchains). See `35-entry-forensics.md` for the exact PowerShell.
- **SEH fault backtrace:** `src/core/seh_win.cpp` `seh_filter` logs `SEH FAULT code=… fault_addr=…
  bt: …` (symbolized) when it catches a hardware fault — reveals where a recovered-but-dead
  worker faulted. **WAIT_REG_MEM** stuck-detector in `command_processor.cpp` logs a stalled
  GPU poll. (Both are diagnostics in the full SDK patch.)

## Remaining blocker (what would make it playable)
The boot hangs because a **worker thread faults during image/asset load** and my SEH
first-cut catches it → the worker dies → the main thread waits forever on its completion
flag (presenting black frames). Root cause: `sub_8242EA70` is the game's **`RtlRestoreContext`
/ longjmp** (restores f14–f31, r13–r31, v64–v127, SP=`[buf+144]`, PC=`[buf+308]` from a
CONTEXT buffer, then `blr`) — a **non-local jump** the static recomp emits as a plain C++
`return`, so it returns to the caller carrying the restored (setjmp-time) `r31` → null write.
The game uses standard Win32 table-based SEH (`RtlCaptureContext`, `RtlUnwind`,
`__C_specific_handler`); there is no guest `setjmp`, so `setjmp_address`/`longjmp_address`
does not apply. **The fix is a fuller SEH implementation** in the runtime/codegen:
implement the exception dispatch (`RtlUnwind`/`__C_specific_handler`/`RtlRestoreContext`)
to drive a correct non-local jump to the (mid-function) resume target — the hardest part
of static recomp, and the maintainer's chosen "implement SEH" direction. Once a worker can
take an SEH path and resume, the boot should clear the loading wait toward the menu, then
Phases 4–6 (rendering correctness, input, audio, save) become normal iteration.
