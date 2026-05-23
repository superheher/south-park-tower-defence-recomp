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

// XUsbcam (Xbox Live Vision USB camera) imports — not present on this host and unused
// by an offline tower-defense title. After regenerating the codegen from the CORRECTED
// default.xex (the prior loose extraction had a corrupt .text/.data; see KB
// 35-entry-forensics), the analyzer found the camera-handling functions (which the
// corrupt codegen had missed), so these 7 imports are now referenced but rexglue's
// runtime does not implement them. Stub them to "unavailable" (non-zero failure in r3)
// so any camera path fails gracefully. Weak (DEFINE_REX_FUNC) -> a real rexglue impl
// would override harmlessly. The codegen calls __imp__XUsbcam* directly.
#define STUB_XUSBCAM(name) \
  DEFINE_REX_FUNC(__imp__##name) { REX_FUNC_PROLOGUE(); ctx.r3.u64 = 0x80004005u; }
STUB_XUSBCAM(XUsbcamCreate)
STUB_XUSBCAM(XUsbcamDestroy)
STUB_XUSBCAM(XUsbcamGetState)
STUB_XUSBCAM(XUsbcamReadFrame)
STUB_XUSBCAM(XUsbcamSetCaptureMode)
STUB_XUSBCAM(XUsbcamSetConfig)
STUB_XUSBCAM(XUsbcamSetView)
