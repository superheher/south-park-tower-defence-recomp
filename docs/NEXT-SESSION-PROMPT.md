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
The 39 % front-end decomposes (resteer.sh, current BKG) into **three Coffee-Lake CAPACITY walls**, none
codegen-reducible: **(a) BTB capacity** — BACLEARS 3.56 B fetch-resteers (4 K-entry BTB can't cover
~157 K branch sites); **(b) RSB capacity** — returns mispredict **~22 %** (448 M of 598 M total
mispredicts; conditional only 2 %, near_call 6 % post-mcmodel) because the 16-entry return-stack-buffer
overflows on combat's deep guest call chains (guest `blr` IS a clean host `ret`, tail-calls ARE TCO'd —
this is *not* a codegen defect); **(c) DSB capacity** — 87 % MITE (1.5 K-µop uop-cache too small).
**Why every lever (this + prior session) is neutral:** all attack code size / layout / per-insn count,
but the binding limits are the *count* of branches/calls/stack-depth the hot path touches, which exceeds
this CPU's front-end structures — and you can't reduce that without deleting guest control flow. clang -O3
already minimizes per-insn codegen; the only reducible *retiring* bulk is ctx-register traffic (~21 %),
which is SEH/ABI-blocked AND wouldn't touch the BTB/RSB walls.

## Goal
Move the combat-floor p10. **Keep-bar: detdiff gate PASSES, boots, median p10 > +1.0 swaps/s on
`ab.sh` (≥5 reps).** The honest assessment after 6 sessions: on THIS i9-8950HK the floor (~15 p10) is at
the front-end **capacity** limit for this recompilation approach; no codegen/layout/flag/LTO lever moves
it. The two remaining moves, in priority:

## P1 (DO FIRST) — Cross-CPU check (cheap, decisive, no code change)
Run the SAME `bef1b65c`+`47323bf2` binary's `ab.sh 90 5` + `resteer.sh` on a Zen4 / recent-Core desktop
(far larger BTB, deeper RSB, bigger op-cache). If it floors **materially higher** → the floor is
this-CPU-front-end-capacity-bound, **not a recompiler defect** (the "it's an i9, absurd" intuition —
answer is "newer CPU"), and further in-tree codegen work is pointless. If it floors the **same** → the
bound is guest-work volume and only P2 can help. This host can't self-test; needs a second machine.

## P2 (large, partial, blocked) — GPR-as-local + SEH state-save redesign
Keep guest GPRs in host **locals** instead of the ctx struct → cuts the ~21 % ctx *retiring* traffic +
shrinks code. CAVEAT: it attacks the **Retiring** half, NOT the BTB/RSB capacity walls (branch/call
*count*), so it's **partial** — and the prior cr/xer/ctr-as-local (−16 % .text) was already floor-neutral,
cautioning that even a big size/retiring cut may not move this capacity-bound floor. Also refuted by a
**boot crash** (setjmp/longjmp SEH restores guest state from ctx; `(ctx,base)` ABI passes args via ctx) →
needs locals↔ctx sync only at setjmp/landing-pad/call-arg points. Real multi-session engineering; only
worth it if the cross-CPU check shows the i9 is the outlier AND retiring is confirmed to matter. If you
attempt it, do NOT leave the tree half-changed — it boot-crashes when partial. Measure with `topdown.sh`
(Frontend % + uops/insn must DROP) + `uops.sh`, not just fps. See `sp_aslocal_plan`/`sp_gpr_aslocal_nogo`.

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
- **Switching OS (Win10/macOS) / disabling mitigations:** REJECTED (research + on-machine). BTB/RSB/DSB are
  fixed silicon (OS-independent); IBRS is cleared in user-space + "RSB filling" is per-context-switch not
  per-syscall, so mitigations don't touch the guest-sim thread's prediction; `uksplit.sh` measured the
  Main thread at **97.9 % user / 2.1 % kernel** → `mitigations=off` upper bound ≤ ~2 % (won't clear +1.0).
  Win codegen is worse (`REX_PHYS_HOST_OFFSET` per access); macOS needs MoltenVK + a big port. Only the
  cross-CPU check (P1) is worth it. (Cheapest falsification: `mitigations=off` cmdline + reboot + ab.sh.)
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
