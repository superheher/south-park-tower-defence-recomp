# Variant A — Morning Report (autonomous night session, 2026-05-31 → 06-01)

Branch `experimental/hle-graphics-spike` · **NOT pushed** · prod `.so` `1a3f6076` untouched ·
`rexglue-sdk` untouched · superproject pointer not bumped. Full timeline in `NIGHT-LOG.md`.

> **⚡ LATEST (2026-06-01 continuation, +8 commits `8b6d18b..e405734`):** the runtime now **boots
> multithreaded** well past where this report ends. The heap-init pause-point was a wrong
> NtAllocateVirtualMemory ABI (not missing structs); fixed → full KPROCESS/KTHREAD/KPCR + XEX-TLS env
> → real threading + a Vd* vblank pump get it past the GPU spin → a **cooperative execution token**
> makes it deterministic → it's now grinding the import cascade in the user/profile subsystem init
> (frontier: `XamUserReadProfileSettings` → file VFS → GPU CP → renderer). **See `NIGHT-LOG.md`
> "Session 2026-06-01 (continuation)" + "CURRENT FRONTIER" for the authoritative state + next-steps.**
> Everything below is the earlier (deterministic-front + first-boot) phase.

## TL;DR
The entire **deterministic front is done**: the XenonRecomp recompiler now handles **every** instruction
South Park uses (gap 13,183 → **0**), its jump tables are recovered (**0 → 93** validated), and **all 90
generated TUs compile** (`-fsyntax-only`). The host runtime is **scaffolded, fully enumerated (474
imports), and links-with-stubs into a 103 MB executable** (0 undefined symbols).

**Update (2026-06-01, host-runtime phase started):** the runtime now **boots the guest** — it reserves
4 GiB, loads the XEX, populates the 22,782-entry dispatch table, and the guest entry `0x824499A0`
executes real code. Imports are implemented via a scalable harness (weak trap-stubs in
`import_stubs.gen.cpp` overridden by strong impls in `runtime/kernel.cpp`); `NtAllocateVirtualMemory`
+ `KeGetCurrentProcessType` + the critical-section trio are done. The guest now executes into the
**CRT startup (`_xstart`)** and reaches the CRT **heap initialization**, which fails on null-based
pointers read from the (still-zeroed) guest **thread/process structures** — pinpointed via gdb
backtraces (the KPCR/current-thread link is verified working). **The autonomous loop is paused here**:
the deterministic front (TASK 1–4) is a complete committed milestone, and the remaining boot is the
genuine multi-week tail — better advanced as focused systematic work (populate the X_KTHREAD/KPROCESS/
default-heap environment → XEX TLS static init → the import cascade → video → renderer; full plan +
"resume" instructions in `NIGHT-LOG.md` "PAUSE POINT"). Re-run `/loop …` to resume autonomously.

## What landed
### TASK 1 — instruction gap closed: 13,183 → 0  ✅
Added the ~51 missing emitter cases to `recompiler.cpp` (+ 2 helpers to `ppc_context.h`), in 3 verified batches:
- **Scalar (7):** `addc addme subfze bdzf bdnzt cror crorc` — carry ops on the `ADDE`/`SUBFE` two-term
  pattern; CTR-decrement branches select the **real** CR bit (`cr(BI/4).{lt,gt,eq,so}`), not hard-coded eq.
- **Vector (26+):** `vslh vsrh vsrah vsrab vrlh` (per-lane shifts/rotate), `vsubshs vmaxsh vminsh vminsw
  vsububm vaddsbs vsubsbs vavguh` (simde intrinsics), `vaddsws vavgsw` (int64-clamp), `vspltish`,
  `vcmpgtsh vcmpgtsw vcmpequh` (+ CR6), `vpk{shss,swss,swus,uhus}[128]` (packs), `stvebx`; aliased
  `vsel128`→`VSEL`, `lvehx`→`LVX`. Helpers `simde_mm_vctuxs`, `simde_mm_vslo` ported into `ppc_context.h`.
- **Trajectory:** 13,183 (start of project) → 10,960 (prior session) → 10,363 → 94 → **0**.
- Every non-trivial op was cross-referenced against RexGlue's recompilation of this exact XEX.

### TASK 2 — jump tables recovered: 0 → 93  ✅ (with caveat)
Extended XenonAnalyse (`XenonAnalyse/main.cpp`): NOP-tolerant `SearchMask`, **role-based `ReadTableSP`**
(identifies prologue ops by opcode, not fixed offset), and SP-ordered patterns. Root cause of XenonAnalyse's
"0 detected": SP's MSVC-360 inserts alignment NOPs and reorders `rlwinm`/`addi` vs Unleashed. → detects
**102** tables; cross-validated every one against RexGlue (0 misreads); shipped the **93 fully-in-bounds**
in `sp_switch_tables.toml`. 93 `bctr` sites now emit real `switch` statements (were indirect calls).

### TASK 3 — generated C++ stays syntax-clean  ✅
All 90 `ppc_recomp.*.cpp` + `ppc_func_mapping.cpp` pass `clang++ -std=c++20 -fsyntax-only`, re-checked
after the jump tables landed.

### TASK 4 — host runtime: scaffold + enumeration + **link-with-stubs**  ✅ (night-scope)
`varianta/runtime/`: the recompiler ABI is documented; **474** kernel/xam imports enumerated
(`IMPORTS-TODO.md`); `gen_import_stubs.py` emits link-ready trap-stubs (correct C++ linkage);
`CMakeLists.txt` + `host_stub.cpp` + `README.md`. **The link-with-stubs build SUCCEEDED**: all 93 TUs
(22,782 guest funcs + 474 import stubs + host) compile with clang++ and link into `sp_td_varianta`
(103 MB) with **0 undefined symbols / 0 errors**, and the exe runs (prints scaffold notice, exits 0).
This is the doc's stated TASK-4 scope met ("get it LINKING with stubs"; "Don't try to boot"). It also
proves the recompiled image's *only* external references are the enumerated imports. The real runtime
(memory + XEX loader + 474 import impls + entry) and the renderer remain the genuinely multi-week phase.

## ⚠ Flagged for human review
1. **Function-boundary problem (TASK 2 caveat, README-acknowledged).** 9 detected jump tables and 161
   pre-existing `// ERROR <addr>` conditional-branch markers target addresses XenonAnalyse split into
   *separate* functions, so `goto` can't reach them. Verified the switch tables add **zero** new errors.
   Fix needs manual function-boundary overrides (or a boundary-analyzer extension). Not a compile blocker.
2. **Latent pre-existing upstream gaps** (independent of my work, surfaced in the recompile log): 39
   `vcmpgtuh.` record-form sites don't set CR6 (upstream `VCMPGTUH` has no Rc handling); 20 `VPKD3D128`
   "unexpected float16_4 pack" warnings. Low priority until runtime.

## Exact next step
Run the link-with-stubs to close TASK 4's first milestone:
```
cd varianta && (regenerate ppc/ per README) \
  && python3 runtime/gen_import_stubs.py \
  && cmake -S runtime -B runtime/out -G Ninja && cmake --build runtime/out
```
Then begin the real runtime (memory + XEX loader + func-table init), porting the 474 imports from
`third_party/rexglue-sdk/src/` per `runtime/IMPORTS-TODO.md`. After that: function-boundary overrides to
reclaim the 9 deferred jump tables + the 161 branch markers, then the native renderer (Plume + 19 shaders).
