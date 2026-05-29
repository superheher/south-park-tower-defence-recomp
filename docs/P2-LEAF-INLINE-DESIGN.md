# P2 selective leaf-inlining — verified design notes (codegen map from the wf investigation)

## The obstacle (why dropping `noinline` alone does nothing)
Intra-recomp calls are emitted as `sub_X(ctx, base)` — the **weak alias** (interposable → never inlined)
— and the callee body lives in a **different** TU (49 `*_recomp.N.cpp`). So same-TU + non-weak are BOTH
needed for the compiler to inline. The fix is to give eligible leaves an `always_inline` body in a
**shared header** and route eligible callsites to it.

## Emit structure (verified)
- `DEFINE_REX_FUNC(name)` (init_h.inja:69) = `weak,noinline` alias `sub_X` → strong out-of-line
  `__imp__sub_X` (the real body, emitted by `FunctionNode::emitCpp`, function_graph.cpp:482).
- Dispatch table `PPCFuncMappings[]` (init_cpp.inja:22) points at the **strong `__imp__sub_X`**, NOT the
  weak alias → inlining the alias-callsites leaves dispatch + overrides untouched. ✅
- Call emit point: `BuilderContext::emit_function_call` → `println("\t{}(ctx, base);", name)`
  at **builders/context.cpp:236** (name = `targetFn->name()`). BL→build_bl (control_flow.cpp:61),
  tail B→build_b (43), bctr switch case (149).

## CORRECTED mechanism (the wf's "emit STATIC_INLINE before DEFINE in same TU" is WRONG — callsites
## are cross-TU and call the weak alias; a same-TU static body is invisible to other TUs).
1. Codegen emits, for each inline-eligible leaf, an `always_inline` function in a NEW shared header
   `{project}_inlines.h` (included at the END of `{project}_init.h`, after DECLARE_REX_FUNC, so it is
   visible in every recomp TU):
   `[[gnu::always_inline, maybe_unused]] static inline REX_FUNC(sub_X_inl) { <body> }`
   Using a real inline *function* (not a textual body splice) avoids the temp/ea/vTemp local-name
   collisions the wf flagged — params bind cleanly, compiler scopes the leaf's own locals.
2. The out-of-line `__imp__sub_X` body becomes a one-liner thunk `{ sub_X_inl(ctx, base); }` (the
   inline expands into it) so the dispatch table still has the full real body at one address. Body is
   thus emitted ONCE (the header inline), referenced by both the thunk and every callsite.
3. At eligible callsites, emit `sub_X_inl(ctx, base);` instead of `sub_X(ctx, base);`
   (the one-line change at context.cpp:236, gated on the eligibility flag).

## Eligibility predicate (FunctionNode, function_node.h) — exclude unless ALL hold:
- `calls().empty() && tailCalls().empty()` (no bl/bctr/bctrl/blrl out)  [no leaf calls __savegprlr/__restgprlr either — those ARE calls]
- `!hasExceptionInfo() && !hasExceptionHandler()` (no SEH/C++ EH/setjmp frame)
- no instruction with `reads_lr` (mflr / mfspr SPR_LR=8 / bclr / bclrl) — inlining changes LR
- entry NOT a midAsmHook target AND no instruction addr in body is a midAsmHooks key (config.h:108)
- `size() < THRESHOLD` (start ~48–64 B guest; the smallest subset first)
- not the longJmp/setJmp address; not `xstart`; not an import

## BLOAT RISK (the thing to watch — could REGRESS the floor)
`.text` grows ~ (#callsites × body_size); the out-of-line copy stays. The BKG's dominant front-end
capacity miss is ALREADY **DSB_MISS 77M/1e9i** (vs BACLEARS 30M/1e9i), so bloating `.text` can raise
DSB/i-cache misses MORE than it lowers BTB resteers → net front-end loss. Mitigation:
- strict size threshold + start small; measure `.text`, DSB_MISS, BACLEARS together via resteer.sh.
- consider also bounding by incoming-callsite count (compute caller fan-in from the call edges) so a
  tiny leaf called 500× isn't inlined 500× — the header parse + bloat cost isn't worth it.
- header parsed by all 49 TUs → compile-time cost ∝ #inline-eligible; keep the set small.
GO/NO-GO: keep only if gate passes AND p10 > +1.0 AND BACLEARS drops AND DSB_MISS doesn't rise enough
to wash it out.

## Implementation touch-points
- function_node.h: add `bool isInlineLeaf_` + accessor; compute in `seal()`/discover (needs
  midAsmHook + reads_lr scan over blocks; size + calls already available).
- function_graph.cpp:482 emitCpp: emit thunk body `{ sub_X_inl(ctx,base); }` when isInlineLeaf.
- codegen_writer.cpp:234: also write `{project}_inlines.h` (new inja template) with the inline bodies.
- builders/context.cpp:236: route eligible callees to `{name}_inl`.
- init_h.inja: `#include "{project}_inlines.h"` after the DECLARE_REX_FUNC loop; add
  `STATIC_INLINE_REX_FUNC` macro.
- template data: expose `fn.is_inline_leaf` to the templates.
