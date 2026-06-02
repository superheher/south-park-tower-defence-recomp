// Variant A — minimal from-scratch Vulkan renderer (host display). Increment 1: window + clear-present.
// Gated behind REX_RENDER=1 so the default headless boot is unchanged.
#pragma once
#include <cstdint>

namespace rex_render {
// True if REX_RENDER is set in the environment (renderer active).
bool Enabled();
// Called from VdSwap with the guest front-buffer address. Increment 1 ignores the address and presents a
// clear color (proves window + swapchain + present); later increments read/translate the framebuffer.
void Present(uint32_t frontBufferGuestAddr);
}
