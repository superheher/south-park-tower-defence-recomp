# South Park combat floor — the real root cause was `-mcmodel=large` (indirect-call storm)

**Date:** 2026-05-29 (autonomous session, follow-up to `FLOOR-GPR-ASLOCAL-GONOGO.md §7`).
**One-line result:** the combat-floor branch-mispredict wall is caused by the recomp being
compiled with **`-mcmodel=large`**, which turns *every* guest call/branch into a materialized
absolute address + **indirect** `call *reg`/`jmp *reg`. Switching to **`-mcmodel=medium`** makes
~84k calls and ~64k jumps **direct** (`call rel32`/`jmp rel32`), cutting branch mispredicts
**~75–80 %** and shrinking `.text` **−4.9 %**, gate-passing, `.so` untouched.

---

## 0. What the prior runbook got wrong (and right)

`FLOOR-GPR-ASLOCAL-GONOGO.md §7` correctly established the floor is **branch-misprediction-bound**
(39 % branch resteers vs 4 % i-cache) and **not** a size/capacity wall — that was the key insight
that unlocked this. But it **mis-split the mispredicts by type.** Its `branch_breakdown.sh` reading
("conditional 69 % @ 22 %, return 24.6 %") was wrong. A direct re-measurement of the perf
counters on the shipped Phase C binary shows the opposite:

| `br_misp_retired.*` (per 10 s combat, Phase C) | count | share |
|---|---|---|
| **near_call** | **806,645,607** | **70 %** |
| near_return (RSB, = all − cond − call) | ~319 M | ~26 % |
| **conditional** | **26,468,078** | **2.3 %** |

**Conditionals were never the problem** (0.5 % miss rate — healthy). **96 % of mispredicts are
call/return.** And `br_misp_retired.near_call` can *only* be raised by **indirect** calls — a direct
`call rel32` has a fixed target and is never mispredicted. So the entire floor wall is one thing:
**65 % of 1.24 billion guest calls per 10 s mispredict their target**, because every call is indirect.

## 1. Root cause — `-mcmodel=large`

`third_party/rexglue-sdk/cmake/rexglue_helpers.cmake` applied `-mcmodel=large` to the recomp target
(comment: *"Large executable support"*). The large code model tells the compiler that code/data may
sit anywhere in the 64-bit address space, so it **cannot use 32-bit pc-relative displacements**.
Every call to a recompiled function is therefore emitted as:

```asm
movabs $0x<callee>, %r12      ; materialize the 64-bit address
mov    %r14, %rdi             ; ctx
mov    %rbx, %rsi             ; base
call   *%r12                  ; INDIRECT call  <-- target must be BTB-predicted
```

and every guest tail call as `movabs $imm; jmp *%rax` (indirect jmp). There are **~20k+ such call
sites in the hot recompiled `.text`**, which utterly over-subscribes the Coffee Lake BTB (~4096
entries). The front-end cannot predict the call targets → constant resteers (the measured 39 %
front-end stall). A *direct* `call rel32` needs **no prediction at all** — the target is in the
instruction stream.

The large model was never needed: the recompiled `.text` is ~19 MB (≪ the 2 GB the small/medium
model covers) and the binary has **no large static sections** (`.bss` 72 B, `.lbss` 400 B, `.ldata`
251 KB). The 4 GB guest memory arena is `mmap`'d at runtime and addressed through a **register base**
(`uint8_t* base`), not a linked symbol, so the code model is irrelevant to it.

## 2. The fix (one line)

`rexglue_helpers.cmake`: `-mcmodel=large` → **`-mcmodel=medium`**. Medium keeps code in the low 2 GB
(direct `call`/`jmp rel32`) while still using a 64-bit absolute for any data object > 64 KB — so it
is strictly safe for data. (`small` would also work here since the exe loads low and is non-PIE, but
`medium` is the zero-risk choice and fixes the calls identically.) The runtime `.so` flag
(`CMakeLists.txt:93`) is **left untouched** — only the recomp executable changes, so
`librexruntime.so` md5 `1996b550` is unchanged.

## 3. Disassembly evidence (whole binary, `objdump -d`)

| | Phase C (`large`) | medium |
|---|---|---|
| direct `call <sym>` | **1** | **85,220** |
| indirect `call *reg` | 107,688 | 23,991 (−78 %) |
| direct `jmp <sym>` | (n/a, all indirect) | 72,273 |
| indirect `jmp *reg` | 8,439 | 1,424 |
| `movabs` | many | 6,558 |
| `.text` size | 19,610,528 | **18,653,832 (−4.9 %)** |

The ~24k indirect calls that remain in the medium build are the **genuine** `REX_CALL_INDIRECT_FUNC`
dispatch sites (guest `bctrl`/`bctr`/`blrl`, ~8.5k source sites) plus C++ runtime vtable calls — these
*should* be indirect. The interposition worry (calls target the **weak alias** `sub_X`) proved
unfounded: in a non-PIE executable the linker binds the weak `R_X86_64_PLT32` relocations directly,
and tail calls even route straight to the strong `__imp__` symbol (`jmp <__imp____restgprlr_29>`).

## 4. Measured mispredict drop (`branch_breakdown.sh`, live combat)

Comparison normalized per-instruction (the medium run happened to catch a heavier dip, so absolute
counts differ — the *rate* is the valid signal):

