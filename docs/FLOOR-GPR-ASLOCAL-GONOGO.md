# South Park combat floor — GPR-as-local GO/NO-GO + pivot decision

**Date:** 2026-05-29 (autonomous session). **Question posed:** before investing in the
SEH/longjmp/ABI state-save redesign that GPR-as-local needs, run a *cheap* go/no-go —
**project `.text` after the full `non_volatile`+`non_argument`-as-local set: does it reach the
~12 MB L3 cache-fit, and would cache-fit move the capacity-bound p10 floor at all?** If no:
STOP, report, pivot (PGO for avg/max, or the idle GPU). See `docs/CODEGEN-ASLOCAL-REPORT.md`
and the memory `sp_combat_perf_frontend_bound`.

---

## 1. TL;DR — **NO-GO** (decided on *measured* numbers, no redesign needed)

> ## ⚠ MAJOR CORRECTION (2026-05-29, supersedes the whole "capacity-bound" thesis below — see §7)
> **The floor is NOT i-cache / L2 / DSB *capacity*-bound. It is BRANCH-MISPREDICTION-bound.** A
> direct TMA-L3 front-end breakdown during combat shows the fetch stall is **39.3 % branch-resteers
> vs only 3.6 % i-cache-miss and 3.4 % iTLB.** Corroborated: the hottest functions are **464–669
> *bytes*** (fit L1i thousands of times over — capacity was never their problem); the **conditional
> branch mispredict rate is 22 %** and the **return mispredict rate is 21 %** (healthy is <2–5 %);
> branch density is **1 conditional branch per 27 instructions**. The Main (guest-sim) thread runs at
> **1.0 CPU** (genuinely CPU-bound, not pacing-waiting) and the GPU is **17–27 % idle**.
> **This means everything below — and the L3/L2/DSB "capacity wall" framing in §3b/§3c/§4 — diagnosed
> the wrong bottleneck.** Size-reduction levers (AS_LOCAL, movbe+outliner, BOLT, the hybrid) were all
> neutral *because they attack code size, not branch prediction.* The NO-GO verdicts on GPR-as-local
> and GPU-offload still stand (they were never going to help), but the *reason* the floor doesn't move
> is misprediction, and **misprediction is at least partly a fixable codegen problem, not a hardware
> wall** (it would also improve on machines with better predictors). Read §7 for the corrected
> mechanism + the candidate fixable levers. The text below is kept verbatim for provenance but its
> "capacity-bound / hardware-bound" conclusion is **retracted**.

The full GPR/FPR/VR-as-local set — the **maximum** AS_LOCAL footprint reduction physically
available — was **already built and its `.text` measured** in the prior session (Phase D.1/D.2).
Reading those numbers as what they are (cumulative *on top of* the shipped Phase C cut) gives the
projection directly, no new build required:

| Build | `.text` (B) | vs baseline | vs 12 MB L3 |
|---|---|---|---|
| baseline | 23,361,720 | — | 1.95× |
| **Phase C** (cr/xer/ctr/reserved) — *shipped, correct, floor-neutral* | 19,610,528 | −16.06 % | 1.63× |
| C + `non_volatile` (r14-31,f14-31,v14-31,v64-127) — measured D.1 | 18,576,208 | −20.5 % | 1.55× |
| C + `non_argument` (r0,r2,r11,r12,f0,v32-63) — measured D.2 | 18,875,104 | −19.21 % | 1.57× |
| **C + both** (disjoint reg sets → ~additive) — **projection** | **≈17,840,784** | **−23.6 %** | **1.49×** |

**The maximal AS_LOCAL build lands at ~17.84 MB ≈ 1.49× the 12 MB L3. It does NOT reach
cache-fit.** And the floor was already measured **dead-flat** at 19.61 MB (Phase C: p10 14.6 vs
base 14.3) and 19.95 MB (cr: 14.6 vs 14.8) — neither fits L3, and 17.84 MB doesn't either, so
there is **no basis** to expect it to move a *capacity*-bound floor. Therefore:

