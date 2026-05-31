# HLE Graphics — Phase-1 findings (native-render attempt: locate the dominant guest renderer)

> Branch `experimental/hle-graphics-spike`. Continues `HLE-PHASE0-PROGRESS.md`. Goal this phase:
> RE `sub_821CC830`, then natively override the dominant-draw renderer via the Vulkan backend,
> then benchmark. This logs what RE + measurement actually found.

## TL;DR (the commit-or-fallback datum) — NO-GO for in-place variant B → fallback to variant A

The named hook target `sub_821CC830` is **D3D frame-begin, not the draw emitter**, and a multi-angle
search (RE + a guest-side pass-attribution probe + visual stub-bisection) finds **no clean guest-side
function to override** for the dominant draw group. The title's XDK Direct3D inlines all draw/state
emission as PM4 stores, and the render is structurally bundled (begin / submit / resolve passes with
draws woven inline deep in their subtrees). **The in-place variant-B native render is NOT tractable for
this title via guest-function override**, on two decisive, independent grounds:

1. **The spike's enabling premise is falsified.** Variant B (and Sonic Unleashed's HLE) relies on a
   *hookable D3D API* (DrawIndexedPrimitive/SetTexture as real functions). This title's statically-linked
   XDK D3D **inlines** all of it as direct PM4 stores — confirmed at the call-structure level (§1): there
   is no per-draw / per-sprite / per-material guest function to override.
2. **Texture-binding wall.** A native override bypasses PM4 emission, so the CP register file never gets
   the texture/shader SET_CONSTANT packets — the native path would have to reconstruct each sprite's
   texture from a high-level guest handle. The structurally-bundled render exposes **no override level**
   where geometry + texture handle + state are clean inputs AND the function isn't frame-critical (§3).

The only place the dominant group exists as decoded draws (the CP's `IssueDraw`) is the
already-measured-CLOSED draw-batch lever (DRAW-BATCHING-STEP2-PLAN: 43 interleaved textures, order-safe
merge ~1.37×/floor-neutral). So there is nothing tractable to natively render-and-benchmark in place.
**Recommendation: fall back to variant A (XenonRecomp full stack)** — the XEX is clean XEX2, so a from-
scratch recomp re-emits the guest including D3D, giving the clean basis for HLE that this prebuilt SDK
recomp does not. (A theoretical alternative — hooking the GAME-LOGIC sprite/HUD system *above* D3D —
is a large unguided RE effort over ~30k functions with no localization signal, and still faces the
texture-mapping implementation.)

## 1. RE of `sub_821CC830` — it is D3D frame-begin (BeginScene), not the draws

`generated/default/south_park_td_recomp.6.cpp:29648` (278 lines). Behaviour:
- Calls the reserve `sub_821C6D58` once, then `sub_821C5BA8` **×7** to allocate + chain-link 7
  command-buffer ring segments.
- `sub_821C5BA8` (`recomp.6.cpp:12823`) allocates a 4224-byte segment via `sub_821C60D8`, patches the
  previous segment's header with the **GPU-physical address** of the next (`addr − 0x40000000`, the
  canonical Xbox-360 guest→GPU translation; `lis r30,16384` = 0x40000000), and sets the write/end
  pointers. Classic XDK deferred command-buffer ring management.
- After the 7 segment resets, writes ONE setup packet (header `0x80000000`, then `[dev+13364]`, a
  `count = [dev+12924]`, then `count` 16-byte records copied with a viewport-origin subtraction).

So skipping it (the prior `REX_HLE_STUB_CC830` test) blanks the whole frame + stops swaps because the
command buffers are never allocated — **frame-begin is frame-critical, but it is not where the 729
sprite draws are**. This **corrects** the Phase-0 note "sub_821CC830 IS the variant-B hook target."

Its caller `sub_821BEF00` (`recomp.5.cpp:73886`, 542 lines) calls `sub_821CC830` once near its end and
has only small counter-bounded state loops — no 729× sprite loop, no per-sprite call. Confirms the
Phase-0 finding (now at the call-structure level): **there is no per-draw guest D3D function to hook.**

## 2. Guest-side pass-attribution probe — and why kicks are not a draw proxy

