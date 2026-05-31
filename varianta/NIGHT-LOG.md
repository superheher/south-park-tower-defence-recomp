# Variant A — Autonomous Night Session Log

Branch `experimental/hle-graphics-spike` · started **2026-05-31 22:31 MSK** · user asleep → fully autonomous.
Driver: `docs/NEXT-SESSION-VARIANTA-PROMPT.md`. **NEVER push.** Commit each increment as `superheher`.

## Baseline (verified at start)
- HEAD `925b657` on `86dfa35`. Toolchain clone `rexglue-recomps/third_party/XenonRecomp` @ `ddd128b`
  (upstream hedge-dev), with my **124-line `recompiler.cpp` diff == committed
  `varianta/patches/xenonrecomp-sp-instructions.patch`** (verified byte-identical). Binaries built.
- Unrecognized instructions = **10,960** (18 instructions implemented so far).
- Pre-existing dirty files in repo (`tools/perf/*.sh`, `tools/perf/detdiff/cand_*.json`) are NOT mine —
  keep every commit scoped to `varianta/` only.

## Guardrails (active)
- Do NOT touch `third_party/rexglue-sdk` or game-dir `librexruntime.so` (baseline `1a3f6076`).
- Do NOT bump the superproject `main` submodule pointer. Do NOT re-open variant B / Xenia CP opts.
- Do NOT run the game during TASK 1–3. Regenerate the patch after every `recompiler.cpp` change.

## Progress
- **[22:31]** Oriented; clone diff == committed patch (byte-identical). Started background rebuild of
  `XenonRecomp` to re-confirm the 10,960 baseline; reading `recompiler.cpp` to map emitter patterns.
- **[22:45]** Mapped emitter conventions. Key findings: (1) the full-vector byte-reversal on load
  *preserves each element's value* and merely relabels lane `i ↔ N-1-i` ⇒ per-lane symmetric ops
  (add/sub/shift/min/max/avg/compare) are reversal-agnostic (use the matching simde intrinsic or scalar
  loop, like existing `VSLW`/`VADDSHS`). (2) `setFromMask` is movemask-based: `imm=0xFFFF` for
  byte+halfword compares (each true halfword = 2 set bits), `0xF` via the `simde__m128`/float overload
  for word compares. (3) CR struct = `uint8_t lt/gt/eq/so`. Cross-referenced EVERY subtle op against
  RexGlue's recomp of this exact XEX (`generated/default/*`): carry ops, packs (operand order `(op2,op1)`),
  `vpkuhus` (unsigned-sat = `packus_epi16(min_epu16(op2,0xFF), min_epu16(op1,0xFF))` — matches RexGlue's
  16-lane scalar exactly), `vcfpuxws128`=`simde_mm_vctuxs`, `vslo`=`simde_mm_vslo`, `vsel128`≡`VSEL`.
- **[22:52] BATCH 1 (scalar) DONE — 10,960 → 10,363 (−597, exact).** Added `addc addme subfze bdzf
  bdnzt cror crorc`. All 7 verified ABSENT from histogram; build + recompile clean. Each emit cross-checked
  byte-for-byte vs RexGlue (carry = ADDE/SUBFE two-term form, alias-safe ordering; CR-bit branches select
  the REAL bit via `cr(BI/4).{lt,gt,eq,so}[BI%4]`, not hard-coded eq; `cror/crorc` = per-CR-bit logical).
  Patch regenerated, committed (`8e1e0dd`).
