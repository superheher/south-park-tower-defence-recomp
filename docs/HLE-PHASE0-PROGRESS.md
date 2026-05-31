# HLE Graphics — Phase-0 progress (variant B, incremental)

> Branch `experimental/hle-graphics-spike`. Follows `HLE-GRAPHICS-SPIKE-REPORT.md`
> (the GO decision). This logs the incremental execution of Phase-0: prove the
> interception loop, then identify the dominant draw's guest function.

## TL;DR

- **Increment 1 — Present interception: DONE, committed, verified.** The variant-B
  override mechanism works on a real graphics-path guest function. `src/hle_graphics.cpp`
  overrides the title's per-frame Present/Swap routine `sub_821BFF48` with a strong symbol;
  `[hle] present count` tracks the engine's `[pacing-diag]` swaps/s 1:1. This is the loop
  everything else builds on (override → build → run → observe).

- **Increment 2 — Draw identification: MAJOR progress, the R1 risk is substantially
  de-risked.** Built a working guest-provenance technique and mapped the render call tree.
  Key findings below. The dominant group's render function is **not yet 100% confirmed** but
  has a strong leading candidate.

## What the title's GPU submission actually looks like (measured)

The XDK Direct3D is statically linked recompiled guest code. The relevant facts, all
**measured on this build**, not assumed:

1. **The command processor is asynchronous.** `UpdateWritePointer` only signals an event;
   a separate "GPU Commands" worker thread parses the ring (`command_processor.cpp`
   WorkerThreadMain). So there is **no guest call-stack at `IssueDraw`** — provenance must
   be recovered on the guest thread.

2. **The only guest-thread-synchronous GPU seam is the CP_RB_WPTR doorbell.** Guest MMIO
   writes go through `MMIOHandler::CheckStore` (called inline from the recomp store path),
   **not** the guard-page fault path, so `__builtin_return_address(0)` in `CheckStore` is the
   host RIP inside the compiled guest function that wrote the doorbell.

3. **The guest "kick" (doorbell write) = `sub_821C6600`.** 100% of doorbell writes
   (`distinct_rip=1`) come from `south_park_td+0x2E0A6B` = `sub_821C6600+0x47B` (exe is
   non-PIE ET_EXEC, link base 0x400000). This is the XDK command-buffer flush/kick.

4. **PM4 is built dynamically, not from literals.** The "SWAP" fourcc and PM4 type-3 draw
   headers (`0xC0..2200`/`..3600`) do **not** appear as constants in the recompiled guest
   code — so static constant-grep cannot locate draw/shader-set functions. (`0xC0000000`
   appears only as the type-2-bit field mask in a few cmd-buffer helpers.)

5. **Draws are emitted INLINE inside per-frame render functions**, not via a per-packet
   D3D-API call. Evidence: the most-called cmd-buffer-cluster function `sub_821C6D58` (108
   static call sites) fires only ~3×/frame, and `sub_821CC830` fires ~1×/frame — neither is
   per-packet. The hundreds–1700 draws/frame are produced by inline XDK packet-builds inside
   a handful of render functions, which trigger mid-loop overflow kicks.

## The provenance technique (reusable — saved as a patch)

`docs/hle-doorbell-provenance-probe.patch` (apply to `third_party/rexglue-sdk`, gated by
`REX_HLE_DOORBELL_DIAG`):

- In `mmio_handler.cpp::CheckStore`, pass `__builtin_return_address(0)` through the
  (otherwise-null) `ppc_context` write-callback slot.
- In `graphics_system.cpp::WriteRegisterThunk`, for register `0x01C5` (CP_RB_WPTR), record
  that host RIP. **Async-signal-safe capture only** (lock-free atomic table; an early
  version called `dladdr` in the fault path and deadlocked the dynamic-linker lock).
- Sample a full `backtrace()` every 4000 kicks (re-arming); symbolize with `dladdr` off the
  hot path, from the vsync worker thread. In heavy combat, mid-draw-loop overflow kicks
  dominate, so the sampled stack is taken **inside** the render function.

This is a general RexGlue capability: it recovers the guest function behind any MMIO write.

## The render call tree (from a heavy mid-loop kick stack, fps≈15)

Symbolized backtrace, inner→outer (host RVA + 0x400000 → `nm` against the exe):

