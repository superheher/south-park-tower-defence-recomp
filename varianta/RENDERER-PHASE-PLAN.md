# Variant A — Renderer Phase Plan (goal step 5: видео → рендерер)

The boot now runs the **entire** CRT/env/TLS/threading/sync/asset chain and reaches the GPU. The
remaining work — getting the title to render frames — is a multi-week GPU/threading subsystem. This is
the ordered, concrete plan, built from this session's traces. Status as of commit `8b6d18b..HEAD` on
`experimental/hle-graphics-spike` (NOT pushed). Behaviour ref 1:1 = `third_party/rexglue-sdk/src/` +
UnleashedRecomp `gpu/video.cpp`.

## ⬆️ STATUS UPDATE 2026-06-01/02 (the old "wall" below is SOLVED — boot now reaches the intro MOVIE)
The "device is null / worker spins" wall described below is **cleared**. The boot now lifts the CRT-init
join, runs the game main loop, and **reaches & plays the intro movie** (loads 8.4MB sp_xbox_0_intro.wmv,
4 VC-1 decoder threads, render loop presents framebuffers via VdSwap). Commits c0a39ae..c4579fc.
- **Step 1 (scheduler):** the coop token + sub_821B9270 token-yield is sufficient for now (fibers NOT
  needed yet; ordering works — the device is built (0x26F80) before the GPU waits run).
- **Step 2 (CP / null-GPU):** DONE as a **fence-forward STOPGAP** — the GPU completion fences
  (*(device+10896) counter + *(fenceptr+4) segptr) are advanced to the requested target in the hit
  waiters (sub_821C6E58, sub_821C5DF0), since the title defers kicking deferred segments and our CP runs
  only kicked ring content. VdSwap advances the swap counter. The clean replacement = a continuous CP that
  executes the WAIT_REG_MEM-chained deferred IBs (still TODO, but only matters once Step 4 lands).
- **Step 3 (PM4 parse):** DONE — ExecutePM4 parses the full TYPE0/1/3 stream (IB/EVENT_WRITE_SHD/INTERRUPT/
  WAIT_REG_MEM/DRAW). **Translate = still a no-op** (DRAW_INDX counts only).
- **THE REMAINING WALL = Step 4 (the renderer proper).** The intro movie is **renderer-gated**: a 180 s run
  = 1.1M trace lines but never advances past the movie because the video frames are never actually drawn/
  displayed. Everything further (intro→menu→gameplay→visible frames) needs Step 4. This is multi-week and a
  **strategic decision**: variant A's renderer ≈ prod's GPU subsystem (Xenia-based CP→Vulkan that the prod
  oracle already uses), so the options are (a) port a minimal renderer, (b) integrate rexglue's Vulkan
  backend/Plume into variant A's runtime, or weigh variant A vs just using prod. **Autonomous loop stopped
  here — this is a human scope/approach call (see Step 4 below).**

## [HISTORICAL] Where the boot USED to stop (SOLVED — kept for context)
A guest **worker thread** runs `sub_821C6E58(device=r3) → sub_821B9270(&local)` — the **GPU ring-buffer
wait**. `sub_821B9270` reads `r29 = [arg+0] = device`, then polls `[[device+10896]]` (the **RPtr
write-back**, = `g_rptrWriteBack` from `VdEnableRingBufferRPtrWriteBack`) against a target, looping until
the GPU "catches up". At the wall the **device is null** (`r29=0`): the worker started before the title's
D3D device was built (a thread-ordering artifact of my preemptive/coop scheduler), so it spins forever,
holding the cooperative token and starving the main thread that would build the device. **Two root causes,
both must be fixed:** (A) the scheduler lets a busy-wait starve everything; (B) there is no GPU command
processor to advance RPtr. [Both since handled — see status update above.]

