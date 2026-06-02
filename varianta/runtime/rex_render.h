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

// Increment 3 (decoded-frame present): publish the VC-1 decoder's frame-pool buffers (guest base + alloc
// size) so the render thread can present the decoded intro movie in COLOR. The render thread reads guest
// memory directly (g_base + addr), picks the freshest fully-decoded frame, does YUV->RGB, and uploads it.
// Frame layout (RE'd): planar I420, full-range BT.601, as 3 consecutive allocations per frame — Y plane
// (size 0x101440, pitch 1344, 1280x720) then U then V (size 0x40520 each, pitch 672, 640x360).
void PublishVideo(const uint32_t* guestBufAddrs, const uint32_t* guestBufSizes, int count);
}
