# The combat floor is CP PM4→Vulkan TRANSLATION throughput — NOT spin/over-execution

**Date:** 2026-05-30. **One-line:** Implemented the prompt's "runtime GPU-fence write-back / block on
signal" fully (both spin layers now BLOCK instead of busy-wait), measured it rigorously, and proved it
is **floor-neutral**. Per-frame instrumentation then localized the real floor: during heavy combat the
GPU command-processor thread spends **~29 ms/frame in single-threaded PM4→Vulkan translation** (≈72 % of
the heavy-frame wall-clock). **This corrects the `FLOOR-OVEREXEC` thesis** — the busy-waits were an
efficiency cost, not the floor.

## What was implemented (all kept — efficiency + avg/ceiling wins, all gate-pass + render-verified)
1. **CP `WAIT_REG_MEM` → block-on-signal** (`command_processor.cpp`). The CP's hot combat fence is the
   EOP "cleared-to-0" fence at guest `0x..C9B004` (func=equal ref=0), advanced by the guest *after* a
   vsync tick wakes its frame loop — so a parked CP only progresses on a vsync. After a short pure-pause
   poll it now BLOCKs on a `condition_variable` that `RefreshVblankFence()` `notify_all()`s each vblank
   (1 ms timeout backstop + the existing wall-clock escape). Removes the old 8000× `sched_yield`
   fast-poll (was ~17 % of CP cycles in-kernel during a dip). The instrumented `is_vblank_fence` check
   proved the combat wait is **not** the vblank-counter fence — see DEAD END below.
2. **Guest Main-thread spin → vblank-park** (`init_h.inja` `REX_SPIN_BACKOFF`, exported
   `rex::thread::VblankBackoffWait()`/`NotifyVblank()` in `threading_posix.cpp`/`thread.h`, wired from
   `MarkVblank`). The guest's post-frame GPU-fence wait (`sub_821C6E58`→`sub_821B9270`) was **34 % of
   all combat-dip cycles** even after the `bothfix` db16cyc→pause change (a `yield` doesn't deschedule a
   thread that's alone on its core). It now parks on the vblank CV at the long-spin threshold →
   **`sub_821B9270` 34 % → 9.6 %** (heavydip).
3. **`WriteRegister` GetLoggerRaw cache** (`command_processor.cpp`). `GetLoggerRaw()` was called on
   *every* register write just to test the debug level (~3 % of all CP cycles). Cached the logger
   pointer once; `should_log()` is a cheap atomic load.
4. **Skip unchanged shader-constant writes** (`vulkan/command_processor.cpp`). Float/bool/loop constants
   are pure GPU state with no write side effect; re-writing the held value is a no-op for the store AND
   the constant-buffer dirty-mark. Heavy combat re-sets large constant ranges per draw, much identical.
   (Fetch constants are intentionally NOT skipped — their writes have side effects.) This is the first
   **real floor lever**: it improved the heaviest frames (min +2, p10 +0.3).

## The decisive measurement — `[frame-diag]` per-window CP wall-clock split (XE_SWAP instrumentation)
| window | fps | wrm_block (frame-sync wait) | issueswap (present) | **translate (PM4→Vulkan)** |
|---|---|---|---|---|
| **heavy** (swaps≈46–50) | 23–25 | ~21 % (≈8 ms/f) | ~7 % (≈3 ms/f) | **~72 % (≈29 ms/f)** |
| light (swaps≈113–120) | 56–60 | ~55 % | ~3 % | ~42 % (≈7 ms/f) |

Heavy frames spend **~29 ms translating** (vs 7 ms light) — present is ~3 ms, frame-sync wait ~8 ms, GPU
idle 17–26 %, libvulkan 3.7 %. The floor scales with combat entity/draw count = **CP serial translation
throughput**. heavydip top CP function: `WriteRegister` 11.5 % (base 6.2 + Vulkan override 5.3).

## A/B results (floor p10, `ab_both.sh 90 N`, interleaved vs base = `bothfix` exe d4b0f50b/.so 605ce3ee)
| candidate | what | median p10 | avg | max | min |
|---|---|---|---|---|---|
| base (bothfix) | — | 15.1 | 29.9 | 54 | ~11.8 |
| cpfence2 (.so) | CP block only | 15.0 (Δ+0.1) | +1.3 | +3 | — |
| cpfence3 | CP block + weak Main park | 14.5 (Δ−0.3) | +1.3 | +3 | — |
| cpfence5 | + proper Main vblank-park + GetLoggerRaw | 15.1 (Δ0.0) | **+2.3** | **+6** | — |
| **cpfence6** | **+ unchanged-constant skip** | **15.4 (Δ+0.3)** | **+2.4** | **+6** | **+2** |

**Keep-bar (p10 > +1.0) NOT met.** De-spinning is strictly floor-neutral; only cutting real translation
work (the constant-skip) moved the heavy frames at all. The avg/ceiling/min all rose meaningfully and
CPU/power dropped (de-spun), so cpfence6 is kept as the new working binary.

## DEAD END (don't repeat) — block-on-signal cannot lift this floor
Six prior sessions proved codegen can't move the floor; this session proves **de-spinning can't either**
(it's not CPU-contention — freeing the Main core, a full ~34 %→9.6 % reduction, left p10 unchanged). The
floor is the CP's serial translation of a heavy frame's command stream. Instrumentation also showed the
combat CP `WAIT_REG_MEM` is the EOP fence (`0x..C9B004`), **not** the vblank-counter fence
(`0x..C9C04C`) — so an address-restricted CV block was a no-op until broadened to all slow waits.

## THE FLOOR LEVER (next session) — cut heavy-frame CP translation time (~29 ms → <22 ms for 30 fps)
1. **Extend redundant-write elision** beyond shader constants. The constant-skip (proof-of-concept,
   +0.3 p10 / +2 min) shows the title re-sets redundant state per draw. Safely skip unchanged writes for
   pure-state registers (enumerate the side-effect set: scratch, DC_LUT, gamma, COHER_STATUS, fetch,
   event/wait) — verify rendering with mid-combat screenshots, not just the gate.
2. **Devirtualize / bulk the register-write path** (`WriteRegister` is `virtual`, called per-register;
   `RestoreRegisters`/TYPE0 ranges could bulk-store + batch the dirty-mark).
3. **Draw batching / instancing** for the many small sprites (tower-defense = many similar units) — the
   diffuse remainder of the 29 ms is draw issue (pipeline + descriptor + command-buffer build).
4. **Parallelize the CP translation** (hard/risky) — the GPU HW is 17–26 % idle, starved by the serial CP.

## State
Kept binary **cpfence6**: exe `848f191c` (Main vblank-park) + `librexruntime.so` `f1d86a27` (CP block +
GetLoggerRaw cache + constant-skip). Gate PASS, boot→match→win, mid-combat render-verified, no deadlock.
SDK edits in `patches/rexglue-sdk-current-full.patch`. Committed (NOT pushed). New tools:
`tools/perf/ab_both.sh` (both-exe+so A/B); the `[frame-diag]` XE_SWAP instrumentation was removed after use.
