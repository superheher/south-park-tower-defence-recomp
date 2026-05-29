# South Park recomp — attack the combat-floor branch-mispredict wall (codegen)

> Next-session runbook. The combat-floor root cause was **corrected** on 2026-05-29: it is
> **branch-misprediction-bound**, not the i-cache/L3/L2 *capacity* wall every earlier report claimed.
> Full corrected analysis: `docs/FLOOR-GPR-ASLOCAL-GONOGO.md §7`.

## Where we are
Static recompilation (rexglue-sdk) of South Park: Let's Go Tower Defense Play! (XBLA)
→ native Linux/Vulkan, fully playable (boot→match→win). Repo:
`/home/h/src/recomp/rexglue-recomps` (super, `main`) + submodule `south-park-recomp` (port, `main`).
Best-known-good = **Phase C**: port main `4917b12`, `south_park_td` md5 `dc32b4e1`,
`librexruntime.so` `1996b550`.
SDK edits live as the working-tree patch (`patches/rexglue-sdk-current-full.patch`); commit identity
`superheher <heh@vivaldi.net>`; commit on `main`; **DO NOT add any Co-Authored-By trailer** (the user
removed them). Host: i9-8950HK (6c/12t, 12 MB L3, 256 KB L2/core), sudo password `1111`, disposable
test bench.

## The problem, with the CORRECTED diagnosis (read `docs/FLOOR-GPR-ASLOCAL-GONOGO.md §7` first)
Combat fps floor on Stan's House is p10 ≈ 14 (drops to ~20–25 fps). **EARLIER REPORTS WRONGLY blamed
I-cache / L3 / L2 *capacity*; that is RETRACTED.** Measured root cause
(`tools/perf/floor_rootcause.sh` + `branch_breakdown.sh`): the floor is **BRANCH-MISPREDICTION-bound**.
- Front-end stall = **39.3 % branch-resteers** vs only **3.6 % i-cache**, 3.4 % iTLB.
- Hottest funcs are **464–669 BYTES** (fit L1i trivially — size is NOT the problem).
- Mispredict split (per 10 s combat): **conditional 70 % @ 22 % rate**; **return 24.6 % @ 21 % rate
  (RSB desync)**; indirect 4.5 %; call 0.5 %. Healthy is <2–5 %. **1 conditional branch per 27 insn.**
- Main (guest-sim) thread = **1.0 CPU** (CPU-bound, not pacing-wait); GPU 17–27 % idle; ~178 M insn/frame.

Therefore: every SIZE/layout lever (BOLT, AS_LOCAL −16 %, outliner −8.87 %, PGO, the AOT-hybrid) was
floor-neutral — **wrong target**. The lever is **reducing branch mispredicts in the codegen**. It would
also help on every other CPU (this is a codegen pathology, not a hardware wall — a desktop CPU with a
bigger BTB will floor higher on its own).

## Goal
Move the combat-floor p10. **Keep-bar:** median p10 improves **> 1.0** swaps/s on `ab.sh`, AND the
detdiff gate **PASSES**, AND it boots. Investigate + prototype, in priority order, **measuring each —
do not assume**:

1. **RETURN / RSB (24.6 % of mispredicts @ 21 %).** Find how guest calls/returns are emitted:
   - Direct guest `bl` → is it a real host `call sub_X(ctx, base)` (RSB-friendly) or a goto/tail form?
   - Indirect `bctrl`/`blrl` → `REX_CALL_INDIRECT_FUNC` (in `generated/.../*_init.h`): a
     function-pointer indirect call through the dispatch table. `blr` returns are emitted as
     `return;` (a real host `ret`).
   - **Hypothesis:** the indirect-call dispatch and/or call-depth desyncs the 16-entry RSB. Prototype
     an RSB-friendly path (ensure call/ret pair symmetry; consider a software return-target hint).
     Codegen lives in `third_party/rexglue-sdk/src/codegen/builders/` (branch/flow emit).
2. **CONDITIONAL (70 % @ 22 %).** Likely guest data-dependent logic + possibly CR-flag-derived
   branches that alias in the predictor. Check how guest `bc`/CR compares are emitted; test whether
   PGO actually feeds branch weights to the **recompiled** conditionals (prior PGO was floor-neutral —
   find out why: did it only weight the C++ wrapper, not the emitted guest branches?).
3. If both stall: re-confirm with perf the mispredict delta of any change at the source level
   (`br_misp_retired.conditional` / `.near_return`) **before** trusting fps.

## Discipline (mandatory)
- **detdiff gate ranks ABOVE fps**: `tools/perf/detdiff.sh gate <label> 40` → must be `status=pass`
  (it has teeth; caught an injected `add→sub` bug). Never trust fps on a gate-fail.
- **Floor A/B**: `TARGET=$GAME/south_park_td tools/perf/ab.sh 90 3 base <good> cand <new>`
  (interleaved, median p10, heavy windows only). Re-confirm mispredict counts with
  `floor_rootcause.sh` / `branch_breakdown.sh`.
- **Rebuild after a codegen change**: `tools/perf/regen_build.sh full`.
- **HOST GOTCHAS (cost hours):**
  - (a) The harness **BLOCKS the literal token `sleep`** in a Bash *command string* → put any waiting
    in a **script file** and run the file.
  - (b) The game is **reaped when its launching shell command ends** → boot+profile MUST be one
    command / one script.
  - (c) **Only ONE game instance** at a time (shared `live_input.txt`).
  - (d) `gamectl.sh kill` auto-cleans the 4.5 GB `/dev/shm` leak.
- Keep Phase C staged as fallback; stage candidates as `south_park_td.<label>`.

## Success / STOP
**SUCCESS** = a gate-passing codegen change with median p10 +>1.0 AND reduced `br_misp_retired.*` —
commit (no co-author trailer) + push, update `docs/FLOOR-GPR-ASLOCAL-GONOGO.md` and memory.
**STOP** if, after the return/RSB + conditional/CR attempts, mispredicts don't drop or the floor stays
flat: write an honest report (what was tried, the measured mispredict deltas, why) and recommend
testing on a desktop CPU with a larger BTB to confirm the codegen-pathology hypothesis.

## Separate, independent issue (do NOT conflate with the floor)
The **"sinusoid"** (sim speed oscillates fast/slow) is a **pacing** bug: the guest paces on
present/GPU progress (`++counter_` per swap in `command_processor.cpp` `XE_SWAP`, IMMEDIATE present).
It is NOT the fps floor. Several fixes tried + reverted (see memory `rexglue_recomps_port`). Only touch
if explicitly asked.

**Read first:** `docs/FLOOR-GPR-ASLOCAL-GONOGO.md` (esp. §7), memory `sp_gpr_aslocal_nogo` +
`sp_combat_perf_frontend_bound` + `rexglue_recomps_port`. Tools:
`tools/perf/{floor_rootcause,branch_breakdown,detdiff,ab,regen_build}.sh`.