> The SEH/ABI state-save redesign (weeks of work, high risk) would, in the **best imaginable
> case** where it's made 100 % correct, unlock a build whose `.text` (17.84 MB) is still 1.49× L3
> and which the measured floor-neutrality of the 19.61 MB build predicts would be **floor-neutral**.
> **STOP.** Do not undertake the redesign for the floor.

This is independent of — and on top of — the *separate, also-fatal* correctness blocker
(GPR-as-local crashes at boot; the incoming non-volatiles live in the caller's C++ frame, so the
SEH entry-capture `__seh_rN = ctx.r13-31` has nothing correct to snapshot — `CODEGEN-ASLOCAL-REPORT.md §4`).
Either reason alone kills it; the cheap `.text` projection kills it **without** spending the
redesign budget, which is the whole point of running the go/no-go first.

### Why the memory's premise was wrong (the surprise)

The standing assumption (`sp_codegen_size_plan`, `sp_aslocal_plan`) was *"GPR-as-local attacks the
2.82 M `ctx.<reg>` round-trips = the bulk of the 22 MB."* **Measurement refutes this.** Promoting
the **entire** non_volatile + non_argument GPR/FPR/VR set removes only **~1.77 MB** of `.text`
(19.61 → 17.84 MB), not ~10 MB. Reason: non-volatile registers must survive calls, so clang's
register allocator **still spills them around every call** — promotion changes *where* the
load/store lives (named local vs `ctx` field) but the spill traffic, and thus the instruction
bytes, largely remain. The ctx round-trips are cheap 3-4-byte mov instructions; eliminating them is
not where the 22 MB lives. The 22 MB lives in the **sheer number of recompiled guest functions and
their prologue/epilogue/byte-swap idiom**, which AS_LOCAL doesn't touch.

---

## 2. Confirming measurement — live per-symbol combat profile (NEW this session)

First per-symbol self-time profile of the running game during Stan's House combat (the prior
sessions only had aggregate Topdown). `perf record --call-graph lbr -F 2000`, 22 s, 140,281
samples, fps 45-49. Full capture: `tools/perf/combat_profile_2026-05-29.txt`.

**DSO share (whole process):** `south_park_td` (recompiled guest) **38.84 %**, `librexruntime.so`
(runtime+GPU backend) **30.85 %**, kernel 15.62 %, libc 10.76 %, `libvulkan_radeon` **2.15 %**.

**Top self-time symbols (whole process):**

| % | symbol | thread | meaning |
|---|---|---|---|
| 12.61 | `__imp__sub_821B9270` | Main | hottest recompiled **guest** function |
| 8.70 | `TimerQueue::TimerThreadMain` | timer | runtime timer thread (see §4) |
| 5.66 | `__imp____restgprlr_29` | Main | **PPC nonvolatile-GPR restore helper** |
| 5.24 | `__imp__sub_8244CE40` | Main | recompiled guest function |
| 5.11 | `__imp____savegprlr_29` | Main | **PPC nonvolatile-GPR save helper** |
| 4.26 | `__imp__sub_821C6E58` | Main | recompiled guest function |
| 4.06 | `pthread_mutex_trylock` | — | lock traffic |
| 3.03 | `__pthread_mutex_unlock_full` | — | lock traffic |
| 3.01 | `CommandProcessor::WriteRegister` | CP | PM4 register write |
| 2.54 | `VulkanCommandProcessor::WriteRegister` | CP | PM4 register write (vk override) |
| 1.97 | `GetLoggerRaw` | CP | logger-category lookup (combat dead work) |
| 1.97 | `PosixConditionBase::WaitMultiple` | — | guest wait objects |
| 1.32 | `ExecutePacketType0` | CP | PM4 packet parse |

**Three things this profile establishes:**

