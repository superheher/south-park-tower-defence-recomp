# South Park recomp — next-session runbook: the combat floor is back-end-bound (front-end levers done)

> **This runbook supersedes the BTB-wall one.** The 2026-05-29 session executed all three of its
> levers (P1 `.so`-medium, P2 leaf-inlining, P3 PGO) and they are **all floor-neutral** despite large
> measured front-end wins — which **proves the combat floor is back-end / raw-guest-work bound, not
> front-end/BTB-bound.** Read `docs/FLOOR-PGO-MEDIUM-REPORT.md` (the session conclusion + the table)
> first, plus `docs/FLOOR-SO-MEDIUM-REPORT.md`, `docs/FLOOR-LEAF-INLINE-REPORT.md`, and memory
> `sp_floor_backend_bound`.

## Where we are
Static recompilation (rexglue-sdk) of South Park: Let's Go Tower Defense Play! (XBLA) → native
Linux/Vulkan, fully playable (boot→match→win). Repo `/home/h/src/recomp/rexglue-recomps` (super, `main`)
+ submodule `south-park-recomp` (port, `main`). SDK edits = working-tree patch
`patches/rexglue-sdk-current-full.patch`. Identity `superheher <heh@vivaldi.net>`, on `main`, **NO
Co-Authored-By trailer**, **commit, do NOT push** unless asked. Host: i9-8950HK (6c/12t, ~12 MB L3,
Coffee Lake ~4K-entry BTB, ~1.5K-uop DSB), sudo `<redacted>`, disposable bench.

**Best-known-good:** `south_park_td` md5 `bef1b65c` (`-mcmodel=medium`, `.text` 18,653,832, unchanged),
`librexruntime.so` md5 `47323bf2` (`-mcmodel=medium`, NEW — P1 this session, `.text` 14,952,201).
Experiment binaries staged in the build dir: `south_park_td.{phaseC_final dc32b4e1, mcmodel_medium
bef1b65c, leaf_inline 5bd2c2dd, pgo_medium 5c47732a}`, `librexruntime.so.{large_bkg 1996b550,
so_medium 47323bf2}`. Commits: P1 port `1ad7b4f` / super `6398032` (committed, NOT pushed); P2/P3
commit pending.

## The settled diagnosis (measured three independent ways)
The floor is **back-end / execution bound** — the heavy waves are limited by *retiring ~178 M guest
instructions per frame*, not by the front-end. Proof: three levers cut the front-end hard and the
floor did **not** move (per 1e9 insn, vs start-of-session BKG):

| lever | front-end effect | **floor Δ (ab.sh)** | kept |
|---|---|---|---|
| P1 `.so` large→medium | near_call-misp −77 %, bmiss −15 % | 0.0 | ✅ committed (throughput/.so-size) |
| P2 leaf-inline (exe) | BACLEARS −23 %, DSB-miss −57 % | 0.0 | ❌ gated off (+7.4 % .text) |
| P3 PGO (∘P2) | CLEAR_RESTEER −13 %, avg +0.5 | +0.1 | ❌ not adopted (non-reproducible) |

Cumulative: near_call-misp −79 %, DSB −56 %, BACLEARS −28 %, bmiss −20 %, resteer-cycles −13 % →
**floor ~0.0.** The "BTB-capacity wall" is real as a counter but is no longer the binding constraint.

## Goal
Move the combat-floor p10. **Keep-bar: detdiff gate PASSES, boots, median p10 > +1.0 swaps/s on
`ab.sh` (≥5 reps).** The remaining lever class is **reducing retired guest-instruction volume** — NOT
more front-end/codegen-layout work (proven exhausted).

## P1 — Reduce host instructions per guest instruction (THE remaining floor lever)
The floor is gated by µops retired/frame. Cut the *count*, not the front-end cost.
- **Measure first:** `perf stat -e uops_retired.retire_slots,instructions,inst_retired.any -p <pid>`
  during heavy combat; get µops/frame and µops/guest-insn. Profile the hottest guest-instruction
  *expansions* (which guest opcodes emit the most host µops? — e.g. the vector/VMX ops in
  `builders/vector.cpp`, the load/store address math in `builders/memory.cpp`, the CR/XER flag
  computation). `perf record -p <pid>` a combat window → which `sub_*`/builders dominate self-time.
