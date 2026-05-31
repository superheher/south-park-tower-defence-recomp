# South Park recomp — the "structural floor" was a MEASUREMENT ARTIFACT (natural-load re-measure)

> **Bottom line (2026-05-31, prompted by the user pushing back a 3rd time):** the prior campaign's
> conclusion — "the 15-20 fps heavy-combat floor is the title's own fixed-60Hz cost, structural, accept
> it" (`FLOOR-STRUCTURAL-CONCLUSION-REPORT.md`) — was derived from probes that **artificially spam the
> wave-spawn button every 0.3 s**. A clean **normal** level-1 playthrough does NOT show a flat 15-20 fps
> floor: it runs 45-60 fps and **degrades with on-screen entity count**, dipping to 10-15 fps only as
> waves accumulate. The real Xbox 360 runs level 1 with no slowdown, so this is **genuine, fixable recomp
> overhead**, not the game's inherent cost. This report supersedes the "structural/accept it" framing.

## What the user was right about (all three)
1. **Level 1 is not "heavy combat."** A handful of enemies + a few towers. The "heavy combat" label came
   from my probes hammering wave-spawn, an overload normal play never produces.
2. **The dips are real and they start late.** Normal play: fine early, drops "by the end of wave 2" — i.e.
   the slowdown tracks accumulated entity count, exactly as observed.
3. **The 360 reference is the right yardstick.** Native hardware has no level-1 slowdown ⇒ our slowdown is
   overhead to fix, not "how the game is."

## Method (reproducible, no rebuild — uses the SHIPPED .so 1a3f6076)
- `/tmp/normal_play_capture.sh` — drive in via menus (NOT wave-spam), one press / 7 s (human cadence),
  4 min, then merge `pacing-diag` (fps) + `Creating graphics pipeline state…` (shader/pipeline compiles) +
  `[PROF-LOAD] … from disk` (asset streams), all timestamped, from `run.log`. → `/tmp/normal_play_timeline.txt`
- `/tmp/natural_profile.sh` — drive in, let entities accumulate at 1 press / 8 s, wait for fps≤18 **on its
  own**, then `perf record -p <pid>` (whole process, LBR, 15 s); break down by thread / DSO / symbol.
  → `/tmp/natural_profile.txt`, `/tmp/perthread.txt`
- The runtime already logs every compile and every disk-load with timestamps — no instrumentation needed.

## Finding 1 — normal play: entity-scaled, NOT a flat floor, NOT transient stutter
Normal 4-min level-1 timeline (`loading=false` throughout; **0 pipeline compiles, 0 disk-loads** during
play — all ~40 compiles fired in a 4 ms burst at load, 3 s before gameplay, on this warm machine):
```
early:  57 60 60 57 46 47 50 44 52 56 50 52 45 30 59 43 50 59 48 46   (≈45-60, occasional ~30)
late:   …→ 16 15 14.9 12.4 10.2 9.9   (degrades as waves accumulate)
```
⇒ The "transient compile-stutter" regime does NOT explain the warm-machine dips (no compiles fire). The
"flat 15-20 floor" is false. The cost **scales with entity count**. (Cold-cache first-run stutter is a
**separate, real** issue — matches the user's "first 7 days lagged in the same spots then stopped" =
on-disk pipeline cache filling over days — fixable by shipping a pre-warmed cache for fresh installs.)

## Finding 2 — natural deep-dip profile (fps started 15.9 on its own; GPU 3%; cores free)
**TIME BY THREAD** — NOT single-thread-pegged; 3 threads carry it:
| thread | % |
|---|---|
| GPU Commands (CP) | **43.7%** |
| Main XThread (guest) | 26.5% |
| Audio Worker | 17.0% |
| rest | small |

**The serial chain, made visible:**
- Main XThread spends **13.3% of the whole process in `__imp__sub_821B9270`** — i.e. **half the guest
  thread is just waiting for its own render**; the rest of the guest is a flat <0.6% tail (small compute).
