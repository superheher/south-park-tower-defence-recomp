// Hand-provided guest-function stubs (Phase 2 link fix).
//
// rexglue 0.8.1.4 emits the function-table entry `{ 0x0, sub_0 }` and
// `DECLARE_REX_FUNC(sub_0)` for the address-0 null-function sentinel, but never
// emits a body for it — so the module fails to link with `undefined symbol:
// sub_0`. Provide a no-op body, matching rexglue's own low-address sentinel
// stubs (sub_00000001..3, which are likewise empty). DEFINE_REX_FUNC marks the
// symbol weak, so a future rexglue that emits sub_0 itself overrides this
// harmlessly. Guest address 0 is never a legitimate call target.
//
// This lives in our src/ (not generated/) so it survives every `rexglue codegen`.

#include "generated/default/south_park_td_init.h"

DEFINE_REX_FUNC(sub_0) {
  REX_FUNC_PROLOGUE();
}
