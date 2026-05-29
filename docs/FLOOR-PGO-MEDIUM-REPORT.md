# P3 — PGO re-test on the medium tree + SESSION CONCLUSION: the floor is back-end-bound

**Date:** 2026-05-29 (autonomous session). **One-line:** PGO on the medium+leaf-inline tree is
**floor-neutral** (15.1 → 15.2) with a small **avg win (~+0.5)** and a tighter floor distribution —
confirming, for the third time this session, that the combat floor does not respond to front-end
optimization. **All three runbook levers (P1 .so-medium, P2 leaf-inline, P3 PGO) are floor-neutral**,
which together prove the floor is **back-end / raw-guest-work bound**, not BTB/front-end-bound.

## P3 procedure (composed with P2, per the runbook)
- Instrumented build: reconfigured `out/build/linux-amd64-pgo-gen` with `-fprofile-generate=/tmp/sp/pgo2`
  (dropped the prior `-fprofile-continuous`, which never flushed). 28.9 MB exe.
- Profile: the game has no clean exit, so `-fprofile-generate`'s atexit flush never fires. Collected
  via the documented workaround — gdb-attach the live PID and call `__llvm_profile_write_file()`
  (`ptrace_scope=0`); two dumps over ~150 s combat → valid 2.86 MB profraw →
  `llvm-profdata merge` → `port.profdata` (16,233 functions, total count 50.2 B).
- PGO-use build: `out/build/linux-amd64-pgo-use` with `-fprofile-use=port.profdata` (default GNU ld),
  **0 profile-mismatch warnings**. exe md5 `5c47732a`, `.text` 19,744,397 (vs leaf_inline 20,035,688 —
  PGO's hot/cold split shrank it slightly).
- Gate: `detdiff.sh gate pgo_medium 40` → **`status=pass reason=equivalent`**.

## Floor A/B (`ab.sh 90 5`, base = medium BKG `bef1b65c`, cand = PGO `5c47732a`, medium `.so` live)
| | base (medium) | pgo |
|---|---|---|
| **median p10** | **15.1** | **15.2** |
| p10 samples | 12.8 15.6 15.1 15.2 (1 stale) | 15.2 15.2 15.2 15.1 15.4 |
| avg | ~28.4 | ~28.9 |

**Floor Δ = +0.1 (NEUTRAL).** PGO's samples are markedly *tighter* (all 15.1–15.4 vs base's 12.8–15.6)
— it trims the deepest dips a little — and **avg rose ~+0.5** (PGO speeds the common/light frames). This
is the same shape as the prior large-model PGO test EXCEPT the floor is now neutral, not −0.5: the
medium model already removed the indirect-call mispredicts that PGO couldn't fix on the large tree.

## Resteer counters (`resteer.sh`, per 1e9 instructions, heavy combat)
PGO inherits P2's front-end wins and shaves resteer *stall cycles* a little more:
| metric | BKG medium | leaf_inline (P2) | PGO (P3∘P2) |
|---|---|---|---|
| BACLEARS.ANY | 28.61 M | 21.94 M | 22.04 M |
| INT_MISC.CLEAR_RESTEER (cycles) | 48.77 M | 44.99 M | **42.26 M** |
| branch-misses | 5.00 M | 4.62 M | 4.72 M |
| DSB_MISS | 76.66 M | 33.21 M | 33.78 M |

## Verdict — NOT adopted as BKG
Floor-neutral (fails the +1.0 keep-bar). Avg +0.5 and a tighter floor are real but modest, and a PGO
build is **not reproducible from source alone** (it needs a freshly-collected combat profile + the
separate `pgo-use` build path), so adopting it as the shipped BKG would break `regen_build`
reproducibility. The prior session reached the same conclusion (kept the medium exe, not PGO). The
instrumented/optimized build dirs + `port.profdata` are preserved for reference. PGO is the best
binary by **avg/smoothness**, floor-tied.

---

# SESSION CONCLUSION — the combat floor is back-end / raw-work bound (front-end levers exhausted)

| lever | what it changed | front-end effect (per 1e9 i) | **floor Δ** | kept? |
|---|---|---|---|---|
| **P1** `.so` `-mcmodel=medium` | .so indirect calls 142k→14k (−90%) | near_call-misp **−77%**, bmiss −15% | **0.0** | ✅ (strict throughput/.so-size win) |
| **P2** leaf-inlining (exe) | inline 1,679 leaves / 17,448 sites | **BACLEARS −23%, DSB −57%** | **0.0** | ❌ gated off (+7.4% .text, neutral) |
| **P3** PGO (∘P2) | profile-guided layout | CLEAR_RESTEER −13% (vs BKG), avg +0.5 | **+0.1** | ❌ not adopted (neutral, non-reproducible) |

**Cumulative front-end improvement** from the start-of-session BKG (medium exe + LARGE `.so`) to
PGO+P2: near_call-misp **−79%**, DSB-miss **−56%**, BACLEARS **−28%**, branch-misses **−20%**,
resteer stall-cycles **−13%**. **The combat-floor p10 moved ~0.0 across ALL of it.**

This is decisive. The prior runbook's "BTB-capacity wall" is real *as a counter* (BACLEARS is high),
but **moving below it does not raise fps**, because after the medium-model fix the recomp is **back-end
/ execution bound** — the heavy frames are limited by *retiring the ~178 M guest instructions per
frame*, not by fetching/predicting/decoding them. Reducing front-end stalls (BTB resteers, uop-cache
misses, mispredict flushes) cannot help when the execution ports are the bottleneck. Three independent
large front-end improvements yielding zero floor movement is the proof.

## STOP (per the runbook's stop condition) + corrected recommendation
The codegen **front-end** levers are exhausted for the floor. The runbook said to recommend confirming
the *BTB-capacity* hypothesis on a bigger-BTB CPU; the sharper, corrected recommendation is:
1. **The floor is execution-bound, not BTB-bound.** To move it, reduce *retired guest-instruction
   volume*: better instruction selection / peephole in the recompiler builders (fewer host µops per
   guest insn) — measure µops-retired/frame, target the hottest guest-insn expansions. This is the
   only remaining lever class with floor headroom.
2. **Cross-CPU check:** run the SAME binary's `ab.sh` on a wider-issue desktop CPU (Zen4 / recent
   Core). If it floors materially higher, it confirms execution/issue-width limited (the user's "it's
   an i9, this is absurd" intuition — but the bound is back-end throughput, not predictor capacity).
3. The separate **pacing "sinusoid"** is unrelated (do not conflate).

## Shipped state (new BKG)
- exe `south_park_td` md5 **`bef1b65c`** (`-mcmodel=medium`, `.text` 18,653,832) — unchanged from
  start-of-session.
- `librexruntime.so` md5 **`47323bf2`** (`-mcmodel=medium`, NEW this session, P1) — `.text` 14,952,201.
- P2 leaf-inlining infra: committed but **gated off** (`markInlineLeaves` maxBytes=0).
- Experiment matrix staged in the build dir: `south_park_td.{phaseC_final dc32b4e1, mcmodel_medium
  bef1b65c, leaf_inline 5bd2c2dd, pgo_medium 5c47732a}`, `librexruntime.so.{large_bkg 1996b550,
  so_medium 47323bf2}`.
