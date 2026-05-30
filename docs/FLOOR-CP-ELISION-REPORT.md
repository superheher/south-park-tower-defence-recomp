# Cutting the CP floor: generic redundant-register-write elision

**Date:** 2026-05-30. **One-line:** The heavy-combat floor is the GPU command-processor's single-threaded
PM4→Vulkan translation, and that translation is dominated by the per-register write dispatch. New
instrumentation showed **~90 % of all per-register writes re-write the value the register already holds**.
Extending the redundant-write skip from shader-constants-only to ALL pure-state registers (a verified
8-entry side-effect blocklist) elides ~90 % of `WriteRegister` bodies.

> Reads on top of `FLOOR-CP-TRANSLATION-REPORT.md` (the floor = CP translation throughput) and the memory
> `sp_floor_cp_translation`. Prior base = **cpfence6** (exe `848f191c` + `.so` `f1d86a27`): de-spun + Main
> vblank-park + GetLoggerRaw cache + the *constant-only* unchanged-write skip. The +1.0 keep-bar reference
> is **bothfix** (exe `d4b0f50b` + `.so` `605ce3ee`).

## 1. Re-instrumentation (ground truth before optimizing)
Two measurements on the live **cpfence6** binary, mid-combat (heavy dip, fps 19–22):

**(a) Per-function CP dip profile** (`tools/perf/heavydip.sh`, `heavydip_cpfence6_2026-05-30.txt`).
This is strictly more informative than the coarse 3-bucket `[frame-diag]` split used last session (which
only said "translate = 72 %"), so it replaces it. Top CP-thread self-time (whole-proc %):

| CP-thread function | % whole-proc |
|---|---|
| `VulkanCommandProcessor::WriteRegister` | **5.74 %** |
| `ExecutePacketType0` (per-register TYPE0 write loop) | 2.73 % |
| `ExecutePacket` | 2.10 % |
| `CommandProcessor::WriteRegister` (base) | 1.93 % |
| `ExecutePacketType3` | 1.29 % |
| `radv_UpdateDescriptorSets` (libvulkan_radeon) | 1.35 % |

⇒ The translate cost is **dominated by the register-write dispatch** (`WriteRegister` + `ExecutePacketType0`
≈ 10 % of the *whole* process, the largest CP component). Draw issue (`radv_UpdateDescriptorSets`) is minor.
This points squarely at elision (lever 1) and devirtualize/bulk (lever 2), not draw batching.

**(b) Per-register write/unchanged histogram** (temporary `[reg-hist]` diag added at the top of
`VulkanCommandProcessor::WriteRegister`; dumps the busiest registers every ~2 s; raw in
`reghist_2026-05-30.txt`). Across every heavy 2-s window:

- **~90 % of ALL per-register writes were unchanged-value** (e.g. one window: writes=13,434,880,
  unchanged=12,138,095 = 90 %). The title re-sets the same state every draw.
- Hottest, highest-redundancy registers (idx = total(unchanged) in one window):
  - `5000/5001/5002` SHADER_CONSTANT_FLUSH_FETCH_0/1/2 — ~88–100 % unchanged (the single busiest)
  - `2102` VGT_INDX_OFFSET — 100 %; `2200/2203` RB_DEPTHCONTROL/HIZCONTROL; `2180/2181`
    SQ_PROGRAM_CNTL/CONTEXT_MISC; `2203` — all high redundancy (pure render state)
  - `4020–402F` float constants — high (already covered by the prior constant-only skip)
  - `4800–48BF` fetch constants — high, but **must NOT be skipped** (texture-cache side effect)

The redundant writes are overwhelmingly **pure render state (0x2xxx) + flush-fetch (0x5000-2)** that the
previous constant-only skip did NOT cover.

## 2. The must-not-skip side-effect set (enumerated + adversarially verified)
A redundant (unchanged-value) write is safe to elide ONLY for a register whose write merely stores into
`register_file_->values[]` and is consumed lazily at draw time. A multi-agent audit of the whole CP +
GPU subsystem (texture_cache, render_target_cache, pipeline_cache, shared_memory, draw_util) found the
eager-side-effect set is **exactly 8 entries** — and confirmed **no other eager per-write register
reaction exists anywhere** (all non-constant state is a lazy snapshot read at draw time):

| register(s) | idx | eager side effect |
|---|---|---|
| `SCRATCH_REG0..7` | 0x0578–0x057F | guest-memory writeback (interrupt-sync handshake) |
| `COHER_STATUS_HOST` | 0x0A31 | `|= 0x80000000` dirty-OR that arms `MakeCoherent()` cache invalidation |
| `DC_LUT_RW_INDEX..30_COLOR` | 0x1922–0x1925 | gamma-ramp sequential FIFO state machine |
| `SHADER_CONSTANT_FETCH_*` | 0x4800–0x48BF | texture-cache + vertex-buffer-residency invalidation (Vulkan override) |

Notes from the audit: `VGT_EVENT_INITIATOR/DRAW_INITIATOR/DMA_*`, `WAIT_UNTIL`, the `EVENT_WRITE*/MEM_WRITE/
REG_TO_MEM/COND_WRITE/VIZ_QUERY` registers all do their work in the *packet handler*, not in `WriteRegister`
— the embedded store is pure. `REG_RMW` feeds back through `WriteRegister`, so the 8-entry exclusion covers
it automatically. `GraphicsSystem::WriteRegister` (the MMIO CP_RB_WPTR doorbell) is a separate path that
writes the register file directly and is unaffected by a CP-level skip.

