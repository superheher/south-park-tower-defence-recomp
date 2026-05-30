# The combat floor is GUEST OVER-EXECUTION (busy-waits), NOT the CPU — three spin layers, fixed two

**Date:** 2026-05-30. **One-line:** The user's decisive observation — *Sonic Unleashed Recompiled (a full
3D game) runs great on this same machine while our 2D tower-defense drops hard* — proves the floor is **not
a CPU/codegen limit**. Direct measurement confirms: the recompiled game **busy-waits**, burning ~5.3 G
instructions/s **even while PAUSED**. There are **three over-execution layers**; this session fixed two and
diagnosed the third (the floor) precisely. This **supersedes the entire front-end-capacity framing** of
`FLOOR-FRONTEND-REBASELINE-REPORT.md` — that was measuring the cost of a spin loop, not a real bottleneck.

## The decisive measurement (tools/perf/pausetest.sh)
The guest-sim (Main) thread's instruction rate, RUNNING vs PAUSED (a correctly-emulated game does ~nothing
while paused):
| | BKG (before) | after Main-fix |
|---|---|---|
| Main-thread insn/s, running | 5.34 G | 0.26 G |
| Main-thread insn/s, **paused** | **5.30 G (99%)** | 0.20 G |

99% identical paused vs running ⇒ the work is a **busy-wait**, not the sim. Proof it's over-execution: the
original ran 60 fps on a 3.2 GHz in-order Xenon core (≤ ~3.2 G guest-insn/s), so it physically executed
≤ ~50 M guest-insn/frame; we execute ~10× that. A 10×-faster i9 can't help a spin bound by an **external
event** (a fence another thread/the GPU sets), which is exactly why every codegen lever was floor-neutral.

## The three over-execution layers
1. **Main thread — guest spin on the GPU vblank fence.** `sub_821C6E58` (back-edge `0x821C6F10`) polls a
   guest-memory word the GPU command-processor advances via `EVENT_WRITE_SHD`, calling `sub_821B9270`. The
   guest uses the textbook Xenon spin idiom `cctpl` (priority-low) → `db16cyc` backoff → `cctpm` — and the
   recompiler emitted **all of it as no-ops**, so the spin ran at full speed/priority, never yielding.
   **FIXED:** `db16cyc` now emits `REX_SPIN_BACKOFF()` = x86 `pause` + a periodic `std::this_thread::yield`
   (`init_h.inja`, `builders/system.cpp`). Result: **−25× insn-rate** (5.3 → 0.2 G/s), detdiff gate PASS.
2. **Timer thread — disruptorplus spin-wait.** `TimerQueue::TimerThreadMain` was **16.6% of all CPU
   cycles** busy-spinning in `wait_until_published` (it used `spin_wait_strategy`). **FIXED:** switched to
   `blocking_wait_strategy` (+ fixed a latent arg-order bug in `disruptorplus/blocking_wait_strategy.hpp`:
   `wait_until(lock, pred, time)` → `(lock, time, pred)`). Timer thread now blocks; gate PASS.
3. **GPU CommandProcessor — `WAIT_REG_MEM` spin-poll (THE FLOOR).** `ExecutePacketType3_WAIT_REG_MEM`
   (`command_processor.cpp:~1230`) spin-polls a register/memory value while unmatched: up to 8000 iterations
   of `SyncMemory()` (mem barrier) + `MaybeYield()` (= **`sched_yield()` syscall** + barrier), then falls to
   `Sleep`. During a heavy dip the CP thread is **36.8% of all cycles, pegged on a core**, of which only
   ~19.7% is librexruntime user code (WriteRegister/parse) and **~17% is kernel (the `sched_yield` syscalls
   under WAIT_REG_MEM** — `syscall_return_via_sysret`). NOT FIXED — see below.

## Why fixing #1 and #2 didn't move the floor (but is still worth keeping)
The floor p10 stayed ~15 (base 15.2 vs both-fix 15.1, gate PASS) because the floor is layer #3 + the
frame-sync ping-pong, not the Main/timer spins. But #1/#2 are strict wins: idle CPU/power collapsed, the
fps **ceiling/avg rose** (max 53–58 vs 49–51), and the game no longer pegs cores doing nothing. Kept.

## The floor mechanism (measured) and the real fix
Per-thread during a heavy dip (fps ~20→16): **Main 42% (pausing on the fence) + GPU CP 37% (pegged on
WAIT_REG_MEM) + Audio 13.6% (mutex-heavy — a likely 4th spin)**. **GPU hardware is 17–26% IDLE and
libvulkan is 2.4%** ⇒ the floor is **NOT GPU-bound**; it's the **CPU-side frame synchronization done as
mutual spin-polling**: the Main thread spins on the GPU fence *and* the CP spins on WAIT_REG_MEM, and the
frame quantizes to vblank (floor p10 ≈ 15 = 60/4, with 20 = 60/3, 30 = 60/2). This is the "sinusoid"
pacing issue.

**The real fix (the SDK's own comment at `command_processor.cpp:1273` states it): "runtime GPU-fence
write-back."** When the Vulkan GPU work that a WAIT_REG_MEM / the guest's post-frame fence gates completes,
write the awaited value to guest memory/register **promptly via a Vulkan fence/semaphore-completion hook**,
so both waits resolve on a signal instead of spin-polling. That removes the spin entirely and de-quantizes
the frame. This is a moderate, **risky** runtime rework (the pacing area has had fixes tried + reverted).
A cheaper partial: in the WAIT_REG_MEM fast-poll, replace per-spin `sched_yield` with `_mm_pause` + a
*periodic* yield (cut the ~17% CP kernel overhead) — bounded but uncertain (yield is how the CP lets the
guest write the fence).

## New tools (tools/perf/)
`pausetest.sh` (running-vs-paused insn-rate — the over-execution detector), `spinprofile.sh` (paused =
pure-spin profile), `heavydip.sh` / `gpucp3.sh` (per-thread + per-comm dip profiling), `uksplit.sh`.

## State
exe `d4b0f50b` (Main backoff) + `librexruntime.so` `605ce3ee` (timer blocking) = the new working binary,
gate PASS, floor-neutral, over-execution massively reduced. SDK fixes in
`patches/rexglue-sdk-current-full.patch`. The floor still needs layer #3 (GPU-fence write-back).