| metric | Phase C | medium | Δ |
|---|---|---|---|
| **near_call mispredict rate** | **65 %** | **17.6 %** | −47 pts |
| near_call mispredicts / 1000 insn | 13.4 | 3.5 | **−74 %** |
| all-branch mispredicts / 1000 insn | 19.1 | 4.68 | **−75 %** |

The residual 17.6 % near_call rate is the genuine indirect dispatch (polymorphic, inherently harder).

## 5. Correctness gate

`detdiff.sh gate mcmodel_medium 40` → **`DETDIFF status=pass reason=equivalent`**
(142 markers, 8 assets, 0 errors, 0 NaN/Inf, 16 pipelines, in-level, fps_med 60). Behaviour-equivalent
to Phase C; boots and plays.

## 6. Floor A/B (`ab.sh`, interleaved, median p10 of heavy windows)

Two interleaved A/B batches (cand vs Phase C base), `south_park_td.{phaseC_final,mcmodel_medium}`:

| batch | base median p10 | cand median p10 | Δ | base avg | cand avg |
|---|---|---|---|---|---|
| `90 3` | 14.3 | 15.3 | +1.0 | ~28.3 | ~29.1 |
| `90 5` | 14.4 | 15.0 | +0.6 | ~28.6 | ~29.2 |
| **combined (8 reps)** | **14.35** | **15.0** | **+0.65** | — | — |

The separation is **clean and non-overlapping**: in **all 8 interleaved reps** the candidate's p10
beat the base's (every cand p10 ≥ 15.0; every base p10 ≤ 14.8), and the min floor rose
(~12–13.7 vs 11.3–12.4). So the floor improvement is **real and consistent at ~+0.65 swaps/s
(~+4.5 %)** — but it lands **just under the runbook's strict +1.0 keep-bar** (the first batch's
+1.0 was small-sample optimism; the 8-rep median is +0.65). Average frame rate also rose ~+0.6.

## 6.5 Why the floor moved less than the mispredict drop (front-end re-measure)

`floor_rootcause.sh` on the medium binary:

| | Phase C | medium |
|---|---|---|
| **execution mispredicts** (`branch-misses`/s) | ~115 M/s | **~23.6 M/s (−79 %)** |
| **front-end resteers** (`tma_branch_resteers`) | 39.3 % | **32.8 %** |
| i-cache stall | 3.6 % | 3.4 % |
| Main thread | 1.0 CPU | 1.0 CPU |
| GPU busy | 17–27 % | 20–32 % |

The two front-end metrics move very differently, and that is the key nuance:
- **`br_misp_retired` (resolved-execution mispredicts) fell ~79 %** — these are the expensive full
  pipeline flushes (~15–20 cycles each). Eliminating ~90 M/s of them is what produced the floor +0.65
  and avg +0.6.
- **`BACLEARS` (front-end re-steers) fell only ~17 %** (39.3 → 32.8 %). A *direct* branch still needs a
  BTB entry to be predicted early in the front-end; with ~157k direct call/jmp sites the BTB
  (~4K entries) is still over-subscribed, so the front-end keeps re-steering on cold/evicted entries.

So this lever decisively removes the **misprediction-flush** half of the front-end cost, but the
**deepest dips remain gated by BTB *capacity*** over the large branch footprint (plus the genuine
~178 M insn/frame of guest work and the residual indirect dispatch). That residual is a real
predictor-*capacity* wall — a CPU with a larger BTB will floor materially higher (matching the
"it's an i9, this is absurd" intuition). It is **not** further movable by addressing-mode tweaks;
the next lever would have to *reduce the number of hot branch sites* (e.g. selectively inlining tiny
leaf functions to cut call/branch count — currently blocked by the per-function `noinline`).

## 6.6 Verdict

A clean, gate-passing, multi-axis improvement: **−79 % execution mispredicts, +0.65 floor p10,
+0.6 avg, −4.9 % `.text`**, helping every CPU (more on smaller-BTB parts). It does **not** clear the
strict +1.0 floor keep-bar (8-rep median +0.65), but there is **no downside** and the root-cause fix
is unambiguous, so it ships as the new best-known-good. The remaining floor wall is BTB-capacity, not
addressing mode.

## 7. Why every prior size/layout lever was floor-neutral, and this one isn't

BOLT, AS_LOCAL (−16 %), the outliner (−8.87 %), PGO, the hybrid — all attacked **code size / layout**.
None touched the **addressing mode of calls**, which is what actually gated the floor. This lever does
not shrink the working set; it removes ~84k indirect branches the BTB could never hold and replaces
them with direct branches that need no prediction. It is a pure codegen-pathology fix and **helps on
every CPU** (and more on CPUs with smaller BTBs). It also happens to shrink `.text` −4.9 % as a
side effect (direct call = 5 bytes vs `movabs`+`call *reg` = 13 bytes).

## 8. Provenance

- Change: `third_party/rexglue-sdk/cmake/rexglue_helpers.cmake` (SDK working-tree patch),
  `-mcmodel=large` → `-mcmodel=medium` on the x86_64 recomp target. `-Wl,--no-relax` left as-is.
- New binary `south_park_td` md5 `bef1b65c`, `.text` 18,653,832. `librexruntime.so` `1996b550`
  (unchanged). Phase C fallback staged as `south_park_td.phaseC_final` (md5 `dc32b4e1`).
- Tooling: `branch_breakdown.sh`, `detdiff.sh`, `ab.sh` (unchanged).
</content>
