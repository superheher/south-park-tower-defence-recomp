# Next-session launch prompt — HLE graphics (variant B), native-render phase

Paste the block below to start the next session. It is self-contained.

---

GO — continue variant B (HLE graphics) on branch `experimental/hle-graphics-spike` in
`rexglue-recomps/south-park-recomp`. Phase-0 *identification* is essentially done and committed
(4 commits up to `aba68f1`, NOT pushed). This session starts the **native-render phase**.

Read first (in order): `docs/HLE-PHASE0-PROGRESS.md` (full Phase-0 log + findings),
`docs/HLE-GRAPHICS-SPIKE-REPORT.md` (the GO decision), and the memory
`sp_hle_phase0_progress` / `sp_hle_graphics_spike`. The reusable provenance probe is
`docs/hle-doorbell-provenance-probe.patch`.

## State (clean)
- Override shim layer lives in `src/hle_graphics.cpp` (built INTO the `south_park_td` exe).
  Present (`sub_821BFF48`) and the render pass `sub_821CC830` are intercepted (pass-through +
  gated diagnostics). The override mechanism is PROVEN: a strong `REX_EXTERN(sub_X){...}` in
  `src/` beats the generated weak guest alias and catches BOTH direct and indirect/vtable calls.
- SDK is at baseline; game-dir `librexruntime.so` md5 = `1a3f6076`. App build:
  `cmake --build out/build/linux-amd64-release --target south_park_td` (~20s incremental).
- SDK build (only if you re-apply the probe patch): `cmake --build
  third_party/rexglue-sdk/out/build/linux-amd64 --config Release --target rexruntime`, then copy
  `third_party/rexglue-sdk/out/linux-amd64/Release/librexruntime.so` to the game dir. **Keep a
  byte backup of the baseline .so first.**

## Established facts (measured, don't re-derive)
- Command processor is ASYNC (worker thread parses PM4) — no guest stack at IssueDraw.
- The title's XDK Direct3D **inlines** draw/state packet-builds into its render functions; there
  is NO per-packet D3D-API function to hook. The kick (CP_RB_WPTR doorbell) = guest `sub_821C6600`.
- Render call tree (heavy frame): main `sub_824497B8` → `sub_82249638`→`678`→`970`→`AD0` →
  `sub_82150970` (frame render, also a Present caller) → `sub_8212DFB8` → `sub_821BEF00` →
  **`sub_821CC830`** → reserve `sub_821C6D58` → kick `sub_821C6600`.
- **`sub_821CC830` is the variant-B hook target**: a frame-critical per-frame render pass
  (stub-test `REX_HLE_STUB_CC830=1` blanks the whole frame + stops swaps). It is ~278 lines and
  calls `sub_821C5BA8` ×8 + the reserve once — so the dominant 72.5% group's draws are emitted
  deeper (inside `sub_821C5BA8` and/or inline loops), NOT 729 calls in `sub_821CC830` itself.
- The exact dominant-shader (`adf7088205c03df9`) draw instruction is NOT yet pinned — the
  async/inline wall blocks a clean shader→site map. Don't burn time re-chasing it; prefer
  reimplementing the pass.

## This session's plan (native render — large, do incrementally)
1. **RE the draw inputs of `sub_821CC830` → `sub_821C5BA8`.** Read the recompiled bodies
   (`generated/default/south_park_td_recomp.6.cpp`, `DEFINE_REX_FUNC(sub_821CC830)` /
   `sub_821C5BA8`). Determine what each draw reads from guest memory: vertex/index data,
   texture handle(s), the bound pixel shader, blend/render state, transform. Map the small
   surface (DRAW-GROUP-BREAKDOWN.md: ~4 prim types, ~11 hot pshaders, textured/untextured quad
   sprites, alpha blend, one RT, 1280×720).
2. **Decide the native backend path.** Either reuse RexGlue's existing Vulkan backend
   (`third_party/rexglue-sdk/src/graphics/vulkan/`, but it's CP-coupled/bindful) or a minimal
   standalone Vulkan path / Plume for the 2D sprite surface. Port the `SPHud`/`SPTextured`
   shaders (original HLSL recoverable from `private/extracted/media/shaders/*.updb`).
3. **Override `sub_821CC830` natively** for the dominant draws, feeding the chosen backend
   directly; keep PM4 as the fallback for everything else (true hybrid, reversible).
4. **Benchmark** heavy-frame FPS native-vs-PM4 — the commit-or-fallback datum (report §7).
   If native rendering of the pass is intractable, variant A (XenonRecomp) is the standing
   fallback (clean XEX2).

## Harness gotchas
- `gamectl play` nav is flaky (`camp_diagram` often missed) but heavy load (fps ~15–25) is still
  reached even when it reports "could not enter". Clean `/dev/shm/xenia_memory_*` between runs.
  Boot intermittently deadlocks (retry). `pkill`/`gamectl kill` exits non-zero — run it ALONE.
- App logs to `run.log` via the rex logger (`REXLOG_INFO`), NOT stderr. Diagnostics are
  getenv-gated (`REX_HLE_*`). Symbolize a host RVA: exe is non-PIE, `nm -n --defined-only
  south_park_td`, find the `sub_*` whose addr ≤ RVA + 0x400000.
- If you re-apply the provenance probe: capture in the MMIO path must be async-signal-safe
  (NO dladdr/mutex/malloc — they deadlock the linker lock); symbolize from the vsync worker.
  Kick-backtrace sampling denser than ~256-kick spacing starves the guest and trips the boot
  deadlock.

Commit each increment (author `superheher <heh@vivaldi.net>`, Co-Authored-By trailer). Be
rigorous — measure before claiming; present hypotheses as hypotheses.
