# ROADMAP — v2 (post-1.0). The two real levers, preserved in full.

v1.0 (tag `v1.0`, 2026-06-10) ships with a maintainer-accepted residual:
the heaviest waves of level 2+ re-grid to a clean 30.0 fps = exact 2× slow-mo
for tens of seconds (sound/input stay normal). v2 goal: hold ≥ ~50 effective
fps (the "imperceptible" bar) through the heaviest waves of ALL levels.

## The problem, quantified (do not re-derive)

- Serial chain per frame: guest sim → single-thread CP PM4→Vulkan translate →
  present. Translate ≈ **8.4 µs/draw** post-elision (playtest log: xlat
  1060 ms / 125868 draws per 2 s window).
- The title paces itself by an in-stream `WAIT_REG_MEM` on the vblank fence
  plus the XE_SWAP vsync limiter (60 Hz). When translate alone overruns
  16.7 ms, every swap interval becomes exactly 2 vblanks ⇒ clean 30.0 grid;
  sim ticks once per rendered frame ⇒ exact 2× time dilation.
- Measured peaks: level 1 ≈ 1800 draws/swap max (night m9); level 2 =
  **1900–2250** (maintainer playtest 2026-06-10, ~35 s @30.0). Levels 3+ NOT
  measured yet. Current 60-fps ceiling ≈ 1990 draws (16.7 ms ÷ 8.4 µs).
- NOT levers (verified): vsync=false (sim then runs at render speed =
  fast-forward on light scenes); all 24 closed lever classes in
  `docs/archive/FLOOR-*` (codegen, PGO/BOLT, spins, elision beyond shipped,
  GPU offload, order-safe batching, …). Do not retry.

## Step 0 — size the real target (cheap; FIRST thing in any v2 session)

Bench the biggest wave of each remaining level (natural play; pacing-diag
v2.1 `draws` + `iv` fields; `tools/perf/protocol/measure45.sh` pattern).
Decision input: if peaks stay ≤ ~2300 draws/swap, lever A alone may suffice
(needs ≤ ~7 µs/draw, a −15–20% translate cut); if 3000+, lever B is required
(and possibly A too).

## Lever A — texture-array draw batching (translate-cost cut; effort XL)

- Why: per-draw translate cost is the ceiling. Order-safe batching of
  adjacent same-state draws is already proven ~floor-neutral (avg run length
  1.36 — `docs/DRAW-GROUP-BREAKDOWN.md`); the real headroom (~7× run length)
  requires batching ACROSS texture switches ⇒ texture arrays / bindless.
- Recipe and measured group stats: `docs/DRAW-BATCHING-STEP1-RECIPE.md` +
  `docs/DRAW-GROUP-BREAKDOWN.md` (which state changes break groups, how often).
- Expected: raises the 60-ceiling for all levels; no pacing-semantics risk.
- Risks: texture-cache/shader plumbing (array layers, sampler compatibility),
  render correctness — gate with screenshots + detdiff (translation changes).

## Lever B — true guest↔CP pipelining + swap-at-ring-arrival (deep; multi-session)

- Why: removes the 2-vblank re-grid cliff even when translate > 16.7 ms —
  turns "exact 30.0 + 2× slow-mo" into honest 40–56 fps with 1:1-ish time.
- Two coupled changes:
  1. **Double-buffer the guest-visible GPU state** the CP translate reads
     (register file snapshot / shadow state), so guest sim frame N+1 mutates
     state while the CP still translates frame N. Today the serial handoff is
     what makes the chain latency-bound (guest waits its own render —
     `__imp__sub_821B9270`, measured 13.3% of Main).
  2. **Advance the guest's swap/vblank fence when XE_SWAP arrives in the
     ring** (not when present completes): the guest starts its next frame
     immediately; the present pipeline drains one frame behind.
- Risks / known interactions:
  - State hazards: the unchanged-write elision cache (patches 0013/0014) is
    global — under double-buffering it must become per-frame or be rebuilt at
    snapshot boundaries.
  - +1 frame of input→photon latency (acceptable; the title already paces).
  - Determinism gate (`tools/perf/detdiff/`) applies — translation order may
    not change, only its overlap with sim.
  - Boot present/fence deadlock history (patch 0005): the first-present path
    must keep its current semantics — gate boot 10× before anything else.
- Prior art in-tree: patch 0019 (eager fence push) is "step 1" of this lever
  and shipped in v1.0; its pacing-diag v2.1 counters (`wrm/swp/xlat`) are the
  instrumentation for the full version.

## Windows port checklist (maintainer asked, 2026-06-10)

The series is platform-neutral by construction EXCEPT:

1. `rex::thread::VblankBackoffWait()/NotifyVblank()` (patches 0012/0019):
   declared in shared `include/rex/thread.h`, implemented ONLY in
   `src/core/threading_posix.cpp` ⇒ Windows build FAILS AT LINK. Fix is
   mechanical (~15 lines): the impl is pure std::condition_variable — move it
   to shared code (or duplicate in `threading_win.cpp`).
2. `freq_keeper` (patch 0018) calls `pthread_setname_np` in the shared
   `src/graphics/graphics_system.cpp` ⇒ Windows COMPILE error. Guard with
   `#ifdef` + `SetThreadDescription`/`SetThreadPriority(THREAD_PRIORITY_IDLE)`.
   (The layer-A trap itself is a Linux intel_pstate/EPP property; on Windows
   the analog is the power plan — the trap may not exist there at all. Cvar
   defaults to false, so no behavior risk.)
3. Patches 0003/0011 touch `seh_win.cpp`/`window_win.cpp` — those edits have
   NEVER been compiled in this project (Linux-only builds): expect first-build
   fallout there.
4. `-mcmodel=medium` blocks are already guarded (`if(NOT WIN32)` in root
   CMakeLists, `if(UNIX AND NOT APPLE)` in rexglue_helpers.cmake) — Windows
   unaffected (MSVC never had the large-model indirect-call pathology).
5. POSIX-only fixes (0003 seh_posix, 0015 WaitMultiple despin) fix bugs in the
   POSIX emulation layer; Windows uses native paths that never had them.
6. Host-level EPP fix (systemd `cpu-epp-performance.service`) is Linux-only by
   nature. Harness (`gamectl.sh`, `tools/perf/protocol/*.sh`) is Linux-only
   diagnostics (bash/xdotool/import) — gameplay does not need it.

Everything else (CP fence/boot fixes, Vulkan bring-up, write-elision, eager
fence push, vsync limiter, timer-queue wait strategy, codegen patches, SDL
audio, xam/kernel/loader fixes) is shared C++ over Vulkan/SDL/std — carries
over as-is.

## Gates for ANY v2 change (unchanged from v1.0)

`tools/perf/ab_both.sh 40 3` keep-bar (median p10 ≥ +1.0, all reps pairwise ≥
base) · detdiff when the translation path changes · cold-soak
(`tools/perf/protocol/measure7.sh`) + natural run (`measure45.sh`) · render
screenshots · grid-pure `iv` columns at LOW draws = pacing regression ·
patches stay a granular byte-verified series (`patches/README.md`).
