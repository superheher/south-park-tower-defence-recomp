# P2 — selective leaf-inlining: big front-end wins, FLOOR-NEUTRAL → the floor is back-end-bound

**Date:** 2026-05-29 (autonomous session). **One-line:** emitting tiny straight-line leaf functions
(esp. the `__savegprlr_N`/`__restgprlr_N` register save/restore millicode) as `always_inline` bodies
and routing their direct call sites to them cut **BACLEARS (BTB resteers) −23 %** and **DSB-miss
(uop-cache) −57 %** in heavy combat — large, real front-end improvements — yet the **combat floor and
average fps are UNCHANGED**. This is the decisive evidence that, after the medium-model fixes, the
floor is **back-end / raw-guest-work bound**, not front-end-bound. Gate-passes; +7.4 % `.text`.

## The lever (verified design in docs/P2-LEAF-INLINE-DESIGN.md)
Intra-recomp calls target the `weak,noinline` alias and are cross-TU, so dropping `noinline` does
nothing. The implemented mechanism: codegen marks inline-eligible leaves (`FunctionGraph::
markInlineLeaves`), emits their body ONCE as `[[gnu::always_inline]] static inline sub_X_inl` in a
shared generated header `{project}_inlines.h` (included at the end of `{project}_init.h`, visible to
every TU), turns the out-of-line `__imp__sub_X` into a one-line thunk `{ sub_X_inl(ctx,base); }` (so
the dispatch table + weak alias + overrides are unchanged), and routes eligible call sites to
`sub_X_inl(ctx,base)`. Eligibility (excludes unless ALL hold): no calls/tailcalls/jumptables, no
SEH/EH, no LR *data* read (`mflr`/`mfspr`-LR; a terminal `blr` is fine), no mid-asm hook in the body,
size ≤ 64 B. This is SAFE where the refuted GPR-as-local lever was not — the registers are still saved
to memory (SEH snapshots intact); only the *call* to the save/restore helper is inlined away.

## Generation + binary effect (threshold 64 B)
- **1,679** inline-eligible leaf bodies; **17,448** call sites routed to `_inl`. Dominated by the
  hot-path millicode: `__savegprlr_29` alone had 1,351 call sites; the `__save*`/`__rest*` family
  (every non-leaf prologue/epilogue) is the bulk. (The C++ predicate correctly includes `__restgprlr_N`
  — `mtlr` write + terminal `blr` are inline-safe — which the cruder static projection had excluded.)
- direct `call` 85,220 → **76,038**; `.text` 18,653,832 → **20,035,688 (+7.4 %)**.
- exe md5 `5bd2c2dd` (vs BKG medium `bef1b65c`). `.so` unchanged (medium `47323bf2`).

## Correctness gate
`detdiff.sh gate leaf_inline 40` → **`DETDIFF status=pass reason=equivalent`** (142 markers, 8 assets,
0 errs, 0 NaN/Inf, 16 pipelines, in-level, fps_med 60). The relaxed-LR predicate (allowing
`__restgprlr`) is behaviour-safe.

## Resteer counters (`resteer.sh`, per 1e9 instructions, heavy combat) — BIG front-end wins
| metric | BKG (medium exe+so) | P2 leaf_inline | Δ |
|---|---|---|---|
| **BACLEARS.ANY** (BTB resteer proxy) | 28.61 M | **21.94 M** | **−23.3 %** |
| **FE DSB_MISS** (uop-cache capacity) | 76.66 M | **33.21 M** | **−56.7 %** |
| FE L1I_MISS | 1.84 M | 1.50 M | −18 % |
| ICACHE_64B.IFTAG_MISS | 10.59 M | 9.44 M | −11 % |
| INT_MISC.CLEAR_RESTEER (stall cycles) | 48.77 M | 44.99 M | −7.8 % |
| branch-misses (exec flushes) | 5.00 M | 4.62 M | −7.5 % |

