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

// Increment 3 (decoded-frame present): publish the VC-1 decoder's frame-pool buffers (guest addresses) so
// the render thread can present the decoded intro movie. The render thread reads guest memory directly
// (g_base + addr), picks the freshest fully-decoded buffer, and uploads its luma plane.
// Frame layout (RE'd from the dump): Y (luma) plane is LINEAR, pitch=1344 bytes, 1280x720 visible, at
// offset 0; 8-bit. Chroma layout is non-standard (TODO: color) — presented as grayscale for now.
void PublishVideo(const uint32_t* guestBufAddrs, int count);
}
