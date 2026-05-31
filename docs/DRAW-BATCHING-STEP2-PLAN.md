# Draw-batching — Step 2 plan (CP-side merge; the de-risked path)

Status: design only. Step 1 + the host-view gate are DONE (real data); this is the plan for the
render-affecting implementation, which is the **supervised** part (user must eyeball mid-combat shots).

## What the measurements settled
- Dominant heavy-combat group = ONE pixel shader `adf7088205c03df9`, TriangleList, ~20 verts/draw,
  ~816 draws/frame = **72.5%** of heavy-frame draws. (#2 `26d9e78bbcb29443` ~257 draws, 18.6%.)
- Those ~816 draws bind only **~43 distinct host pixel-texture view sets/frame** (~19:1 alias). So they
  are mergeable WITHOUT bindless/descriptor-indexing/shader-array — the XL path is unnecessary.
- Bindless was confirmed XL/NO-GO (Xenia's Vulkan path is bindful; translator has no texture-array mode).

## The approach (effort ~M–L): merge consecutive same-state runs into instanced draws
Group, **within submission order**, maximal runs of consecutive draws that share:
1. the same pixel-texture view set (one of the ~43), AND
2. the same VkPipeline (same vtx+pixel shader + render state), AND
3. the same render target / blend / depth / scissor / viewport dynamic state.
Emit ONE instanced draw per run (instanceCount = run length) instead of N separate draws. Because
instances render in instance order = submission order, **alpha-blend order is preserved** → this is the
ORDER-SAFE subset (no reordering). The CPU win = fewer PM4→Vulkan draw translations (the floor driver),
proportional to average run length.

### The one mechanism that still needs care: per-instance geometry
Geometry is NOT in vertex buffers — the (vertex) shader pulls it from shared memory via `vfetch` using
per-draw **fetch constants** (in the per-draw `kConstantBufferFetch` UBO). To merge N draws into one
instanced draw, each instance must use its own fetch constants:
- Add a per-instance fetch-constant record (SSBO indexed by `gl_InstanceIndex`, or a small per-instance
  vertex attribute), populated CP-side for the run.
- Narrow vertex-path translator change: index fetch constants by instance for the merged path (a shader
  variant or a uniform-gated branch). This is FAR narrower than the bindless texture rewrite — it touches
  only `vfetch` constant selection in the vertex stage, not texture declaration/sampling. Still a
  translator change ⇒ regression surface ⇒ gated hard.
- Pipeline layout currently has `pushConstantRangeCount=0`; feed the per-instance data via SSBO to avoid
  new push-constant plumbing.

## Open question — MEASURED 2026-05-31 — ANSWER: runs are SHORT/interleaved ⇒ safe merge is marginal
**Run length:** measured (lean diag, 2785 heavy frames, dip→14.6fps, key=(host pixel-view-set FNV,
VkPipeline), baseline restored). Result for the dominant group `adf7088205c03df9` (~808 draws/frame):
- runs ≈ 584/frame, **avg_run = 1.36, max_run caps ~9-11 every frame** (pipes 2-6).
- run-length histogram: **h1 = 85%**, h2_4 = 12%, h5_9 = 3%, h10_19 = 0.2%, **h20_49 = 0, h50p = 0**.
- ≥63% of dominant draws are length-1; ≤2% live in runs ≥10.

⇒ the title does NOT submit its UI/sprites in long same-texture runs; the texture-view-set flips almost
every draw. The **order-safe** consecutive merge therefore eliminates only ~224 draws/frame = **~1.37×**
on the dominant group (~20% of all draws), and only in the deepest dips.

**Arithmetic (projected from measurement, NOT yet A/B-built):** a deep-dip frame ≈ 66 ms (render ≈ 34 ms);
eliminating ~20% of draw calls — where draw-translate is only part of render (register-writes ~23%,
descriptors ~7%, etc., per cp_oncpu_profile) and instancing does NOT remove per-instance geometry fetch or
the ~43 descriptor binds — saves on the order of a few ms, **well under the ≥16 ms needed to cross a vblank
boundary**. Same arithmetic that made 7 prior CPU levers floor-neutral. So the safe merge is **projected
floor-neutral**, AND it still requires the render-risky per-instance-fetch-constant translator change +
supervised screenshot validation.

## VERDICT — the draw-batch lever is CLOSED by measurement (all variants)
- **Bind-elision within runs** (no shader change): negligible — avg_run 1.36 leaves almost no consecutive
  redundancy to elide.
- **Order-safe instanced merge** (alpha-correct): ~1.37×, projected floor-neutral (~few ms << 16 ms),
  needs a risky translator change. Bad trade even as an efficiency win (unlike the .so-only, byte-identical
  trace-writer/audio wins we kept).
- **Reorder-all-same-state merge** (the ~19× big collapse the host-view gate hinted at): **alpha-blend-
  UNSAFE** for this transparent UI/sprite group → visual corruption. NO-GO for correctness.
- **Texture-array / bindless**: XL, whole-renderer regression surface. NO-GO (separately confirmed).

⇒ The structural floor conclusion now holds **including** the last headroom lever: nothing safe crosses a
vblank boundary. See `sp_drawbatch_runlen` / `sp_drawbatch_feasibility` (memory) and
`FLOOR-STRUCTURAL-CONCLUSION-REPORT.md`.

## Caveat (re-analysis, 2026-05-31 — one variant is unsized, not provably closed)
The "all variants closed" above assumes the texture stays in the merge key. A **bounded texture array**
would remove it, making the relevant run length **pipeline-only** (only 2-6 pipelines/frame → much longer
consecutive runs → a bigger order-PRESERVING win than 1.37×, no reorder needed). That variant is
**UNSIZED**: the `[psize]` pipeline-only sizing diag was built + gate-verified but the run was BLOCKED
(the scripted drive topped out ~30-46 fps and the dominant group never rendered in a swap-closing frame;
only `drawbatch_probe.sh` reliably reaches the dip). It also has an **open correctness question** —
indexing a sampler array by a per-instance value (`gl_InstanceIndex`) is likely non-uniform and may need
`shaderSampledImageArrayNonUniformIndexing` (descriptor indexing) after all, not just a plain array.
So the lever is *mostly* closed, not provably closed in that variant. It stays the supervised
"do WITH the user" lever; size it (with a working drive-to-dip) and resolve the indexing question before
any build.

## Validation gate (mandatory; the supervised part)
- `tools/perf/detdiff.sh gate <label> 40` = pass/equivalent (catches determinism breaks).
- `tools/perf/smoke_shot.sh <tag>` → drive to combat → 3+ shots → **USER eyeballs** sprites/text/colors
  across several combat states (the deterministic gate will NOT catch a wrong-state blend/order glitch).
- Floor A/B `tools/perf/ab_both.sh 60 6` on the deep dip; keep-bar median p10 > +1.0 swaps/s.

## Honest payoff
Per the structural report: best case ≈ one vblank-boundary cross (15→20 fps) in the DEEPEST dips only;
it will NOT reach 60 fps and most of the time is guest logic, not render. The merge cuts only the render
half of a heavy frame, and only the dominant group's share of it. Marginal but possibly real at the floor.

## Build mechanics
See `DRAW-BATCHING-STEP1-RECIPE.md` (Ninja Multi-Config; build dir `out/build/linux-amd64` with
`--config Release --target install`; lean diag = file-scope static + getenv in command_processor.cpp;
prod `.so` md5 `1a3f6076`, keep a byte backup since rebuilds don't reproduce the md5).
