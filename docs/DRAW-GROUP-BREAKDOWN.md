# Draw-group breakdown of a heavy-combat frame (Step 1 result)

Status: **REAL `[drawgroup]` data captured 2026-05-31** from an instrumented Release `librexruntime.so`
(runtime-gated, `getenv("REX_DRAWBATCH_DIAG2")`), driven into genuine heavy combat. Source AND `.so` then
**reverted to exact baseline** (source md5 `8e5cf7acc751`, install + game-dir `.so` md5 `1a3f6076`); see
"Revert / baseline status" at the bottom.

Heavy regime confirmed: **max frame_draws = 1301**, thousands of frames in the **789–1301** range
(the recipe's "hundreds-to-~1700" heavy bar). 5089 frames dumped; **2253 frames had frame_draws >= 180**
and were used for the aggregate below. Drive method: place towers (DPAD+A), then advance overlapping
waves ~3 min (KB doc 72); fps fell to ~20 under load.

`prim` = `xenos::PrimitiveType` (xenos.h): **4 = TriangleList, 5 = TriangleFan, 6 = TriangleStrip,
13 = QuadList**. Group key = (pixel-shader ucode hash, prim). Per-frame top-8 (count, %-of-frame, avg
host vertices) computed in the `.so` and dumped at frame close in `EndSubmission`.

## Top draw groups — aggregate over the 2253 heavy frames (frame_draws >= 180)
Ranked by total draw calls summed across those frames. Grand total over the 2253 heavy frames =
**2,266,139 draw calls**; percentages are of that total. (the quantity Step 2 must cut.)

| rank | pshash             | prim            | total draws | % of heavy draws | avg_verts |
|------|--------------------|-----------------|-------------|------------------|-----------|
| 1    | `adf7088205c03df9` | 4  TriangleList | **1,642,356** | **72.5%**      | 20        |
| 2    | `26d9e78bbcb29443` | 4  TriangleList | 421,428     | 18.6%            | 64        |
| 3    | `ad00fd645578b33c` | 13 QuadList     | 87,820      | 3.9%             | 44        |
| 4    | `1737d76853c430b2` | 13 QuadList     | 43,980      | 1.9%             | 25        |
| 5    | `985bd5f3e3958e25` | 4  TriangleList | 24,548      | 1.1%             | 6         |

(rank 6+: `e925976a489af5a3` TriList 16,968 (0.7%); `c3b8c62b63e1bb9e` QuadList 11,144; `a4a965c189287b99`
prim8/RectangleList 10,695; `e2467bcffc6f4c1e` prim6/TriStrip 7,200 — each <=0.7%.)

Group #1 averages **729 draw calls per heavy frame** (1,642,356 / 2253).

### A heaviest frame (frame_draws = 1301, 9 groups), exactly as emitted to run.log:
```
[drawgroup]   rank=1 pshash=adf7088205c03df9 prim=4  count=872 pct=67 avg_verts=30
[drawgroup]   rank=2 pshash=26d9e78bbcb29443 prim=4  count=332 pct=25 avg_verts=71
[drawgroup]   rank=3 pshash=ad00fd645578b33c prim=13 count=48  pct=3  avg_verts=47
```
(Group #1 is rank-1 here too, at 67% of this 1301-draw frame.)

Cross-check — which group is **rank #1 in the most individual frames** (all 5089 dumped frames):
`adf7088205c03df9` is rank-1 in **2163** frames, `1737d76853c430b2` in 1371, `c3b8c62b63e1bb9e` in 902.
So group #1 dominates both by total volume and by per-frame rank.

## Interpretation
- **Group #1 `adf7088205c03df9` (TriangleList, avg 20 verts) is THE dominant cost and the Step-2 batching
  target.** It is 72.5% of all draw calls across heavy frames — ~729 draws per heavy frame on average,
  872 (67%) in a sampled 1301-draw frame — and ~3.9x the next group. ~20 verts/draw = a handful of small
  quads/strips per call, one shared pixel shader, emitted many hundreds of times per frame. This is the
  signature of **per-instance UI / text / sprite geometry** (combat damage numbers, HUD counters, many
  small on-screen sprites). It confirms the Step-1 hypothesis: per-glyph/UI/sprite draws are the bulk of a
  heavy frame.
- **Group #2 `26d9e78bbcb29443` (TriangleList, avg 64 verts, ~19%)** — a second large recurring TriList
  group, present in nearly every heavy frame; larger meshes (agents/effects). Secondary batching target.
- **Groups #3/#4 (`ad00fd645578b33c`, `1737d76853c430b2`, QuadList)** — quad-list UI/sprite batches,
  present in 100% of heavy frames at moderate volume.

## Step-2 recommendation
Batch **Group #1 (`adf7088205c03df9`, prim 4 TriangleList)** first: it alone is 72.5% of heavy-frame draw
calls, all sharing one pixel shader, each ~20 verts — a prime candidate to merge into one dynamic vertex
buffer + a few draws per frame. That collapses the dominant share of a heavy combat frame's CP draw load.
Group #2 (`26d9e78bbcb29443`) is the clear next target. Whether each group's draws also share a texture
(trivial merge) vs. need a texture array (harder) is the separate `drawbatch_probe.sh` tex-vs-pipe
question, independent of this count breakdown.

## Revert / baseline status
- Source `third_party/rexglue-sdk/src/graphics/vulkan/command_processor.cpp`: restored to baseline
  md5 `8e5cf7acc751`; **0** `[drawgroup]`/`getenv("REX_DRAWBATCH_DIAG2")`/`DrawGroupDiag`/`cstdlib`/
  `unordered_map` leftovers; `git diff` = 47 lines = ONLY the pre-existing WriteRegister redundant-write
  elision that this tree's baseline already carried (the production `.so 1a3f6076` was built from it).
- install + game-dir `librexruntime.so`: md5 **`1a3f6076f09147f8f550ebd446f99568`** (exact original
  production artifact), 0 `drawgroup` strings. NB: a from-source rebuild does NOT reproduce this md5
  bit-for-bit (ELF build-id/timestamps differ → a rebuild yields e.g. `fb71159b`); the exact original was
  restored from the preserved artifact `/tmp/librexruntime.so.PROD_BACKUP_1a3f6076`.
- No `south_park_td` instances running; `/dev/shm/xenia_memory_*` cleared.

## Method (reproduction) — corrected build path
Single-TU runtime-gated instrumentation in `command_processor.cpp` only (no header edit, no -D macro): a
file-scope `std::unordered_map<uint64_t, DrawGroupDiag>` in the existing anon namespace; in `IssueDraw`
after a successful `UpdateBindings`, accumulate per (pixel_shader->ucode_data_hash(), prim_type) the draw
count + `primitive_processing_result.host_draw_vertex_count`; in `EndSubmission`'s `is_closing_frame`
branch dump top-8 + clear. **Build dir is `third_party/rexglue-sdk/out/build/linux-amd64`** (NOT
`out/linux-amd64` — that path holds only the per-config output subdirs and has no CMakeCache, so building
it fails with "Error: could not load cache"; that was the original blocker). Command:
`cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --config Release --target install`; deploy
the install `.so` to `south-park-recomp/out/build/linux-amd64-release/`; run with `REX_DRAWBATCH_DIAG2=1`.
