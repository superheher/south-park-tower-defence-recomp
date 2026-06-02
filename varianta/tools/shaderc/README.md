# Variant A renderer — shader toolchain

The title's 19 pixel shaders ship as `.updb` files (in `private/extracted/media/shaders/`) that carry the
**original D3D9 HLSL source** (ps_3_0) plus interpolator/constant/sampler metadata. The PM4->Vulkan renderer
uses them directly — **no Xenos-microcode translator needed** (validated 2026-06-02).

Pipeline: extract the HLSL from the `.updb` -> port D3D9-isms to Vulkan GLSL (`sampler2D`+`tex2D` ->
`sampler2D`+`texture()`, `: COLOR`/`: TEXCOORD0` interpolators -> `layout(location=reg)` per the `.updb`
`<Interpolator Register=...>`, sampler `sN` -> `set=0,binding=N`, `uniform floatN` constant -> push constant)
-> compile to SPIR-V with `libshaderc`.

- `compile.cpp` — GLSL->SPIR-V via libshaderc (declares the C API inline; dev headers aren't installed).
- `ported/*.frag` — hand-ported shaders. Pattern coverage so far: `Simple`/`SPTextured` (texture*color),
  `SPUntextured` (vertex color), `SimpleCol` (color*const), `SpMovie` (3 planes Y/U/V -> YUV->RGB,
  studio-range BT.601 — the movie-quad shader). The remaining 14 are mechanical variants of these.
- `build.sh` — builds the compiler and compiles every `ported/*.frag` to `out/<name>.spv`.

VERTEX shaders are NOT in the `.updb` (all 19 are `.psh`); the `.xbv` are compiled Xbox VS binaries. The
renderer will use handwritten generic vertex shaders per the common vertex layouts (screen-space UI, etc.).

⚠ The translator that consumes these (PM4 draw-state -> Vulkan pipeline + draws) is BLOCKED on real draws:
no currently-reachable title state builds textured PM4 draws — the intro emits only untextured rects, and
the menu/frontend is gated behind screen-setup INDIRECT-NULL blockers (sub_8215DE84 / 0x82292D08 / ...).
Real draws require getting the title to a live screen first (deep RE). See NIGHT-LOG cont.11.
