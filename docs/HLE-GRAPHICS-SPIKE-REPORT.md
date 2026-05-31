# HLE-Graphics Migration Spike — Feasibility Report

> **Status:** 🟡 IN PROGRESS (spike) · **Branch:** `experimental/hle-graphics-spike`
> **Date:** 2026-05-31 · **Author:** Claude Code (driven by project owner)
> **Decision pending:** go / no-go on replacing Xenia-style GPU emulation with a
> native renderer + Direct3D API interception.

---

## 0. TL;DR

**Verdict: 🟢 GO for variant B, done incrementally.** All four make-or-break
questions resolved favorably:

- **Interception works today, no new codegen.** RexGlue's weak-alias override
  replaces any guest function (direct *and* vtable calls) with host C++ — the exact
  facility variant B needs (Q-B2).
- **The surface is small.** ~4 primitive types, ~11 hot pixel shaders, ~43
  texture-sets/frame, no bindless/instancing, fixed 1280×720 (Q-B1). Only **19
  named shaders, with original HLSL recoverable** from the `.updb` blobs (Q-S).
- **The renderer is not from scratch.** Reuse RexGlue's existing Vulkan backend (or
  Plume), cribbing the D3D-API mapping from Unleashed's `gpu/video.cpp` (Q-B3).

**The single real risk:** the binary has **no symbols** — every D3D entry point is
`sub_<hexaddr>`, so they must be identified before hooking. This is **bounded and
mitigated**: the XDK D3D library is a known, community-mapped ABI; the cleanest
hook is the **one D3DDevice vtable** (known method ordering); the named shaders
anchor the shader-set call sites; and **RexGlue already ships `vtable_scanner.cpp`
+ `sig_scanner.cpp`** for exactly this.

**The de-risker:** B is an **incremental hybrid**, not a big-bang rewrite —
intercept the hottest draw/swap functions first as drop-in strong symbols feeding
the existing backend, **measure the FPS win**, leave everything else on the PM4
path, expand only if it pays. Every step is reversible.

**Effort:** ~days to a measured proof-of-concept (identify the device vtable →
intercept Present + the dominant draw → render natively → benchmark); weeks to full
coverage. **Variant A (full XenonRecomp stack) is retained as fallback** — the XEX
is a clean standard XEX2 that XenonRecomp ingests directly.

---

## 1. Why we are doing this — the diagnosis (established before the spike)

The perf floor of the recomp is **architectural, and confirmed in source**, not a
hypothesis:

- **RexGlue's graphics runtime is Xenia's GPU emulator, verbatim.**
  `third_party/rexglue-sdk/src/graphics/command_processor.cpp:1` opens with
  `Xenia : Xbox 360 Emulator Research Project, Copyright 2022 Ben Vanik …
  @modified Tom Clay, 2026 — Adapted for ReXGlue runtime`. The whole GPU subsystem
  is Xenia 1:1: `xenos.cpp`, `register_file.cpp`, `packet_disassembler.cpp`,
  `primitive_processor.cpp`, `shared_memory.cpp`, `trace_{dump,viewer,writer}`,
  `d3d12/` + `vulkan/` backends.

- **Mechanism** (documented in `knowledge-base/general/75-runtime-graphics-…md`):
  on the 360 Direct3D is a *static XDK library inside the title* that builds
  **PM4 command packets** into a ring buffer. After recompilation that code runs as
  guest code and writes packets into guest memory; RexGlue's **command processor**
  parses the ring buffer every frame and translates draws/state/bindings/shaders
  into Vulkan — **single-threaded**.

- **Measured cost** (prior floor campaign, see `FLOOR-CP-TRANSLATION-REPORT.md`):
  a heavy combat frame is **~72% PM4→Vulkan translation (~29 ms)**, 21% sync-wait,
  7% present; **GPU idle 17–26%**, CPU cores free. The floor is the
  single-threaded command-processor *translation throughput*, and every lever that
  stays inside Xenia's model (redundant-write elision, draw-batch via texture
  arrays / bindless) was found to be either a small win or XL / NO-GO.

