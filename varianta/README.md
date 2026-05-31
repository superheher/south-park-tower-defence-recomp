# Variant A — XenonRecomp full-stack recomp (bootstrap)

Chosen 2026-05-31 after the variant-B native-render NO-GO (`docs/HLE-PHASE1-FINDINGS.md`): this
title's XDK Direct3D is inlined with no hookable API, so HLE-in-place is blocked. Variant A re-recomps
from the clean XEX2 with the Unleashed-style stack (XenonRecomp CPU + XenosRecomp shaders + a native
renderer + a host runtime), which re-emits the guest *including* D3D — the clean basis for HLE.

> Status: **bootstrapped + recompile working + instruction gap being filled.** This is a multi-phase
> project (the spike estimated weeks); this dir holds the reproducible config + toolchain patches.

## Done (this session)
1. **Toolchain.** Cloned `hedge-dev/XenonRecomp` (recursive) → `third_party/XenonRecomp`; built
   `XenonAnalyse` + `XenonRecomp` (host clang 21 / cmake / ninja) at
   `third_party/XenonRecomp/out/build/`.
2. **Config (`sp_xenon.toml`).** Cribbed from the existing RexGlue analysis of this exact XEX (no
   byte-hunting): register save/restore from `generated/default/south_park_td_register.cpp`,
   `setjmp=0x8242EEA0` / `longjmp=0x8242EA70` from `south_park_td_manifest.toml` (SEH-fix root-caused).
3. **XenonAnalyse** on `private/extracted/default.xex` → `sp_switch_tables.toml`: **0 jump tables
   detected** — the documented "tuned for Sonic Unleashed" limitation (South Park's compiler patterns
   differ). TODO below.
4. **XenonRecomp** → generated **93 `ppc/*.cpp`** files (reached 100%). The XEX2 decrypts and recompiles.
5. **Instruction gap quantified + validated fillable.** The Unleashed-tuned emitter is missing ~60 PPC
   instructions South Park uses (decoder already supports them all — `thirdparty/disasm/ppc-inst.h` has
   the `PPC_INST_*` enums — so it is purely missing `recompiler.cpp` switch cases). Implemented the 10
   integer load/store-with-update variants (`patches/xenonrecomp-sp-instructions.patch`) → unrecognized
   dropped **13,183 → 11,821**. The extend→rebuild→recompile loop is proven.

## Remaining instruction gap (~11,821 occurrences, ~50 distinct)
RexGlue's recomp implements ALL of these — use `generated/default/*` (or Xenia) as the reference, and
validate with `XenonTests`. Watch the **reversed 16-byte vector element order** convention (README §Vectors).
- **VECTOR / VMX (~10,358)** — the bulk: `vslh` 3980, `vsubshs` 2291, `vsrah` 1630, `vspltish` 767,
  `vaddsws` 687, `vmaxsh` 277, `vminsh` 108, `vcfpuxws128` 73, `vcmpgtsh[.]`, `vpk{swss,swus,uhus,shss}[128]`,
  `vsububm`, `vrlh`, `vaddsbs`, `vsrh`, `vcmpequh[.]`, `vavg{uh,sw}`, `vslo[128]`, `vminsw`, `vsrab`,
  `vsubsbs`, `vsel128`, `lvehx`, `stvebx`. (Many are 16-bit-lane variants of already-implemented 32-bit ops.)
- **SCALAR (~1,463)**: `bdzf` 436 (+`bdnzt`) branch-decrement-conditional; `stfsu`/`lfsu`/`lfdu`/`stfdu`/
  `lfsux`/`stfsux` float load/store-update; `eqv` 165, `subfze[.]` 70, `addc` 52, `addme` 33 (carry/XER);
  `lhbrx` 18 byte-reversed load; `cror`/`crorc` CR ops.

## Then (the larger phases, per the spike report §6)
- **Jump tables.** XenonAnalyse found none → extend its detection for South Park's pattern, OR author
  `sp_switch_tables.toml` from RexGlue's known computed-jump targets (`config/sp_functions.toml`) +
  the switch statements already in RexGlue's `generated/default/*`. Without these, `bctr` jump-table
  functions recompile incorrectly.
- **Host runtime** (the big piece — XenonRecomp provides none): kernel/xam HLE, video (Vd* + present),
  XMA audio, SDL input, VFS, the setjmp/SEH model. RexGlue's runtime is a 1:1 reference for behaviour.
- **Native renderer**: port/reuse Plume (Unleashed's `gpu/video.cpp` is the D3D-API→RHI mapping
  reference) + the 19 shaders (original HLSL in `private/extracted/media/shaders/*.updb`).
- **Build + boot.**

## Reproduce
```
# toolchain (one-time)
cd third_party/XenonRecomp && git apply ../../south-park-recomp/varianta/patches/xenonrecomp-sp-instructions.patch
cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build out/build --target XenonAnalyse XenonRecomp
# analyse + recompile
cd ../../south-park-recomp/varianta && mkdir -p ppc
../../third_party/XenonRecomp/out/build/XenonAnalyse/XenonAnalyse ../private/extracted/default.xex sp_switch_tables.toml
../../third_party/XenonRecomp/out/build/XenonRecomp/XenonRecomp sp_xenon.toml ../../third_party/XenonRecomp/XenonUtils/ppc_context.h
```
`ppc/` (generated C++) is git-ignored — regenerate it; it is incomplete until the instruction gap is closed.
