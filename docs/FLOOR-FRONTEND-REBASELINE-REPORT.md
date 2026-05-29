# Combat-floor re-baseline: the floor is FRONT-END-DELIVERY + RETIRING bound (NOT back-end), and the
# per-instruction codegen lever class is EXHAUSTED because clang -O3 already canonicalizes the idioms

**Date:** 2026-05-30 (autonomous session). **One-line:** Direct measurement of the *Main/guest-sim
thread during heavy combat* shows the floor is **Frontend-Bound 39% + Retiring 50%, Backend only 10%,
L1-hit 99.9%** — so the prior runbook's "back-end / raw-guest-work bound" diagnosis is **wrong**, and
its prescribed P1 lever ("reduce host µops via better instruction selection in `builders/*`") has
**no headroom**: clang -O3 already lowers the emitted C++ idioms to minimal host code. Three concrete
µop/instruction-count levers were built and measured — **all dud** (movbe ↑uops, rlwinm already-optimal,
non-volatile ↑instructions + fails the gate). The reducible host work that remains is the
**ctx-register memory traffic** (~21% of the hottest function, measured by `perf annotate`), which is
structural to the recompiler's `(ctx, base)` calling convention and blocked by SEH (GPR-as-local).

> Supersedes the "back-end-bound" framing in `FLOOR-PGO-MEDIUM-REPORT.md` / memory `sp_floor_backend_bound`.
> Those were *inferred* (front-end levers were neutral ⇒ "must be back-end"); this report **measures**
> the Main thread directly and finds it is front-end + retiring, with back-end only 10%. The earlier
> inference's flaw: front-end **layout/prediction** levers (mcmodel/leaf-inline/PGO) were neutral not
> because the back-end binds, but because they don't reduce the front-end's actual problem (see §3).

## The measurement (tools/perf/topdown.sh on the Main thread, heavy combat, BKG bef1b65c)
TMA level-1, share of 4×cycles issue slots:
| component | value | meaning |
|---|---|---|
| **Frontend Bound** | **39.1 %** | front-end fails to deliver uops to a ready back-end |
| Bad Speculation | 1.1 % | mispredicts negligible (mcmodel already fixed the indirect-call storm) |
| **Retiring** | **49.7 %** | real work retired |
| **Backend Bound** | **10.1 %** | **execution ports are NOT the bottleneck** |

Stall decomposition: `stalls_mem_any` = **9.8 %** of cycles, `stalls_l3_miss` 0.8 %, **L1 load hit =
99.9 %** → **NOT memory-latency bound**, working set fits L1. uop source: **MITE 87 % / DSB 13 %** →
the hot code runs from the slow legacy decoder, not the uop cache. `uops_executed/cyc` ≈ 2.5 of 4 →
back-end has spare capacity. `perf annotate` of the #1 hot fn (`sub_821B9270`, 12-16 %): cycles
**smeared evenly across ~35 instructions at 1.4–5.5 % each**, including trivial ops (`mov $1,%r12d`
5.6 %) and branches — the classic **front-end-starvation signature** (back-end waits ~uniformly).
~21 % of that function's self-time is **ctx-struct loads/stores** (`%r14`-relative).