```
sub_821C6600  (kick / doorbell write)
sub_821C6C80  (flush)
sub_821C6AF8
sub_821C6D58  (cmd-buffer reserve / flush-check)
sub_821CC830  ← XDK packet emit, ~1×/frame, does the inline draw loop
sub_821BEF00  ← title render fn (calls sub_821CC830 at +0x2DC = lr 0x821BF1DC)
sub_8212DFB8
sub_82150970  ← frame-render fn (also a direct caller of Present sub_821BFF48)
sub_82249AD0 → sub_82249970 → sub_82249678 → sub_82249638 → sub_824497B8 (main loop)
```

Frontend (light) kicks only ever flush at the frame boundary (`sub_821C6D58` path);
heavy combat adds the deep mid-loop kicks that reveal the render functions above.

## Leading candidate for the dominant (72.5%) render function

**`sub_821CC830`** — a per-frame (~1×/frame) render pass, called once from `sub_821BEF00`,
that performs an inline draw loop heavy enough to trigger mid-render overflow kicks. It is
intercepted today in `src/hle_graphics.cpp` (pass-through + caller histogram).

⚠ **It is BROADER than the 72.5% group — refined by a stub test.** Skipping `sub_821CC830`'s
body entirely (`REX_HLE_STUB_CC830=1`) blanks the **whole** frame to black **and stops swaps**
(no `[pacing-diag]`), with the process still alive. If it rendered only the dominant sprite
group, the background and other groups would remain and swaps continue — instead everything
stops. So `sub_821CC830` is a **central per-frame render/submit function** that the frame
depends on (it does the inline draw loop AND essential setup/submit), not the isolated 72.5%
group. The dominant group is emitted *within* `sub_821CC830`'s inline loop (or a callee), at a
finer grain than a whole-function hook.

**Implication for variant B:** a whole-function override of `sub_821CC830` is too coarse — it
would have to faithfully reproduce the entire pass. The right hook grain is either (a) a
**mid-asm hook** at the specific inline-draw site for the dominant shader inside `sub_821CC830`
(RexGlue supports `[[midasm_hook]]` at a guest instruction address), or (b) intercept at the
shader-bind for `adf7088205c03df9` and the draws that follow. Pinning that site is the next step:
correlate the dominant pixel shader with the exact inline-draw instruction(s) inside the
`sub_821BEF00`/`sub_821CC830` render path (CP `IM_LOAD` ucode-addr logging + the kick-backtrace
RVA that falls inside `sub_821CC830`).

## Next steps

1. **Confirm the dominant render function.** Either (a) histogram the render frame across many
   mid-loop kick stacks (the dominant group's fn appears in the most), or (b) instrument the
   CP `IM_LOAD` (shader bind) to log the guest ucode addr for hash `adf7088205c03df9`, then tie
   it to the render fn that binds it. Pick the function that actually emits the 72.5% group.
2. **Intercept it natively.** Override that render fn (strong symbol, already proven) and, for
   the dominant draws, drive the existing Vulkan backend directly with the ported
   `SPHud`/`SPTextured` shader instead of building PM4. Keep PM4 as the fallback for everything
   else (true hybrid).
3. **Benchmark** heavy-frame FPS native-vs-PM4 for that group — the commit-or-fallback datum.

## Build / run notes

- App override: edit `src/hle_graphics.cpp`, build target `south_park_td` in
  `out/build/linux-amd64-release` (incremental ≈20s). Overrides a generated **weak** guest
  symbol; the dispatch table also binds the weak name, so both direct and indirect callers route
  through it.
- SDK probe: apply the patch, build `third_party/rexglue-sdk/out/build/linux-amd64`
  `--config Release --target rexruntime`, copy `out/linux-amd64/Release/librexruntime.so` to the
  game dir. **Keep a byte backup of the baseline .so (md5 1a3f6076).**
- Heavy combat is reachable even when `gamectl play` reports "could not enter" (the attract /
  in-progress render still drives fps to ~15–21). Clean `/dev/shm/xenia_memory_*` between runs.
- Symbolize a host RVA: `nm -n --defined-only south_park_td`, find the `__imp__sub_*`/`sub_*`
  whose address ≤ (RVA + 0x400000).