- **[23:05] BATCH 2 (vector analogs) DONE — 10,363 → 94 (−10,269, exact).** Added 26 vector ops:
  `vslh vsrh vsrah vsrab vrlh` (per-lane scalar shift/rotate), `vsubshs vmaxsh vminsh vminsw vsububm
  vaddsbs vsubsbs vavguh` (direct simde intrinsics), `vaddsws vavgsw` (int64-clamp scalar like `VSUBSWS`),
  `vspltish` (`set1_epi16`), `vcmpgtsh vcmpgtsw vcmpequh` (compare + CR6 `setFromMask`: 0xFFFF int-overload
  for halfword, 0xF float-overload for word), `vpkshss(128) vpkswss(128) vpkswus(128) vpkuhus(128)` (packs,
  operand order `(op2,op1)`; vpkuhus = `packus_epi16(min_epu16(·,0xFF), …)`), `stvebx` (single-byte element
  store). Aliased `vsel128`→`VSEL` (identical operands) and `lvehx`→`LVX` group (⚠ RexGlue treats `lvehx`
  as a FULL aligned vector load, not single-element — matched the validated reference, not the bare spec).
  Build + recompile clean; all 26 ABSENT. Remaining **94** = `vcfpuxws128`(73)+`vslo`(14)+`vslo128`(7),
  needing 2 `ppc_context.h` helpers (Batch 3). Committed (`d7f16e5`).
- **[23:20] BATCH 3 (helper-backed) DONE — 94 → 0.  🎯 TASK 1 COMPLETE: INSTRUCTION GAP FULLY CLOSED
  (13,183 → 0).** Added `vcfpuxws128` (float→uint saturate; unsigned analog of `VCTSXS`, `.u32` store) +
  `vslo`/`vslo128` (shift-left-by-octet), backed by 2 helpers ported into `XenonUtils/ppc_context.h`
  (`simde_mm_vctuxs`, `simde_mm_vslo`) verbatim from RexGlue's validated `rex::ppc` runtime (`vslo`
  simplified to a portable byte-loop — both SDK arch branches were identical). Recompile = **0 unrecognized**,
  90 generated TUs.
- **[23:24] TASK 3 (syntax-clean) DONE.** All 90 generated `ppc_recomp.*.cpp` + `ppc_func_mapping.cpp`
  pass `clang++ -std=c++20 -fsyntax-only` (parallel over all cores, **zero FAILs**) — validates both the new
  emitter cases AND the 2 new `ppc_context.h` helpers compile. The patch now spans `recompiler.cpp` +
  `ppc_context.h`. Committed (`ec0f24b`).
- **[23:55] TASK 2 (jump tables) — XenonAnalyse extended; 93 validated tables shipped (was 0).**
  Root cause of "0 detected": SP's MSVC-360 (a) inserts alignment NOPs mid-prologue and (b) emits
  `rlwinm` BEFORE the table-base `addi` (vs Unleashed) — defeating XenonAnalyse's fixed-sequence
  `SearchMask` + `ReadTable`'s hardcoded operand offsets. Fix (`XenonAnalyse/main.cpp`): NOP-tolerant
  `SearchMask`; role-based `ReadTableSP` (finds lis/addi/load/rlwinm by opcode, derives TYPE from the
  load op — order/NOP independent, reuses XA's address arithmetic); 2 SP-ordered patterns. → XA now
  detects **102 tables**. VALIDATION (safety): each candidate cross-checked vs RexGlue — 67 exact-match,
  35 all-labels-are-RexGlue-jump-targets, **0 misreads** (a wrong base shifts labels off all 98,729 loc
  targets; none did). Of 102, **93 are fully in-bounds → shipped** in `sp_switch_tables.toml`; **9 are
  boundary-limited → deferred**. Recompile = 0 unrecognized, **0 switch-case errors**, 93 `switch`
  statements emitted (were PPC_CALL_INDIRECT_FUNC). Re-ran TASK 3 syntax check WITH tables = all 90 TUs
  pass. Tooling + pipeline: `varianta/tools/jumptables/`. Patch now also spans `XenonAnalyse/main.cpp`.
  - ⚠ **FLAGGED (function-boundary problem, README §"Function Boundary Analysis" — no auto-solution):**
    the 9 deferred tables AND 161 pre-existing `// ERROR <addr>` (no-colon) CONDITIONAL-branch markers
    share one root cause — XenonAnalyse splits functions at computed-jump targets, so some branch/case
    targets land outside their function. Verified switch tables add ZERO errors (count identical with/
    without tables; switch-case errors `// ERROR:`-with-colon = 0). Fix path = manual function-boundary
    overrides or a boundary-analyzer extension (separate task).
