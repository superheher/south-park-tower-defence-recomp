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

// GPU-build piece 3b (Layer 2): submit extracted menu geometry to the render thread. `clipXY` is
// `vertCount` interleaved clip-space (x,y) float pairs (already transformed); the render thread draws
// them as a TRIANGLE_LIST via the menu-quad pipeline on the next present. Replaces the REX_MENUTEST
// hardcoded quads once real geometry arrives. One-shot from the CP at a menu frame for now.
void SubmitMenuGeometry(const float* clipXY, int vertCount);

// Task #8 (disk-resource path, cont.23): submit TEXTURED menu geometry. `posUV` is `vertCount`
// interleaved (pos.x, pos.y, u, v) tuples (clip-space position + [0,1] UVs); the render thread draws
// them as a TRIANGLE_LIST via the textured pipeline, sampling the disk-loaded background .png. Used
// for the backdrop quadrants — synthetic screen→[0,1] UVs reassemble the full background across them.
// Gated behind REX_MENUTEX (the renderer must also be in REX_MENUTEST mode for the present path).
void SubmitTexturedGeometry(const float* posUV, int vertCount);

// cont.60 (REX_MENULIVE): composite a live-decoded per-draw menu texture (the 0xA2 working buffers,
// cont.58) into a render-side contact sheet. Called from kernel.cpp's REX_TEXDECODE after it decodes each
// bound menu texture (rgba = w*h*4 RGBA8; base = the texture's GPU base, used to dedup into a grid cell).
// The render thread (PresentOnce) uploads + draws the sheet, proving the working-buffer texture -> live
// Vulkan render path on-screen. Gated behind REX_MENULIVE (no-op otherwise).
void BlitMenuCell(const uint8_t* rgba, int w, int h, uint32_t base);

// cont.123: the font glyph atlas (256x256 FMT_8) is a DYNAMIC cache whose guest address VARIES per run
// (cont.122: the cont.115 hardcode 0xA337D000 was empty this run). The kernel carve reads the prim-13 text
// draw's bound texture fetch const (the live atlas base) and publishes its PHYS offset here; LoadFontAtlasOnce
// uploads from g_base + this offset instead of the stale hardcode. physOffset = base & 0x1FFFFFFF.
void SetFontAtlasAddr(uint32_t physOffset);

// cont.141 (REX_MENURECON): publish a full-screen (1280x720) RGBA frame onto which the kernel has
// composited the title's REAL decoded menu/UI text labels (REX_TEXTRENDER, cont.70-71) at a
// RECONSTRUCTED layout. The render thread uploads it into g_tex and draws it as a full-screen quad,
// so variant A's own Vulkan pipeline renders the real menu text (SELECT GAME MODE / NEXT / BACK / ...).
// Layout is reconstructed, NOT the title's real screen positions (those need the per-frame transform =
// the cont.73 A<->B wall). Called repeatedly as labels arrive; the renderer re-uploads on growth (capped).
// Gated behind REX_MENURECON (no-op otherwise).
void SetReconMenuFrame(const uint8_t* rgba, int w, int h);
}