`src/hle_graphics.cpp` (gated `REX_HLE_PASSPROBE`) overrides the kick `sub_821C6600` (the CP_RB_WPTR
doorbell, 100% of doorbell writes per the Phase-0 provenance probe) and each top-level render pass (the
direct children of the frame-render `sub_82150970`, which calls Present), attributing the kick-count
delta across each pass's subtree via a thread-local depth guard. Windowed deltas isolate the heavy
regime.

**Measured (fps spanned 14.6–60, p50 27.8 — the deep dip WAS reached):** total doorbell kicks are a
rock-steady **~11/frame regardless of draw count** (180–1301 draws/frame). So the doorbell is a fixed
structural per-frame flush (≈ once per pass/submit), **NOT draw-proportional** — kick-count localizes
which passes *flush/submit*, not which emit the most draws. (No guest-side call-count — kick, reserve
`sub_821C6D58`, or segment-alloc `sub_821C5BA8` — is cleanly draw-proportional, because draws are inline
stores that only touch the ring machinery on segment overflow.) This is why the localization had to fall
back to visual stub-bisection.

## 3. Visual stub-bisection (`REX_HLE_STUB=<guest hex addr>`)

Each candidate pass is stubbed (returns immediately); a heavy-combat screenshot shows what it rendered.
Baseline `/tmp/sp/baseline_a.png`: full Stan's-House combat — background art, 2D paper-cutout character
sprites (Stan + range circle, towers, Ginger-Kid enemies), text/numbers, bottom HUD bar.

Top-level passes (all turned out structural, not isolable content):
- **`sub_821BF298`** (680 lines; manages command buffers: `sub_821C5BA8`×3 + `sub_821C6D58`×2 + submit
  `sub_821CCA28`) → **breaks boot entirely** (no rendering) ⇒ critical submitter.
- **`sub_82167248`** (→ `0x822Cxxxx` subsystem) → **corrupts the whole frame** (horizontal EDRAM-tile
  garbage) ⇒ structural resolve/blit, not isolable content.

Deeper round (main-scene internals under the critical pass `sub_8212DFB8` → `sub_821BC3E8`/`sub_821BC738`,
both calling shared `sub_821B97C0`):
- **Inconclusive — blocked by capture tooling.** On this 4K HiDPI display the SDL window is composited
  such that per-window `import` returns a blank 1-bit pixmap and `import -window root` is unsupported;
  `ffmpeg x11grab` of the root showed black (the GL window isn't on the captured root), and the scripted
  drive did not reliably re-reach the heavy regime after the many relaunches (`sub_821B97C0` stub ran at
  60 fps; `sub_821BC3E8` stub produced no `pacing-diag` — consistent with it also being frame-critical,
  but not visually confirmed). Baseline + the two top-level stubs above were captured cleanly *before*
  the window topology churned. **This does not change the determination:** grounds (1) and (2) in the
  TL;DR are architectural and independent of which inner function emits the sprites.

## 4. Architecture constraint on native rendering

The Vulkan backend (`VkDevice`, `texture_cache`, `VulkanCommandProcessor`) is internal to
`librexruntime.so`. More fundamentally: a native override of a guest render function bypasses PM4
emission, so the CP's register file never receives the texture/shader SET_CONSTANT packets — meaning the
native path would have to **reconstruct each sprite's texture binding from high-level guest state**
(a readable `D3DTexture*`/material handle), then map it via `texture_cache`. That requires a guest
override level where geometry + texture handle + render state are all clean, coherent inputs. The inline,
structurally-bundled rendering does not expose such a level (see §1–3).

## Tools added (branch, gated, behaviour-neutral when off)
- `src/hle_graphics.cpp`: `REX_HLE_PASSPROBE` (pass kick-attribution), `REX_HLE_STUB=<addr>` (visual
  stub-bisection of any of the 10 top-level passes + 6 deeper sub-functions), `REX_HLE_PRESENT_TRACE`.
- `tools/perf/passprobe.sh`, `tools/perf/shot_stub.sh`, `tools/perf/bisect_seq.sh`.
- App-only build (`cmake --build out/build/linux-amd64-release --target south_park_td`, ~8s); no SDK
  rebuild. Overrides are strong symbols beating the generated weak guest aliases.