- **Why Sonic Unleashed Recompiled runs fine on this same host** (3D, real
  shaders) while our 2D title stutters: Unleashed does **not** emulate the GPU. It
  uses XenonRecomp (CPU) + XenosRecomp (shaders translated **ahead of time**) + a
  **native renderer (Plume/RT64)** driven by **intercepting the Direct3D API at a
  high level**. No PM4, no command processor, no shared-memory write-watch. The
  prosody of "2D is simpler" is irrelevant — our cost is the emulation layer, not
  the game.

**Conclusion that motivates this spike:** the floor cannot be broken *inside* the
Xenia model. The escape is to stop emulating the GPU and instead render natively
by intercepting the title's Direct3D API — the Unleashed approach. This spike
measures whether that is feasible **for this title, on this codebase**.

## 2. The two migration routes

| | **(A) Full Unleashed stack** | **(B) Swap only graphics inside RexGlue** ⭐ preferred |
|---|---|---|
| CPU recomp | Re-do under XenonRecomp | **Keep RexGlue codegen** (unchanged) |
| Shaders | XenosRecomp (AOT) | Reuse RexGlue's existing Xenos→SPIR-V, or AOT |
| Runtime | Unleashed base (kernel/audio/input) | **Keep RexGlue runtime** (kernel/XMA/SDL/VFS/SEH fix) |
| Graphics | Unleashed native renderer | **New HLE path: hook D3D API → native renderer** |
| Fixes "patch churn" pain | Yes (leaves the early-dev SDK) | No (stays on RexGlue) |
| Fixes perf | Yes | Yes |
| Path maturity | Proven, documented recipe | Novel integration |
| Reusable beyond this title | New port baseline | **Contribution back to the SDK** — every future RexGlue title benefits |

**Owner's preference: (B).** Rationale: the HLE graphics path becomes a reusable
SDK capability (a theoretical contribution + a win for any future RexGlue title),
and it preserves all the non-graphics work that already runs (kernel/XAM, XMA
audio, SDL input, VFS, the SEH-unwind fix, the autonomous harness, the playable
state). Variant A is retained as the **fallback** if a RexGlue wall blocks B.

This spike is therefore **scoped to validate variant B**, while opportunistically
checking that variant A remains a viable fallback.

## 3. Make-or-break questions (what this spike answers)

| ID | Question | Serves | Status |
|----|----------|--------|--------|
| **Q-B2** | Does RexGlue have a host-function/override mechanism that can redirect the **D3D API** to host C++? | B (critical) | ✅ **PASS** — general weak-alias override, direct+vtable, no new codegen |
| **Q-B1** | Is South Park's **D3D API boundary identifiable** in the binary, and how large is the surface? | A+B (critical) | 🟢 surface small / 🟠 identification is the one risk (mitigated) |
| **Q-S**  | How are shaders translated, how many, and is the output reusable by a native renderer? | A+B | ✅ only 19 shaders, **original HLSL recoverable** |
| **Q-B3** | Can Unleashed's native renderer be reused behind RexGlue's hooks, or do we write a minimal 2D one? | B | ✅ Plume reusable + `video.cpp` reference; minimal 2D renderer viable |
| **Q-A**  | Does the XEX ingest cleanly under XenonRecomp/XenonAnalyse (A-fallback) + binary sanity? | A (fallback) | ✅ standard XEX2, only xam/xboxkrnl imports |

---

## 4. Findings

### Q-B2 — RexGlue host-function / override mechanism  _(B-critical)_

**Why it matters:** Variant B works only if we can stop the static XDK D3D code
from running and redirect those entry points to host C++. RexGlue already
host-implements kernel/XAM; the question is whether that generalizes to arbitrary
guest functions (the D3D library) and how invasive adding it would be.