1. **The hot thread is Main (guest sim), not CP.** The single hottest item, `sub_821B9270`
   (12.6 %), plus the next guest funcs and the GPR helpers, all live on Main — recompiled **guest**
   code. The CP thread's hottest item is `WriteRegister` at 3.01 %. The floor is gated by the
   recompiled-guest-code footprint, exactly the capacity wall — and exactly what AS_LOCAL can't fit.

2. **The GPR save/restore helpers are real and large — and they are the smoking gun for the
   NO-GO.** The `__savegprlr_29` + `__restgprlr_29` pair is **10.77 %** of whole-process self-time
   (5.66 + 5.11). This is precisely the cross-call nonvolatile-GPR spill traffic GPR-as-local was
   meant to attack — and seeing it at ~11 % is *why* the lever looked attractive. **But the emitted
   body proves the traffic is irreducible by AS_LOCAL** (verified in
   `generated/default/south_park_td_recomp.15.cpp`):

   ```c
   __savegprlr_14: …
     REX_STORE_U64(ctx.r1.u32 + -168, ctx.f30.u64);   // stfd f30,-168(r1)
     REX_STORE_U32(ea, ctx.r1.u32);                    // stwu r1,-528(r1)
     // … stw r3,548(r1) … (spill r14..r31 to the GUEST stack at ctx.r1 + offset)
   ```

   These `__save/__restgprlr_N` are the **PowerPC ABI's out-of-line nonvolatile save/restore
   prologue/epilogue** — recompiled guest functions whose entire job is to write the guest's
   nonvolatile registers **to the guest's own stack** (`REX_STORE` to `ctx.r1 + offset`, a
   guest-computed guest-memory address). That spill is **semantically mandated** by the guest ABI:
   the guest's own unwinder, the title's setjmp/longjmp, and the runtime's SEH all read those exact
   stack slots. **AS_LOCAL eliminates non-escaping *intra-function* `ctx` round-trips; it cannot
   eliminate ABI spills to guest memory** — under GPR-as-local the value would still have to be
   materialized and `REX_STORE`d here (and the caller's incoming nonvols would still need to reach
   this helper — the very "incoming nonvols live in the caller's frame" blocker from §4). So this
   ~11 % is structural. It explains all three facts at once: the small measured `.text` delta
   (1.77 MB, §1), this profile line, and why even a *correct* GPR-as-local would be floor-neutral.

3. **GPU is idle and irrelevant to this wall.** `libvulkan_radeon` is 2.15 %; the RX 560 sits idle
   during dips. The bottleneck is CPU front-end on recompiled guest code, which no GPU work shrinks.

---

## 3. Pivot A — offload CP-thread work to GPU compute: **also NO-GO**

(This pivot was scoped because the idle GPU is tempting. A read-only static-analysis fan-out across
the six Vulkan GPU subsystems + the live profile above settle it.)

**Verdict: NO-GO.** Three independent reasons:

1. **Hotness/nature mismatch.** Both *high-hotness* CP subsystems are **branchy-parse**, not
   data-parallel: `command_processor` (`WriteRegister` switch/state-machine called ~1000-4000×/frame,
   cascading side effects — gamma ramp, `COHER_STATUS_HOST`→`MakeCoherent`, dc_lut state machine;
   `command_processor.cpp:438`) and `pipeline_and_shader` (per-draw `PipelineDescription` build +
   200-byte hash lookup, SPIR-V translation a CPU library call, pipeline creation a driver op;
   `pipeline_cache.cpp:1049-1569`). Moving a branchy parse loop to the GPU is the *same* i-cache
   work plus a PCIe round-trip — strictly worse.

2. **The big bulk-SIMD workload is already on the GPU.** `texture_cache` — guest→host texture
   untiling/swizzle/endian/format conversion, the one large data-parallel job — is **100 % GPU
   compute already** (40+ `texture_load_*_cs.h` SPIR-V shaders; `texture_cache.cpp:42-93`,
   `:1212-1783`). Nothing left to offload. (My initial "no texture compute shader" guess was wrong.)
   EDRAM resolve, host-depth-store, and gamma are likewise already compute.