## Step 1 — Deterministic cooperative scheduler (fibers)  [prerequisite]
Replace the `std::thread` + single-mutex token (commit e56368f; `kernel.cpp` `g_waitMutex`/`GuestThreadRun`)
with **fibers** (ucontext or a fiber lib), exactly like RexGlue (`rex/thread/fiber.h`, `X_FIBER_CONTEXT`,
`XThread::main_fiber_`). One guest fiber runs at a time; a fiber yields to the scheduler **only at explicit
waits** (KeWaitForSingleObject/MultipleObjects, Rtl CS contention, KeDelayExecutionThread). This makes the
boot **deterministic** (current coop token serializes execution but not OS-handoff *order* → non-deterministic
stop points: GPU spin / `RtlRaiseException` / crash). Keep the gate findings: preemptive (`REX_NOTOKEN=1`)
is worse (hangs at XamInputSetState); coop is the right base. Ensure thread *creation order* matches the
guest's expectation so the D3D device is built before the GPU worker fiber first runs.

## Step 2 — GPU command processor (the "null GPU" + then real)  [the core]
Run the GPU on a **real host thread** (NOT a fiber) so a busy-waiting guest fiber's RPtr condition becomes
true without the fiber yielding (RexGlue/Xenia model).
- **Capture WPtr:** the guest publishes the write pointer via GPU register **CP_RB_WPTR (index 0x01C5)**
  (rexglue `graphics_system.cpp:278`). Two options: (a) find the CP register window's guest address
  (GPU reg base + 0x01C5*4 = +0x714; trace where the title writes it / `VdGetSystemCommandBuffer`) and POLL
  it; or (b) add **MMIO write-interception** for the GPU register range (mprotect the page + SIGSEGV decode,
  or a recompiler write-callback) — rexglue `system/mmio_handler.cpp` is the ref.
- **Null GPU first:** the host GPU thread advances `[g_rptrWriteBack]` → WPtr (pretend all PM4 consumed),
  writing guest memory directly (no token). That releases `sub_821B9270`. ⚠ The device must be non-null
  first (Step 1 ordering, or build a minimal device — the structure read at device+10888/10896/10908/11008).
- **Vd\* completeness:** beyond the stubs in `kernel.cpp` (VdInitializeRingBuffer/EnableRingBufferRPtrWriteBack/
  SetGraphicsInterruptCallback/InitializeEngines), implement VdGetSystemCommandBuffer, VdSwap,
  VdCallGraphicsNotificationRoutines, VdRetrainEDRAM, etc. as the boot surfaces them.

## Step 3 — PM4 parse + translate  [the renderer proper]
Consume the ring buffer the title fills (base/size from `VdInitializeRingBuffer`, now in `g_ringBufferBase`/
`g_ringBufferSize`). Parse the **PM4** packet stream (TYPE0 register writes, TYPE3 draw/state packets) →
issue draws. This is the bulk of Xenia's `command_processor.cpp`. Crib the **prod RexGlue `.so`**
(`librexruntime.so` = Xenia CP 1:1 — see memory) and UnleashedRecomp `gpu/video.cpp`.

## Step 4 — Plume Vulkan backend + shaders
Wire the translated draws to **Plume** (the in-tree Vulkan backend) and port the **19 title shaders**
(`private/extracted/media/shaders/*.updb` — `Simple`, `SimpleCol`, `SPBackdrop*`, …; the `.updb` carry
original HLSL per the spike report) → SPIR-V. Window/swapchain via SDL (as the prod path does).

## Step 5 — Loose ends surfaced by the above
- **SEH:** `RtlRaiseException` (now reachable on some interleavings) — XenonRecomp models SEH via
  setjmp/longjmp; implement to drive the guest `__except` path.
- **Remaining imports:** XAudio* (audio worker), XamInputSetState/input, more Xam — implement as hit
  (soft-stub trace `REX_KTRACE=1`; `REX_STUBTRAP=1` to pinpoint).

## Build / run / debug
`cmake --build varianta/runtime/out --target sp_td_varianta` →
`./sp_td_varianta <abs path>/private/extracted/default.xex`. `REX_KTRACE=1` import trace, `REX_STUBTRAP=1`
hard-trap unimpl imports, `REX_NOSPAWN=1` single-thread, `REX_NOTOKEN=1` preemptive. gdb backtraces work
(-O0 host frames); ⚠ live `ctx.*` inspection fights the token under all-stop — prefer tracing/`fprintf`.