**Findings — the override facility (first-class, general):**
- RexGlue emits **every** recompiled guest function as a strong impl
  `__imp__<name>` **plus a weak alias** `<name>`
  (`DEFINE_REX_FUNC`, `resources/templates/codegen/init_h.inja:107-109`;
  `REX_WEAK_FUNC`, `include/rex/ppc/context.h:50-52`). Defining a **strong**
  symbol `<name>` in the title's `src/` **replaces** the original at link time
  while `__imp__<name>` stays callable — the documented `PPC_FUNC` pattern
  (`knowledge-base/general/80-patching-hooks-overrides.md:9-27`).
  **Live precedent in this title:** `south-park-recomp/src/stubs.cpp`
  (+ `CMakeLists.txt:11-17`) already overrides weak symbols from `src/`.
- **One definition redirects BOTH call paths.** Direct `bl` sites call the weak
  `name` (`src/codegen/builders/context.cpp:244`); indirect/vtable dispatch indexes
  a per-module table (`PPCFuncMappings`, `init_cpp.inja:22-25`, installed
  `src/system/runtime.cpp:186-205`) via `REX_CALL_INDIRECT_FUNC`. Both resolve to
  the overridable weak symbol. **This is decisive for D3D**, whose device methods
  are virtual (vtable) calls.
- **Not import-limited — works for ANY address**, including functions inside the
  statically-linked XDK D3D library. (Imports differ only in that they emit a
  direct `__imp__<name>` call, `context.cpp:274`; ordinary guest functions emit
  the weak `name`, so overriding one is a pure drop-in strong symbol with **no
  codegen change**.) Mid-asm hooks (`[[midasm_hook]]`) and `[functions]` naming
  are also available (`src/codegen/config.cpp:184-376`).
- **RexGlue already ships identification tooling** — `src/codegen/vtable_scanner.cpp`
  and `sig_scanner.cpp` — directly applicable to locating the D3D vtable / methods
  (the Q-B1 risk).

**Findings — obligations a native path inherits:** bypassing the PM4 ring removes
the CP parse cost but the HLE shims must still drive **present/swap** (today a PM4
`VdSwap` packet → `IssueSwap` → presenter), **EDRAM model + resolve**,
**shared-memory/texture upload + write-watch**, **shader translation** (reuse the
existing `src/graphics/pipeline/shader/spirv_*` translator), and **vsync/fence
pacing**. The HLE shims call the **same Vulkan/EDRAM/presenter backend**, just from
D3D-API entry points instead of from `ExecutePacket`.