3. **The only genuinely-offloadable remainders are tiny slices of medium-hotness subsystems** and
   are more cheaply fixed on the CPU: index **endian-swap** (`ReplaceResetIndex32To24`/`GpuSwap`,
   `primitive_processor.h:391-452`) — self-estimated **5-10 %** of a medium subsystem; and index
   **topology conversion** (TriangleFan/QuadList/LineLoop, the only currently-non-SIMD transforms,
   `primitive_processor.cpp:500-566`) — **15-25 %** of a medium subsystem, better fixed with CPU
   SIMD than a ~100-300 µs RX-560 PCIe round-trip on the already-front-end-bound CP thread.

**Decisive:** even a *fully successful* offload of both remainders is bounded to single-digit % of
**CP-thread** cycles, and the CP thread is the *secondary* gate (Main dominates, §2). It cannot
move the floor, which is set by recompiled-guest `.text` ≫ L3 — a footprint GPU offload does not
shrink, while adding round-trip latency on a weak GPU's critical path is a credible regression. The
gate to revisit: a CP-thread profile showing those bulk-SIMD functions sum to >30 % of CP cycles
*and* the consuming draw can be deferred one frame for async overlap — the data predicts neither.

---

## 3b. Pivot — compile-level "exotic" levers (movbe fusion + Machine Outliner): floor-neutral

(Scoped when asked whether any "ancient-wizard" lever remained. The build was using **default
`-march`** — so clang emitted 264,539 `mov`+`bswap` pairs for guest byte-swaps and **0 `movbe`**,
despite this i9-8950HK supporting MOVBE — and the **LLVM Machine Outliner was OFF**. Both are the
canonical "dedupe the repeated PPC→x86 idiom / fuse the load-swap" levers prior reports named as the
*only* path to the floor. So they were built and **measured** — the first time these compile-level
levers were tested here.)

| variant (stacked on Phase C) | `.text` | vs Phase C | vs 12 MB L3 | bswap | movbe | gate | floor p10 |
|---|---|---|---|---|---|---|---|
| Phase C (live baseline) | 19,610,528 | — | 1.63× | 264,539 | 0 | — | **14.6** |
| `-march=native` | 19,218,637 | −2.00 % | 1.53× | 40,358 | 254,114 | **pass** | 14.3 (Δ −0.3) |
| `-march=native` + Machine Outliner | **17,870,591** | **−8.87 %** | **1.42×** | 15,146 | 147,935 | **pass** | 14.0 (Δ −0.6) |

(Outliner flags: `-mllvm -enable-machine-outliner=always -mllvm -outliner-leaf-descendants`.
Floor A/B = `ab.sh 90 3`, interleaved, median p10 of heavy windows. From the original 23.36 MB
baseline the outliner build is **−23.5 %** total.)

**Both pass detdiff (behaviour-safe) but are floor-neutral — and this is the *strongest* proof of
the capacity wall yet.** The Machine Outliner is the maximal *automatic* idiom-dedup tool — exactly
the "codegen-level absolute `.text` reduction" every prior report named as the one path to the floor.
It really cut −8.87 % (fusing 250 k byte-swaps + outlining repeated prologue/epilogue idiom), and the
floor **still did not move** (in fact −0.6: the outlined code adds calls/taken-branches that slightly
hurt I-cache locality). Reason: 17.87 MB is **still 1.42× L3** — it does not fit. This closes the
last loophole in the "maybe the *hot per-frame working set* is smaller than total `.text` and could be
squeezed under L3" reasoning: even −23.5 % absolute + MOVBE fusion does not approach cache-fit or
move the floor. **(Value elsewhere: movbe+outliner are a legitimate win for distribution size /
average-frame density / power — they are correct and gate-passing — just not for the floor. Could be
shipped alongside PGO if a smaller/cooler build is wanted.)**

## 3c. AOT+interpreter hybrid — staged feasibility + the decisive cache-level test (NO-GO for floor; corrects the mechanism)