## Levers built + measured this session — ALL DUD for the floor
| lever | what | result | why |
|---|---|---|---|
| **arch flags** `-mmovbe -mbmi -mbmi2 -mlzcnt -mpopcnt` (exe) | fuse load+bswap→movbe, flagless shifts, lzcnt | **Main-thread uops +10 %** (87.7B→96.6B, uops/insn 1.056→1.188); .text +6.5 % | `movbe` *forces* byteswaps clang had **elided** (eq-compares / store-back of loaded values) AND defeats load-folding into ALU ops (a `movbe` can't be a memory operand). Wrong direction. |
| **rlwinm/rlwnm/rlwimi special-casing** | replace `rotl64(dup)&mask` with `shl`/`shr`/`and` for slwi/srwi/extract | **zero headroom** | clang -O3 **already** canonicalizes the idiom — `cur_slwi`/`cur_srwi`/`cur_gen` emit byte-identical asm to the hand-written `shl/shr/and`. The rotate/dup exists only in the C++ source. |
| **non-volatile guest RAM** (drop `volatile` from REX_LOAD/STORE) | unblock CSE/reorder/hoist of guest loads | **neutral perf + FAILS detdiff gate** | guest loads have distinct un-provable-alias addresses ⇒ little CSE; the optimizer's added scheduling freedom *grew* hot-fn instruction count (sub_821B9270 117→134). And `sync/lwsync/eieio` are **no-ops** (no compiler barrier) — `volatile` is the *only* thing preventing the compiler hoisting polling loads across barriers, so removal broke boot/nav (`session_failed_no_in_level`). |

## §3 — Why front-end levers (this session + prior) are neutral, mechanistically
The front-end stall is **fetch-latency dominated** (resteers + MITE delivery), and it is **not fixable by
layout or per-instruction codegen**:
- **clang already minimizes** the per-guest-instruction host code (proven by rlwinm, and by the workflow
  analysis confirming CR-fusion / width / addressing levers are all optimizer-redundant). So you cannot
  shrink the hot path one instruction at a time.
- **The hot working set does not fit the DSB** (87 % MITE). PGO (layout-for-DSB) was neutral ⇒ the code
  is fundamentally too large / too branch-dense per 32-byte region to cache, regardless of layout.
  leaf-inlining *grew* code (+7.4 %), trading resteers for footprint → net neutral.
- The only way to shrink the hot path is to cut the **architectural overhead** that clang *can't* remove:
  the **ctx-register memory traffic** (every guest reg read/write is a load/store to the ctx struct,
  forced by the `(ctx,base)`-by-reference ABI and the spill-at-every-call requirement). That ~21 % is
  exactly what **GPR-as-local** would eliminate — and it would *also* shrink the code enough to start
  fitting the DSB, attacking **both** binding components (frontend + retiring) at once.

## The one real lever (blocked) + recommendation for next session
**GPR-as-local** (keep guest registers in host locals instead of the ctx struct) is the lever that
matches the measured bottleneck — but it is refuted by a **boot crash** because the setjmp/longjmp SEH
unwinder restores guest state from the ctx struct, and the `(ctx,base)` ABI uses ctx fields as the
inter-function argument channel. Making it work needs an **SEH state-save redesign**: sync host locals
↔ ctx only at the (rare) setjmp save points and exception landing pads, keeping the hot path local.
This is real recompiler engineering (the "open lever" from `sp_aslocal_plan`), and it is now
**the only lever with measured headroom** (theoretical ceiling ≈ 2× if frontend+backend stalls were
eliminated; realistically a chunk of the 39 % frontend + the ~21 % ctx-traffic retiring).

### HT-contention hypothesis — TESTED and REJECTED
The profile shows **`TimerThreadMain` ~17 % of total CPU** and Audio-worker mutex spin ~11 %. On a
6c/12t CPU the **front-end is shared between HT siblings**, so a spinning sibling *could* manifest as
the measured front-end starvation. Tested with `affinity_test.sh` (interleaved floor A/B, one boot,
live re-pin so load-drift cancels):
- First pass (Main isolated on cpu0 vs default, 30 s windows): isolated 21.5 vs default 20.3 (+1.2) —
  **but samples 12–21, within noise**, and confounded (isolating Main repacked 45 other threads).
- Clean retest (whole process on 6 physical cores = **no-HT** vs default 12 logical, 60 s windows, 4
  interleaved rounds): **no-HT median p10 = 20.2 vs default 21.0 (−0.8, neutral-to-worse).**

⇒ **HT contention is NOT the floor cap.** Restricting to physical-only cores does not help. The 39 %
front-end bound is the recompiled code's own MITE-delivery starvation, not a stolen shared front-end.
The TimerThread's 17 % is wasteful but runs on spare cores and does not gate the Main-thread floor.
(The absolute p10 in this session's combat ran ~20–21 on 60 s windows vs ~15 on ab.sh's 90 s windows —
load drifts between sessions; the *internal* A/B is what's valid.)

## New tools (tools/perf/)
- `topdown.sh` — Main-thread TMA L1 + stall-decomposition + load-hit + port-pressure during heavy combat (the measurement that re-baselined the diagnosis).
- `uops.sh` — Main-thread + whole-proc uops_retired / insn / DSB-vs-MITE mix (the µop-delta triage that killed movbe/nonvol fast).
- `annotate.sh` — per-instruction cycle attribution of the hottest recompiled function.
- `affinity_test.sh` — interleaved floor A/B under different CPU affinities (HT-contention test).

## DEAD ENDS added this session (do NOT retry — measured)
- `-mmovbe`/arch-flags on the exe: **raises** Main-thread uops (elision/fold loss). 
- rlwinm/rlwimi shift-mask special-casing: clang already canonicalizes ⇒ identical asm.
- Non-volatile guest RAM (naive): neutral perf + breaks correctness (no sync barriers). The *proper*
  version (non-volatile + `atomic_signal_fence` at sync/lwsync/eieio) would boot but perf is neutral
  (instruction count rises, not falls) — not worth it for this front-end-bound floor.
- All per-instruction "emit better C++" codegen levers generally: clang -O3 already minimizes.
