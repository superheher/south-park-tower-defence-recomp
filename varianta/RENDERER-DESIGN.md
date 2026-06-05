# Variant A — Menu Renderer: implementation design (from the cont.13–19 RE)

> ⚠ **PARTIALLY SUPERSEDED — see `GPU-RESOURCE-BUILD-PLAN.md` (cont.23) for the current plan.**
> cont.21 measured that the producer/consumer below is per-submission *bookkeeping*, NOT the
> per-draw path (real draws are PM4 `DRAW_INDX` in the kicked IBs / `device+13568` segments), and
> cont.22–23 localized the real wall to the resource loader's GPU resource-create (child[0]). The
> "execute the commands so completion happens naturally" thrust (steps 1–2 below) still holds; the
> "*(item+16) = the real textured draw" detail is wrong. Read the build plan first.


This consolidates the night-of-2026-06-04 reverse-engineering (NIGHT-LOG cont.13–19) into a
concrete implementation plan for the remaining multi-session work: a **faithful CP / GPU-completion
model** so the title's render submission completes and builds real, complete render work.

## The render pipeline (fully mapped, verified by prod oracle)

```
title render thread (sub_82249638→…→sub_821CC830→sub_821C6D58 flush→sub_821C6C80→sub_821C6600 kick)
  builds per-frame command segments + INCREMENTS the pending-counter device+0x2b04
        │  (kick fires only when device+0x2b04 == 0)
        ▼
primary ring IB  ──CP──▶  INTERRUPT (type-3 op 0x54) packet
        ▼
GPU fires source=1 (command-complete) gfx-interrupt  ──▶  sub_821C7170
        │   callback = *( *(device+0x2A94) + 0x10 )      [device+0x2A94 = completion object B]
        ▼
producer sub_821CC7A0(ctx)   [ctx = B+0x14, the per-submission render context]
        │   stores ctx into the per-process work-ring (base=*(0x8200098C)+procType·108+11328, count+11412)
        │   KeSetEvent → wakes the consumer
        ▼
consumer sub_821CC310 (runs on tid=10, dispatch loop sub_821CC5D0)
        │   dequeues the work item, DECREMENTS device+0x2b04 (via sub_821CC140)
        ▼
issues the draw via *(item+16)   [the work-item handler = the real textured draw]
```

Prod values: device=0x40016f80, B=*(device+0x2A94)=0xffc9a000, B={[0]=4, +0x10=0x821CC7A0, +0x14=ctx 0xddd10180}.
device+0x2b04 oscillates 0↔1 in prod (consumer keeps pace); KeGetCurrentProcessType==1 for all threads.

## Where variant A breaks (each measured)

1. **Counter never decremented** → kick deferred forever after 6 init kicks (device+0x2b04 climbs 0→1→2→… ;
   prod kicks 7062×/45s, variant A 6×). Because the consumer never runs to decrement it.
2. **Consumer never signalled** → it blocks in KeWaitForSingleObject (sub_821CC5D0) because the producer
   never fires, because…
3. **Producer never fires** → the completion object B is ALL ZERO in variant A (B[0]/+0x10/+0x14 = 0;
   the callback slot is null so sub_821C7170's null-check skips the call). B is populated via an aliased
   CP mapping a CPU-side HW watchpoint can't see — i.e. it's populated as a *result* of real GPU command
   execution, which variant A's fence-forward stub never performs.
4. **Even when forced, work data is incomplete** → REX_PUMPCB (drive the producer from the pump context
   with the device+13568 records' real ctx) makes the pipeline RUN (producer fires, consumer drains the
   batch) but the work-item handler *(item+16) is null → no textured draw + crash. The render work itself
   isn't built completely without real GPU execution.

## The fix: a faithful command processor (replace the fence-forward stub)

The stopgaps to remove: fence-forward (sub_821C5DF0 / sub_821C6E58) and the synthetic VblankPump interrupts.
Replace with a CP that actually EXECUTES the title's command stream so the title's own bookkeeping advances:

1. **Execute the kicked ring/IBs for real**, INCLUDING op 0x54 INTERRUPT packets → fire the source=1
   gfx-interrupt at the point the title's stream reaches it (with the completion object B set as the title
   set it for that submission — NOT synthetically). This makes the producer fire with the *title's own*
   B/ctx, so the work item is the one the title built.
2. **Drive GPU completion as a real result**: when the CP finishes a submission, signal the completion the
   title waits on (the fence at device+10896 / the segment pointer) AS A REAL RESULT of executing the
   commands — so the consumer's decrement of device+0x2b04 and the title's flush bookkeeping stay coherent.
3. **Let the consumer (tid=10) run under real concurrency** (NOTOKEN + the cont.12 orphan-CS fix) so it
   drains + decrements + issues draws via *(item+16). The draw handler will be non-null because the title
   built it during a real submission.
4. **Translate the issued draws to Vulkan**: the consumer's *(item+16) handler emits the actual draw
   (state from SET_CONSTANT reg-file 0x4000 ALU / 0x4800 fetch + textures + RT/viewport → VkPipeline +
   vkCmdDraw). The 19 .updb shaders carry original HLSL → SPIR-V (no microcode translator needed). The
   render thread owns the swapchain (rex_render.h / vulkan_render.cpp — already presents the movie).

The crux is **step 1+2**: stop faking completion; execute the commands so the title's render submission
completes naturally and builds real work items. Everything downstream (producer/consumer/draw) is already
present and verified to run when driven (cont.17–19); it just needs *real* work, which needs *real* execution.

## Diagnostics retained (gated, default boot unregressed)
REX_INTLOG (B + source=1 fires), REX_KICKGATE (device+0x2b04), REX_ENQLOG (producer/consumer),
REX_FINDCB (callback records), REX_INVOKECB / REX_PUMPCB (route-B drive — scaffolds), REX_BOOTSTRAP.

## Already working
The intro VC-1 movie decodes + renders in color via REX_RENDER=1 REX_FAIRSCHED=1 (cont.10/11), verified by
in-engine PPM capture (REX_RENDER_SHOT=<presented frame ~1000>). The menu renderer above is the open work.