## 3. The change (lever 1: generic elision)
`src/graphics/vulkan/command_processor.cpp` — replaced the constant-only unchanged-write skip with a
generic one gated on a single inline predicate:

```cpp
static inline bool RegisterWriteHasSideEffect(uint32_t index) {
  return (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
          index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) ||
         (index >= XE_GPU_REG_SCRATCH_REG0 && index <= XE_GPU_REG_SCRATCH_REG7) ||
         (index >= XE_GPU_REG_DC_LUT_RW_INDEX && index <= XE_GPU_REG_DC_LUT_30_COLOR) ||
         index == XE_GPU_REG_COHER_STATUS_HOST;
}
// at the top of VulkanCommandProcessor::WriteRegister:
if (index < RegisterFile::kRegisterCount && register_file_->values[index] == value &&
    !RegisterWriteHasSideEffect(index)) {
  return;
}
```

`.so`-only change (exe stays cpfence6 `848f191c`); candidate `.so` = **elision1** `5276a282`.

## 4. Validation
- **Mid-combat screenshots** (`elision1_combat_{1,2,3}.png`, fps 35/29/18): render is pixel-correct —
  sprites, colors, HUD, the snowball-aura circle, transforms, text, the "SCRAPBOOK UPDATED" toast all
  correct, including during an 18-fps dip. No glitch.
- **Determinism gate** `detdiff.sh gate elision1 40`: **status=pass reason=equivalent** (errs=0, naninf=0,
  in_level=True, 142 markers, 16 pipelines) — behaviorally identical to the reference.
- **Floor A/B** `ab_both.sh 90 6` interleaved, two runs (median p10, heavy windows only):

| variant | run-A (base/cpf6/elis1) | run-B (base/elis1/elis2) | vs bothfix |
|---|---|---|---|
| base (bothfix `d4b0f50b`/`605ce3ee`) | 15.0 | 14.9 | — |
| cpf6 (cpfence6, constant-skip) | 15.9 | — | +0.9 |
| **elis1 (generic elision `5276a282`)** | **16.1** | **18.2** | **+1.1 / +3.3** |
| elis2 (+TYPE0 dispatch-skip) | — | 17.1 | +2.2 |

  **elis1 clears the +1.0 keep-bar in both runs** (+1.1, +3.3 vs bothfix; run-B per-rep deltas
  +2.8/+4.5/+3.2/+4.1/+3.2/+3.4, all ≥ +2.8). The worst-frame `min` also rose (base 10–12 → elis1 14–15).
  The floor is **noisy run-to-run** (the heavy-window combat intensity isn't perfectly controlled, so the
  absolute p10 drifts ~±2 between 45-min runs — base is the stable anchor at ~15.0), but elis1 is
  unambiguously and consistently above base.

## 5. Lever 2 (TYPE0 dispatch-skip) — measured FLOOR-NEUTRAL, dropped
`ExecutePacketType0` (2.73 %) calls the **virtual** `WriteRegister` per register; lever 1 still pays that
indirect dispatch for the 90 % it then early-returns from. **elis2** added a pre-filter in
`ExecutePacketType0` (same `RegisterWriteHasSideEffect` predicate, promoted to `command_processor.h`) that
`continue`s past the virtual call entirely for a redundant pure-state write — removing ~millions/sec of
indirect calls. It is **render-correct + gate-pass** but **floor-neutral**: median p10 17.1 vs elis1 18.2
(within the noise band, and slightly noisier), `avg` no higher. Removing the indirect calls did not move
the floor ⇒ the floor is NOT branch-prediction-bound at this point (the `-mcmodel=medium` fix already
eliminated the dominant indirect-call source; the per-register virtual call is minor and well-predicted).
**Decision: keep elis1 (single-file, in the Vulkan override), drop elis2** (it touches the base-class core
`ExecutePacketType0` path used by all backends for zero measurable benefit). `RestoreRegisters` and
`ExecutePacketType1` are cold (not in the dip top-30), so there was no other dispatch site worth touching.

## 6. Bottom line
The generic redundant-write elision is the **completion of the constant-skip** (now covering ALL pure-state
registers — ~90 % of every per-register write the title issues, vs the previous ~half). It is provably
behavior-preserving (8-entry verified side-effect blocklist), render-verified, gate-pass, and clears the
+1.0 keep-bar vs bothfix robustly — the best-measured combat-floor lever of the 7-session campaign. Its
*marginal* over the constant-only cpfence6 is small in the one head-to-head (16.1 vs 15.9 = +0.2),
consistent with the standing finding that the floor is CP-translation-**throughput**-bound and CPU-side
cuts have low floor-elasticity; but it is the correct, more-general form and a real CPU/power efficiency
win (90 % of `WriteRegister` bodies elided). Kept as the new working binary **elis1**: exe `848f191c`
(cpfence6, unchanged) + `.so` `5276a282`. Next levers remain on the *throughput* side: devirtualize/bulk
the bulk-constant path, draw batching/instancing for the many small sprites, or (hard) parallelize the
serial CP translation — the GPU HW is still 17–26 % idle, starved by the single CP thread.
