# South Park recomp — overnight framerate report

**Date:** 2026-05-28 → 05-29 (autonomous session, ~23:00 → ~03:10).
**Goal:** raise the *combat* framerate **floor** (p10/min swaps/s on Stan's House) toward 60,
without regressing boot→menu→level. Metric = the FLOOR via interleaved A/B (`tools/perf/ab.sh`),
keep-rule = median `p10` improves **> 1.0** swaps/s AND boots AND GPU stays idle.

---

## 1. TL;DR

- **Baseline floor:** `p10 ≈ 14–15` swaps/s in heavy-wave sessions (`min ≈ 11–13`, `avg ≈ 29–30`,
  `max ≈ 51–58`). (A lighter early session read `p10 ≈ 20.8`; combat load drifts run-to-run, so all
  verdicts below come from **interleaved** A/Bs, never absolute numbers.)
- **Best floor achieved: the baseline.** **No change cleared the +1.0 keep-bar.** Eight independent
  levers were tried; every one is **floor-neutral within noise** (or refuted). One (BOLT-on-`.so`)
  looked like +1.1 at REPS=3 but regressed to +0.3 at REPS=5 — noise.
- **Winning config (staged):** the **unchanged baseline** — port `south_park_td` md5 `024e365a`
  + `librexruntime.so` md5 `1996b550` (the prior session's GetRegisterInfo `.so`).
- **Definition of Done:** **stable 60 in combat was NOT reached, and is not reachable on this HW with
  the available (non-codegen) levers.** This is the *honest, expected* outcome the plan anticipated:
  the recomp is **CPU front-end / I-cache *capacity*-bound** — the hot recompiled code (port `.text`
  22.3 MB + the CP `.so`'s hot code) vastly exceeds the 12 MB L3, and **layout/branch/modest-size
  optimizations cannot shrink the working set enough to matter during the heaviest frames.** Proven
  three different ways below.
- **Secondary win (not a floor win):** **PGO improves average and peak fps** (faster light/medium
  frames) without regressing the floor — worth shipping if smoother *average* play is desired (see §5).

---

## 2. Per-experiment results

All A/Bs interleave base vs candidate per rep and report **median p10 over heavy windows**.
Keep-rule: Δ median p10 **> +1.0** AND boots AND GPU idle.

| # | Change | Scope | base → cand (median p10) | Δ | Verdict | Kept? |
|---|---|---|---|---|---|---|
| P1 | ICF `--icf=all` | port link | .text 23,361,720 → 23,364,673 | ~0 | **refuted** (folds 871 B; needs lld, which *grows* .text 3.8 KB) | reverted |
| P2 | BOLT layout (port) | port post-link | 15.1 → 14.9 (REPS=3) | −0.2 | **neutral** (dyno taken-br −71% on Main, floor flat) | reverted |
| 3a | WriteRegister hot/cold split | `.so` | 14.5 → 12.9 (REPS=3) | −1.6 | **refuted** (added 7 cmp/write vs O(1) jump table) | reverted |
| 3b | trace-writer gating | `.so` | — | — | skipped (mechanism <0.1% of CP cycles ≪ noise) | n/a |
| 3c | hot/branch attrs | `.so` | — | — | skipped (placement hint; 3a refuted the line) | n/a |
| 3d | Type3 dispatch reorder | `.so` | — | — | **no-op** (already a jump table: `jmp *%rax`) | n/a |
| P4 | PGO | port recompile | 14.8 → 14.3 (REPS=3) | −0.5 | **neutral on floor** (avg/max ↑, .text −1.3%) | reverted |
| P4 | PGO∘BOLT | port | 14.3 → 15.0 (REPS=3) | +0.7 | **neutral** (within noise; BOLT-on-PGO taken-br only −6.8%) | reverted |
| P5 | -Os size flag | port | 14.9 → 14.6 (REPS=3) | −0.3 | **neutral** (.text −4.2%, still ≫ L3) | reverted |
| 6a | timer spin→blocking | `.so` | — | — | **refuted** (won't compile: blocking timed-wait vs steady_clock) | reverted |
| 6b | turbo re-confirm | host | — | — | inconclusive (turbo-off window light); "marginal" prior stands | n/a |
| BONUS | **BOLT the CP `.so`** | `.so` post-link | 14.6 → 14.9 (REPS=5) | +0.3 | **neutral** (REPS=3 gave +1.1 = noise; CP taken-br −61.8%, floor flat) | reverted |

`-O2`, `--gc-sections`, ThinLTO, and AutoFDO were not run — see §5 for which and why (each either
bracketed-neutral by the results above, or unavailable/low-EV); this is logged, not silently skipped.

---

## 3. Mechanism evidence (why the wins didn't reach the floor)

**Baseline micro-arch (combat, `tools/perf/profile.sh`):**
- Topdown frontend-bound **45.1%** whole / **46.9%** Main / **52.3%** CP. Backend only 20.4 / 8.6 /
  15.1% — *not* data/memory-bound.
- Front-end L2 split: **fetch-latency 32.6% ≫ fetch-bandwidth 14.5%** → icache/iTLB/resteer latency,
  i.e. the hot code doesn't *fit*, not that the decoder is slow.
- IPC **0.78**; **L1-icache-miss 614 M** (≈3× the 205 M dcache-miss); branch-miss **11.8%**;
  **DSB:MITE uops = 4.1 B : 19.8 B ≈ 17 : 83** (83% from the legacy decoder — the uop cache is
  swamped by the 22 MB footprint). dTLB 0.06% (non-issue).
- `catch_dip` (final-config re-validation): at every floor dip (18–21 fps) the **GPU engine is ~20%
  (idle)** while **both Main and the CP thread are ~90–100%** → CPU-bound, and the floor is
  **co-gated by Main AND the CP `.so`**.

**The layout techniques worked at the micro-arch level but the floor didn't move:**
- BOLT(port) dyno-stats: **taken branches −71.4%**, taken-forward −98%. BOLT(`.so`): **taken
  branches −61.8%**. Huge, real branch-layout improvements — yet **both floor-neutral**. Reordering
  changes *which* lines are touched, not *how many*; the heavy-frame working set still overflows L1i/
  L2/L3.
- PGO: `.text` −1.3%, **0 profile-mismatch**, and it **raised avg (≈+1) and max (51→57)** — it
  genuinely sped the common path — but the **floor (heaviest frames) was flat/−0.5**. PGO∘BOLT added
  only −6.8% taken branches on top of PGO (they overlap) → also neutral.
- Pure size: ICF folded **871 B** (codegen'd PPC functions aren't bitwise-identical). -Os cut `.text`
  only **−4.2%** (22.38 MB) — still ≫ 12 MB L3 — and was floor-neutral/slightly-worse (less-optimized
  code is marginally slower per-instruction).

**Conclusion:** the combat floor is bounded by **instruction-fetch *capacity*** during the heaviest
frames. The only things that would move it are (a) a large *absolute* reduction of hot code size
(needs codegen-level change — out of scope) or (b) fewer guest instructions per heavy frame
(algorithmic — out of scope). Layout, branch-prediction, and ≤4% size cuts cannot, and the data
shows they don't. PGO is the one lever with a real (non-floor) benefit.

---

## 4. Final host & build state

- **Host knobs:** governor `performance`, `no_turbo=0` (turbo ON), `perf_event_paranoid=-1`,
  `kptr_restrict=0`. (Toggled `no_turbo` during 6b; **restored to 0**.) CPU i9-8950HK (6C/12T,
  Coffee Lake, base 2.9 / turbo 4.8 GHz). 31 GB RAM, 65 GB free disk.
- **Staged (the kept best = baseline), in `out/build/linux-amd64-release/`:**
  - `south_park_td` = md5 **024e365a** (== `south_park_td.baseline`)
  - `librexruntime.so` = md5 **1996b550** (== `librexruntime.so.good`)
- **Fallbacks on disk:** `south_park_td.baseline` (024e365a), `librexruntime.so.good` (1996b550).
  Also kept for reference: `south_park_td.{pgo,pgobolt,os,relocs}`, `librexruntime.so.{bolt,relocs}`,
  and `/tmp/sp/port.profdata` (the PGO profile), `/tmp/sp/*.perf` (BOLT profiles).
- **SDK working tree: reverted to its original patch state** (timer_queue.cpp pristine,
  command_processor.cpp/.h back to the GetRegisterInfo-only patch; 36 modified files, unchanged from
  session start). **No net SDK source change this session**, so the existing patch
  (`patches/rexglue-sdk-current-full.patch`) is no more out of date than before; if the project's
  patch was already behind the tree, regenerate from the SDK dir with the project's usual
  `git diff > patches/rexglue-sdk-current-full.patch`.
- **Tooling fixes made this session (kept — they harden the harness, no game-behaviour change):**
  1. `tools/gamectl.sh`: wrapped `wid`(xdotool) in `timeout 4` and the post-nav screenshot `import`
     in `timeout 6` — a transient X glitch had hung `play` for 8 min in `wait4`, which (since
     `ab.sh` calls `play` with no timeout) would stall every A/B.
  2. `tools/gamectl.sh kill_all()`: now `rm -f /dev/shm/xenia_memory_*` after confirming no live
     instance. **The game leaks a 4.5 GB `/dev/shm/xenia_memory_*` per launch and never removes it;**
     58 had accumulated (13 GB) and filled the 16 GB tmpfs → **SIGBUS on launch** mid-session. This
     fix self-reclaims them so long autonomous A/B sweeps don't die. (See memory
     `sp_devshm_xenia_memory_leak`.)
- Extra build dirs created (safe to delete to reclaim ~1 GB): `out/build/linux-amd64-{pgo-gen,
  pgo-use,size-os}`.

---

## 5. What's left / recommendations

**Is 60 fps floor reachable on this HW?** Not via host knobs or post-/compile-time layout/size on the
current recompiled output. The floor is a hard I-cache-capacity wall (22 MB port + large `.so` hot
code vs 12 MB L3). Honest ceiling with these tools ≈ the current baseline (~14–21 p10, swinging with
wave density; avg ~30, max ~57).

Highest-leverage ideas **not** done (in rough priority):
1. **Codegen-level hot-code-size reduction (the real fix).** The lever that matters is *absolute*
   `.text` shrink, which only the recompiler can deliver: e.g. de-duplicate the repeated PPC→x86
   prologue/epilogue/byte-swap boilerplate into shared helpers; emit smaller sequences for common
   idioms; outline cold guest blocks. Out of scope tonight (plan said don't touch codegen) but it's
   where a *floor* win lives.
2. **Ship PGO anyway for better average/peak.** PGO (`-fprofile-use`) raised avg/max with no floor
   regression and 0 profile-mismatch — smoother typical play. The profile is at
   `/tmp/sp/port.profdata`; rebuild via the `-pgo-use` recipe in the log. **Profile collection caveat:
   the game has no clean-exit path** (ignores SIGTERM/SIGINT/WM_DELETE; `-fprofile-continuous` failed
   to page-align even under lld) — profiles were captured by **gdb-dumping `__llvm_profile_write_file()`
   from the live process** (documented in the log); reuse that.
3. **AutoFDO** (sampling PGO; needs `create_llvm_prof`, which isn't installed — build from the autofdo
   repo). Lower-overhead than instrumented PGO and sidesteps the flush problem, but expect the same
   *floor*-neutral / avg-positive shape as PGO.
4. **Use the idle GPU.** GPU sits ~20–28% busy at the floor — there's ~75% headroom. Rendering at
   higher internal resolution / better filtering is ~free (won't hurt the CPU-bound floor) and is the
   one place to spend the slack for visible quality.
5. **Don't** re-try: ICF, BOLT (port or `.so`), PGO∘BOLT, -Os, the CP micro-opts, thread pinning,
   THP/huge-pages, turbo — all measured neutral/negative here or in the prior session.

---

## 6. Memory updated

`sp_combat_perf_frontend_bound` revised with the verdicts: front-end diagnosis re-confirmed, and the
**new finding that layout/branch/≤4%-size optimization (ICF/BOLT-port/PGO/PGO∘BOLT/-Os/CP-micro-opts/
BOLT-.so) is all floor-neutral — the floor is capacity-bound; PGO helps avg/max only.** Also added
`sp_devshm_xenia_memory_leak` (the SIGBUS-on-launch gotcha + the gamectl auto-clean fix).

Full chronological detail + the hypothesis ledger: `docs/PERF-OVERNIGHT-LOG.md`. Raw A/B lines:
`tools/perf/results.log`.
