# The audio-worker de-spin, and what it taught us: the floor is LATENCY-bound, not CPU-bound

**Date:** 2026-05-30. **One-line:** The last large CPU waste in a heavy-combat dip is the **Audio Worker
thread**, burning ~20% of the *entire process* in a `pthread_mutex_trylock`/`__pthread_mutex_unlock_full`
storm (upstream Xenia `WaitMultiple` hot-yields with no backoff when it loses a trylock race with the SDL
audio callback). Blocking that spin is a real, gate-clean, .so-only CPU/power win — and it is **floor-
neutral**, because a `perf sched` probe proved the cores are *free* (CP scheduling delay ≈ 5 µs) and the
floor is the **serial render-pipeline round-trip latency**, not CPU throughput or core contention.

> Reads on top of `FLOOR-CP-ELISION-REPORT.md` / `FLOOR-CP-TRANSLATION-REPORT.md` and memory
> `sp_floor_latency_bound`. Base for the keep-bar is **bothfix** (exe `d4b0f50b` + `.so` `605ce3ee`); prior
> working binary is **elis1** (exe `848f191c` cpfence6 + `.so` `5276a282`). This session's candidate is
> **audiofix** = cpfence6 exe (UNCHANGED) + `.so` `8d2bf92e` (patch 0015 on top of elis1).

## 0. Two hypotheses I floated and then REFUTED on-machine (recorded honestly)
A fresh heavydip profile of elis1 showed the Audio Worker as the single biggest CPU consumer
(`trylock` 8.56% + `unlock_full` 5.71% + `WaitMultiple` 3.12% + `signaled` 2.45% ≈ **20% whole-proc**;
`libc` = 22% of all cycles). Audio had **never** been touched in 6 prior floor sessions. I hypothesized
the floor might be **thermal/power-coupled** (i9-8950HK, 45 W) or **CPU-saturated**, so that freeing the
audio spin would let the CP/Main threads clock up and raise the floor. **Both were wrong:**

- **Thermal:** `load_probe.sh` (per-core `scaling_cur_freq`): idle ≈ 4.2-4.3 GHz all-core; **in-dip still
  ≈ 4.1-4.2 GHz all-core.** Temps high (87-92 °C) but **below the throttle knee — no throttle.** My
  earlier "throttled to 3.0 GHz" working note was an assumption written before the data; it was WRONG.
- **Saturation:** in-dip `sum_pcpu ≈ 135%`, `threads>20% = 3`, loadavg ≈ 4.2 on 6c/12t — **~1.35 of 12
  logical cores busy; 11 ~idle.** Not saturated.
- **A workflow-agent mis-read** claimed the *XMA Decoder* spins at 99.5%. `ps` shows it **cold (1.4-2.2%)**;
  its loop (`xma_decoder.cpp:140-164`) already blocks on `work_event_`. It is fine and was NOT touched.

So the only real audio waste is the **Audio Worker `WaitMultiple` busy-poll**, and with 11 free cores,
de-spinning it was *a priori* likely floor-neutral. I did it anyway (real efficiency win) and measured.

## 1. The decisive measurement: the floor is latency-bound (cores are free)
`cp_offcpu.sh` (`perf sched record` over a 10 s heavy-dip window) — the most important new fact of the
campaign:

| thread | on-cpu ms / 10 s | ctx-switches | avg sched-delay |
|---|---|---|---|
| GPU Commands (CP) | 4969 (~50% of ONE core) | 144 058 (~14k/s) | **0.005 ms (5 µs)** |
| Audio Worker | 4148 | 118 723 | 0.005 ms |
| all guest threads | 17 707 | 149 994 | 0.006 ms |

- **CP average scheduling delay = 5 µs.** Whenever the CP becomes runnable it gets a core in 5 µs — **the
  cores are free.** The floor is therefore NOT CPU-core-contention bound, and freeing a spare core cannot
  help it. ⇒ audio de-spin will be FLOOR-NEUTRAL by construction (it idles a spare core).
- **The CP is only ~50% on-cpu and context-switches ~14 000×/s** — it runs in tiny bursts and blocks
  constantly (drain ring → block until the guest produces more / until the GPU/present fence). This
  refines the standing "CP serial-translation **throughput**" framing into **serial pipeline round-trip
  LATENCY**: guest-sim → CP-translate → GPU-exec → present, with every stage partly idle (CP ~50%, GPU
  17-26% idle per prior reports, Main partly parked on the vblank fence). The heavy frame is gated by the
  *length of the serial dependency chain per frame*, not by any one stage being CPU-bound.

