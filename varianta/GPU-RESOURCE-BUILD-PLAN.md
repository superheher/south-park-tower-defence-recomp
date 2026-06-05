# Variant A — GPU-Resource Subsystem: build plan (cont.23, 2026-06-05)

The authoritative, current plan for the remaining deep build. **Supersedes** the cont.13–19
producer/consumer framing in `RENDERER-DESIGN.md` (cont.21 measured that producer/consumer is
per-submission *bookkeeping*, NOT the per-draw path; the real draws are PM4 `DRAW_INDX` in kicked
IBs / the deferred device+13568 segments). Consolidates the cont.21–23 measurements into an
actionable design so a focused implementation session can start cleanly.

## Goal
Get variant A to render the **real, textured** menu. This is gated on a GPU-resource subsystem:
the title's resource loader must produce **non-null, real GPU resources** so it completes → the
title advances to a populated menu state → builds real content → which a real renderer draws.

## The wall — two-sided, exhaustively measured (cont.23)
- **Rendering side:** the geometry render path is PROVEN — the backdrop (4 EDRAM-tile quadrants),
  2 nested dialog panels, and a real 63-glyph text label all render via the debug menu-quad
  pipeline (`REX_EXECSEGS`/`REX_VBFILL`/`REX_UITEXT`, this session). BUT **every UI texture is
  empty**: the static font atlas `0xA5004800` AND the EDRAM backdrop `0xB0000000` both read all
  zeros (`[scene-tex]`/`[atlas]`). Nothing textured can draw — there is no texture data.
- **Loader side:** `child[0]` (`0x82657578`) is the **sole sequential blocker** — at the loader's
  active poll it is state 2 (processing) while children[1..19] are state 0 (not started); they
  queue behind it (`[children]` survey). child[0] cycles 2→3→6→8→10 producing a **null resource**,
  and `sub_8224F918` (driven from frontend `sub_8214FFD0`) re-queues it forever.
- **Root (both sides agree):** child[0]'s **GPU resource-create** is inlined XDK-D3D that submits
  create-PM4 + waits on a GPU completion variant A's present-only CP never produces. cont.22
  PROVED **no shortcut exists**: force-state (`REX_POLLFORCE`), latch-at-done, and fence-forward
  all fail — the title *uses* the real resource downstream, so a fake/null one breaks later.

## What variant A HAS vs LACKS
**HAS (leverage these):**
- `ExecutePM4` — the PM4 interpreter (type-0/1/2/3 packets, SET_CONSTANT→reg file `0x7FC80000+reg*4`,
  DRAW_INDX, INTERRUPT, EVENT_WRITE, fences). The natural home for resource-create modeling.
- `REX_CPCOMPLETE` — faithful per-vblank drain of the pending-counter `device+0x2b04` (cont.21).
- `REX_EXECSEGS` — executes the deferred `device+13568` segment directory each swap (the content
  the title builds but never kicks); the `DRAW_INDX` carve + the menu-quad bridge already render
  its geometry.
- A debug **menu-quad VkPipeline** (float2 pos + push-const {mat4 mvp, vec4 color}, alpha blend) +
  `rex_render::SubmitMenuGeometry` bridge + the present path (movie YUV blit already works).
- Relocated dispatch table (host mmap, `HostFnAt`) — the string-as-code crash class is fixed.
- A stable full-menu run base: `REX_NOTOKEN=1 REX_CSLEAK=1 REX_CPCOMPLETE=1 REX_MOVIE_EOF=30`.

**LACKS (the build):** a real Xenos→Vulkan renderer — **no** tiled-texture decode, **no** `.xbv`→
SPIR-V shader translation, **no** EDRAM render-targets/resolves, **no** real draw-state→VkPipeline
translation (the menu-quad path is debug-only: position+flat-color, no textures/UVs/real formats).