- So: `guest sim (small) → WAIT on CP (13%) → CP translates ~816 draws (43%, broad/flat) → present`.
  **The CP is the long pole; the guest blocks on it; the GPU is idle.** On the 360 the CP was dedicated
  hardware running concurrently with the CPU sim — here it's a software thread the guest waits on
  synchronously, so the per-frame cost the 360 hid in parallel hardware is serialized onto our timeline.

**CP per-symbol is FLAT — no hotspot:** WriteRegister 4.2+1.1%, ExecutePacketType0/3 + ExecutePacket
4.85%, descriptors (radv_UpdateDescriptorSets + UpdateBindings) 2.6%, IssueDraw / RenderTargetCache /
RequestTextures / UpdateSystemConstants / LoadShader / texcache-hash ≈5%. ⇒ deleting any single function
saves <1-4% — which is **why 7 sessions of micro-opts were floor-neutral** (that part of the old
conclusion was correct; the framing around it was not). It is throughput-bound, ~816 draws/frame of
genuine PM4→Vulkan translation, scaling with entities.

**Audio Worker 17% is a busy-poll, not contention:** `AudioSystem::WorkerThreadMain → WaitMultiple`
(`threading_posix.cpp`) is a **1 ms polling loop** that trylocks 9 handles + checks + unlocks every
iteration (~1000 wakes/s). Off the critical path. (CP's real lock contention is tiny, ~0.65% in
`SharedMemory::RequestRange` under `IssueDraw`.)

## Honest lever ranking (on real data — these are NOT exhausted)
1. **Overlap CP-translate(frame N) with guest-sim(frame N+1).** The guest waits synchronously as if the
   CP were instant 360 hardware. If ~16 ms sim and ~17 ms CP overlapped instead of serializing, a 33 ms
   frame could approach ~17 ms — a real fps jump, using the idle cores. **Never actually tried** — the
   prior "block-on-signal" only de-spun the *wait*, it did not remove the *serialization*. Architectural
   and render-risky (double-buffer guest-visible GPU state, careful fences). Highest value.
2. **Reduce draw count** (batching) — re-aimed at real combat load now (the dominant sprite shader
   `adf7088205c03df9` is ~72% of draws). Big/risky; see `DRAW-BATCHING-STEP2-PLAN.md`.
3. **Fix the audio busy-poll** — would free ~17% CPU + a core but is **off the critical path**, so
   expected fps-neutral (efficiency/heat only). ⚠ A naive de-spin BACKFIRES — see next section.
4. **Per-symbol micro-opts** — confirmed marginal. Don't.

## Negative result this session — naive audio de-spin BACKFIRED (reverted)
Attempted the "quick win": replace `WaitMultiple`'s uncontended 1 ms `sleep_for` with a bounded wait on
the shared `g_wait_notify_cv`, and notify it from every signal site. It **smoke-tested correct** (booted,
rendered pixel-correct) but a re-profile showed a **~3× REGRESSION**: Audio Worker 17%→49%, mutex
trylock+unlock ~10%→~29%, libc 18.7%→38.6%. **Cause = thundering herd:** the shared wake-all CV woke the
audio worker on *every* unrelated signal in the emulator (thousands/s in combat), each wake re-running the
9-handle scan; the old `sleep_for(1ms)` had been *throttling* the poll. **Lesson:** a per-object-mutex
multi-wait cannot be de-spun with a global wake-all CV; it needs a **per-waiter targeted wake**
(eventfd/epoll per wait-set, or a futex-backed `WaitMultiple`) so only signals on *that* waiter's handles
wake it — a real rewrite, not a quick win. Reverted clean (game `.so` `1a3f6076`, source baseline).
Note: the deterministic gate would NOT have caught this (the change was correct, just slower) — the
**re-profile** caught it. Always re-profile an efficiency change, don't just gate it.

## Status
Reopened, not closed. The honest conclusion: **fixable, entity-scaled, CP-serial-translation-bound**, with
two real (risky) levers — CP/guest overlap (highest value) and draw-count reduction. Paused here by user.
