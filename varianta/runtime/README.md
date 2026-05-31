# Variant A — host-runtime scaffold (TASK 4)

This is the **scaffold** for the remaining large phase: a host runtime that the recompiled `ppc/`
links against. XenonRecomp provides the CPU translation + `ppc_context.h` (memory access + dispatch
macros) but **no runtime** — that's this directory.

## Recompiler ABI (from `ppc_context.h`)
- Every guest function is `PPC_FUNC(f)` = `void f(PPCContext& ctx, uint8_t* base)`.
- Guest memory is `base + guest_address`; `PPC_LOAD_U32(a)` = `bswap32(*(u32*)(base+a))`, etc.
  `PPC_MEMORY_SIZE` = 4 GiB.
- Indirect calls use `PPC_LOOKUP_FUNC(base, addr)` → a `PPCFunc*` read from a table laid out in
  guest memory at `base + PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (addr - PPC_CODE_BASE) * 2`.
- `PPCFuncMappings[]` (address → host function) is generated in `ppc/ppc_func_mapping.cpp`.

## Surface (enumerated from the recompiled output)
- **22,782** recompiled guest functions (`sub_*`, defined by `ppc/`).
- **474** kernel/xam imports to implement — see `IMPORTS-TODO.md` (Xam 44, Nt 30, Net 28, Ke 26,
  Rtl 21, **Vd 20** [video], Ex 11, …). RexGlue's `third_party/rexglue-sdk/src/` implements all of
  these for this exact title → **1:1 behaviour reference**.

## Files
- `IMPORTS-TODO.md` — the 474 imports, grouped, with the behaviour reference.
- `gen_import_stubs.py` — emits `import_stubs.gen.cpp`: a trap-stub `PPC_FUNC_IMPL(__imp__<name>)` for
  every import, so the image can **link with stubs** before the real runtime exists.
- `CMakeLists.txt` — globs `../ppc/*.cpp` + `runtime/*.cpp`, sets the include paths, C++20.
- `host_stub.cpp` — placeholder `main` + the `base`/func-table TODOs.

## Build (link-with-stubs)
```
# 1. regenerate ppc/ (it is git-ignored) — see ../README.md "Reproduce"
# 2. emit the import stubs
python3 runtime/gen_import_stubs.py
# 3. configure + build
cmake -S runtime -B runtime/out -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build runtime/out
```
> Status: scaffold compiles against the recompiler ABI; a full link-with-stubs has not been run yet
> (90 heavy TUs). The remaining work to *boot* is the real runtime (steps 1–5 in `host_stub.cpp`).

## Completion path (after link-with-stubs)
1. **Memory + loader** — reserve 4 GiB, load `default.xex` sections, populate the func-pointer table
   from `PPCFuncMappings[]`. (Crib UnleashedRecomp's loader; the XEX is already decrypted by
   XenonRecomp's `XexPatcher`/`Image`.)
2. **Kernel/xam imports** — implement `IMPORTS-TODO.md`, porting behaviour 1:1 from
   `rexglue-sdk/src/` (kernel, Vd* video + present, XMA audio, SDL input, VFS, setjmp/longjmp at
   `0x8242EEA0`/`0x8242EA70`).
3. **Native renderer** — Plume + the 19 shaders (`private/extracted/media/shaders/*.updb`); reference
   Unleashed's `gpu/video.cpp` for the D3D→RHI mapping. (Phase after the runtime.)
