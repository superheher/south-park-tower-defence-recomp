# Next-session launch prompt — variant A (XenonRecomp), AUTONOMOUS night session

Paste the block below to start the next session. It is self-contained and written to run UNATTENDED.

---

You are continuing **variant A** (XenonRecomp full-stack recomp of *South Park: Let's Go Tower Defense
Play!*) on branch `experimental/hle-graphics-spike` in `rexglue-recomps/south-park-recomp`. This is an
**AUTONOMOUS night session — the user is asleep and will NOT answer anything.**

## Operating rules (autonomous)
- **Never ask questions / never wait for the user.** Make every decision yourself using the rules below.
- **When blocked on task N:** write the blocker into `varianta/NIGHT-LOG.md`, commit what you have, and
  move to the next *independent* task. Never spin or stop on one obstacle.
- **Rigor still applies** (the user demands it): validate before claiming; if you can't validate an
  instruction's semantics, implement the conservative/spec-faithful form and **flag it in NIGHT-LOG.md**
  — do not silently guess.
- **Commit every increment** (`git -c user.name=superheher -c user.email=heh@vivaldi.net commit`,
  Co-Authored-By trailer). **NEVER push.** Regenerate `varianta/patches/xenonrecomp-sp-instructions.patch`
  after each recompiler.cpp change.
- **Guardrails — do NOT:** touch `third_party/rexglue-sdk` or the game-dir `librexruntime.so`
  (baseline `1a3f6076` — variant A is fully separate); bump the superproject `main` submodule pointer
  (work stays on the experimental branch); re-open variant B / re-chase Xenia CP optimizations (both
  CLOSED — see `docs/HLE-PHASE1-FINDINGS.md`); run the game during the instruction/jump-table/compile
  phases (they don't need it). If you ever do run it: clean `/dev/shm/xenia_memory_*` + kill
  `south_park_td` after, and run any `pkill`/`gamectl kill` ALONE (it exits non-zero and cancels batched
  calls).
- **Keep a running `varianta/NIGHT-LOG.md`** (create it first): timestamped progress, decisions, blockers.
- **Write `varianta/MORNING-REPORT.md` before you finish**: what landed, the unrecognized-count trajectory,
  what's flagged for human review, exact next step.

## State (committed `86dfa35`, NOT pushed; machine clean, prod `.so` untouched `1a3f6076`)
- Toolchain: `third_party/XenonRecomp` (recursive clone of hedge-dev/XenonRecomp; NOT a submodule —
  on-disk, untracked). Built at `third_party/XenonRecomp/out/build/{XenonAnalyse,XenonRecomp}`. My 18
  added instructions are already in that clone's `XenonRecomp/recompiler.cpp` AND captured in
  `varianta/patches/xenonrecomp-sp-instructions.patch` (verified applies to a pristine clone). The clone
  is on disk — use it as-is; do NOT re-apply the patch to it. Only re-clone+patch if the dir is gone.
- `varianta/sp_xenon.toml` (config), `sp_switch_tables.toml` (empty), `varianta/ppc/` (93 generated C++,
  git-ignored). Current **unrecognized instructions = 10,960** (from 13,183).
- Read for context: `varianta/README.md`, `docs/HLE-PHASE1-FINDINGS.md`, memories `sp_varianta_bootstrap`
  / `sp_hle_phase0_progress`.

## Recompile loop (deterministic, no game needed)
```
cd third_party/XenonRecomp && cmake --build out/build --target XenonRecomp        # after editing recompiler.cpp
cd ../../south-park-recomp/varianta
../../third_party/XenonRecomp/out/build/XenonRecomp/XenonRecomp sp_xenon.toml ../../third_party/XenonRecomp/XenonUtils/ppc_context.h > /tmp/xr.log 2>&1
grep -oE ': [a-z0-9._]+$' /tmp/xr.log | sort | uniq -c | sort -rn        # remaining, by mnemonic
```

## Plan (ordered; do as far as you get — full variant A is weeks, so MAXIMIZE the deterministic front)

### TASK 1 — close the instruction gap to 0 (the night's main, well-bounded job)
Add the ~48 missing `case PPC_INST_*:` to `third_party/XenonRecomp/XenonRecomp/recompiler.cpp`. The
decoder already knows them all (`thirdparty/disasm/ppc-inst.h`), so this is pure emitter cases.
**Decision rules:**
- **Model each on the closest existing case** in recompiler.cpp (the 18 I added show the pattern), and on
  the PPC spec.
- **Validation (do NOT block on XenonTests — Xenia test bins are a setup rabbit hole):** the authoritative
  cross-reference is **RexGlue's own recompilation of THIS XEX**, which is correct (it ships a playable
  build). For any instruction you're unsure of, find the same guest instruction in
  `generated/default/south_park_td_recomp.*.cpp` (grep the `// <mnemonic>` asm comment) and match its
  semantics. If XenonTests happens to build trivially, use it too, but it is optional.