## The pieces to build (in `ExecutePM4` / the CP)
1. **Resource-create completion model** — when the loader's child[0] submits its create stream,
   EXECUTE it for real: model the upload (DMA/`MEMORY` writes into the resource's backing) and
   signal the exact completion the loader waits on (its fence/event), so the state machine reaches
   done *with real backing*. ⚠ DECISIVE OPEN QUESTION (test first, see below): does the loader need
   only the upload-fence + valid resource *memory*, or the fully-created GPU object (compiled
   shader / decoded texture)? If the former, this piece alone may unblock the loader cheaply.
2. **Xenos tiled-texture decode** — fetch-constant (reg `0x4800+slot*6`, format/dims/tiled fields,
   already decoded in `REX_DRAWLOG`) → untile → `VkImage` + sampler. Needed for the font atlas,
   sprites, and backdrop sampling. Ref: Xenia `texture_conversion` / `texture_info`.
3. **`.xbv` → SPIR-V shader translation** — child[0] loads `Simple.xbv` (612 B, a shader). The
   `.xbv`/`.updb` carry original **D3D9 `vs_3_0`/HLSL** (cont.22: version token `FF FE 03 00`, a
   `$worldviewProj` constant). 19 `.updb` HLSL sources exist → compile to SPIR-V (no microcode
   translator needed). Ref: the HLE-spike "Plume" path + the 19 `.updb`.
4. **EDRAM render-targets + resolves** — the backdrop samples EDRAM (`0xB0000000`); the title
   renders the background art to EDRAM then blits it. Model render-to-EDRAM-target + resolve→texture.
5. **Real draw-state → VkPipeline + vkCmdDraw** — replace the debug menu-quad carve: per draw,
   build the pipeline from reg-file 0x4000 ALU consts (the transform — already extracted as World+
   screen-ortho Proj) + 0x4800 fetch consts (verts + textures) + RB surface/blend/viewport state +
   the translated shaders, and `vkCmdDraw`. The vert/transform plumbing is already proven
   (`REX_UITEXT` count-correlation, the backdrop carve).

## Recommended build order (front-load the loader-unblock; defer the heavy renderer)
- **STEP 0 — decisive test (cheap, do first):** model child[0]'s create-PM4 **upload + completion
  fence** (piece 1, minimal: real backing memory + signal the fence), WITHOUT decode/compile. Run
  and watch the `[children]` survey + the transition: does child[0] reach done *and stay* (not
  re-queued), and do children[1..19] start? **If yes** → the loader needs only real backing+fence,
  and the title will progress to a populated menu (then build pieces 2–5 to render it). **If no**
  (still re-queued) → the loader needs the fully-created GPU object → pieces 2/3 are prerequisites
  to the unblock (heavier path). This test decides the whole build's shape — run it before
  committing to the heavy pieces.
- **STEP 1:** whichever the test dictates — either (a) finish the lightweight completion model and
  let the title progress, or (b) build texture-decode (piece 2) + shader-translation (piece 3)
  enough to make child[0]'s specific resource real, then complete.
- **STEP 2:** with the title progressing + building real content, build the real draw translation
  (piece 5) + the textures (piece 2) → render the populated menu. EDRAM targets (piece 4) for the
  background art.
- Each step is committable + render-verifiable (PPM capture) and must keep default boot unregressed.

## Entry points / known facts (addresses)
- **Loader:** state machine `sub_82248010` (ppc_recomp.29); parent obj `0x826574E8`, children at
  `0x82657578 + i*216` (state @+136, ready-flag @+208); pump call at guest lr `0x82248224`
  (`REX_POLLDIAG`); re-queue `sub_8224F918`←frontend `sub_8214FFD0` (ppc_recomp.30); done = state
  1/12; `*(child+208)`=ready (=1 in prod). child[0] reaches its states but produces a null resource.
- **Resource:** child[0] loads a file via `sub_822485A0` (returns size; 612=`Simple.xbv`). The
  create is inlined XDK-D3D in the frontend — no clean guest-function hook (HLE-spike + cont.22).
- **Create completion:** the title waits on a GPU fence (cont.22: `device+10896` / the segment
  pointer) + the pending-counter `device+0x2b04` (drained by `REX_CPCOMPLETE`).
- **Content:** built into `device+13568` (segment directory, `{0x81LLLLLL, phys}` descriptors;
  guest = `0xA0000000|(phys&0x1FFFFFFF)`); executed by `REX_EXECSEGS`. Per frame the menu state is
  SPARSE (~7-8 draws: 4 backdrop quadrants + 1 on-screen prim-4 quad + off-screen sprite/text) —
  richer content appears only after the loader completes + the title advances.
- **VB binding (UI draws):** the per-draw vertex binding is stubbed — the UI VS reads fetch slot-0
  = empty `0xA2000000`; the real verts are written by `sub_821F8E60` (D3D dynamic-VB Lock
  [vtable+120=`sub_822052B0`→alloc `sub_821C48B0`→`0xA022FFF0`] / fill `sub_8242BF10` / Unlock
  [vtable+124=`sub_822052F8`, device bookkeeping]) but the SetStreamSource that binds it is never
  emitted into the segment ([esset]/[esset2]=0). A real renderer must bind the VB the draw's
  dynamic buffer (count-correlation, `REX_UITEXT`, is a working stand-in for that mapping).

## Risks / open questions
- **STEP 0's answer** is the biggest unknown — it determines whether the loader-unblock is cheap
  (fence+backing) or requires the heavy GPU-object creation. Resolve it first.
- **No clean hook** for the inlined create ⇒ the build must intercept at the **PM4 level**
  (ExecutePM4), reading the create stream the title submits, not overriding a guest create function.
- **Scope:** the full renderer (pieces 2–5) is large (Xenia-scale). The reachable menu state is
  simple (background + a few buttons), which bounds it somewhat — but EDRAM/texture/shader are each
  substantial. This is a sustained, multi-session build with no quick visible payoff (cont.22's
  "poor /loop fit" — confirmed). A debug prod-oracle build (`-g`/DWARF) would unblock cross-checks
  (prod currently has no DWARF — `readelf -S` shows only `.symtab`).
- **Alternative to weigh:** the whole variant-A renderer exists to escape the prod CP-translation
  perf floor; building it is large. Worth a deliberate go/no-go vs. other floor approaches before
  committing the multi-session effort.

## Diagnostics retained (gated, default boot unregressed)
`REX_EXECSEGS` (+[esdraw/esvf/esidx/esprim/esalu/esset/esset2]), `REX_VBFILL` (+[text]),
`REX_UITEXT`/`REX_UITEXT_FIT` (text geometry), `REX_UITRACE` (VB Lock/Unlock), `REX_SCENE`
(+[scene-tex]/[atlas] — the texture-empty probe), `REX_POLLDIAG` (+[children] — the loader survey),
`REX_CHUNKDUMP` census, `REX_CPCOMPLETE`, `REX_DRAWLOG`/`REX_DRAWDECODE`.
