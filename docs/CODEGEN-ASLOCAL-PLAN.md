# South Park recomp — make register/flag `*_as_local` CORRECT (recompiler engineering)

**You are a fresh session. Execute top-to-bottom, autonomously, no questions to the user.
This is recompiler-runtime engineering** — the deep lever the prior session found but could not
take safely. Project memory is auto-loaded; the anchors are [[sp_combat_perf_frontend_bound]]
(the floor is I-cache **capacity**-bound; the codegen footprint lever is register/flag promotion
to C++ locals — `cr_as_local` alone = **−14.6 % `.text`**) and [[sp_devshm_xenia_memory_leak]].

> **Read first:** `docs/CODEGEN-SIZE-REPORT.md` + `docs/CODEGEN-SIZE-LOG.md` (the prior session).
> They proved: the `*_as_local` promotion is *the* footprint lever (cr=−3.41 MB; GPR-as-local would
> attack the 2.82 M `ctx.<reg>` round-trips — the bulk of the 22 MB), but enabling any flag **breaks
> this title** and is auto-reverted by the determinism gate. Your job is to **fix the two root
> causes** so the promotion is correct, then measure whether the resulting `.text` cut moves the
> combat floor (baseline p10 = 13.5).

---

## 0. Prime directive & success criteria

**Goal:** make the codegen's register/flag-as-C++-local promotion **behaviour-correct** on this
SEH/setjmp-heavy title, enable it (start with `cr_as_local`, then `xer`/`ctr`, then the big one,
GPR-as-local), and **measure the combat floor** (median p10 swaps/s, Stan's House). Success =
a kept change that is **detdiff-pass AND** (floor p10 **> +1.0** in interleaved A/B `tools/perf/ab.sh`
**OR** a large `.text`/L1i-miss cut with no floor regression). The real prize: enough `.text`
shrink to lift the floor toward native. Getting cr correct alone (−14.6 %) and measuring its floor
effect is already a win worth banking.

**Non-negotiable:** the **determinism-diff gate is mandatory and ranks above fps.** It is already
built (§2). Run it after every codegen change before trusting any fps. Divergence ⇒ auto-revert,
log "refuted: correctness".

---

## 1. What is PROVEN (do NOT re-derive — measured/inspected last session)

- **The lever:** `cr_as_local` (CR0-7 → zero-init C++ locals; clang DCEs dead CR0 updates + keeps CR
  in host regs) cuts `.text` **23,361,720 → 19,950,608 B (−3,411,112, −14.6 %)** in one flag.
  `xer`+`ctr`+`reserved` together = −730 KB. GPR-as-local (`non_volatile`/`non_argument`) is untested
  but targets the **2.82 M ctx round-trips** = the dominant remaining bloat.
- **Two root-cause blockers (both proven), why every `*_as_local` flag is refuted:**
  1. **Port↔runtime-`.so` `PPCContext` layout mismatch.** `cr_as_local` makes the codegen emit
     `#define REX_CONFIG_CR_AS_LOCAL` into the **port's** `generated/default/south_park_td_init.h`.
     In `third_party/rexglue-sdk/include/rex/ppc/context.h` that macro is `#if !defined(...)`-guarded
     **around the `cr0..cr7` struct members** — so the **port's** `PPCContext` loses those fields
     (shifting `fpscr`/`f*`/`v*` offsets). The shared `librexruntime.so` is **not** rebuilt with that
     macro → port and `.so` disagree on offsets → corruption once FP/vector + setjmp code runs.
  2. **The title's setjmp/SEH snapshot the WHOLE ctx.** setjmp (`builders/context.cpp`, ~L173-188)
     emits `env = ctx` / `ctx = env` (full `PPCContext` copy; setjmp_addr `0x8242EEA0`, longjmp
     `0x8242EA70`, for JPEG/TGA/PNG format detection). SEH (`function_graph.cpp`, entry-capture
     `__seh_r13..r31 = ctx.r13..r31` ~L669-671, catch restores them ~L607-624; 77 scopes). Both
     assume every guest register lives in ctx — AS_LOCAL violates that.
- **Symptom of the breakage:** every `*_as_local` build boots to early init then logs
  `SEH: unwind through sub_824499A0 → return to caller` / `Execution complete` (the title aborting
  via its first-cut SEH recovery during image-format detection) and never reaches the level.
- **Dead ends (don't retry):** outlining `CRRegister::compare` (`[[gnu::noinline]]`) *grew* `.text`
  (+0.17 MB — too compact); in-ctx dead-CR0 elision = −896 B (clang already DCEs in-ctx; the win
  needs **non-escaping locals**); `base __restrict` = −80 B (ctx already `__restrict`).
- **Key map (file : what):**
  - `include/rex/ppc/context.h` — `PPCContext` struct with `#if !defined(REX_CONFIG_*_AS_LOCAL)`
    guards around members; `REX_FUNC`; `CRRegister`/`XERRegister`.
  - `src/codegen/builders/context.cpp` — accessors `r()/f()/v()/cr()/ctr()/xer()/reserved()`
    (~L45-107, return `"crN"` local vs `"ctx.crN"` based on `config().crRegistersAsLocalVariables`
    etc.); **setjmp emit** (~L173-188); save/restore-helper elision when non-volatile-as-local
    (~L197-201).
  - `src/codegen/function_graph.cpp` — **local var declarations** (zero-init, ~L626-661); **SEH
    entry-capture + catch** (~L607-671).
  - `src/codegen/config.cpp` — TOML flag parsing (~L108-122).
  - `src/codegen/codegen_writer.cpp` — `config_flags` JSON fed to templates (~L80-90).
  - `resources/templates/codegen/init_h.inja` — **emits the `#define REX_CONFIG_*_AS_LOCAL`**
    (~L12-18) — this is where the struct-shrinking is triggered.
  - Flags are TOML keys in the **port** manifest `south_park_td_manifest.toml` `[entrypoint]`
    (`cr_as_local`, `xer_as_local`, `ctr_as_local`, `reserved_as_local`, `non_argument_as_local`,
    `non_volatile_as_local`) — editing the manifest is a port-repo change (allowed).

---

## 2. Tooling you INHERIT (built + validated last session — reuse, don't rebuild)

- **Correctness gate:** `tools/perf/detdiff.sh {baseline|gate|capture}` + `tools/perf/detdiff_fp.py`.
  A fixed scripted run (boot→Stan's House→40 s combat) → semantic fingerprint of `run.log` (ordered
  markers, asset set, error/warning/critical sets, NaN/Inf, GPU pipelines, in-level) vs a baseline
  reference. **Reference already built + baseline-stable** at `tools/perf/detdiff/reference.json`
  (3 baselines bit-identical; injected-bug control passed). Usage:
  `tools/perf/detdiff.sh gate <label> 40` → last line `DETDIFF status=pass|fail reason=...`.
  It does NOT block on `gamectl play`'s hanging tail (polls the log) and re-verifies in-level after
  the dwell. **Re-validate it once** at the start (baseline gate must pass; an injected `add`→`sub`
  must fail) since you're touching the emitter deeply.
- **Build:** `tools/perf/regen_build.sh full` (emitter/manifest change: rebuild rexglue tool → regen
  → fixup → build port; ~140 s; **rebuilds `librexruntime.so` in `$SDK_INSTALL/lib64` too if you
  touch `context.h`** but does NOT copy it to the game dir — see §3). `regen_build.sh port`
  (header-only, no regen). Prints `.text` size + md5.
- **Footprint+floor:** `tools/perf/measure_baseline.sh` (combat profile+floor+catch_dip),
  `tools/perf/ab.sh <SECS> <REPS> base <fileA> cand <fileB>` (interleaved median-p10, keep>+1.0),
  `tools/perf/floor.sh`, `tools/perf/profile.sh`, `tools/perf/catch_dip.sh`.
- **Game control:** `tools/gamectl.sh {play|bench|kill}` (kill auto-cleans the leaked
  `/dev/shm/xenia_memory_*`). **Hazards:** game leaks 4.5 GB shm/launch (kill before launching);
  no clean exit (gdb-dump `__llvm_profile_write_file()` for PGO); **KILL before staging** any binary
  (overwriting a live mmap SIGBUSes); **don't build during a measurement** (steals cores).
- **Paths:** `ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp`,
  `SDK=$ROOT/../third_party/rexglue-sdk`, `GAME=$ROOT/out/build/linux-amd64-release`,
  `GEN=$ROOT/generated/default`. Baseline fallbacks on disk: `$GAME/south_park_td.baseline`
  (024e365a), `$GAME/librexruntime.so.good` (1996b550). Host bench, sudo `<redacted>`.

---

## 3. The `.so`/struct mismatch — the FIRST thing to settle (Phase A)

`regen_build.sh full` rebuilds `librexruntime.so` into `$SDK_INSTALL/lib64` but does **not** copy it
to `$GAME`. So the staged `.so` (1996b550) is built WITHOUT any `REX_CONFIG_*` macro. Two consequences:
- If you keep the struct-shrinking macro, you MUST stage the freshly-built `.so` too AND it must be
  compiled with the SAME macro as the port — else mismatch. **Update `regen_build.sh` (or stage
  manually) to copy `$SDK_INSTALL/lib64/librexruntime.so` → `$GAME` whenever you change the layout.**
- Whether the `.so` even tolerates the macro: **grep the runtime sources** for accesses to the
  to-be-removed fields, e.g. `grep -rn 'ctx\.cr\|\.cr[0-7]\b\|ctx\.r1[4-9]\|ctx\.r2[0-9]\|ctx\.r3[01]'
  $SDK/src` (and `ctx.xer`, `ctx.ctr`). If the runtime references a removed field, it won't compile
  under the macro and must be fixed.

**RECOMMENDED STRATEGY (decouple — avoids the `.so` problem entirely):**
Keep `PPCContext` **full** (ABI-stable; `ctx.cr0-7` etc. always present, so the `.so` never
mismatches and setjmp's `env=ctx` still has somewhere to snapshot CR), but still **emit the
registers as per-function C++ locals** and **sync locals↔ctx only at the ctx-snapshot boundaries**.
Concretely: split the `init_h.inja` `#define REX_CONFIG_*_AS_LOCAL` from the struct guards — i.e.
make the codegen NOT shrink the struct while STILL having `cr()/r()/...` return the local name and
declaring the zero-init locals. Then the locals are non-escaping within hot functions (clang DCE +
register allocation = the win), the struct stays consistent everywhere, and you only owe a sync at:
  - **setjmp** (`builders/context.cpp` setjmp emit): before `env = ctx`, write the promoted locals
    INTO ctx (`ctx.crN = crN; ...`); after `ctx = env`, read them back (`crN = ctx.crN; ...`). The
    promoted set is known from the config flags. (Last session this couldn't compile because the
    macro had removed `ctx.cr*`; with the struct kept full, it compiles.)
  - **SEH** (`function_graph.cpp`, only for GPR-as-local): the entry-capture already reads
    `ctx.r13-31`; ensure the promoted GPR locals are synced to ctx before any throwing call / at the
    try boundary, and that the catch's `ctx.rN = __seh_rN` restore is reflected back into the locals.
This is surgical and is the cleanest first cut. (Alternative if decoupling is impractical: keep the
struct-shrinking macro, rebuild + stage the `.so` with matching macros, fix any `.so` field uses.)

---

## 4. The QUEUE — smallest blast radius first, each detdiff-gated

After EACH change: `regen_build.sh full` → **detdiff gate** (pass before trusting fps) → `size` →
(if footprint cut) `ab.sh` floor + `profile.sh`. Append `docs/CODEGEN-ASLOCAL-LOG.md` (create it) +
`tools/perf/results.log`. Keep the best-known-good staged; revert aggressively on divergence.

- **Phase A — diagnose + re-validate gate (~1 h).** Re-validate detdiff (baseline pass; injected
  `add`→`sub` fail). Re-enable `cr_as_local`, build, and **pinpoint the crash** (gdb on the live PID
  at the SEH-unwind, or add a one-shot log in `ppc_setjmp`/the unwind; compare `sizeof(PPCContext)`
  port-side vs `.so`-side). Decide: is the primary fault the struct mismatch, the setjmp logic, or
  both? This picks your Phase-B path.
- **Phase B — make `cr_as_local` correct (~3-4 h, the core).** Implement the decouple strategy (§3):
  keep the struct full, emit cr as locals, add the setjmp cr↔ctx sync. Build, **detdiff gate**. If
  pass → measure `.text` (expect ~19.95 MB), `ab.sh` floor vs `south_park_td.baseline`, `profile.sh`
  (L1i-miss, DSB:MITE), `catch_dip`. **This is the first bankable win.** If it still diverges, iterate
  (the gate's `reason=` + the crash log tell you what desynced; add the missing sync point).
- **Phase C — xer/ctr/reserved (~1 h).** Same decouple+sync, on top of cr. Each gated. Volatile, so
  setjmp sync only (no SEH). Bank footprint cuts.
- **Phase D — GPR-as-local: the big one (`non_volatile` then `non_argument`) (~4 h, gated, high
  reward).** This attacks the 2.82 M ctx round-trips. Needs the SEH sync (Phase D of §3) since the
  entry-capture/catch touch `ctx.r13-31`. Incremental, soak detdiff hard, revert if shaky. Even
  `non_volatile` alone could be the floor-mover. Watch the `.so` boundary (args r3-r12 stay in ctx —
  do NOT promote argument registers that the runtime reads).
- **Phase E — measure the floor (the actual goal).** On the best correct build, `ab.sh 90 3`
  (bump to 5 near ±1.0) vs baseline. Does the `.text` cut lift p10 above 13.5? Re-`profile.sh`:
  did L1i-miss / DSB:MITE / fetch-latency improve? `catch_dip` to confirm still CPU-bound (or, the
  success-to-ceiling case, newly GPU-bound).
- **(Stacking) PGO** on the new codegen for avg/max (orthogonal; gdb-dump method). Optional.

---

## 5. Correctness, state, stop conditions

- **Gate every change** with detdiff before fps. Auto-revert on `fail`. Periodically re-check the
  injected-bug sensitivity. The gate's limit: behaviour-equivalence on one scripted path — for a
  calling-convention change like GPR-as-local, **soak longer** and consider a second scripted route
  (e.g. a different level) if available.
- **SDK edits stay working-tree-only** (don't commit the submodule). If you change `regen_build.sh`
  to stage the `.so`, that's a tools change (fine).
- **Always leave the best-known-good staged + booting** before finishing. Fallbacks:
  `south_park_td.baseline` / `librexruntime.so.good`.
- **Stop & report if:** cr_as_local can't be made correct after a focused attempt (revert, bank
  nothing, report the precise blocker) OR the floor reaches ~55-60 p10 (solved) OR `catch_dip` shows
  it became GPU-bound (success-to-ceiling). Honest "partway" is acceptable — but unlike last session
  you now have a concrete fix path, so push to at least bank `cr_as_local` correct + its floor number.

## 6. FINAL REPORT (`docs/CODEGEN-ASLOCAL-REPORT.md`) + memory
TL;DR (baseline→best `.text`/floor; which flags made correct + kept); the fix (how the decouple +
setjmp/SEH sync works); per-flag ledger (detdiff, Δ`.text`, ΔL1i, floor Δ, kept); mechanism
(working set vs L3, expansion-ratio improvement, did the floor move and why); correctness (gate +
its limits); final staged md5s + SDK working-tree edits + patch-regen note. Update
[[sp_combat_perf_frontend_bound]] with which flags became correct and the floor delta. Then a concise
user summary.

---
### Appendix — cheat sheet
```
ROOT=/home/h/src/recomp/rexglue-recomps/south-park-recomp
SDK=$ROOT/../third_party/rexglue-sdk ; GAME=$ROOT/out/build/linux-amd64-release
# enable a flag: edit $ROOT/south_park_td_manifest.toml [entrypoint]:  cr_as_local = true
# build (emitter/manifest/header change): rebuild tool->regen->fixup->build port (+rebuilds .so)
$ROOT/tools/perf/regen_build.sh full
# IF you keep the struct-shrinking macro, also stage the matching .so:
cp -f $SDK/out/install/linux-amd64/lib64/librexruntime.so $GAME/   # (KILL the game first)
# gate (MUST pass before trusting fps):
$ROOT/tools/perf/detdiff.sh gate <label> 40
# footprint + floor:
size $GAME/south_park_td
TARGET=$GAME/south_park_td $ROOT/tools/perf/ab.sh 90 3 base $GAME/south_park_td.baseline cand $GAME/south_park_td
$ROOT/tools/perf/profile.sh 12 ; $ROOT/tools/perf/catch_dip.sh 27 3
# verify a flag took effect:  grep -c REX_CONFIG_CR_AS_LOCAL $GEN/south_park_td_init.h ;  grep -rho 'ctx\.cr0' $GEN/*.cpp | wc -l
```
```
```