The one "big enough in principle" lever (§4) is a hybrid: don't recompile cold guest functions to
native `.text` — interpret them compactly — to slash the recompiled footprint. Two questions were
asked: **(a) can it be done incrementally with diagnosable milestones?** and **(b) would it actually
move the floor?** Both were answered without writing an interpreter.

**(a) Yes — it is the most stageable lever we have. The infrastructure already exists** (probed in
the runtime, read-only):
- `FunctionDispatcher` holds `std::unordered_map<uint32_t, PPCFunc*> function_table_`
  (`include/rex/system/function_dispatcher.h:127`) with a public `SetFunction(guest_addr, func)`
  (`function_dispatcher.cpp:240`) — **any of the 15,617 guest functions can be swapped, per-function,
  at runtime.**
- Uniform ABI: every function is a `PPCFunc*(PPCContext&, uint8_t* base)` → one interpreter trampoline
  fits every address.
- The unresolved-address fallback path **already exists**: `south_park_td_init.h:236` —
  *"unregistered slots fall back to `rex::runtime::ResolveIndirectFunction`"* — the exact hook point.
- The native↔interpreted call boundary is already uniform (a `bl` looks the target up in the same
  table), so mixed execution needs no special-casing.
- **Milestones:** M0 = PGO projection (done, below); M1 = interpreter opcode-by-opcode, verified
  **offline & differentially** (run the same `ctx` through the recompiled function vs the interpreter,
  compare output `ctx` — no game run); M2 = exclude functions **coldest→hottest** (10 %→20 %→…), each
  step gated by detdiff + Δ`.text` + `ab.sh` floor A/B. The interpreter keeps **all** state in `ctx`
  (no register promotion), so unlike GPR-as-local it is **inherently SEH/setjmp-compatible** — the
  blocker that killed GPR-as-local is simply absent. So progress is monotone and each milestone is
  independently diagnosable. **(a) = GO.**

**(b) But it will NOT move the floor — and the cache-level test proves why, correcting the
"L3-bound" wording used in all prior reports.**

M0 projection (`tools/perf/m0_hybrid_projection.py`, joining the PGO profile `/tmp/sp/port.profdata`
— 36.7 B block-executions — with per-symbol `.text` sizes):
- **9,788 of 15,380 guest functions (63 %) never execute once during combat.** Only 5,592 ever run.
- **All code that ever runs in combat = 5.51 MB** of `.text` (of 17.49 MB total guest code).
- 99 % of all execution = **176 functions = 0.44 MB**; 99.9 % = 731 funcs = 1.30 MB.

So the hybrid *can* produce a tiny native image — but that already exposes the contradiction with
"`.text` 19.6 MB ≫ 12 MB L3 ⇒ overflows L3": if ≤5.5 MB ever runs in combat, it **fits** L3. The
decisive per-cache-level combat profile (`tools/perf/cache_levels.sh`, 20 s heavy combat, fps ~51)
settles it:

| counter | value | reading |
|---|---|---|
| IPC | 0.79 | front-end-bound (confirmed) |
| L1-icache-load-misses | 1.56 B | spill to L2 |
| **L2 code-read miss** | **380 M (24.8 % of code reads)** | ¼ of code misses L2 |
| L2 code-read hit | 1.15 B (75 %) | L2 catches ¾ |
| **LLC (L3) load-miss → DRAM** | **4.07 M (9.2 %)** | almost nothing reaches DRAM |
| cache-misses → DRAM (code+data) | 76 M (13 %) | low |
| DSB : MITE : MS uops | **12.8 : 87.2** | uop-cache overwhelmed (worse than the earlier 18:82) |
| fetch-latency vs bandwidth | 35.9 % vs 14.7 % | latency, not decode-bandwidth |

