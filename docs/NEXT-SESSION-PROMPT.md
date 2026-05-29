# South Park recomp — next-session runbook: the combat floor is FRONT-END-DELIVERY + RETIRING bound
# (measured), the per-instruction codegen lever class is EXHAUSTED, and the one real lever is SEH-blocked

> **This runbook supersedes the "back-end-bound" one.** The 2026-05-30 session **measured the Main
> thread directly** (`tools/perf/topdown.sh`, heavy combat) and found **Frontend 39.1 %, Retiring
> 49.7 %, Backend only 10.1 %, L1-hit 99.9 %** — so the floor is front-end-delivery + retiring bound,
> NOT back-end/raw-work bound and NOT memory-latency bound. It then built+measured three µop/instruction
> levers (arch-flags/movbe, rlwinm special-casing, non-volatile RAM) — **all dud** — and rejected the
> HT-contention hypothesis. Read `docs/FLOOR-FRONTEND-REBASELINE-REPORT.md` first, plus memory
> `sp_floor_frontend_bound`. The prior `FLOOR-PGO-MEDIUM-REPORT.md` / `sp_floor_backend_bound` framing
> is corrected there.

## Where we are
Static recompilation (rexglue-sdk) of South Park: Let's Go Tower Defense Play! (XBLA) → native
Linux/Vulkan, fully playable (boot→match→win). Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`)
+ submodule `south-park-recomp` (port, `main`). SDK edits = working-tree patch
`patches/rexglue-sdk-current-full.patch`. Identity `superheher <heh@vivaldi.net>`, on `main`, **NO
Co-Authored-By trailer**, **commit, do NOT push** unless asked. Host: i9-8950HK (6c/12t, HT siblings
0/6…5/11, ~12 MB L3, ~1.5K-uop DSB, ~4K-entry BTB), governor=performance, sudo `<redacted>`, disposable bench.

**Best-known-good (UNCHANGED — no lever this session was worth keeping):** `south_park_td` md5
`bef1b65c` (`-mcmodel=medium`, `.text` 18,653,832), `librexruntime.so` md5 `47323bf2`
(`-mcmodel=medium`). Floor p10 ≈ 15.1 on `ab.sh 90 5` (deep-dip windows).

## The settled diagnosis (MEASURED, not inferred)
The Main/guest-sim thread during heavy combat (topdown.sh): **Frontend 39 % + Retiring 50 %**, Backend
10 %, mem-stall 9.8 %, **L1 hit 99.9 %**, **uop source 87 % MITE / 13 % DSB**, uops_executed ≈ 2.5/4
(back-end has spare capacity). `perf annotate` of the #1 hot fn `sub_821B9270` (12–16 %): cycles
**smeared ~evenly across ~35 instructions** incl. trivial ones & branches = the front-end-starvation
signature; **~21 % of it is ctx-struct loads/stores** (`%r14`-relative guest-register traffic).
**Why the prior session's front-end levers (mcmodel/leaf-inline/PGO) were neutral:** they change
layout/prediction, not the front-end's real problem. The hot working set doesn't fit the DSB (→MITE),
and clang -O3 **already** minimizes the per-guest-instruction host code, so you can't shrink the hot
path one instruction at a time. The only reducible bulk is the **ctx-register traffic**, which clang
can't remove (forced by the `(ctx,base)`-by-ref ABI + spill-at-every-call).

## Goal
Move the combat-floor p10. **Keep-bar: detdiff gate PASSES, boots, median p10 > +1.0 swaps/s on
`ab.sh` (≥5 reps).** Per the measured diagnosis, the lever must **shrink the hot path's host
instruction/uop count** (helps both Frontend-delivery and Retiring) — and the only place with headroom
is the **architectural ctx-traffic**, because per-instruction codegen is already optimal.

## P1 — GPR-as-local with an SEH state-save redesign (THE remaining lever; large but matched to the data)
Keep guest registers in host **locals** instead of the ctx struct. This cuts the ~21 % ctx
load/store traffic (Retiring↓) **and** shrinks the recompiled functions enough to start fitting the
DSB (Frontend↓) — it attacks **both** binding components. It is currently refuted by a **boot crash**:
the setjmp/longjmp SEH unwinder restores guest state from the ctx struct, and the `(ctx,base)` ABI uses
ctx fields as the inter-function argument channel.
- **Redesign:** sync host locals ↔ ctx only at the rare points that need it (setjmp save points,
  exception landing pads, and guest call boundaries for the argument regs), keeping the *hot straight-line
  path* on locals. See `sp_aslocal_plan` / `sp_gpr_aslocal_nogo` for the prior attempt's crash mode.
- **Validate hard:** this touches correctness foundation — `detdiff.sh gate` (40s) + a long playable
  soak + `ab.sh 90 5`. Measure with `tools/perf/topdown.sh` (Frontend % and uops/insn must DROP) and
  `uops.sh` (Main-thread uops↓), not just fps.
- This is real, multi-step recompiler engineering. If you don't have budget to complete+validate it,
  do NOT leave the tree half-changed — it boot-crashes when partial.

## P2 — Cross-CPU confirmation (cheap, decisive for "is this approach/CPU-bound?")
Run the SAME `bef1b65c`+`47323bf2` on a wider-issue desktop (Zen4 / recent Core, bigger DSB+BTB+issue
width). If it floors materially higher, the front-end-delivery bound is this-CPU-specific (Coffee Lake
MITE/DSB limits) and the recompiler approach is fine; if it floors the same, the host-instruction
volume is the wall (→P1 is the only path). This host can't self-test; needs a second machine.

## DEAD ENDS — do NOT re-try (MEASURED this session + prior)
- **Arch flags `-mmovbe -mbmi -mbmi2 -mlzcnt -mpopcnt` on the exe:** RAISES Main-thread uops +10 %
  (movbe forces byteswaps clang had elided for eq-compares/store-back, and defeats load-folding into
  ALU ops). `tools/perf/uops.sh` proved it. Wrong direction.
- **rlwinm/rlwnm/rlwimi shift-mask special-casing:** ZERO headroom — clang -O3 already canonicalizes
  `rotl64(dup)&mask` to identical `shl`/`shr`/`and` asm (micro-tested). Same for CR-fusion, dest-width,
  and addressing-fuse levers — all optimizer-redundant.
- **Non-volatile guest RAM** (drop `volatile` from REX_LOAD/STORE): neutral perf (hot-fn instruction
  count *rose*; guest loads rarely provably-alias so little CSE) AND **fails the gate** (sync/lwsync/
  eieio are no-ops, so volatile is the only barrier; removal hoists polling loads → boot/nav hang).
  The proper non-volatile+`atomic_signal_fence`-at-sync version would boot but perf is still neutral.
- **ThinLTO on the exe** (`-flto=thin`): cross-TU inlining of the hot mid-size call chain works +
  gate-passes, but floor-neutral (+0.3 over 8 reps = within noise) at +5.2 % .text + slower build.
  Same branch-removal-vs-code-growth wash as leaf-inline. Only useful for avg-smoothness, not the floor.
- **HT contention:** REJECTED — `affinity_test.sh` no-HT (6 physical cores) vs default (12 logical) is
  neutral-to-worse (−0.8). The 39 % front-end is the code's own MITE starvation, not a stolen sibling
  front-end. TimerThread's 17 % CPU is wasteful but runs on spare cores; doesn't gate the floor.
- **All prior front-end/layout levers** (mcmodel done+kept, leaf-inline, PGO, BOLT/ICF/AS_LOCAL/
  outliner): floor-neutral. They don't reduce host-instruction count or fit the DSB.

## Discipline (mandatory)
- `detdiff.sh gate <label> 40` must be `status=pass`. Ranks above fps.
- Floor A/B: `TARGET=$GAME/south_park_td tools/perf/ab.sh 90 5 base south_park_td.mcmodel_medium cand
  <new>` (≥5 reps; floor noise large + boot-flakes ~⅓, run enough). `.so` change → `TARGET=$GAME/librexruntime.so`.
- For any codegen change: characterize with `tools/perf/topdown.sh` (the bottleneck-class re-baseline),
  `uops.sh` (Main-thread uops/insn + DSB-vs-MITE), and `annotate.sh` (per-insn cycles). **resteer/DSB
  wins ≠ floor wins** (proven). The metric that matters now: **Frontend % and Main-thread uops/insn DOWN.**
- `regen_build.sh full` after a codegen/emitter change; `regen_build.sh port` after a header/flag-only
  change (faster). HOST GOTCHAS: (a) harness BLOCKS a literal `sleep` token in a Bash string → waits go
  in script files; (b) game reaped when its launching shell ends → boot+measure in ONE script;
  (c) ONE game instance only; (d) `gamectl kill` auto-cleans the /dev/shm leak; (e) `bash tools/perf/<x>.sh`.

## Tools (tools/perf/)
NEW this session: `topdown.sh` (Main-thread TMA/stall/load/port — the re-baseline measurement),
`uops.sh` (uops/insn + MITE/DSB triage), `annotate.sh` (per-insn cycle attribution), `affinity_test.sh`
(HT/affinity floor A/B). Plus prior: `{resteer,floor_rootcause,branch_breakdown,detdiff,ab,regen_build}.sh`.

## Separate, independent issue (do NOT conflate with the floor)
The **"sinusoid"** (sim speed oscillates) is a pacing bug (`command_processor.cpp` `XE_SWAP`,
`++counter_` per swap, IMMEDIATE present). Only touch if asked.