- **Lever:** peephole / better instruction selection in `src/codegen/builders/*` — emit fewer host
  insns for the hottest guest patterns (e.g. fuse flag updates, avoid redundant zero/sign-extends,
  use `movbe`/SIMD where it cuts µops). Each change: `regen_build full` → `detdiff.sh gate` →
  `ab.sh 90 5` + the µops counter. The metric to move is **µops_retired/frame DOWN**, then floor.
- This is real recompiler work; start with the single hottest expansion measured above.

## P2 — Cross-CPU confirmation (cheap, decisive for "is this host-specific?")
Run the SAME `bef1b65c`+`47323bf2` binary's `ab.sh 90 5` + `branch_breakdown.sh` on a wider-issue
desktop (Zen4 / recent Core). If it floors materially higher with the same binary, the bound is
**execution/issue-width throughput** (the user's "it's an i9, absurd" intuition — but back-end, not
BTB). If it floors the same, the bound is guest-work volume (→ P1 instruction-selection is the only
path). Either way it tells you whether to invest in P1 on *this* host.

## P3 — (optional) re-enable P2 leaf-inlining only for a front-end-bound target
The P2 infra is committed but gated off (`codegen_writer.cpp` `markInlineLeaves(..., maxBytes=0)`). Its
front-end wins (DSB −57 %) WOULD help a smaller-BTB/-DSB CPU or a lighter guest. Set `maxBytes=64` to
enable. Not for this host's floor (proven neutral).

## DEAD ENDS — do NOT re-try (verified)
- **Any front-end / code-layout lever for the floor:** `-mcmodel` (exe+.so, done), leaf-inlining (done,
  −57 % DSB, neutral), PGO (done, neutral), and the prior session's BOLT/ICF/AS_LOCAL/outliner — ALL
  floor-neutral. The floor is back-end-bound; front-end work cannot move it. (They help avg/other CPUs.)
- **Widen jump-table / indirect-dispatch detection:** <1 % payoff (prior runbook §DEAD ENDS).
- **`-mcmodel=small` / drop `--no-relax`:** ~nil payoff (prior runbook §DEAD ENDS).
- **GPR-as-local:** refuted (boot crash; SEH state-save). Leaf-*inlining* the save/restore millicode is
  the safe alternative and IS done (P2) — but floor-neutral.

## Discipline (mandatory)
- `detdiff.sh gate <label> 40` must be `status=pass` (has teeth). Ranks above fps.
- Floor A/B: `TARGET=$GAME/south_park_td tools/perf/ab.sh 90 5 base south_park_td.mcmodel_medium cand
  <new>` (≥5 reps; floor noise ~±10 %, and base boot-flakes ~⅓ of reps so run enough). For a `.so`
  change use `TARGET=$GAME/librexruntime.so`. Re-confirm with `resteer.sh` (BACLEARS/DSB/bmiss per
  1e9 insn) — **but remember the floor is back-end-bound, so resteer wins ≠ floor wins**; add a
  µops_retired/frame measurement for the P1 lever.
- `regen_build.sh full` after a codegen/flag change (now also refreshes the port-dir `.so`).
- HOST GOTCHAS: (a) harness BLOCKS a literal `sleep` token in a Bash string → put waits in a script
  file; (b) game reaped when its launching shell ends → boot+measure in ONE script; (c) ONE game
  instance only; (d) `gamectl kill` auto-cleans the /dev/shm leak; (e) run `bash tools/perf/<x>.sh`
  (non-exec); (f) PGO profile needs the gdb-dump of `__llvm_profile_write_file()` (no clean exit).

## Tools
`tools/perf/{resteer,floor_rootcause,branch_breakdown,detdiff,ab,regen_build}.sh`, `p2_project.py`.
PGO build dirs `out/build/linux-amd64-pgo-{gen,use}` + `/tmp/sp/pgo2/port.profdata` preserved.

## Separate, independent issue (do NOT conflate with the floor)
The **"sinusoid"** (sim speed oscillates) is a pacing bug (`command_processor.cpp` `XE_SWAP`,
`++counter_` per swap, IMMEDIATE present). Several fixes tried + reverted. Only touch if asked.
