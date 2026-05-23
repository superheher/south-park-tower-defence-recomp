# RUN.md — building, running, and continuing bring-up

> **Status (2026-05-23):** the recomp **builds and runs** (boots the full rexglue
> runtime and executes guest code), but is **not yet playable** — the title's XEX
> entry is a stub and its real boot is kernel-orchestrated (research-grade; it does
> not boot in Xenia either). See `knowledge-base/titles/south-park-lgtdp/35-entry-forensics.md`
> and `90-progress-report.md`. This doc covers how to build/run to the current state
> and the harness for continuing the boot work.

## Prerequisites
- Clang 18+ (built with 22.1.6), CMake, Ninja, Python 3 (+ `pip install pycryptodome capstone` for tools).
- rexglue-sdk built & installed (see `BUILD.md`); local SDK patches under `patches/`
  applied (`0001` r1 headroom, `0003` lmw, `0004` entry-override) — re-apply after a
  submodule bump, then rebuild/install the SDK.
- Game assets: `private/extracted/` (the extracted title tree incl. `default.xex`).
  **Never committed** (git-ignored).

## Build
```powershell
cmake --build --preset win-amd64-relwithdebinfo
```
Produces `out/build/win-amd64-relwithdebinfo/south_park_td.exe` (+ `rexruntime.dll`).
If you rebuilt the SDK, copy its `rexruntime.dll` next to the exe.

## Run (current state: boots runtime, runs the stub entry)
```powershell
cd <repo>\south-park-recomp
out\build\win-amd64-relwithdebinfo\south_park_td.exe `
  --game_data_root=<repo>\south-park-recomp\private\extracted
```
- `--game_data_root` is **required** (else: "--game_data_root was not provided").
- Logs auto-write to `out\build\win-amd64-relwithdebinfo\logs\south_park_td_NNN.log`
  (sequential). Add `--log_level=trace --log_noisy=true` for maximum detail.
- The app needs an **interactive desktop** (D3D12 window). From an automated/headless
  shell it hangs at window creation — launch it from a logged-on session (or a
  `schtasks /it` task; see `~\xbla-refs\xenia-bin\run_app.bat`).
- Expected today: runtime initializes (D3D12/XMA/SDL/VFS), loads `game:\default.xex`,
  runs the guest entry (`TRACE xstart entered r3=0`), logs `Execution complete`, exits.
  No rendered frame (the entry is a stub).

## Continuing the boot work (harness)
- **Entry override (patch 0004):** `set REX_ENTRY_OVERRIDE=0x82xxxxxx` before launch to
  start the guest main thread at a different function (e.g. a candidate `mainCRTStartup`).
  Confirmed working ("Entry point OVERRIDDEN …" in the log).
- **Candidate finder:** `python tools/find_zeroref_roots.py private/default_dec.bin`
  → `private/entry_candidates.txt` (zero-reference, prologue-having functions).
- **Brute-force scripts** (reusable, in `~\xbla-refs\xenia-bin\`):
  `brute_force.ps1` (default-log pass), `brute_force3.ps1` (run-length/exit), driven via
  `schtasks /it`. Result so far: **no forced entry boots** (kernel-orchestrated boot).
- **Xenia oracle:** `~\xbla-refs\xenia-bin\` has stock + canary configured for a
  boot trace (license_mask=1, discord=false, trace logging). Both crash/stall at the
  stub entry — use to validate any future fix against the reference emulator.
- **Analysis tools:** `tools/` — `xex_decrypt.py --save`, `pe_inspect.py`, `pdata.py`,
  `ppc_dis.py`, `callgraph.py`, `find_crt*.py`. See `tools/README.md`.

## The remaining blocker (what would make it playable)
The boot is not reachable by "call the XEX entry" or by forcing any entry. It needs
either an **interactive decompiler** session (Ghidra/IDA — find how the real init is
triggered) or a **real-hardware boot trace**. See `35-entry-forensics.md` for the full
evidence. Once the real entry/sequence is known, wire it via `REX_ENTRY_OVERRIDE` (or a
launch hook) and Phases 4–6 (rendering, audio, input, save) become normal iteration.
