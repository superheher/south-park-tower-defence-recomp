# Draw-batching — Step 1 (find the dominant draw group): the working recipe

Status: **measurement not yet captured.** First attempt (2026-05-31) failed on build mechanics, not
on approach; tree was left clean (source baseline, production `.so` `1a3f6076` redeployed). This doc is
the de-risked recipe so the retry is cheap (~10-15 min) and reliable.

## Goal
Of the ~700-1700 draws in a heavy-combat frame, find which **group dominates** so step 2 batches the
biggest win first. Group key = (pixel-shader ucode hash, primitive type). Report top-8 by count with
%-of-frame and avg vertex count. Hypothesis to confirm/refute: per-glyph text + UI is the bulk.

## The two things that defeated the first attempt (DO THIS RIGHT)
1. **Ninja Multi-Config.** The SDK build (`CMakePresets.json`, generator `Ninja Multi-Config`) emits to
   `third_party/rexglue-sdk/out/linux-amd64/<Config>/librexruntime.so`. The install tree
   (`out/install/linux-amd64/lib64/librexruntime.so`) and the running game use the **Release** binary.
   A plain `ninja` / `cmake --build` with no `--config` builds the **wrong** config, so a "diag" build
   silently ships the unchanged production `.so`. **Always pass `--config Release`.**
   - **Build dir = `third_party/rexglue-sdk/out/build/linux-amd64`** — this is the configured cmake
     binary dir (has `CMakeCache.txt` + `build.ninja`). `out/linux-amd64/` holds ONLY per-config output
     subdirs (`Debug/`, `Release/`, …) with no cache, so building there fails `could not load cache`
     and silently leaves the production `.so` in place. (Verified 2026-05-31 — corrected from the wrong
     path an earlier draft had.)
   - Correct: `cmake --build third_party/rexglue-sdk/out/build/linux-amd64 --config Release --target install`
   - **Verify before running the game:** `strings <game-dir>/librexruntime.so | grep -c drawgroup` must
     be **> 0**. If it's 0, you deployed the wrong artifact — do not bother running.
2. **Don't clobber CXX flags.** If you add a compile macro via `-DCMAKE_CXX_FLAGS=...` you MUST keep the
   preset's arch flag, i.e. `-DCMAKE_CXX_FLAGS="-march=x86-64-v3 -D<macro>"`, else `thirdparty/snappy`
   fails on missing SSE/CRC intrinsics. **Better: avoid the macro entirely (see lean design below) so
   no reconfigure/full-rebuild is needed.**

## Lean instrumentation design (single-TU incremental build — no header edit, no -D macro)
Keep EVERYTHING in `src/graphics/vulkan/command_processor.cpp` using a **file-scope static**, gated at
**runtime by `getenv("REX_DRAWBATCH_DIAG2")`** (always compiled, inert unless the env var is set). This
recompiles ONE translation unit + relink (~1-3 min), not the whole SDK, and the revert is just restoring
the file. Editing the header (e.g. adding a member) would force a near-full rebuild — avoid it.

Anchors (verified 2026-05-31 against the current working tree, which carries prior perf mods):
- `VulkanCommandProcessor::IssueDraw` — `command_processor.cpp:3638`
- pixel shader: `active_pixel_shader()` available by `:3704`; identity = `pixel_shader->ucode_data_hash()`
  → `uint64_t` (decl `include/rex/graphics/pipeline/shader/shader.h:834`)
- `prim_type` is the `xenos::PrimitiveType` arg (enum `include/rex/graphics/xenos.h:44`: TriangleList=4,
  TriangleFan=5, …); per-draw size = `primitive_processing_result.host_draw_vertex_count`
- the actual draw is recorded just after the `// Draw.` anchor at `:4166` (`CmdVkDraw` 4170 /
  `CmdVkDrawIndexed` 4201) — accumulate right before/at that point, after a successful `UpdateBindings`
  (`:4024`).
- frame boundary: `EndSubmission` (`:5300`); frame closes at `is_closing_frame = is_swap && frame_open_`
  (`:5362`) — dump + clear the map there.
- log via `REXGPU_INFO(...)` so lines land in `out/build/linux-amd64-release/run.log` (prefix
  `[drawgroup]`).

Sketch (adapt names to the file):
```cpp
// file scope (top of command_processor.cpp; <unordered_map>/<vector>/<algorithm> already included)
namespace { struct DG { uint64_t hash=0; uint32_t prim=0; uint32_t count=0; uint64_t verts=0; };
  std::unordered_map<uint64_t, DG> g_dg; }
// in IssueDraw, right before the draw is recorded (after shaders + host_draw_vertex_count known):
if (getenv("REX_DRAWBATCH_DIAG2")) { auto* ps = active_pixel_shader();
  uint64_t h = ps ? ps->ucode_data_hash() : 0; uint64_t k = (h<<4) ^ uint64_t(prim_type);
  auto& g = g_dg[k]; g.hash=h; g.prim=uint32_t(prim_type); g.count++;
  g.verts += primitive_processing_result.host_draw_vertex_count; }
// in EndSubmission, inside the is_closing_frame branch:
if (getenv("REX_DRAWBATCH_DIAG2") && !g_dg.empty()) {
  std::vector<DG> v; uint32_t tot=0; for (auto&kv:g_dg){v.push_back(kv.second);tot+=kv.second.count;}
  std::sort(v.begin(),v.end(),[](const DG&a,const DG&b){return a.count>b.count;});
  REXGPU_INFO("[drawgroup] frame_draws={} groups={}", tot, v.size());
  for (size_t i=0;i<v.size()&&i<8;i++) REXGPU_INFO(
    "[drawgroup]   rank={} pshash={:016x} prim={} count={} pct={} avg_verts={}",
    i+1, v[i].hash, v[i].prim, v[i].count, tot? v[i].count*100/tot:0, v[i].count? v[i].verts/v[i].count:0);
  g_dg.clear(); }
```

## Run + measure
- Env-gating is confirmed: `tools/gamectl.sh` launches via `setsid env … ./south_park_td`, so
  `export REX_DRAWBATCH_DIAG2=1` **before** `gamectl.sh play` reaches the process (checked via
  `/proc/<pid>/environ`).
- Reuse `tools/perf/drawbatch_probe.sh`'s drive-to-dip logic: `gamectl.sh play` → wait `IN LEVEL` →
  press to spawn waves → poll `run.log` `pacing-diag` for fps<24 → hold ~15s → `gamectl.sh kill`.
- Always `gamectl.sh kill` + `rm -f /dev/shm/xenia_memory_*` first; ONE game instance at a time.
- Collect: `grep '\[drawgroup\]' out/build/linux-amd64-release/run.log`. Confirm `frame_draws` is in the
  hundreds-to-~1700 (i.e. you actually captured heavy frames) before trusting the breakdown.

## Revert (restore baseline)
Snapshot `command_processor.cpp` to `.bak` BEFORE editing; after measuring, copy it back and rebuild
`--config Release --target install`, then copy the install `.so` into `out/build/linux-amd64-release/`.
Confirm game-dir `librexruntime.so` md5 == `1a3f6076`. `git diff` in the SDK must be back to baseline.

## Harness gotchas (these caused most of the first attempt's thrash)
- A Bash that exits non-zero CANCELS later tool calls in the same message — run build/game steps ONE per
  message and end fallible Bash with `; true`. (Note: `grep -c` returns exit 1 when count is 0.)
- Never run two `ninja`/build invocations concurrently (they race and corrupt the build dir).
- Don't launch a 2nd game while one runs.
- `sleep` as a bare token is blocked in this harness; put waits in a script (`tools/perf/wait_for.sh`).
