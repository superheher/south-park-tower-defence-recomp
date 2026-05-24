# RUN.md — building, running, and continuing bring-up

> **Status (2026-05-24):** the recomp **PLAYS A MATCH TO A WIN — single-player is playable** —
> `boot → intro → title → MAIN MENU → LOCAL GAME → lobby → game-mode (Campaign) → level select
> (Stan's House) → the MATCH (waves spawn, units defend) → "STAGE COMPLETE!" (win) → CONTINUE`
> (screenshot-verified, **input works**, **no crash anywhere**, rendering correct, XMA audio
> thread running). Two root fixes got here: the
> **image-load setjmp/longjmp EH** (below) and the **session-enroll fix** (the local signed-in
> player must be enrolled as a "session player" — `fix_recomp_labels` Fix 6 — or session-player
> queries null-deref at lobby→match). **Remaining for full single-player playability:** play
> through to win/lose, save/continue, audio (XMA→SDL). Evidence:
> `knowledge-base/titles/south-park-lgtdp/{40-seh,50-menu-input-and-lobby,90-progress-report}.md`.
>
> **To reach the match (automation):** `--mnk_mode=true` + the injector
> `REX_INJECT_SCRIPT="66:0010,78:1000,92:0010,106:1000,120:1000,134:1000,150:1000"` (Start→menu,
> A→LOCAL GAME, Start→lobby-begin, A→game-mode NEXT, A→level select, A→Stan's House → match).
> A real user just presses the buttons on the focused window.
>
> **Driving input (important):** real users press a focused gamepad/keyboard normally — the
> plumbing is wired (`XamInputGetState`/`GetKeystroke` ← `input_system` ← mnk/SDL). **Automated**
> input must use the env-gated injector (synthetic OS keys never reach SDL here):
> `REX_INJECT_SCRIPT="66:0010"` presses START at 66 s (`0010`=START, `1000`=A; comma-separate
> `t:hexbtn` steps; `REX_INJECT_DUR` = hold seconds). That's how the menu/lobby were reached.
>
> **Focus-INDEPENDENT live input (`REX_INPUT_FILE`):** set `REX_INPUT_FILE=<path>` and the runtime
> re-reads that file every poll for the current XInput button mask (hex) and reports it as pad
> state **without needing window focus** (OS key injection is ignored by SDL when unfocused;
> input-system-level injection is reliable). Masks: `0010`=START `0020`=BACK `1000`=A `2000`=B
> `0001/0002/0004/0008`=DPAD U/D/L/R. Write the mask to press, write `0` to release (a tap = mask
> then 0); menus get edge-triggered keystrokes. It is OR'd with real SDL pad/keyboard. **One-click
> launcher with this wired in:** `out/build/win-amd64-relwithdebinfo/launch_game.bat` — run it, and
> the game can be driven by writing masks to `live_input.txt` next to the exe (great for remote/RustDesk
> use, or automation, where the window can't hold focus).
>
> **⚠ Boot deadlock (2026-05-24, environment-sensitive):** after ~250 launch/kill cycles in one
> long session the build began **deadlocking at the first frame present, ~100% of boots** (log
> freezes ~4 KB after `SetInterruptCallback`, no input ever polled). Root cause (cdb): a
> **present/vsync deadlock** — the swap path (`XE_SWAP → IssueSwap → RefreshGuestOutput`) blocks
> while `Presenter::DXGIUITickThread` spins on a disruptor `wait_until_published`. `--vsync=false`
> clears it but then stalls on a `WAIT_REG_MEM` GPU fence (`0x1FC9B006`, no escape). The same
> build booted to a **match win** earlier the same session, so this is degraded host GPU-driver/DWM
> vsync delivery (or a latent present-path race). **A reboot is the fastest restore**; then boot
> with default `vsync=true`. Full analysis: `knowledge-base/titles/south-park-lgtdp/60-boot-present-deadlock.md`.
>
> **Known blockers past the lobby:** a **non-deterministic GPU-fence stall** (main thread
> spins in `sub_821C6E58` waiting for a guest GPU fence that sometimes doesn't advance → some
> runs never reach input; proper fix = runtime GPU fence write-back). (The earlier lobby→match
> crash `SEH 0x1A in sub_82101AF0` is **FIXED** — the session-enroll fix, `fix_recomp_labels` Fix 6.)
>
> **The fix that reached the title screen (config-only, no SDK change):** the post-render
> hang was a **custom setjmp/longjmp EH for image-format detection** — the loader tries
> each format, "try JPEG" does `setjmp` (`sub_8242EEA0`), and a non-JPEG (the assets are
> **TGA**) `longjmp`s (`sub_8242EA70`) back so the loader tries the next format. Set in the
> manifest `[entrypoint]`: `setjmp_address = 0x8242EEA0`, `longjmp_address = 0x8242EA70`
> (rexglue's `ppc_setjmp/longjmp`). Then the boot hit a cross-function-branch FATAL
> (`0x821F23EC`); fixed by registering the 18 unresolved-branch targets in
> `config/sp_functions.toml` (via `tools/gen_missing_funcs.py` KNOWN_COMPUTED, now
> cumulative). See `40-seh-implementation-plan.md`.

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
- **Expected today:** runtime init → loads `game:\default.xex` → shaders/pipelines → TGA
  asset load → **animated intro** (Cartman over the town, ~30s) → intro movie (black; the
  WMV has no decoder) → **TITLE SCREEN "PRESS START" (~55–60s)**. Stays there awaiting input.
  Exit codes: `0xC0000005` = AV, `0xC0000409` = fail-fast.
- **At the title screen, press Start** (gamepad Start, or `--mnk_mode=true` + **Escape**) to
  advance to the main menu. NOTE: the window must hold **input focus** (the mnk driver gates
  on it). Synthetic/automated key injection from a background process is unreliable (other
  desktop windows steal focus); test input with the game window actually focused (click it).
  The input plumbing is wired (`XamInputGetState → input_system → mnk`, `has_focus_` defaults
  true) — pressing Start as a real user with the window focused is the way to verify.

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

## Next steps (toward playable)
The title screen is reached. To progress through `boot → menu → match → win/lose → save`:
1. **PRESS START → main menu.** Verify input as a real user (window focused; gamepad Start
   or mnk Escape). If a real user's Start doesn't register, debug the input path
   (`XamInputGetState`/`XamInputGetKeystroke` ← `input_system` ← mnk/SDL; check the SDL
   focus event fires and which input API the title polls). Automated injection here is
   unreliable — not a reliable signal of a port bug.
2. **Menu → match.** Expect more analyzer-missed cross-function branch targets (REX_FATAL
   "Unresolved call ... to 0x...") on new code paths — harvest them
   (`grep -rho "Unresolved call from .* to 0x[0-9A-F]*" generated/default/*.cpp`), add to
   `gen_missing_funcs.py` KNOWN_COMPUTED, regen. (11 such targets currently remain as traps
   but are off the boot→title path; their bodies ARE emitted — if one is hit, also teach
   `fix_recomp_labels` Fix-5 to tail-call an already-emitted target function.)
3. **Intro movie:** WMV3+WMA2 has no decoder, so it shows black with an "A SKIP" overlay;
   it advances on its own to the title. If a later movie blocks, stub movie playback to
   complete-immediately.
4. Then Phases 4–6 (render correctness, audio XMA→SDL, save/continue) are normal iteration.

## What fixed the old SEH/image-load hang (history)
The post-render hang was NOT standard `.xdata` SEH (the earlier guess). It was a **custom
hand-rolled setjmp/longjmp** used for image-format detection: the loader (`sub_82459B00`)
tries each format; "try JPEG" (`sub_82458010`) `setjmp`s a CONTEXT (`sub_8242EEA0`,
buf=`r1+720`=vtable+144), and on "not a JPEG" the decoder's raiseError (`sub_82456198`)
`longjmp`s (`sub_8242EA70`=RtlRestoreContext) back, so the loader tries the next format
(TGA). Modeled exactly by rexglue's `ppc_setjmp/longjmp` (same buffer, live setjmp frame).
Fix = manifest `setjmp_address=0x8242EEA0` + `longjmp_address=0x8242EA70` (config-only).
Full root-cause + the dead-ends in `knowledge-base/titles/south-park-lgtdp/40-seh-implementation-plan.md`.