The **DSB −57 %** is the standout: I expected `.text` bloat to *worsen* uop-cache pressure, but inlining
the save/restore millicode removed the constant call→helper→`blr`→return churn on every function
entry/exit, making the hot instruction stream far more linear → the uop cache (and L1i) hold much more
of the hot path. So both BTB *and* uop-cache front-end pressure dropped substantially.

## Floor A/B (`ab.sh`, interleaved, TARGET=exe, medium `.so` live throughout) — NEUTRAL
base = BKG medium exe (`bef1b65c`), cand = leaf_inline (`5bd2c2dd`):

| batch | base median p10 | cand median p10 | Δ |
|---|---|---|---|
| `90 5` | 15.2 (3 clean, 2 boot-skips) | 15.5 | +0.3 |
| `90 6` | 15.1 | 15.0 | −0.1 |
| **combined (~10 samples each)** | **~15.2** | **~15.15** | **≈ 0.0 (NEUTRAL)** |

Average fps also neutral (~29.3 both). The first batch's +0.3 was small-sample optimism; the combined
~20 reps show the floor is unchanged within noise.

## Verdict & interpretation — the floor is back-end / raw-work bound
P2 delivered the **largest front-end improvement of any lever this session** (BACLEARS −23 %, DSB
−57 %) and the combat floor did **not move at all**. Combined with P1 (.so-medium: near_call
mispredicts −77 %, also floor-neutral), this is decisive: **after the medium-model fixes the recomp is
no longer front-end-bound — it is back-end / execution bound**, retiring the sheer ~178 M guest
instructions per heavy frame. Reducing fetch/predict/decode stalls (BTB, DSB, mispredicts) cannot
raise fps when the bottleneck is the volume of work the execution ports must retire. The "BTB-capacity
wall" the prior runbook chased is real *as a counter*, but moving below it does not help here because a
different (back-end) wall is now binding.

**Keep decision:** P2 FAILS the +1.0 floor keep-bar (neutral) and costs +7.4 % `.text` with no fps
benefit on this host/workload, so per the project rule (revert floor-neutral code-growth levers; the
prior session reverted PGO/BOLT/ICF/AS_LOCAL on the same grounds) it is **gated OFF by default**
(the `markInlineLeaves` size threshold defaults to 0 = disabled). The infrastructure is correct,
gate-passing, and preserved — its front-end wins (esp. DSB −57 %) WOULD help a front-end-bound target
(smaller-BTB/-DSB CPU, or a less execution-heavy guest), so it is kept available, not deleted. Enable
by raising the threshold (e.g. 64) in `codegen_writer.cpp`.

## What would actually move the floor (next directions)
Not front-end codegen. The floor is set by retired guest-instruction volume, so the levers are:
1. **Reduce host instructions per guest instruction** (better instruction selection / peephole in the
   recompiler's builders) — fewer retired µops for the same guest work.
2. **Reduce guest work executed per frame** (game-logic / sim-rate level — out of scope; tied to the
   separate pacing "sinusoid" issue).
3. Confirm the back-end-bound thesis on a wider-issue desktop CPU: the SAME binary should floor
   materially higher if it is execution-port/throughput limited (more so than from a bigger BTB).

## Provenance
- Codegen change (SDK working-tree patch): `function_node.h` (+`inlineLeaf_`/`isInlineLeaf()`),
  `function_graph.{h,cpp}` (`markInlineLeaves` + emitCpp inline/thunk split), `function_types.h`
  (`EmitContext::inlinesOut`), `codegen_writer.cpp` (mark + write `{project}_inlines.h`),
  `builders/context.cpp` + `builders/control_flow.cpp` (route `_inl` call sites),
  `resources/templates/codegen/init_h.inja` (`STATIC_INLINE_REX_FUNC` macro + include).
- Build: leaf_inline exe md5 `5bd2c2dd`, `.text` 20,035,688, staged `south_park_td.leaf_inline`.
- Tool: `tools/perf/p2_project.py` (static go/no-go projection).