**L2 code misses (380 M) are ~80× the code traffic that reaches DRAM. The hot working set FITS IN L3
but NOT in L2.** The real wall is one level lower than every prior report stated: **L2 (256 KB/core)
and the DSB µop-cache (KB-scale)** — 87 % of µops come from the legacy MITE decoder. This is the
precise reason BOLT, AS_LOCAL (−16 %), and the outliner (−8.87 %) were **all** floor-neutral: each
shrank total code below 12 MB (which already fit L3), but none brought the hot set near the **256 KB
L2** or the **KB DSB**.

**This kills the hybrid as a floor lever by *mechanism*, not analogy:** the hybrid interprets exactly
the 63 % of functions that *never execute in combat* — they are already cache-cold and never enter
L1i/L2/DSB during a frame. Removing them from `.text` changes the hot path's cache behaviour by
**zero**. The hot 5.5 MB stays native and untouched, and its pressure on L2/DSB *is* the floor. (BOLT
hot/cold-split already simulated this exact effect on the fetched set — and was measured neutral.)
**(b) = NO-GO for the floor.** (The hybrid is still a real win for **distribution size / load time /
RAM** — it removes 12 MB of never-in-combat code — and is well-de-risked/stageable if those are the
goal.)

**Why nothing reaches the real target:** to fit L2, even the 99.9 % hot core (1.30 MB) must get ~5×
denser. The only way to 5× density is more abstraction (interpreter / threaded code) — which is
**slower per instruction**. You cannot be 5× more compact *and* not slower; that contradiction is the
floor. AS_LOCAL already demonstrated it (made code denser by removing ctx traffic → 1.16× isn't 5× →
neutral).

---

## 4. The floor is at the hardware limit (capstone)

Every avenue to the **floor** is now closed with measurement:

- **Post-link tools** (BOLT / PGO / ICF / `-Os` / `.so` micro-opts): floor-neutral
  (`PERF-OVERNIGHT-REPORT.md`).
- **Codegen footprint** (`*_as_local`): the correct, shippable subset (cr/xer/ctr/reserved,
  −16.06 %) is floor-neutral (`CODEGEN-ASLOCAL-REPORT.md`); the full GPR set is **(a)** correctness-
  blocked at boot **and (b)** — proven here — projects to 17.84 MB ≈ 1.49× L3, still no cache-fit.