**Findings — the de-risking path (incremental hybrid):** the weak-alias facility
supports overriding D3D entry points **one at a time**. So B can start by
intercepting only the **highest-cost, lowest-count** functions (the draw / resolve
/ swap path) as host shims feeding the existing backend, **leaving everything else
on the PM4 path** — a measurable, reversible hybrid with zero codegen changes. The
optional, cleaner SDK contribution is a codegen `[[override]]` table keyed by
address that emits `__imp__` and **suppresses the weak body** (so the original
PM4-building code isn't even compiled) — a low-risk addition and exactly the
reusable capability that motivates variant B.

**Verdict:** ✅ **PASS (make-or-break).** Interception is feasible **today** with
no new codegen; it is general (any address, direct + vtable). Remaining cost is
(a) identifying the stripped D3D functions (Q-B1) and (b) the *scope* of
re-implementing the 360 D3D9 ABI — both made tractable by the **incremental
hybrid** (start with the hottest few functions, measure, expand).

### Q-B1 — D3D API boundary & surface size  _(A+B-critical)_

**Why it matters:** To redirect the D3D API we must first identify its entry
points in the recompiled binary, and the host-side reimplementation cost scales
with how much of the API the title actually uses (small for a 2D sprite game).

**Findings — data-flow boundary (confirmed):**
- The guest→host graphics boundary today is **80 `Vd*` video-kernel exports + a
  single MMIO doorbell**. The doorbell is `GraphicsSystem::WriteRegister` →
  `case 0x01C5 // CP_RB_WPTR → command_processor_->UpdateWritePointer(value)`
  (`src/graphics/graphics_system.cpp:274-289`); GPU MMIO window
  `0x7FC80000–0x7FCFFFFF` (`graphics_system.cpp:144-151`). Ring setup is the
  kernel video shim: `VdInitializeRingBuffer`, `VdEnableRingBufferRPtrWriteBack`,
  `VdSwap` (`src/kernel/xboxkrnl/xboxkrnl_video.cpp:320-572`). The CP spins on
  `read_ptr != write_ptr` → `ExecutePrimaryBuffer` →
  `PM4_DRAW_INDX → IssueDraw` (`src/graphics/command_processor.cpp:297-326,1694`).
- **Direct3D is statically-linked recompiled GUEST code, not a host shim** — there
  are zero D3D/Direct3D kernel exports. The XDK D3D library that builds PM4 lives
  inside the **15,144** recompiled `sub_<addr>` functions.

**Findings — D3D-API identifiability (this is the spike's one real risk):**
- **No symbol/PDB/MAP data exists.** Every function is address-named
  (`DEFINE_REX_FUNC(sub_82100000)`); `config/sp_functions.toml` is **723 bare
  guest addresses** with no names; the manifest names no D3D functions/hooks.
- A hook is **mechanically available**: the recomp's indirect-call seam
  `REX_CALL_INDIRECT_FUNC` / `REX_LOOKUP_FUNC`
  (`generated/default/south_park_td_init.h:275-311`) indexes a per-module dispatch
  table; replacing a slot for a known guest address redirects that call. **But you
  must first discover which `sub_<addr>` is `DrawIndexedPrimitive` / `SetTexture` /
  etc.** — there is no shortcut from this build's metadata.
- **Identifiability = LOW from artifacts, MODERATE with RE.** Mitigations are
  strong: (1) the XDK D3D library is a **fixed, well-known binary** the Xenia /
  XenonRecomp communities have already mapped — signature/pattern matching, not
  blind RE; (2) the **named shaders with embedded HLSL paths** (Q-S) anchor the
  `Set/CreatePixelShader` call sites; (3) **the cleanest hook is the D3DDevice
  vtable** — the XDK `D3DDevice` vtable has a *known, fixed method ordering*, so
  identifying the *one* device vtable (a vtable is already among the 723
  `sp_functions.toml` addresses) and swapping it for host pointers hooks the whole
  device API at once, turning "identify N functions by signature" into "identify 1
  vtable + apply the known XDK layout."

**Findings — surface size (small, favorable):**
- Heavy-combat capture (`docs/DRAW-GROUP-BREAKDOWN.md`): only **4 primitive types**
  (TriangleList/Fan/Strip + QuadList, rare RectangleList); max ~1301 draws/frame
  but **72.5% one pixel shader**, top-5 groups ≈ **98%**, **~11 distinct hot pixel
  shaders**. ~**43 distinct texture view-sets/frame**
  (`docs/DRAW-BATCHING-STEP2-PLAN.md:9-17`); **no bindless, no instancing,
  push-constants = 0**; fixed **1280×720** single front buffer
  (`launcher/INTEGRATION.md:66-79`).
- Reimplementation target is genuinely small: ~19 shaders, textured/untextured
  triangle+quad sprite draws, alpha blend, one render target, a dynamic
  vertex/index buffer, texture binding.

**Verdict:** 🟢 Surface is small and well-understood — favorable. 🟠 The **D3D-API
identification step is the dominant cost/risk**, but it is de-risked by the known
XDK D3D library + shader anchors + the device-vtable hook strategy. Mechanically
hookable once located (`REX_CALL_INDIRECT_FUNC`). **Feasible, with the vtable
identification as the first implementation milestone to prove.**

### Q-S — Shaders & translation path

**Why it matters:** A native renderer needs the title's shaders in a usable form.
RexGlue already translates Xenos→SPIR-V; if that output is reusable we avoid the
"XenosRecomp is Unleashed-specific" risk entirely.

**Findings:**
- **Only 19 named shader programs** ship in `private/extracted/media/shaders/`,
  each a vertex/pixel pair (`.xbv` / `.xbp` + `.updb` debug). Names are semantic:
  `SPHud`, `SPHudWithMask`, `SPHudGrayScale`, `SPTextured`, `SPUntextured`,
  `SPBackdrop{Textured,Untextured,DropShadow}`, `SPDropShadow`, `SPBuildStructure`,
  `SPPoint`, `SpMovie`, `Simple`, `SimpleCol`, …
- **The original HLSL source is recoverable.** The `.updb` blobs embed the literal
  HLSL and original paths (e.g. `private\games\SouthPark\Shaders\SPHud.psh`,
  `struct PS_IN { float4 Color : COLOR; float2 Tex : TEXCOORD0; … }`). ~11 of the
  19 are hot in combat; runtime cache `private/userdata/cache/shaders/shareable/
  58410931.xsh` is only 3392 bytes.
- RexGlue's translator (`src/graphics/pipeline/shader/spirv_*`) is **coupled to the
  CP draw path** (SPIR-V emitted lazily inside `ConfigurePipeline` with Xenia's
  bindful descriptor layout + EDRAM assumptions). Usable as an **offline one-shot
  converter**, but not a drop-in runtime shader source for an independent renderer.

**Verdict:** ✅ Very favorable. Because there are only ~19 shaders **with original
HLSL recoverable**, the clean path is to **author/port the 19 shaders directly**
(no dependence on XenosRecomp's Unleashed-tuned path, no dragging in the bindful
CP/EDRAM context). The `spirv_*` translator remains available as an offline
cross-check.

### Q-B3 — Native renderer reuse (Unleashed / Plume)

**Why it matters:** Determines whether B means "port a proven renderer" or "write
a minimal 2D renderer". For a 2D title the latter may be the faster spike target.

**Findings (web + GitHub):**
- Unleashed's renderer is a **clean, self-contained subsystem**, not smeared into
  engine code: `UnleashedRecomp/gpu/video.cpp` (+ `video.h`) is the whole renderer;
  `gpu/shader/`, `gpu/cache/`, `gpu/imgui/` beside it. Its top-level layout
  (`gpu/ cpu/ apu/ kernel/ hid/ os/ exports.cpp`) **maps 1:1 onto RexGlue's
  subsystem split** (graphics/audio/input/kernel) — so conceptually B slots in.
- The renderer drives **Plume** — a *standalone, reusable* rendering-hardware
  interface (`github.com/renderbag/plume`, D3D12 / Vulkan / Metal). Plume is **not
  coupled** to Unleashed; it could be reused as B's backend.
- `gpu/video.cpp` host-implements the **Xbox 360 Direct3D API** against Plume; the
  guest→host bindings are declared in `exports.cpp/h` via Unleashed's `PPC_FUNC`
  mechanism. This file is the **canonical reference for exactly which D3D API
  functions to implement and how to map each to an RHI** — extremely valuable to
  crib from.
- **What is NOT drop-in reusable:** the guest bindings are tied to Unleashed's
  *binary addresses* and to **its** host-function plumbing (`PPC_FUNC`), and the
  shaders go through XenosRecomp's Unleashed-tuned path. So we cannot copy
  `video.cpp` wholesale into RexGlue.

**Verdict:** ✅ Favorable. **Reusable:** Plume (optional backend) + `video.cpp` as
the D3D-API-mapping reference. **Not reusable:** binary-specific bindings + the
PPC_FUNC plumbing (RexGlue has its own — see Q-B2). **Pragmatic spike target for a
2D title:** a *minimal* native renderer implementing South Park's small D3D
surface (Q-B1) behind RexGlue host functions (Q-B2), cribbing the API mapping from
`video.cpp`. Reusing RexGlue's *existing* Vulkan backend (Xenia's) is an option
too, decoupled from the command processor. Final renderer choice depends on the
Q-B1 surface size.

### Q-A — XEX characterization & variant-A fallback

**Why it matters:** Keeps variant A alive as a fallback and sanity-checks the
binary. (Note: RexGlue already produced a playable build, so the XEX is known to
ingest under RexGlue; this checks the *XenonRecomp* path specifically.)

**Findings:**
- `private/extracted/default.xex` is a **standard `XEX2`** image (magic `XEX2`,
  `file` → "Microsoft Xbox 360 executable, XA-2353, media ID 7F1BDFA7"), 8.5 MB.
- **Imports are only the standard system libraries: `xboxkrnl.exe` and
  `xam.xex`.** The title's own module is `SouthPark.exe`. There is **no separate
  Direct3D import library** → **Direct3D is statically linked into the title**,
  exactly as the KB describes the XDK model. This is the central fact for B: the
  D3D API is *internal functions*, identifiable by signature/symbol, **not** by an
  import table.
- Because it is a bog-standard XEX2 with only xam/xboxkrnl imports, the **variant-A
  fallback is viable at the binary-format level** — XenonRecomp ingests standard
  XEX2 directly. A full `XenonAnalyse` run (jump tables / SEH boundaries) is
  **deferred**: it is only needed if variant B hits a wall, and RexGlue has
  already proven the binary recompiles (playable build exists).

**Verdict:** ✅ Binary is standard and clean. D3D is static (must be identified
internally for B). A-fallback viable at format level; deep XenonAnalyse deferred.

---

## 5. Go / No-Go verdict

**🟢 GO — variant B, executed as an incremental hybrid, with variant A as a
standing fallback.**

The spike set out to disprove the thesis "we can stop emulating the GPU and render
natively by intercepting the D3D API." It failed to disprove it on every axis:

| Make-or-break | Result |
|---|---|
| Can we intercept guest D3D at all? | ✅ Yes, today, no new codegen (Q-B2). |
| Is the API surface tractable? | ✅ Small; 2D sprite subset (Q-B1). |
| Are the shaders a blocker? | ✅ No — 19, HLSL recoverable (Q-S). |
| Do we have a renderer? | ✅ Reuse existing Vulkan backend / Plume (Q-B3). |
| Is the binary even normal? | ✅ Standard XEX2 (Q-A). |

The **only** open risk is identifying the stripped D3D functions, and it is bounded
(one known-layout vtable, community-mapped XDK ABI, in-tree scanners). It is also
**testable cheaply and early** — which sets the decision rule:

> **Decision rule:** the first PoC milestone is to identify the `D3DDevice` vtable
> and render the dominant draw group natively (§7). If that lands and shows an
> FPS win on a heavy frame, **commit to Phase 1**. If D3D-function identification
> proves intractable, **fall back to variant A** (XenonRecomp ingests the clean
> XEX2 directly) — no work is wasted, because the shader port, the surface map, and
> the renderer integration all transfer to A.

Why this is worth doing rather than continuing to optimize the CP: the floor
campaign established that ~72% of a heavy frame is single-threaded PM4→Vulkan
translation with the **GPU 17–26% idle and cores free**. Native rendering removes
that translation cost wholesale — there is real, measured headroom for it to land.

## 6. Effort & risk estimate

**Phased effort** (single developer; estimates, not measurements):

- **Phase 0 — measured PoC (~days–2 weeks).** Identify the `D3DDevice` vtable
  (in-tree `vtable_scanner`/`sig_scanner` + known XDK ABI + shader anchors);
  intercept **Present** (the `VdSwap`/swap path) and the **one** dominant draw
  function (72.5% pixel-shader group); render that group natively to the window via
  the existing Vulkan backend using a directly-ported `SPHud`/`SPTextured` shader;
  **benchmark heavy-frame FPS native-vs-PM4.** This proves the whole thesis
  end-to-end and produces the go/no-go data point.
- **Phase 1 — full small surface (~weeks).** Cover the remaining draw/state/
  resource/texture/blend/resolve calls so the entire game renders natively; keep
  PM4 as the fallback for any not-yet-migrated call (true hybrid).
- **Phase 2 — correctness + cleanup (~weeks).** EDRAM/resolve edge cases, render
  targets, draw ordering/alpha; retire the PM4 path for this title; optionally land
  the codegen `[[override]]` table as the SDK contribution (§8).

**Risk register:**

| # | Risk | Severity | Mitigation |
|---|------|----------|------------|
| R1 | Identifying stripped D3D functions (no symbols) | **Medium** | One known-layout `D3DDevice` vtable; community-mapped XDK ABI; in-tree `vtable_scanner`/`sig_scanner`; shader-call-site anchors. Tested in Phase 0. |
| R2 | Faithfully re-HLE-ing the 360 D3D9 ABI (state/resolve/EDRAM/ordering) | **Medium** | Small surface; reuse existing Vulkan/EDRAM/presenter backend + shader translator; crib `video.cpp`; incremental hybrid bounds blast radius. |
| R3 | Shaders | Low | 19 only; original HLSL recoverable. |
| R4 | Renderer backend | Low | Reuse RexGlue's Vulkan backend or Plume. |
| R5 | Stays on early-dev RexGlue (doesn't fix patch-churn) | Process | Accepted trade for the reusable-SDK-capability upside; A remains the escape hatch. |

**Net:** medium effort, medium risk, **high de-riskability** — every phase is
measurable and reversible, and the expected payoff (removing the dominant
per-frame cost) is large and backed by prior measurement.

## 7. Recommended next steps (Phase 0 PoC)

1. **Identify the `D3DDevice` vtable.** Run/extend RexGlue's
   `src/codegen/vtable_scanner.cpp` + `sig_scanner.cpp` against `default.xex`;
   cross-anchor via the named-shader `Set/CreatePixelShader` call sites; map the
   known XDK `D3DDevice` method ordering onto the vtable. Output: an address→method
   map committed to this branch.
2. **Intercept Present.** Override the guest swap/Present path (strong symbol in
   `src/`) → route to the existing presenter; confirm the window still presents a
   frame (even if drawn by the PM4 path initially).
3. **Intercept the dominant draw.** Override the function emitting the 72.5%
   pixel-shader group; translate its draws to the existing Vulkan backend using the
   directly-ported `SPHud`/`SPTextured` shader. Render that group natively; verify
   visually against the PM4 output.
4. **Measure.** Heavy-frame FPS, native-draw vs PM4, for that group (reuse
   `tools/perf/` harness). This is the commit-or-fallback data point.
5. **Branch decision.** Green → Phase 1 (expand the hybrid). Intractable
   identification → fall back to variant A.
6. **(Optional, parallel) Validate A-fallback.** Clone `hedge-dev/XenonRecomp`,
   run `XenonAnalyse` on `default.xex` to confirm jump-table/SEH ingestion, so A is
   shovel-ready if ever needed.

## 8. What feeds back into the knowledge base / SDK

This is the upside that makes variant B more than a one-title fix:

- **KB entry — "HLE vs LLE graphics for 360 recomps."** When to emulate the command
  processor (Xenia/LLE: accurate, but per-draw translation cost — our floor) vs hook
  the D3D API (HLE: fast, with a per-title identification cost). South Park as the
  first case study, with the measured CP-translation floor as the motivating data.
- **SDK contribution — a reusable HLE graphics path for rexglue-sdk.** The codegen
  `[[override]]` table (address-keyed host override that suppresses the weak body)
  + a D3D9-360 HLE shim layer driving the existing Vulkan backend. **Any future
  RexGlue title that is draw-call/CP-bound benefits** — the "contribute to the SDK
  + win on other games" goal.
- **Reusable tooling — an XDK D3D signature/vtable pack.** The identification work
  (signatures + `D3DDevice` vtable layout for this XDK version) transfers to every
  other XBLA/XDK title built against the same SDK, turning the R1 risk into a
  one-time community asset.

---

## Appendix A — Evidence log

_Record of commands run, files inspected, and raw measurements, so the verdict is
reproducible. Paths are relative to `rexglue-recomps/` unless absolute._

**Architecture / provenance**
- `third_party/rexglue-sdk/src/graphics/command_processor.cpp:1` — Xenia provenance
  header ("Xbox 360 Emulator Research Project … Adapted for ReXGlue runtime").
- GPU subsystem = Xenia 1:1: `xenos.cpp`, `register_file.cpp`,
  `packet_disassembler.cpp`, `primitive_processor.cpp`, `shared_memory.cpp`,
  `trace_{dump,viewer,writer}`, `d3d12/` + `vulkan/` backends.
- `knowledge-base/general/75-runtime-graphics-…md:9-24` — static-XDK-D3D → PM4 →
  command-processor model (canonical statement).

**Q-A — XEX**
- `private/extracted/default.xex`: magic `XEX2`; `file` → "Microsoft Xbox 360
  executable, XA-2353, media ID 7F1BDFA7"; 8.5 MB.
- Imports (plaintext in header): only `xboxkrnl.exe` + `xam.xex`; module
  `SouthPark.exe`. No D3D import → D3D is statically linked.
- Toolchain: clang 21.1.8, cmake 3.31.11, ninja 1.13.1.

**Q-B2 — override mechanism** (agent-verified, all under `third_party/rexglue-sdk/`)
- `resources/templates/codegen/init_h.inja:107-109` `DEFINE_REX_FUNC` weak alias;
  `include/rex/ppc/context.h:50-52` `REX_WEAK_FUNC`.
- direct-call emit `src/codegen/builders/context.cpp:244` (weak `name`) vs import
  emit `:274` (`__imp__name`); mid-asm hooks `:358-432`.
- indirect/vtable table `init_cpp.inja:22-25` → `src/system/runtime.cpp:186-205`
  (`SetFunction`) → `_indirect_call.inja` / `src/system/function_dispatcher.cpp:42`.
- in-tree identification tooling: `src/codegen/vtable_scanner.cpp`, `sig_scanner.cpp`.
- live override precedent: `south-park-recomp/src/stubs.cpp` (+ `CMakeLists.txt:11-17`).
- `knowledge-base/general/80-patching-hooks-overrides.md:9-44`.

**Q-B1 — data flow / boundary / surface**
- doorbell `src/graphics/graphics_system.cpp:274-289` (`CP_RB_WPTR` 0x01C5 →
  `UpdateWritePointer`); MMIO window `:144-151`.
- ring setup `src/kernel/xboxkrnl/xboxkrnl_video.cpp:320-572` (`Vd*` shims).
- CP `src/graphics/command_processor.cpp:284-332,730-781,1181-1197` (worker,
  `ExecutePrimaryBuffer`, `VdSwap`→`IssueSwap`); draws `:968-972,1694`.
- no symbols: all funcs `sub_<addr>` (`generated/default/*.cpp`); indirect seam
  `generated/default/south_park_td_init.h:275-311`; `config/sp_functions.toml` =
  723 bare addresses.
- surface: `south-park-recomp/docs/DRAW-GROUP-BREAKDOWN.md`,
  `docs/DRAW-BATCHING-STEP2-PLAN.md:9-17`; res `launcher/INTEGRATION.md:66-79`.

**Q-S — shaders**
- 19 named programs in `private/extracted/media/shaders/` (`.xbv`/`.xbp`/`.updb`);
  HLSL recoverable from `.updb`. Cache `private/userdata/cache/shaders/shareable/
  58410931.xsh` (3392 B). Translator `src/graphics/pipeline/shader/spirv_*`.

**Q-B3 — Unleashed**
- `hedge-dev/UnleashedRecomp`: renderer `UnleashedRecomp/gpu/video.cpp` (+ Plume,
  `renderbag/plume`); guest bindings `exports.cpp`; layout maps to RexGlue subsystems.

**A-fallback note (correction):** `hedge-dev/XenonRecomp` + `XenosRecomp` are
described as submodules in `rexglue-recomps/README.md`, but are **not** wired in
the super-repo `.gitmodules` (only `rexglue-sdk`, `knowledge-base`,
`south-park-recomp` are) and the `third_party/XenonRecomp{,os}` dirs are absent.
For variant A they would be cloned fresh from hedge-dev.