## 2. The change (patch 0015) — .so-only, surgical
`src/core/threading_posix.cpp`, `PosixConditionBase::WaitMultiple` (upstream Xenia code). Each pass it
`pthread_mutex_trylock`s ALL handles (the Audio Worker waits on 8 client semaphores + a shutdown event =
9 **robust** mutexes — every unlock takes glibc's `__pthread_mutex_unlock_full` slow path). On the
**contention path** (it loses the all-or-nothing trylock because the SDL audio callback is mid-`Release`
on a client semaphore) the upstream code did `std::this_thread::yield(); continue;` with **no backoff** —
an unbounded hot spin until the other thread drops the mutex.

The fix: on that contention path, **park up to 200 µs on a process-global condition variable** instead of
hot-yielding; **every `PosixCondition<Semaphore>::Release()` notifies that CV**, so a real semaphore
release wakes the waiter promptly. The park is time-bounded, so a missed notify costs ≤ 200 µs and the
loop self-heals; the wait/timeout semantics are unchanged (it never adds a lock nor changes which handle
satisfies the wait), so guest `KeWaitForMultipleObjects` behavior is identical. ~+22 lines, one file.

## 3. Validation
- **detdiff gate** (`gate audiofix 40`): **status=pass reason=equivalent** — behaviorally identical.
- **Mid-combat screenshots** (`smoke_shot.sh audiofix`): 3 full frames captured (fps 29/37/21), **0
  errors/asserts/nan**, render pixel-correct (sprites, HUD, colors, transforms). The change touches only
  thread *wakeup timing*; there is no code path from audio-worker wakeup latency to GPU state, so
  rendering is invariant by construction.
- **heavydip on audiofix:** the Audio Worker's `trylock`/`unlock_full`/`WaitMultiple`/`signaled` entries
  (~20% combined on elis1) **drop out of the top-30 hot functions entirely**, and the `libc` DSO share
  falls **22.4% → 10.3%.** The spin is gone. The new hot set is all CP: `WriteRegister` 7.0%,
  `radv_UpdateDescriptorSets` 7.5%, `ExecutePacket*` ~17%, plus the Main vblank spin 10.7%.
- **Floor A/B** `ab_both.sh 75 6`, interleaved, bothfix / elis1 / audiofix (median p10, heavy windows):

| variant | median p10 | vs bothfix |
|---|---|---|
| bothfix (`d4b0f50b`/`605ce3ee`) | 14.9 (n=4) | — |
| elis1 (`848f191c`/`5276a282`) | 17.7 (n=3) | +2.8 |
| **audiofix (`848f191c`/`8d2bf92e`)** | 18.4 (n=2) | +3.5 (≈ elis1, not audio) |

  (3-way medians from `tools/perf/ab_audiofix.txt` per-variant heavy p10s: bothfix 14.6/14.8/15.0/15.0;
  elis1 15.2/17.7/17.7; audiofix 19.0/17.7. Several reps dropped to `status=stale` — the floor sampler's
  log-staleness guard — leaving few clean per-variant samples, so the medians are noisy.)

  **The marginal that matters is audiofix − elis1 (the audio de-spin itself), and it is ~ZERO.** Both sit
  well above bothfix, but that gap is the **inherited elision1 lever, not audio**. A dedicated interleaved
  **elis1-vs-audiofix head-to-head** (`tools/perf/ab_confirm_2026-05-30.txt`, terminated at 5 clean reps)
  gave **elis1 p10 = 15.4 / 18.2 / 18.9 (median 18.2)** and **audiofix p10 = 18.3 / 18.8 (median ~18.5)** —
  the two interleave within the run and the audiofix−elis1 delta is **inside the ±2 run-to-run noise band**.
  (Note the large cross-run absolute drift, elis1 ~15–17 in one window vs ~18 in another: that *is* the
  documented combat-intensity noise — only the *within-run interleaved* delta is meaningful, and it is ~0.)
  **⇒ the audio de-spin is FLOOR-NEUTRAL**, precisely as §1 predicted (the cores are free at a 5 µs
  sched-delay, so removing the spin idles a spare core rather than giving the CP a core it was denied). It
  does NOT clear the +1.0 keep-bar *as a floor lever* — and per §1 nothing CPU-side can.

## 4. Decision and bottom line
**Keep audiofix** — not as a floor lever, but as a correct, gate-clean, .so-only **CPU/power efficiency
win** (~20% of the whole process eliminated on a laptop the user actually runs), exactly the rationale by
which elis1's generic elision was kept despite a small floor marginal. The kept working binary is now
exe `848f191c` (cpfence6, unchanged) + `.so` `8d2bf92e` (patch 0015 on elis1).

**The real lesson is §1:** the floor is **serial render-pipeline round-trip latency**, with free cores and
a CP that is only ~50% busy. This explains, in one stroke, why **every** CPU-side lever for 7 sessions
(codegen/µop/layout/mcmodel/PGO/BOLT/ICF/ThinLTO/GPR-as-local, all three de-spins, the register-write
elision, and now the audio de-spin) has been floor-neutral: **none of them shorten the per-frame serial
dependency chain.** Only levers that do can move the floor:
1. **Overlap CP-translate with GPU-exec** — the GPU is 17-26% idle, starved by the serial CP burst.
   Pipelining the next frame's command translation against the current frame's GPU execution attacks the
   round-trip directly. (Largest headroom; non-trivial.)
2. **Shorten the per-heavy-frame CP critical path** — draw batching/instancing for the many small
   tower-defense sprites (`radv_UpdateDescriptorSets` is now 7.5%): fewer pipeline lookups / descriptor
   updates / command-buffer records per frame.
3. **Cut sync-wait latency** on the guest→CP→present handshakes (the *latency* of the round-trip, not the
   CPU spent spinning).
A bulk-constant `memcmp`-skip in `WriteRegistersFromMem` remains a valid *throughput* micro-opt but, per
§1, is expected to be floor-neutral too (it cuts CP CPU, not the dependency-chain length).

Artifacts (`tools/perf/`): `audio_thermal_findings_2026-05-30.txt` (raw measurements + the two refuted
hypotheses), `heavydip_elis1_baseline.txt`, `cp_offcpu.sh`/`load_probe.sh`/`turbo_probe.sh` (new probes),
`ab_audiofix.txt`. SDK change = `patches/0015-perf-despin-block-the-audio-worker-multi-wait-conten.patch`.