- **VECTOR/VMX (~10.4k occ — the bulk):** `vslh` `vsubshs` `vsrah` `vspltish` `vaddsws` `vmaxsh` `vminsh`
  `vminsw` `vcfpuxws128` `vcmpgtsh[.]` `vcmpequh[.]` `vpk{swss,swus,uhus,shss}[128]` `vsububm` `vrlh`
  `vaddsbs` `vsubsbs` `vsrh` `vsrab` `vavg{uh,sw}` `vslo[128]` `vsel128` `lvehx` `stvebx`. ⚠ **XenonRecomp
  stores each 128-bit vector with REVERSED 16-byte element order** — every vector case must account for it
  (most are 16-bit/8-bit-lane analogs of already-implemented 32-bit ops like `VADDSWS`↔existing `VADDSHS`,
  so copy the analog and change the lane width + intrinsic). Cross-check a few against RexGlue.
- **SCALAR (~600, subtle — model on the named existing case):** `addc`/`addme`/`subfze` (XER carry — model
  on `ADDE`/`SUBFE`/`ADDZE`), `bdzf`/`bdnzt` (CTR-decrement branch — model on `BDZ`/`BDNZ`/`BDNZF`, and
  select the real CR bit from `operands[0] % 4` = lt/gt/eq/so, do NOT hard-code eq), `cror`/`crorc`
  (CR-bit logical — these address individual CR bits; implement bit extract/set).
- After each batch: rebuild → re-run → confirm the batch's mnemonics hit 0 → regenerate the patch → commit.
- **Done-criteria:** 0 unrecognized (or a short, justified residual list in NIGHT-LOG.md).

### TASK 2 — jump tables
XenonAnalyse found 0 (it's tuned for Sonic Unleashed). Author `varianta/sp_switch_tables.toml` from
RexGlue's data: the computed-jump targets in `config/sp_functions.toml` + the `switch`/jump-table
statements already in `generated/default/south_park_td_recomp.*.cpp`. Match XenonRecomp's switch-table
TOML schema (see its README §"Jump Tables" / the SWA example). Re-run the recompile; commit.
**Done-criteria:** the `mtctr;bctr` sites that RexGlue resolves are resolved here too (spot-check a few).

### TASK 3 — compile-check the generated C++
Verify `varianta/ppc/*.cpp` is syntactically sound: `clang++ -std=c++20 -fsyntax-only -I<ppc dir> ppc/ppc_recomp.0.cpp`
(it includes `ppc_context.h` + `ppc_config.h`; link will need the runtime, but syntax must be clean).
Fix any emitter bugs surfaced. Commit. **Done-criteria:** all generated TUs pass `-fsyntax-only`.

### TASK 4 — scaffold the host runtime (only if 1–3 are done; this is the large remaining phase)
Stand up a CMake project skeleton that compiles the generated `ppc/` against a runtime, cribbing
behaviour 1:1 from RexGlue's `third_party/rexglue-sdk/src/` (kernel/xam, video Vd*+present, XMA, SDL
input, VFS, setjmp=0x8242EEA0/longjmp=0x8242EA70). Don't try to boot — just get it LINKING with stubs,
and enumerate (in MORNING-REPORT.md) the runtime functions still to implement. The native renderer
(Plume + the 19 shaders in `private/extracted/media/shaders/*.updb`) is the phase after; do not start it.

## One-time re-clone (only if `third_party/XenonRecomp` is missing)
```
cd third_party && git clone --recursive --depth 1 https://github.com/hedge-dev/XenonRecomp.git
cd XenonRecomp && git apply ../../south-park-recomp/varianta/patches/xenonrecomp-sp-instructions.patch
CC=clang CXX=clang++ cmake -S . -B out/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build out/build --target XenonAnalyse XenonRecomp
```
