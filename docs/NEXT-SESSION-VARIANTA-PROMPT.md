# Next-session launch prompt — variant A (XenonRecomp full-stack recomp)

Paste the block below to start the next session. It is self-contained.

---

Continue **variant A** (XenonRecomp full-stack recomp of South Park: Let's Go Tower Defense Play!) on
branch `experimental/hle-graphics-spike` in `rexglue-recomps/south-park-recomp`. Variant B (in-place HLE
graphics) was proven NO-GO (`docs/HLE-PHASE1-FINDINGS.md`): the XDK D3D is inlined (no hookable API) and
texture bindings exist only as PM4 — so we fell back to A, which re-recomps from the clean XEX2.

Read first: `varianta/README.md` (roadmap), `docs/HLE-PHASE1-FINDINGS.md` (why B is NO-GO), and the
memories `sp_varianta_bootstrap` / `sp_hle_phase0_progress`.

## State (committed `46aca48`, NOT pushed; machine state clean, prod `.so` untouched `1a3f6076`)
- Toolchain: `third_party/XenonRecomp` (fresh recursive clone of hedge-dev/XenonRecomp, NOT a
  submodule — untracked on disk; reproducible via the recipe below). Built:
  `third_party/XenonRecomp/out/build/{XenonAnalyse,XenonRecomp}`.
- My XenonRecomp edits (18 instructions added) live ONLY in that clone's `XenonRecomp/recompiler.cpp`,
  captured as `varianta/patches/xenonrecomp-sp-instructions.patch` (verified: applies cleanly to a
  pristine clone). If the clone is gone, re-clone + `git apply` the patch. If it's still on disk, the
  edits are already in it — don't re-apply.
- `varianta/sp_xenon.toml` (config, cribbed from RexGlue's analysis of this XEX), `sp_switch_tables.toml`
  (empty — XenonAnalyse found 0 jump tables), `varianta/ppc/` (93 generated C++ files; git-ignored,
  regenerable).

## Recompile (reproduce / refresh after editing recompiler.cpp)
```
cd third_party/XenonRecomp && cmake --build out/build --target XenonRecomp     # if recompiler.cpp edited
cd ../../south-park-recomp/varianta
../../third_party/XenonRecomp/out/build/XenonRecomp/XenonRecomp sp_xenon.toml ../../third_party/XenonRecomp/XenonUtils/ppc_context.h 2>&1 | grep -c Unrecognized
```
Current unrecognized: **10,960** (was 13,183; 18 safe scalar instructions done).

## Next steps (priority order)
1. **Close the instruction gap (~48 left).** Edit `third_party/XenonRecomp/XenonRecomp/recompiler.cpp`
   (add `case PPC_INST_*` — the decoder in `thirdparty/disasm/ppc-inst.h` already knows them all;
   RexGlue's `south-park-recomp/generated/default/*` implements every one = reference). After each batch,
   rebuild + re-run + update the patch. **Set up `XenonTests` FIRST** and validate against it — the
   remaining ones are NOT trivial:
   - VECTOR/VMX (~10.4k occ): `vslh` 3980, `vsubshs` 2291, `vsrah` 1630, `vspltish`, `vaddsws`, `vmaxsh`,
     `vminsh`, `vcfpuxws128`, `vcmpgtsh[.]`, `vpk{swss,swus,uhus,shss}[128]`, `vsububm`, `vrlh`,
     `vaddsbs`, `vsrh`, `vcmpequh[.]`, `vavg{uh,sw}`, `vslo[128]`, `vminsw`, `vsrab`, `vsubsbs`,
     `vsel128`, `lvehx`, `stvebx`. ⚠ XenonRecomp stores vectors with **reversed 16-byte element order**
     — match it (many are 16-bit-lane analogs of existing 32-bit ops).
   - SCALAR (~600, subtle): `bdzf` 436 / `bdnzt` (CTR-decrement branch — note the existing `BDNZF`
     hard-codes "assume eq"; pick the real CR bit from `operands[0] % 4`), `addc`/`addme`/`subfze`
     (XER carry — model on `ADDE`/`SUBFE`/`ADDZE`), `cror`/`crorc` (CR-bit logical).
2. **Jump tables.** XenonAnalyse found none (tuned for Unleashed). Author `sp_switch_tables.toml` from
   RexGlue's computed-jump targets (`config/sp_functions.toml`) + the switch statements already in
   RexGlue's `generated/default/*`, or extend XenonAnalyse's detection. Needed for `bctr` correctness.
3. **Host runtime** (the big piece — XenonRecomp ships none): kernel/xam, video (Vd* + present), XMA
   audio, SDL input, VFS, setjmp/SEH. RexGlue's runtime (`third_party/rexglue-sdk/src/`) is the 1:1
   behaviour reference; setjmp=0x8242EEA0 / longjmp=0x8242EA70.
4. **Native renderer**: Plume (crib Unleashed `gpu/video.cpp` for the D3D→RHI map) + port the 19 shaders
   (original HLSL in `private/extracted/media/shaders/*.updb`). Then a runnable build + boot.

## One-time: re-clone the toolchain (only if the on-disk clone is gone)
```
cd third_party && git clone --recursive --depth 1 https://github.com/hedge-dev/XenonRecomp.git
cd XenonRecomp && git apply ../../south-park-recomp/varianta/patches/xenonrecomp-sp-instructions.patch
CC=clang CXX=clang++ cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/build --target XenonAnalyse XenonRecomp
```

Commit each increment (author `superheher <heh@vivaldi.net>`, Co-Authored-By trailer); update the patch.
Be rigorous — validate instructions with XenonTests before trusting the recompile.
