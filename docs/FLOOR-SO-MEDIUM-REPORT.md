# P1 — `-mcmodel=medium` for `librexruntime.so` (and all SDK libs): strict throughput win, floor-neutral

**Date:** 2026-05-29 (autonomous session, follow-up to FLOOR-MCMODEL-MEDIUM-REPORT.md).
**One-line:** flipping the SDK global code model `large → medium` makes the runtime `.so`'s internal
call graph direct (indirect calls **142,767 → 14,244, −90 %**; movabs **221,677 → 9,126, −96 %**;
`.so .text` **17.55 → 14.95 MB, −14.8 %**). Whole-process execution mispredicts drop hard
(**near_call −77 %, branch-misses −15 %**) and average fps edges up, but the **combat floor is
unchanged** — the floor is gated by the recomp **EXE's** BTB resteers, not the `.so`/CP thread. Strict,
gate-passing, no-downside win → shipped as the new best-known-good `.so`.

## What changed
`third_party/rexglue-sdk/CMakeLists.txt:93` (directory-scope, applies to ALL SDK targets):
`add_compile_options(-mcmodel=large)` → `-mcmodel=medium`. This is the **comprehensive** fix:
the `.so` is composed of the OBJECT libs `rexcore/rexgraphics/rexaudio/rexui/rexinput/rexfilesystem`
+ the `rexruntime` SHARED lib — and the hot CP-thread functions (`CommandProcessor::WriteRegister`,
`TimerQueue::TimerThreadMain`) live in **rexgraphics**, NOT in rexruntime's own sources. The narrow
`target_compile_options(rexruntime PRIVATE -mcmodel=medium)` the runbook offered would have missed them;
the global flip covers every lib that composes the `.so`. The recomp EXE is built in the PORT tree
(`find_package`, not this directory) and already had medium via `rexglue_apply_target_settings`, so the
exe is **unchanged** (`south_park_td` md5 `bef1b65c`, `.text` 18,653,832).

The `.so` is PIC (`CMAKE_POSITION_INDEPENDENT_CODE ON`), so cross-module calls still go via GOT/PLT;
medium fixes the **intra-`.so`** call graph (PC-relative direct `call rel32`), which is the bulk of the
142 k. (Did not pursue `-fvisibility=hidden`/`-Bsymbolic` to de-PLT cross-module calls — the intra-`.so`
win is the dominant population and the floor verdict below makes further `.so` work low-value.)

## Disassembly (install `.so`, the deployed artifact)
| | large (`1996b550`, BKG) | medium (`47323bf2`, new BKG) |
|---|---|---|
| direct `call <sym>` | 84 | **130,235** |
| indirect `call *reg` | 142,767 | **14,244 (−90 %)** |
| `movabs` | 221,677 | **9,126 (−96 %)** |
| `.so .text` | 17,551,023 | **14,952,201 (−14.8 %)** |
| `.so` total | 21,518,392 | 18,070,840 |

## Correctness gate
`detdiff.sh gate so_medium 40` → **`DETDIFF status=pass reason=equivalent`** (142 markers, 8 assets,
0 errs, 0 NaN/Inf, 16 pipelines, in-level, fps_med 60). Boots and plays.

## Floor A/B (`ab.sh 90 5`, interleaved, TARGET=`librexruntime.so`, exe fixed at `bef1b65c`)
base = large `.so` (`1996b550`), cand = medium `.so` (`47323bf2`):

| | base (large) | cand (medium) |
|---|---|---|
| **median p10** | **15.1** | **15.2** |
| p10 samples | 15.4 14.4 15.1 14.9 15.1 | (r1 boot-skip) 15.4 15.2 13.1 15.4 |
| avg | ~29.3 | ~29.4–30.0 |

**Floor Δ = +0.1 (NEUTRAL, within noise).** Average frame rate edged up. Floor keep-bar (+1.0) NOT met.

## Resteer counters (`resteer.sh`, per 1e9 instructions, heavy combat)
| metric | BKG (large `.so`) | medium `.so` | Δ |
|---|---|---|---|
| **near_call mispredicts** | 4.86 M (rate 24.5 %) | **1.11 M (5.5 %)** | **−77 %** |
| **branch-misses** (exec flushes) | 5.87 M | **5.00 M** | **−15 %** |
| BACLEARS.ANY (BTB resteer) | 30.70 M | 28.61 M | −6.8 % |
| INT_MISC.CLEAR_RESTEER (cycles) | 48.64 M | 48.77 M | flat |
| DSB_MISS (uop-cache capacity) | 77.21 M | 76.66 M | flat |

## Why throughput improved but the floor didn't (the key nuance)
The `.so`-medium change removed ~128 k indirect calls and cut whole-process **execution mispredicts**
hard (near_call −77 %, branch-misses −15 %). That work is mostly on the **Command-Processor / runtime
thread** (and light/average frames), so **average fps rose** and the CP-side stalls fell. But the
**combat-floor p10** is set by the heaviest waves, which are bound on the **Main / guest-sim thread =
the recomp EXE**. The EXE's BTB resteers (BACLEARS) only fell **−6.8 %** from a `.so` change, and
BACLEARS is the dominant remaining front-end resteer. So the floor wall is **inside the exe's branch
footprint**, exactly where P2 (selective leaf-inlining of the exe's hot millicode) aims.

## Verdict — KEEP (strict win, floor-neutral)
−90 % `.so` indirect calls, −15 % whole-process flushes, −14.8 % `.so .text`, +avg, gate-pass, no
downside, helps every CPU (more on smaller-BTB parts). Floor unchanged (+0.1). Shipped as the new BKG
`.so` per the "commit successful work / partial win still worth committing" rule. The floor lever moves
to the EXE → see P2.

## Process note (a real gap fixed)
The exe loads `librexruntime.so` via `RUNPATH=$ORIGIN` (the port build dir), but `regen_build.sh` had
**no rule** to refresh that copy from the freshly-installed `.so` — so the first medium-`.so` build
silently did NOT reach the game (the port dir kept the old large `.so`). Fixed: `regen_build.sh full`
now copies `$SDK_INSTALL/lib64/librexruntime.so` → the port build dir after the port build. Any future
`.so`/SDK change now actually deploys.

## Provenance
- Change: `third_party/rexglue-sdk/CMakeLists.txt:93` `large→medium` (in the SDK working-tree patch
  `patches/rexglue-sdk-current-full.patch`).
- New BKG `.so` md5 `47323bf2` (install), `.text` 14,952,201. Exe `bef1b65c` (unchanged). Large-model
  `.so` fallback staged as `librexruntime.so.large_bkg` (`1996b550`); medium staged as
  `librexruntime.so.so_medium` (`47323bf2`).
- Tooling: new `tools/perf/resteer.sh` (P0 BTB-resteer isolation), `tools/perf/p2_project.py` (P2
  go/no-go projection), `floor_rootcause.sh` augmented, `regen_build.sh` `.so`-deploy fix.