- **Compile-level idiom levers** (§3b): `-march=native` (movbe fusion) + LLVM Machine Outliner —
  −8.87 % `.text` (−23.5 % from the original baseline), gate-passing, and **floor-neutral** (the
  maximal auto-dedup tool can't reach cache-fit; 17.87 MB still 1.42× L3).
- **GPU offload** (§3): wrong thread, the bulk-SIMD work is already on GPU, residue is single-digit %.

**Exotic levers that don't apply on this hardware (checked, not just assumed):** Intel CAT/L3
cache-partitioning (would pin the hot code into reserved L3 ways) — **this i9-8950HK has no CAT**
(`cat_l3` absent in `/proc/cpuinfo`, `resctrl` won't mount); hugepages for `.text` — **iTLB is not
the bottleneck** (0.055 % miss); MSR prefetcher tweaks (`/dev/cpu/*/msr` present) — prefetch adds no
L3 *capacity*. The one lever big enough in principle is an **AOT+interpreter hybrid** (don't compile
cold functions — interpret them compactly — to cut the bulk of the 17.9 MB), but that is weeks of
SDK redesign and effectively a different recompiler, not an in-tree knob.

**Mechanism:** the combat floor is **I-cache *capacity*-bound**. The recompiled `.text` (≥17.84 MB
even in the unreachable best case; 19.61 MB shipped) is 1.5-1.6× the **12 MB L3** of this 2018
mobile i9-8950HK, and the per-frame working set of the hot Main (guest-sim) thread overflows
L1i/L2/L3 on heavy frames regardless of layout, branch prediction, or ≤24 % size cuts. **The floor
is a property of (this recompiler's expansion ratio) × (this title's code size) ÷ (this CPU's L3).**
It moves only with **more L3** (different CPU) or a **fundamentally more compact recompilation**
(a different codegen contract than rexglue's, out of scope). It is not movable by any in-tree lever.

---

## 5. What remains worth doing (orthogonal to the floor)

These help **avg/max** and power, not the capacity-bound floor:

1. **PGO** — the pre-named pivot. Overnight already concluded PGO is *worth shipping for avg/max*
   (sped light frames, no floor regression). It has not yet been re-applied **on top of Phase C**
   (the new best-known-good `.text`); doing so is the clean next shippable artifact. Procedure
   (from `PERF-OVERNIGHT-LOG.md`): build instrumented → collect a combat profile by **gdb-dumping
   `__llvm_profile_write_file()` from the live PID** (the game has no clean exit) → merge → build
   optimized → gate (detdiff) → A/B avg/max + floor.
2. **Combat dead-work micro-opts (avg only, in `librexruntime.so`):** `GetLoggerRaw` ~2-3 % on the
   CP hot path with no info-level logging in combat (gate it like the prior `GetRegisterInfo`
   fix); and investigate `TimerQueue::TimerThreadMain` at **8.70 %** self-time — a timer thread
   actively executing (not blocked in futex) suggests a poll/spin that could be quieted to free a
   core and reduce L3/scheduler pressure. (Prior `.so` micro-opts were floor-neutral; expect
   avg-only here too, but the timer-thread share is large enough to be worth a look for avg/power.)

---

## 6. Final state / provenance

- **No build change this session** — the go/no-go was answered from the prior session's already-
  measured D.1/D.2 `.text` numbers + a new live profile. Best-known-good remains **Phase C**
  (`south_park_td` md5 `dc32b4e1`, `.text` 19,610,528, `librexruntime.so` `1996b550`), already
  committed (`47ccd07` in the port; superproject `509c217`).
- **New artifacts:** this report; `tools/perf/combat_profile_2026-05-29.txt` (the live per-symbol
  capture); `tools/perf/cp_profile.sh` (the self-contained boot+profile harness used to get it).
- **Decision recorded:** GPR-as-local floor lever = **CLOSED (NO-GO, measured)**. GPU-offload pivot
  = **CLOSED (NO-GO)**. The floor is hardware-bound. Orthogonal avg/max work (PGO, dead-work
  micro-opts) is the only remaining in-tree perf headroom.

---

## 7. CORRECTED ROOT CAUSE — the floor is branch-misprediction-bound (2026-05-29)

This section **supersedes** the "I-cache / L2 / DSB capacity wall" mechanism asserted in §3b, §3c,
§4 and in the earlier reports (`PERF-OVERNIGHT-REPORT.md`, `CODEGEN-*-REPORT.md`). Those reports
correctly observed *front-end-bound* and correctly found every size-reduction lever neutral — but
they mis-attributed the cause to instruction-fetch **capacity**. A direct front-end breakdown shows
it is **branch misprediction**.

### 7.1 The decisive measurement (`tools/perf/floor_rootcause.sh`, live Stan's House combat)

**TMA front-end (level-3) breakdown — where the fetch stall actually comes from:**

| cause | share of stall |
|---|---|
| **branch resteers (BACLEARS.ANY = 1.85 B)** | **39.3 %** ← dominant |
| i-cache misses (ICACHE_16B.IFDATA_STALL) | 3.6 % |
| iTLB (ICACHE_TAG.STALLS) | 3.4 % |
| MS switches (microcode) | 2.3 % |
| DSB↔MITE switches | 1.1 % |
| LCP (length-changing prefix) | 0.0 % |

The stall is **~11× more branch-resteer than i-cache.** The "capacity" levers were neutral because
i-cache was never more than ~4 % of the problem.

**Corroborating facts (all from the same run):**
- **Hot functions are tiny** — `sub_821B9270` (12.6 % of all self-time) = **464 B**; `sub_821C6E58`
  = 669 B; `sub_8244CE40` = 31 B; the GPR helpers 66/74 B. Each fits L1i (32 KB) hundreds–thousands
  of times over. A 464 B hot function cannot be "i-cache capacity"-bound; it stalls on *mispredicts*.
- **Main thread = 1.0 CPU utilized** → genuinely CPU-bound, NOT blocked on pacing/sync. (Rules out
  the "it's just the present-pacing bug" hypothesis for the floor — that bug causes the *sinusoid*,
  a separate issue.)
- **GPU 17–27 % busy** → not GPU-bound (confirms prior).
- **~178 M instructions per rendered frame** at the ~25 fps dips (121 B insn / 20 s ÷ ~34 fps avg) —
  enormous over-execution for a 2010 tower-defense title (the recompiler expands ~4–7× per guest insn).

### 7.2 Mispredict breakdown by branch TYPE (`tools/perf/branch_breakdown.sh`, per 10 s combat)

| branch type | mispredicts | share | mispredict rate |
|---|---|---|---|
| **conditional** | 148.5 M | **69.2 %** | **22.1 %** (672 M conditional br) |
| **return** | 51.9 M | **24.6 %** | **20.6 %** (252 M returns) |
| indirect | 9.5 M | 4.5 % | — |
| call | 1.1 M | 0.5 % | — |

Branch **density: 1 conditional branch every 27 instructions**; 1 call/94; 1 return/73. Healthy
native code mispredicts <2–5 %; here conditionals miss at **22 %** and returns at **21 %** — both
pathological.

### 7.3 What this means — and what is plausibly FIXABLE (hypotheses, not yet tested)

The floor is **not a hardware capacity wall** — a 22 % conditional + 21 % return mispredict rate is a
**codegen pathology**, and it would also be less severe on a CPU with a bigger BTB / better predictor
(so the user's "it's a Core i9, this is absurd" intuition is right — and a *different* desktop CPU
will likely floor higher). Two candidate levers, in priority order, each to be **measured** (detdiff
gate + `ab.sh` floor A/B) before believing:

1. **Return mispredicts (24.6 %, RSB).** Returns missing at 20.6 % means the CPU's Return-Stack-Buffer
   is desynced — typical when a recompiler's guest call/return doesn't map to host `call`/`ret` pairs
   (tail-call threading, indirect `blr` dispatch, or call-depth > 16-entry RSB). *Investigate how
   `blr` is emitted* (`builders/branch.cpp` / `REX_CALL_INDIRECT` in `generated/.../*_init.h`): if a
   guest return goes through the indirect function-table call rather than a real `ret`, the RSB can't
   work. Making returns real `ret`s (or adding an RSB-friendly thunk) could recover a chunk of the
   24.6 %. **This helps on every CPU.**
2. **Conditional mispredicts (69 %, 22 % rate).** Harder — partly the guest's own data-dependent
   logic (tower targeting/AI), partly possibly the recompiler emitting CR-flag-derived branches that
   alias in the predictor. PGO *should* help branch layout but was floor-neutral — worth re-checking
   *why* (does PGO actually feed branch-weight to the recompiled conditionals, or only to the C++
   wrapper?). Lower-confidence; needs a focused look at how guest `bc`/CR comparisons are emitted.

### 7.4 Honest status of this correction

This overturns the headline conclusion of three prior reports. What is **certain** (measured,
reproducible): the floor is misprediction-bound (39 % resteers vs 4 % i-cache), conditional+return
dominated, on tiny hot functions, CPU-bound, GPU-idle. What is **hypothesis** (not yet built/tested):
that the return/RSB path is fixable in codegen for a real floor gain. The NO-GO verdicts on
GPR-as-local and GPU-offload are unaffected (both were size/throughput levers that never addressed
misprediction). Next concrete step if the floor is to be pursued: read the `blr`/return emission,
prototype an RSB-friendly return, gate + floor-A/B it. Tooling for this section:
`tools/perf/floor_rootcause.sh`, `tools/perf/branch_breakdown.sh`.
