// Variant A — minimal from-scratch Vulkan renderer. INCREMENT 1: SDL window + Vulkan swapchain +
// clear-and-present. No PM4 translation yet — this establishes the host display/present pipeline that
// draws/shaders build on. Gated behind REX_RENDER=1.
//
// Architecture: a dedicated host RENDER THREAD owns the SDL window + all Vulkan objects and runs the
// present loop at vsync. The guest's VdSwap only publishes the current front-buffer address (atomic) and
// lazily starts the thread — it NEVER calls Vulkan/SDL or blocks on present, so a slow/stalled compositor
// can't stall the guest. Single frame-in-flight, FIFO present, clear via vkCmdClearColorImage.
#include "rex_render.h"
#include "test_shaders.h"   // GPU-build piece 1: embedded SPIR-V for the test-triangle pipeline
#include "menu_shaders.h"   // GPU-build piece 3b: float2-position menu-quad pipeline (mvp/color push const)
#include "menutex_shaders.h" // disk-resource path (cont.23): textured UI pipeline (pos.xy+uv.xy + sampler2D)
#include "rex_texture.h"    // cont.36: Xenos texture decoder — used to decode the title's real DDS textures
#include <png.h>            // disk-resource path: decode media/Assets/*.png menu art (libpng simplified API)
#include <zlib.h>           // cont.37: inflate the title's zlib'd Textures/*.bin DDS containers
#include <algorithm>        // std::min
#include <vulkan/vulkan.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

extern uint8_t* g_base;   // guest memory base (4 GiB), defined in runtime.cpp — render thread reads it directly

namespace rex_render {
namespace {

const bool g_enabled = (getenv("REX_RENDER") != nullptr);
std::atomic<bool> g_started{false};
std::atomic<bool> g_running{false};
std::atomic<uint32_t> g_frontBuffer{0};   // latest guest front-buffer addr from VdSwap (used in incr. 2)
std::atomic<uint64_t> g_swaps{0};         // VdSwap count (guest frame submissions)
std::thread g_thread;

SDL_Window*      g_window   = nullptr;
VkInstance       g_instance = VK_NULL_HANDLE;
VkSurfaceKHR     g_surface  = VK_NULL_HANDLE;
VkPhysicalDevice g_phys     = VK_NULL_HANDLE;
VkDevice         g_device   = VK_NULL_HANDLE;
uint32_t         g_qfam     = 0;
VkQueue          g_queue    = VK_NULL_HANDLE;
VkSwapchainKHR   g_swapchain = VK_NULL_HANDLE;
VkFormat         g_format   = VK_FORMAT_B8G8R8A8_UNORM;
VkExtent2D       g_extent   = {1280, 720};
std::vector<VkImage> g_images;
VkCommandPool    g_cmdPool  = VK_NULL_HANDLE;
VkCommandBuffer  g_cmd      = VK_NULL_HANDLE;
VkSemaphore      g_acquireSem = VK_NULL_HANDLE;
VkSemaphore      g_presentSem = VK_NULL_HANDLE;
VkFence          g_fence    = VK_NULL_HANDLE;
uint64_t         g_frame    = 0;

// --- GPU build (piece 1): graphics pipeline foundation ------------------------------------------------
// A real render pass + per-image framebuffers + a VkPipeline (the test-triangle shaders), the machinery the
// DRAW_INDX->Vulkan translator plugs into. The current present path is clear/blit only; this adds the draw
// path. Gated behind REX_DRAWTEST so the default present (movie/clear) is unchanged until the translator
// wires the title's real draws in. See NIGHT-LOG cont.22 "GPU build".
const bool g_drawtest = (getenv("REX_DRAWTEST") != nullptr);
VkRenderPass     g_renderPass = VK_NULL_HANDLE;
VkPipelineLayout g_pipeLayout = VK_NULL_HANDLE;
VkPipeline       g_testPipe   = VK_NULL_HANDLE;
std::vector<VkImageView>   g_imageViews;
std::vector<VkFramebuffer> g_framebuffers;

// --- GPU build (piece 3b): menu-quad pipeline (float2 screen-space pos -> mvp -> clip; flat color) -----
// The menu content draws are float2 (x,y) vertices (8-byte stride, RE'd) transformed by $worldviewProj.
// This pipeline takes a float2 vertex buffer + a push-constant {mat4 mvp, vec4 color}, alpha-blended for
// UI compositing (blend src=SRC_ALPHA dst=ONE_MINUS_SRC_ALPHA — the title's 0x7060706). REX_MENUTEST
// validates it with hardcoded clip-space quads; the real path feeds extracted menu geometry.
const bool g_menutest = (getenv("REX_MENUTEST") != nullptr);
VkPipelineLayout g_menuLayout = VK_NULL_HANDLE;
VkPipeline       g_menuPipe   = VK_NULL_HANDLE;
VkBuffer         g_menuVB     = VK_NULL_HANDLE;     // host-visible float2 vertex buffer (persistently mapped)
VkDeviceMemory   g_menuVBMem  = VK_NULL_HANDLE;
void*            g_menuVBMap  = nullptr;
VkDeviceSize     g_menuVBCap  = 0;
struct MenuPC { float mvp[16]; float color[4]; };   // push constant: 80 bytes (<=128 min)
// Layer 2: menu geometry submitted by the CP (clip-space XY pairs), drawn instead of the test quads.
std::mutex          g_menuGeomMtx;
std::vector<float>  g_menuGeom;                      // interleaved clip XY, guarded by g_menuGeomMtx
std::atomic<int>    g_menuGeomVerts{0};              // vertex count currently submitted

// --- Textured UI pipeline (disk-resource path, cont.23): pos.xy+uv.xy (stride 16) + sampler2D ----------
// Bypasses the stuck loader: loads shaders (media/shaders/*.updb HLSL -> menutex_shaders.h) + textures
// (media/Assets/*.png) from disk. REX_TEXTEST validates the pipeline with a procedural checkerboard quad.
const bool g_textest = (getenv("REX_TEXTEST") != nullptr);
// cont.37: REX_HUDSHEET — decode the title's REAL HUD textures (the zlib'd DDS container) via the cont.36
// decoder + render them as a contact sheet. The disk-resource render of the title's own texture data.
const bool g_hudsheet = (getenv("REX_HUDSHEET") != nullptr);
VkPipelineLayout      g_texLayout    = VK_NULL_HANDLE;
VkPipeline            g_texPipe      = VK_NULL_HANDLE;
VkDescriptorSetLayout g_texSetLayout = VK_NULL_HANDLE;
VkDescriptorPool      g_texDescPool  = VK_NULL_HANDLE;
VkDescriptorSet       g_texSet       = VK_NULL_HANDLE;
VkImage               g_texImg       = VK_NULL_HANDLE;
VkDeviceMemory        g_texImgMem    = VK_NULL_HANDLE;
VkImageView           g_texView      = VK_NULL_HANDLE;
VkSampler             g_texSampler   = VK_NULL_HANDLE;
VkBuffer              g_texVB        = VK_NULL_HANDLE;   // pos.xy+uv.xy, host-visible, persistently mapped
VkDeviceMemory        g_texVBMem     = VK_NULL_HANDLE;
void*                 g_texVBMap     = nullptr;

// --- Task #8 (disk-resource path): texture the SUBMITTED menu geometry (the backdrop quadrants) -------
// REX_MENUTEX draws the CP's carved backdrop through the textured pipeline + a disk-loaded background
// .png (REX_BGTEX=<path>, default media/Assets/Frontend/Graphics/LevelSelectBG.png — 1280x720). The
// submitted verts carry synthetic screen→[0,1] UVs, so the 4 quadrants reassemble the full background.
// Requires REX_MENUTEST (shares its present path); the debug menu-quads then draw panels/text on top.
const bool g_menutex = (getenv("REX_MENUTEX") != nullptr);
const bool g_menulive = (getenv("REX_MENULIVE") != nullptr);  // cont.60: live menu-texture contact sheet
bool                  g_bgLoaded     = false;            // background texture uploaded into g_tex* (once)
std::mutex            g_texGeomMtx;
std::vector<float>    g_texGeom;                          // interleaved pos.xy+uv.xy, guarded by g_texGeomMtx
std::atomic<int>      g_texGeomVerts{0};                  // submitted textured-geometry vertex count
VkBuffer              g_texGeoVB     = VK_NULL_HANDLE;    // host-visible pos.xy+uv.xy, persistently mapped
VkDeviceMemory        g_texGeoVBMem  = VK_NULL_HANDLE;
void*                 g_texGeoVBMap  = nullptr;
VkDeviceSize          g_texGeoVBCap  = 0;

// In-engine screenshot (REX_RENDER_SHOT=<frame> -> capture that present to /tmp/varianta_shot.ppm).
// Self-contained: external Wayland capture tools (spectacle/grim/import) don't work here.
const uint32_t   g_shotTarget = []{ const char* s = getenv("REX_RENDER_SHOT"); return s ? (uint32_t)atoi(s) : 0u; }();
VkBuffer         g_capBuf   = VK_NULL_HANDLE;
VkDeviceMemory   g_capMem   = VK_NULL_HANDLE;
VkDeviceSize     g_capSize  = 0;

// --- Decoded-frame (intro movie) present (increment 3) -------------------------------------------------
// VC-1 frame-pool buffers published from VdSwap. Layout RE'd from the live decode dump: planar I420,
// full-range BT.601, 3 consecutive allocations per frame — Y (size 0x101440, pitch 1344, 1280x720) then
// U then V (size 0x40520 each, pitch 672, 640x360 — chroma sharp-stride confirmed at 672, mean ~128).
constexpr uint32_t kVidPitch = 1344, kVidW = 1280, kVidH = 720, kVidYOff = 0;
constexpr uint32_t kYSize = 0x101440, kCSize = 0x40520, kCPitch = 672, kCW = 640, kCH = 360;
std::atomic<const uint32_t*> g_vbufs{nullptr};   // -> kernel.cpp's g_videoBufs[] (addresses stable once captured)
std::atomic<const uint32_t*> g_vsizes{nullptr};  // -> kernel.cpp's g_videoBufSz[] (alloc sizes; Y=0x101440)
std::atomic<int>             g_vbufN{0};
VkBuffer       g_vidBuf    = VK_NULL_HANDLE;      // host-visible BGRA staging (1280x720x4), persistently mapped
VkDeviceMemory g_vidMem    = VK_NULL_HANDLE;
void*          g_vidMapped = nullptr;
uint32_t       g_vidSig[24] = {0};                // per-buffer luma signature for freshness (motion) tracking
uint64_t       g_vidSettledAt[24] = {0};          // present index at which each buffer last STOPPED changing
uint32_t       g_vidStable[24] = {0};             // consecutive presents a buffer has been unchanged
int            g_vidLastSel = -1;
uint64_t       g_vidFrame   = 0;                  // count of presents that showed a decoded frame
uint32_t       g_vidShownSig = 0;                 // luma sig of the last DISPLAYED frame (motion measure)
uint64_t       g_vidDistinct = 0;                 // count of distinct movie frames actually displayed

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[render] %s = %d\n", #x, (int)_r); return false; } } while (0)

uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    return 0;
}

void EnsureCaptureBuffer() {
    VkDeviceSize need = (VkDeviceSize)g_extent.width * g_extent.height * 4;
    if (g_capBuf && g_capSize >= need) return;
    if (g_capBuf) { vkDestroyBuffer(g_device, g_capBuf, nullptr); vkFreeMemory(g_device, g_capMem, nullptr); }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = need; bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &bci, nullptr, &g_capBuf) != VK_SUCCESS) { g_capBuf = VK_NULL_HANDLE; return; }
    VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(g_device, g_capBuf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_device, &mai, nullptr, &g_capMem) != VK_SUCCESS) { vkDestroyBuffer(g_device, g_capBuf, nullptr); g_capBuf = VK_NULL_HANDLE; return; }
    vkBindBufferMemory(g_device, g_capBuf, g_capMem, 0);
    g_capSize = need;
}

// Write the mapped capture buffer (swapchain format B8G8R8A8) to a PPM (RGB).
void WriteCapturePPM(const char* path) {
    void* p = nullptr;
    if (vkMapMemory(g_device, g_capMem, 0, g_capSize, 0, &p) != VK_SUCCESS) return;
    const uint8_t* src = (const uint8_t*)p;
    FILE* f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", g_extent.width, g_extent.height);
        std::vector<uint8_t> row(g_extent.width * 3);
        for (uint32_t y = 0; y < g_extent.height; y++) {
            const uint8_t* s = src + (size_t)y * g_extent.width * 4;
            for (uint32_t x = 0; x < g_extent.width; x++) {   // BGRA -> RGB
                row[x*3+0] = s[x*4+2]; row[x*3+1] = s[x*4+1]; row[x*3+2] = s[x*4+0];
            }
            fwrite(row.data(), 1, row.size(), f);
        }
        fclose(f);
        fprintf(stderr, "[render] captured frame %llu -> %s\n", (unsigned long long)g_frame, path);
    }
    vkUnmapMemory(g_device, g_capMem);
}

// Create the persistently-mapped host-visible BGRA staging buffer the decoded frame is uploaded through.
void EnsureVideoBuffer() {
    if (g_vidBuf) return;
    VkDeviceSize need = (VkDeviceSize)kVidW * kVidH * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = need; bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(g_device, &bci, nullptr, &g_vidBuf) != VK_SUCCESS) { g_vidBuf = VK_NULL_HANDLE; return; }
    VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(g_device, g_vidBuf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(g_device, &mai, nullptr, &g_vidMem) != VK_SUCCESS) {
        vkDestroyBuffer(g_device, g_vidBuf, nullptr); g_vidBuf = VK_NULL_HANDLE; return;
    }
    vkBindBufferMemory(g_device, g_vidBuf, g_vidMem, 0);
    vkMapMemory(g_device, g_vidMem, 0, need, 0, &g_vidMapped);
}

// Pick the freshest CLEAN frame-pool buffer and expand its luma plane into the BGRA staging buffer.
// Returns true if a frame was written into g_vidMapped (ready to copy to the swapchain image).
// The movie decodes a handful of fps while we present ~60 fps, and the decoder writes one pool slot at a
// time, so a "current" display frame must be chosen carefully — see the in-body comment for the criteria.
bool PickAndFillVideo() {
    const uint32_t* bufs = g_vbufs.load(); const uint32_t* sizes = g_vsizes.load(); int n = g_vbufN.load();
    if (!bufs || n <= 0 || !g_base || !g_vidMapped) return false;
    if (n > 24) n = 24;
    // Selecting the buffer that is changing RIGHT NOW would read it mid-decode (tearing — top rows new,
    // bottom rows stale). Instead present the most-recently-COMPLETED frame: a Y plane that has been written
    // but did NOT change this present (its decode has settled). The decoder writes one pool slot at a time,
    // so at any moment ≤1 slot is mid-write and the rest are clean finished frames.
    // The cooperative scheduler can stall the decoder mid-frame, leaving a buffer half-written (unwritten
    // rows read back as black bands). So a clean display frame must be BOTH settled (not changing now) AND
    // complete (no black-unwritten bands). Sample a grid: a row whose mean luma is ~0 is unwritten; a fully
    // decoded frame has all rows written (sky is ~128, never a full black band). Pick the freshest such frame.
    // Only Y planes (alloc size 0x101440) are candidates; their U/V are the next two pool slots.
    constexpr int kRows = 72, kCols = 32, kMaxBlank = 3;
    int sel = -1; uint64_t bestAge = 0;
    for (int i = 0; i < n; i++) {
        if (sizes && sizes[i] != kYSize) continue;             // skip U/V planes — only Y is a candidate
        const uint8_t* y = g_base + bufs[i] + kVidYOff;
        uint32_t sig = 0; int written = 0;
        for (int r = 0; r < kRows; r++) {
            const uint8_t* rp = y + (size_t)((r * kVidH) / kRows) * kVidPitch;
            uint32_t rs = 0;
            for (int c = 0; c < kCols; c++) rs += rp[(c * kVidW) / kCols];
            sig = sig * 131 + rs;
            if (rs / kCols > 12) written++;                    // row mean > 12 ⇒ written (not a black band)
        }
        bool changed = (sig != g_vidSig[i]);
        g_vidSig[i] = sig;
        if (changed) { g_vidStable[i] = 0; }
        else if (++g_vidStable[i] == 2) g_vidSettledAt[i] = g_frame + 1;  // just finished (stable ≥2 presents)
        bool complete = (written >= kRows - kMaxBlank);        // no large unwritten band
        if (g_vidStable[i] >= 2 && complete && g_vidSettledAt[i] > bestAge) { bestAge = g_vidSettledAt[i]; sel = i; }
    }
    if (sel < 0) sel = g_vidLastSel;        // nothing complete+settled yet — hold the last good frame
    if (sel < 0) return false;              // no frame yet
    // Motion measure: count how many DISTINCT movie frames we actually display (does playback advance, or
    // is it stuck re-decoding the same ~2 frames?). g_vidSig[sel] is this frame's luma signature.
    if (g_vidSig[sel] != g_vidShownSig) { g_vidShownSig = g_vidSig[sel]; g_vidDistinct++; }
    g_vidLastSel = sel;
    // YUV->RGB (planar I420, full-range BT.601). U/V are the next two pool slots (size 0x40520) when present.
    const uint8_t* yp = g_base + bufs[sel] + kVidYOff;
    bool color = (sel + 2 < n) && (!sizes || (sizes[sel + 1] == kCSize && sizes[sel + 2] == kCSize));
    const uint8_t* up = color ? g_base + bufs[sel + 1] : nullptr;
    const uint8_t* vp = color ? g_base + bufs[sel + 2] : nullptr;
    auto clamp8 = [](int v) -> uint8_t { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    uint8_t* dst = (uint8_t*)g_vidMapped;
    for (uint32_t row = 0; row < kVidH; row++) {
        const uint8_t* s = yp + (size_t)row * kVidPitch;
        const uint8_t* ur = up ? up + (size_t)(row >> 1) * kCPitch : nullptr;
        const uint8_t* vr = vp ? vp + (size_t)(row >> 1) * kCPitch : nullptr;
        uint8_t* d = dst + (size_t)row * kVidW * 4;
        for (uint32_t x = 0; x < kVidW; x++) {
            int Y = s[x];
            if (ur) {
                int u = ur[x >> 1] - 128, v = vr[x >> 1] - 128;
                d[0] = clamp8(Y + ((454 * u) >> 8));              // B = Y + 1.772*u
                d[1] = clamp8(Y - ((88 * u + 183 * v) >> 8));     // G = Y - 0.344*u - 0.714*v
                d[2] = clamp8(Y + ((359 * v) >> 8));              // R = Y + 1.402*v
            } else { d[0] = d[1] = d[2] = (uint8_t)Y; }           // no chroma yet -> grayscale
            d[3] = 255; d += 4;
        }
    }
    return true;
}

// GPU build (piece 1): render pass — one color attachment, clear -> present.
bool CreateRenderPass() {
    if (g_renderPass) return true;
    VkAttachmentDescription att{};
    att.format = g_format; att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = 1; ci.pAttachments = &att;
    ci.subpassCount = 1; ci.pSubpasses = &sub;
    ci.dependencyCount = 1; ci.pDependencies = &dep;
    VKCHECK(vkCreateRenderPass(g_device, &ci, nullptr, &g_renderPass));
    return true;
}

// GPU build (piece 1): per-image views + framebuffers (recreated on swapchain change).
bool CreateFramebuffers() {
    for (auto fb : g_framebuffers) if (fb) vkDestroyFramebuffer(g_device, fb, nullptr);
    for (auto iv : g_imageViews) if (iv) vkDestroyImageView(g_device, iv, nullptr);
    g_framebuffers.clear(); g_imageViews.clear();
    if (!g_renderPass) return false;
    g_imageViews.resize(g_images.size()); g_framebuffers.resize(g_images.size());
    for (size_t i = 0; i < g_images.size(); i++) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = g_images[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = g_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(g_device, &vi, nullptr, &g_imageViews[i]));
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g_renderPass; fi.attachmentCount = 1; fi.pAttachments = &g_imageViews[i];
        fi.width = g_extent.width; fi.height = g_extent.height; fi.layers = 1;
        VKCHECK(vkCreateFramebuffer(g_device, &fi, nullptr, &g_framebuffers[i]));
    }
    return true;
}

// GPU build (piece 1): the test-triangle pipeline (no vertex input; dynamic viewport/scissor).
bool CreateGraphicsPipeline() {
    if (g_testPipe) return true;
    auto mkModule = [](const uint32_t* code, size_t bytes) -> VkShaderModule {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = bytes; mi.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE; vkCreateShaderModule(g_device, &mi, nullptr, &m); return m;
    };
    VkShaderModule vs = mkModule(kTestVertSpv, sizeof(kTestVertSpv));
    VkShaderModule fs = mkModule(kTestFragSpv, sizeof(kTestFragSpv));
    if (!vs || !fs) { fprintf(stderr, "[render] test shader module create failed\n"); return false; }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dst{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    vkCreatePipelineLayout(g_device, &pl, nullptr, &g_pipeLayout);
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vin; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps;
    pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pColorBlendState = &cb;
    pci.pDynamicState = &dst; pci.layout = g_pipeLayout; pci.renderPass = g_renderPass; pci.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pci, nullptr, &g_testPipe);
    vkDestroyShaderModule(g_device, vs, nullptr); vkDestroyShaderModule(g_device, fs, nullptr);
    if (r != VK_SUCCESS) { fprintf(stderr, "[render] test pipeline create = %d\n", (int)r); return false; }
    fprintf(stderr, "[render] GPU-build: test-triangle pipeline created (render pass + pipeline ready)\n");
    return true;
}

// GPU build (piece 3b): the menu-quad pipeline — float2 vertex input + {mat4 mvp, vec4 color} push const,
// alpha-blended, TRIANGLE_LIST (quads expanded to 2 tris on the CPU). Reuses g_renderPass.
bool CreateMenuPipeline() {
    if (g_menuPipe) return true;
    if (!g_renderPass) return false;
    auto mk = [](const uint32_t* code, size_t bytes) -> VkShaderModule {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = bytes; mi.pCode = code; VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(g_device, &mi, nullptr, &m); return m; };
    VkShaderModule vs = mk(kMenuQuadVertSpv, sizeof(kMenuQuadVertSpv));
    VkShaderModule fs = mk(kMenuQuadFragSpv, sizeof(kMenuQuadFragSpv));
    if (!vs || !fs) { fprintf(stderr, "[render] menu shader module create failed\n"); return false; }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkVertexInputBindingDescription vb{0, 8, VK_VERTEX_INPUT_RATE_VERTEX};   // float2 = 8-byte stride
    VkVertexInputAttributeDescription va{0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vb;
    vin.vertexAttributeDescriptionCount = 1; vin.pVertexAttributeDescriptions = &va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dst{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MenuPC)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(g_device, &pl, nullptr, &g_menuLayout);
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vin; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps;
    pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pColorBlendState = &cb;
    pci.pDynamicState = &dst; pci.layout = g_menuLayout; pci.renderPass = g_renderPass; pci.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pci, nullptr, &g_menuPipe);
    vkDestroyShaderModule(g_device, vs, nullptr); vkDestroyShaderModule(g_device, fs, nullptr);
    if (r != VK_SUCCESS) { fprintf(stderr, "[render] menu pipeline create = %d\n", (int)r); return false; }
    fprintf(stderr, "[render] GPU-build: menu-quad pipeline created (float2 + mvp/color push const, alpha blend)\n");
    return true;
}

// Textured UI pipeline (disk-resource path): pos.xy+uv.xy (stride 16) + a sampler2D (set0/binding0) +
// {mvp,color} push const. Shaders = menutex.vert / SPTextured.frag (texture(diffuse,uv)*color).
bool CreateTexturedPipeline() {
    if (g_texPipe) return true;
    if (!g_renderPass) return false;
    VkDescriptorSetLayoutBinding dsb{};
    dsb.binding = 0; dsb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    dsb.descriptorCount = 1; dsb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsli.bindingCount = 1; dsli.pBindings = &dsb;
    VKCHECK(vkCreateDescriptorSetLayout(g_device, &dsli, nullptr, &g_texSetLayout));
    auto mk = [](const uint32_t* code, size_t bytes) -> VkShaderModule {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = bytes; mi.pCode = code; VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(g_device, &mi, nullptr, &m); return m; };
    VkShaderModule vs = mk(kMenuTexVertSpv, sizeof(kMenuTexVertSpv));
    VkShaderModule fs = mk(kMenuTexFragSpv, sizeof(kMenuTexFragSpv));
    if (!vs || !fs) { fprintf(stderr, "[render] tex shader module create failed\n"); return false; }
    VkPipelineShaderStageCreateInfo st[2]{};
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkVertexInputBindingDescription vb{0, 16, VK_VERTEX_INPUT_RATE_VERTEX};   // pos.xy + uv.xy = 16-byte stride
    VkVertexInputAttributeDescription va[2] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},      // pos at offset 0
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 8} };    // uv  at offset 8
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vb;
    vin.vertexAttributeDescriptionCount = 2; vin.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dst{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dst.dynamicStateCount = 2; dst.pDynamicStates = dyn;
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MenuPC)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1; pl.pSetLayouts = &g_texSetLayout;
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(g_device, &pl, nullptr, &g_texLayout);
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vin; pci.pInputAssemblyState = &ia; pci.pViewportState = &vps;
    pci.pRasterizationState = &rs; pci.pMultisampleState = &ms; pci.pColorBlendState = &cb;
    pci.pDynamicState = &dst; pci.layout = g_texLayout; pci.renderPass = g_renderPass; pci.subpass = 0;
    VkResult r = vkCreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &pci, nullptr, &g_texPipe);
    vkDestroyShaderModule(g_device, vs, nullptr); vkDestroyShaderModule(g_device, fs, nullptr);
    if (r != VK_SUCCESS) { fprintf(stderr, "[render] tex pipeline create = %d\n", (int)r); return false; }
    fprintf(stderr, "[render] disk-resource: textured pipeline created (pos+uv + sampler2D, alpha blend)\n");
    return true;
}

// Host-visible vertex buffer for the menu quads, persistently mapped (grows on demand).
bool EnsureMenuVB(VkDeviceSize bytes) {
    if (g_menuVB && g_menuVBCap >= bytes) return true;
    if (g_menuVB) { vkDeviceWaitIdle(g_device); vkDestroyBuffer(g_device, g_menuVB, nullptr); vkFreeMemory(g_device, g_menuVBMem, nullptr); g_menuVB = VK_NULL_HANDLE; }
    VkDeviceSize cap = bytes < 0x10000 ? 0x10000 : bytes;   // min 64 KiB
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = cap; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(g_device, &bi, nullptr, &g_menuVB));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_device, g_menuVB, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(g_device, &ai, nullptr, &g_menuVBMem));
    vkBindBufferMemory(g_device, g_menuVB, g_menuVBMem, 0);
    vkMapMemory(g_device, g_menuVBMem, 0, cap, 0, &g_menuVBMap);
    g_menuVBCap = cap;
    return true;
}

// Task #8: host-visible pos.xy+uv.xy vertex buffer for the submitted textured geometry (grows on demand).
bool EnsureTexGeoVB(VkDeviceSize bytes) {
    if (g_texGeoVB && g_texGeoVBCap >= bytes) return true;
    if (g_texGeoVB) { vkDeviceWaitIdle(g_device); vkDestroyBuffer(g_device, g_texGeoVB, nullptr); vkFreeMemory(g_device, g_texGeoVBMem, nullptr); g_texGeoVB = VK_NULL_HANDLE; }
    VkDeviceSize cap = bytes < 0x10000 ? 0x10000 : bytes;   // min 64 KiB
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = cap; bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(g_device, &bi, nullptr, &g_texGeoVB));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_device, g_texGeoVB, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(g_device, &ai, nullptr, &g_texGeoVBMem));
    vkBindBufferMemory(g_device, g_texGeoVB, g_texGeoVBMem, 0);
    vkMapMemory(g_device, g_texGeoVBMem, 0, cap, 0, &g_texGeoVBMap);
    g_texGeoVBCap = cap;
    return true;
}

bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps));
    g_extent = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent : VkExtent2D{1280, 720};
    if (g_extent.width == 0 || g_extent.height == 0) return false;   // minimised — retry later

    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR chosen = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    g_format = chosen.format;

    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = g_surface;
    sci.minImageCount = imgCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = g_extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;   // guaranteed; vsync (paces the render thread to ~60Hz)
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = g_swapchain;
    VkSwapchainKHR newSc = VK_NULL_HANDLE;
    VKCHECK(vkCreateSwapchainKHR(g_device, &sci, nullptr, &newSc));
    if (g_swapchain) vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
    g_swapchain = newSc;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, nullptr);
    g_images.resize(n);
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, g_images.data());
    fprintf(stderr, "[render] swapchain %ux%u fmt=%d images=%u\n", g_extent.width, g_extent.height, (int)g_format, n);
    if (g_drawtest || g_menutest) { CreateRenderPass(); CreateFramebuffers(); }   // GPU build (piece 1 / 3b)
    return true;
}

bool LoadBackgroundOnce();   // Task #8: defined below (with the PNG loaders); called at the end of Init.

bool Init() {
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "[render] SDL_Init: %s\n", SDL_GetError()); return false; }
    g_window = SDL_CreateWindow("South Park: Let's Go Tower Defense Play! (variant A)",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!g_window) { fprintf(stderr, "[render] SDL_CreateWindow: %s\n", SDL_GetError()); return false; }

    unsigned extCount = 0;
    SDL_Vulkan_GetInstanceExtensions(g_window, &extCount, nullptr);
    std::vector<const char*> exts(extCount);
    SDL_Vulkan_GetInstanceExtensions(g_window, &extCount, exts.data());

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "sp_td_varianta";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = extCount;
    ici.ppEnabledExtensionNames = exts.data();
    VKCHECK(vkCreateInstance(&ici, nullptr, &g_instance));

    if (!SDL_Vulkan_CreateSurface(g_window, g_instance, &g_surface)) {
        fprintf(stderr, "[render] SDL_Vulkan_CreateSurface: %s\n", SDL_GetError()); return false;
    }

    uint32_t pdc = 0;
    vkEnumeratePhysicalDevices(g_instance, &pdc, nullptr);
    if (!pdc) { fprintf(stderr, "[render] no Vulkan devices\n"); return false; }
    std::vector<VkPhysicalDevice> pds(pdc);
    vkEnumeratePhysicalDevices(g_instance, &pdc, pds.data());
    for (auto pd : pds) {
        uint32_t qfc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfc);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfc, qfs.data());
        for (uint32_t i = 0; i < qfc; i++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, g_surface, &present);
            if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { g_phys = pd; g_qfam = i; break; }
        }
        if (g_phys) break;
    }
    if (!g_phys) { fprintf(stderr, "[render] no graphics+present queue\n"); return false; }
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(g_phys, &props);
        fprintf(stderr, "[render] GPU: %s (qfam=%u)\n", props.deviceName, g_qfam);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = devExts;
    VKCHECK(vkCreateDevice(g_phys, &dci, nullptr, &g_device));
    vkGetDeviceQueue(g_device, g_qfam, 0, &g_queue);

    if (!CreateSwapchain()) return false;
    if (g_drawtest) CreateGraphicsPipeline();   // GPU build (piece 1): render pass already made in CreateSwapchain
    if (g_menutest) CreateMenuPipeline();       // GPU build (piece 3b): the menu-quad pipeline
    if (g_textest || g_menutex || g_hudsheet || g_menulive) CreateTexturedPipeline();   // disk-resource path: the textured UI pipeline

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_qfam;
    VKCHECK(vkCreateCommandPool(g_device, &pci, nullptr, &g_cmdPool));
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = g_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    VKCHECK(vkAllocateCommandBuffers(g_device, &cbai, &g_cmd));

    VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKCHECK(vkCreateSemaphore(g_device, &semci, nullptr, &g_acquireSem));
    VKCHECK(vkCreateSemaphore(g_device, &semci, nullptr, &g_presentSem));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VKCHECK(vkCreateFence(g_device, &fci, nullptr, &g_fence));

    if (g_menutex) LoadBackgroundOnce();   // Task #8: upload the menu background .png (uses g_cmdPool, made above)

    fprintf(stderr, "[render] initialised — window + Vulkan up.\n");
    return true;
}

void ImageBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// Upload an RGBA8 image to a device-local VkImage + view + sampler + descriptor set (one-time, blocking).
// The disk-resource path: media/Assets/*.png decoded -> here. (REX_TEXTEST uses a procedural checker.)
bool UploadTexture(const uint8_t* rgba, uint32_t W, uint32_t H) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D; ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {W, H, 1}; ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(g_device, &ii, nullptr, &g_texImg));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_device, g_texImg, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size; mai.memoryTypeIndex = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKCHECK(vkAllocateMemory(g_device, &mai, nullptr, &g_texImgMem));
    vkBindImageMemory(g_device, g_texImg, g_texImgMem, 0);
    VkDeviceSize sz = (VkDeviceSize)W * H * 4;
    VkBuffer stg = VK_NULL_HANDLE; VkDeviceMemory stgMem = VK_NULL_HANDLE;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = sz; bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKCHECK(vkCreateBuffer(g_device, &bi, nullptr, &stg));
    VkMemoryRequirements smr; vkGetBufferMemoryRequirements(g_device, stg, &smr);
    VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    smai.allocationSize = smr.size; smai.memoryTypeIndex = FindMemoryType(smr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKCHECK(vkAllocateMemory(g_device, &smai, nullptr, &stgMem));
    vkBindBufferMemory(g_device, stg, stgMem, 0);
    void* map = nullptr; vkMapMemory(g_device, stgMem, 0, sz, 0, &map); memcpy(map, rgba, sz); vkUnmapMemory(g_device, stgMem);
    VkCommandBuffer cmd = VK_NULL_HANDLE; VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = g_cmdPool; cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount = 1;
    vkAllocateCommandBuffers(g_device, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbbi);
    ImageBarrier(cmd, g_texImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy rgn{}; rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; rgn.imageExtent = {W, H, 1};
    vkCmdCopyBufferToImage(cmd, stg, g_texImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    ImageBarrier(cmd, g_texImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(g_queue);
    vkFreeCommandBuffers(g_device, g_cmdPool, 1, &cmd);
    vkDestroyBuffer(g_device, stg, nullptr); vkFreeMemory(g_device, stgMem, nullptr);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_texImg; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VKCHECK(vkCreateImageView(g_device, &vi, nullptr, &g_texView));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKCHECK(vkCreateSampler(g_device, &sci, nullptr, &g_texSampler));
    VkDescriptorPoolSize dps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1; dpci.poolSizeCount = 1; dpci.pPoolSizes = &dps;
    VKCHECK(vkCreateDescriptorPool(g_device, &dpci, nullptr, &g_texDescPool));
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = g_texDescPool; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &g_texSetLayout;
    VKCHECK(vkAllocateDescriptorSets(g_device, &dsai, &g_texSet));
    VkDescriptorImageInfo dii{g_texSampler, g_texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wds{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet = g_texSet; wds.dstBinding = 0; wds.descriptorCount = 1;
    wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wds.pImageInfo = &dii;
    vkUpdateDescriptorSets(g_device, 1, &wds, 0, nullptr);
    fprintf(stderr, "[render] disk-resource: texture %ux%u uploaded + descriptor set ready\n", W, H);
    return true;
}

bool CreateTestTexture() {   // REX_TEXTEST: a 64x64 magenta/cyan checker to validate the textured pipeline
    if (g_texImg) return true;
    if (!g_texSetLayout) return false;
    const uint32_t W = 64, H = 64; static uint8_t px[W*H*4];
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) {
        bool c = ((x >> 3) ^ (y >> 3)) & 1; uint8_t* p = &px[(y*W + x)*4];
        p[0] = c ? 230 : 40; p[1] = c ? 80 : 180; p[2] = c ? 40 : 230; p[3] = 255; }
    return UploadTexture(px, W, H);
}

// Disk-resource path: decode a real .png (media/Assets/*.png) -> RGBA -> texture, via libpng's simplified API.
bool LoadPNGToTexture(const char* path) {
    if (g_texImg) return true;
    if (!g_texSetLayout) return false;
    png_image img; memset(&img, 0, sizeof(img)); img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&img, path)) { fprintf(stderr, "[render] PNG open failed: %s\n", path); return false; }
    img.format = PNG_FORMAT_RGBA;
    std::vector<uint8_t> buf(PNG_IMAGE_SIZE(img));
    if (!png_image_finish_read(&img, nullptr, buf.data(), 0, nullptr)) {
        fprintf(stderr, "[render] PNG decode failed: %s\n", path); png_image_free(&img); return false; }
    uint32_t W = img.width, H = img.height; png_image_free(&img);
    fprintf(stderr, "[render] disk-resource: loaded PNG %s (%ux%u)\n", path, W, H);
    return UploadTexture(buf.data(), W, H);
}

// Task #8: load the menu BACKGROUND .png once (into g_tex* — the textured pipeline's sampled image).
// REX_BGTEX=<path> overrides; the default is the title's 1280x720 frontend background (cwd = varianta/).
bool LoadBackgroundOnce() {
    if (g_bgLoaded) return true;
    const char* p = getenv("REX_BGTEX");
    const char* path = p ? p : "../private/extracted/media/Assets/Frontend/Graphics/LevelSelectBG.png";
    g_bgLoaded = LoadPNGToTexture(path);
    if (!g_bgLoaded) fprintf(stderr, "[render] disk-resource: background load FAILED (%s) — set REX_BGTEX\n", path);
    return g_bgLoaded;
}

// cont.37: inflate a whole zlib stream into `out` (streaming — the DDS containers decompress to tens of MB).
static bool InflateAll(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out) {
    z_stream zs{}; if (inflateInit(&zs) != Z_OK) return false;
    zs.next_in = (Bytef*)src; zs.avail_in = (uInt)srcLen;
    out.clear(); std::vector<uint8_t> chunk(1u << 20); int ret;
    do {
        zs.next_out = chunk.data(); zs.avail_out = (uInt)chunk.size();
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) { inflateEnd(&zs); return false; }
        out.insert(out.end(), chunk.data(), chunk.data() + (chunk.size() - zs.avail_out));
    } while (ret != Z_STREAM_END);
    inflateEnd(&zs);
    return true;
}

// cont.37 (REX_HUDSHEET): the disk-resource render of the title's REAL HUD textures. Reads the zlib'd
// Textures/HudImages.bin container, inflates it, walks its DDS entries, decodes each via the cont.36 Xenos
// decoder (rex_tex), and composites the first N into a contact sheet uploaded as the textured-pipeline image.
// REX_HUDSHEET=<path> overrides the container; the default is HudImages.bin (cwd = varianta/).
bool LoadHudSheetOnce() {
    if (g_texImg) return true;
    if (!g_texSetLayout) return false;
    const char* env = getenv("REX_HUDSHEET");
    const char* path = (env && env[0] && strcmp(env, "1") != 0) ? env
        : "../private/extracted/media/Assets/Global/Textures/HudImages.bin";
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[hudsheet] open FAILED: %s\n", path); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> comp(n > 0 ? n : 0);
    size_t rd = comp.empty() ? 0 : fread(comp.data(), 1, comp.size(), f); fclose(f);
    std::vector<uint8_t> dec;
    if (rd < 2 || !InflateAll(comp.data(), comp.size(), dec)) { fprintf(stderr, "[hudsheet] inflate FAILED\n"); return false; }
    fprintf(stderr, "[hudsheet] %s: %ld compressed -> %zu decompressed\n", path, n, dec.size());

    const uint32_t SW = 1280, SH = 720; const int COLS = 8, ROWS = 6;
    const int CW = SW / COLS, CH = SH / ROWS;     // 160x120 cells, 48 textures
    std::vector<uint8_t> sheet((size_t)SW * SH * 4, 0);
    // subtle grey checker background so transparent (alpha) regions of the HUD art are visible
    for (uint32_t y = 0; y < SH; y++) for (uint32_t x = 0; x < SW; x++) {
        uint8_t g = (((x >> 4) ^ (y >> 4)) & 1) ? 54 : 40; uint8_t* p = &sheet[((size_t)y * SW + x) * 4];
        p[0] = p[1] = p[2] = g; p[3] = 255; }

    size_t pos = 0; int placed = 0, scanned = 0; const int MAXCELLS = COLS * ROWS;
    while (placed < MAXCELLS && pos + 128 <= dec.size()) {
        size_t j = SIZE_MAX;
        for (size_t k = pos; k + 4 <= dec.size(); k++)
            if (dec[k] == 'D' && dec[k+1] == 'D' && dec[k+2] == 'S' && dec[k+3] == ' ') { j = k; break; }
        if (j == SIZE_MAX || j + 128 > dec.size()) break;
        const uint8_t* h = &dec[j];
        uint32_t H = h[12] | (h[13]<<8) | (h[14]<<16) | (h[15]<<24);
        uint32_t W = h[16] | (h[17]<<8) | (h[18]<<16) | (h[19]<<24);
        uint32_t bitcount = h[88] | (h[89]<<8) | (h[90]<<16) | (h[91]<<24);
        uint32_t fourcc = h[84] | (h[85]<<8) | (h[86]<<16) | (h[87]<<24);
        pos = j + 4; scanned++;
        if (W == 0 || H == 0 || W > 2048 || H > 2048 || fourcc != 0 || bitcount != 32) continue;  // 32bpp uncompressed only (this container)
        if (j + 128 + (size_t)W * H * 4 > dec.size()) break;
        rex_tex::Desc d{}; d.format = rex_tex::FMT_8_8_8_8; d.width = W; d.height = H; d.tiled = 0; d.endian = rex_tex::END_NONE;
        std::vector<uint8_t> rgba;
        if (!rex_tex::DecodeBytesToRGBA(&dec[j + 128], d, rgba)) { continue; }
        // place into the cell, scaled to fit (nearest), aspect-preserved, alpha-composited over the checker
        int cx = (placed % COLS) * CW, cy = (placed / COLS) * CH, pad = 6;
        int aw = CW - 2 * pad, ah = CH - 2 * pad;
        float s = std::min((float)aw / (float)W, (float)ah / (float)H);
        int dw = (int)(W * s), dh = (int)(H * s), ox = cx + pad + (aw - dw) / 2, oy = cy + pad + (ah - dh) / 2;
        for (int yy = 0; yy < dh; yy++) for (int xx = 0; xx < dw; xx++) {
            int sx = (int)(xx / s), sy = (int)(yy / s); if (sx >= (int)W || sy >= (int)H) continue;
            const uint8_t* sp = &rgba[((size_t)sy * W + sx) * 4];
            uint8_t* dp = &sheet[((size_t)(oy + yy) * SW + (ox + xx)) * 4];
            float a = sp[3] / 255.0f;
            dp[0] = (uint8_t)(sp[0] * a + dp[0] * (1 - a));
            dp[1] = (uint8_t)(sp[1] * a + dp[1] * (1 - a));
            dp[2] = (uint8_t)(sp[2] * a + dp[2] * (1 - a));
        }
        placed++;
        pos = j + 128 + (size_t)W * H * 4;
    }
    fprintf(stderr, "[hudsheet] decoded + composited %d HUD textures (scanned %d DDS) into a %ux%u sheet\n", placed, scanned, SW, SH);
    if (placed == 0) return false;
    return UploadTexture(sheet.data(), SW, SH);
}

// cont.60 (REX_MENULIVE): live menu-texture contact sheet. kernel.cpp's REX_TEXDECODE decodes each per-draw
// menu texture (the 0xA2 working buffers, cont.58) and calls BlitMenuCell (below) to composite it here; the
// render thread uploads + draws the sheet (proves the working-buffer texture -> live Vulkan render path).
static const int MS_COLS = 4, MS_CELL = 256;
static const int MS_W = MS_COLS * MS_CELL, MS_H = 2 * MS_CELL;   // 1024x512, up to 8 cells
static std::vector<uint8_t> g_menuSheet;
static std::mutex           g_menuSheetMtx;
static uint32_t             g_menuBases[8] = {0};
static std::atomic<int>     g_menuCells{0};

bool LoadMenuSheetLive() {                       // render thread: upload the sheet once enough cells are placed
    if (!g_menulive || g_menuCells.load() < 3) return false;   // wait for several menu textures to decode+blit
    std::lock_guard<std::mutex> lk(g_menuSheetMtx);
    if (g_menuSheet.empty()) return false;
    return UploadTexture(g_menuSheet.data(), MS_W, MS_H);
}

void PumpEvents() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) {
            static bool logged = false;
            if (!logged) { fprintf(stderr, "[render] SDL_QUIT (ignored during bring-up)\n"); logged = true; }
        }
    }
}

// One present iteration on the render thread. Returns false on a fatal error (stops the loop).
bool PresentOnce() {
    vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX);

    uint32_t idx = 0;
    VkResult acq = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, g_acquireSem, VK_NULL_HANDLE, &idx);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(g_device); CreateSwapchain(); return true; }
    if (acq != VK_SUCCESS) { fprintf(stderr, "[render] acquire = %d\n", (int)acq); return true; }

    vkResetFences(g_device, 1, &g_fence);
    vkResetCommandBuffer(g_cmd, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g_cmd, &bi);

    if (g_menutest && g_menuPipe && idx < g_framebuffers.size()) {
        // GPU build (piece 3b): the menu-quad pipeline. REX_MENUTEST validates it with 3 overlapping
        // alpha-blended clip-space rects (proves float2 vertex input + {mvp,color} push const + blend +
        // multi-draw). The real path will feed extracted menu geometry + the $worldviewProj matrix here.
        static const float quads[] = {                          // 3 rects, each 2 tris (6 verts), clip space
            -0.8f,-0.8f,  0.2f,-0.8f,  0.2f, 0.2f,  -0.8f,-0.8f,  0.2f, 0.2f, -0.8f, 0.2f,   // A
            -0.2f,-0.2f,  0.8f,-0.2f,  0.8f, 0.8f,  -0.2f,-0.2f,  0.8f, 0.8f, -0.2f, 0.8f,   // B
             0.1f,-0.6f,  0.6f,-0.6f,  0.6f,-0.1f,   0.1f,-0.6f,  0.6f,-0.1f,  0.1f,-0.1f }; // C
        int subVerts = g_menuGeomVerts.load();                   // Layer 2: real geometry if submitted
        if (subVerts > 0) { std::lock_guard<std::mutex> lk(g_menuGeomMtx);
            EnsureMenuVB(g_menuGeom.size()*sizeof(float));
            if (g_menuVBMap) memcpy(g_menuVBMap, g_menuGeom.data(), g_menuGeom.size()*sizeof(float));
        } else { EnsureMenuVB(sizeof(quads)); if (g_menuVBMap) memcpy(g_menuVBMap, quads, sizeof(quads)); }
        // Task #8: upload the submitted TEXTURED geometry (backdrop quadrants, pos.xy+uv.xy) BEFORE the
        // render pass — EnsureTexGeoVB may recreate the buffer (vkDeviceWaitIdle), unsafe inside a pass.
        int texVerts = g_menutex ? g_texGeomVerts.load() : 0;
        if (texVerts >= 3) { std::lock_guard<std::mutex> lk(g_texGeomMtx);
            EnsureTexGeoVB(g_texGeom.size()*sizeof(float));
            if (g_texGeoVBMap) memcpy(g_texGeoVBMap, g_texGeom.data(), g_texGeom.size()*sizeof(float)); }
        // disk-resource path (REX_TEXTEST): one-time texture upload + textured-quad VB, BEFORE the render pass
        // (the upload uses its own command buffer + a blocking wait, which must not be nested in g_cmd's pass).
        if ((g_textest || g_hudsheet || g_menulive) && g_texPipe && !g_texVB) {
            const char* texfile = getenv("REX_TEXFILE");   // a real .png to load; else the procedural checker
            // cont.37: REX_HUDSHEET decodes the title's real HUD DDS container into a contact sheet; cont.60:
            // REX_MENULIVE composites the live-decoded menu textures (working buffers); else REX_TEXFILE/checker.
            if (g_menulive ? LoadMenuSheetLive() : g_hudsheet ? LoadHudSheetOnce() : (texfile ? LoadPNGToTexture(texfile) : CreateTestTexture())) {
                const float fs = (g_hudsheet || g_menulive) ? 0.98f : 0.9f, lo = (g_hudsheet || g_menulive) ? -0.98f : 0.1f;
                const float tq[] = {                                 // pos.xy + uv.xy, 2 tris, clip-space quad
                    lo,lo, 0.0f,0.0f,  fs,lo, 1.0f,0.0f,  fs,fs, 1.0f,1.0f,
                    lo,lo, 0.0f,0.0f,  fs,fs, 1.0f,1.0f,  lo,fs, 0.0f,1.0f };
                VkBufferCreateInfo bvi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bvi.size = sizeof(tq); bvi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bvi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
                if (vkCreateBuffer(g_device, &bvi, nullptr, &g_texVB) == VK_SUCCESS) {
                    VkMemoryRequirements vmr; vkGetBufferMemoryRequirements(g_device, g_texVB, &vmr);
                    VkMemoryAllocateInfo vai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
                    vai.allocationSize = vmr.size; vai.memoryTypeIndex = FindMemoryType(vmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
                    vkAllocateMemory(g_device, &vai, nullptr, &g_texVBMem);
                    vkBindBufferMemory(g_device, g_texVB, g_texVBMem, 0);
                    vkMapMemory(g_device, g_texVBMem, 0, sizeof(tq), 0, &g_texVBMap);
                    memcpy(g_texVBMap, tq, sizeof(tq));
                }
            }
        }
        VkClearValue cv{}; cv.color.float32[0]=0.06f; cv.color.float32[1]=0.06f; cv.color.float32[2]=0.10f; cv.color.float32[3]=1.0f;
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = g_renderPass; rp.framebuffer = g_framebuffers[idx];
        rp.renderArea.extent = g_extent; rp.clearValueCount = 1; rp.pClearValues = &cv;
        vkCmdBeginRenderPass(g_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp{0.0f, 0.0f, (float)g_extent.width, (float)g_extent.height, 0.0f, 1.0f};
        VkRect2D scs{{0, 0}, g_extent};
        vkCmdSetViewport(g_cmd, 0, 1, &vp); vkCmdSetScissor(g_cmd, 0, 1, &scs);   // dynamic state persists across pipeline binds
        // Task #8 (disk-resource path): draw the TEXTURED backdrop FIRST (under the UI). The submitted
        // backdrop quadrants go through the textured pipeline + the disk-loaded background .png; their
        // synthetic screen→[0,1] UVs reassemble the full 1280×720 background across the 4 quadrants.
        if (g_menutex && g_texPipe && g_texSet && g_bgLoaded && texVerts >= 3 && g_texGeoVB) {
            vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_texPipe);
            VkDeviceSize gto = 0; vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_texGeoVB, &gto);
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_texLayout, 0, 1, &g_texSet, 0, nullptr);
            MenuPC gpc{}; for (int i = 0; i < 16; i++) gpc.mvp[i] = (i % 5 == 0) ? 1.0f : 0.0f;   // identity (clip-space verts)
            gpc.color[0]=gpc.color[1]=gpc.color[2]=gpc.color[3]=1.0f;                              // white tint (texture as-is)
            vkCmdPushConstants(g_cmd, g_texLayout, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gpc), &gpc);
            vkCmdDraw(g_cmd, texVerts, 1, 0, 0);
        }
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_menuPipe);
        VkDeviceSize voff = 0; vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_menuVB, &voff);
        MenuPC pc{}; for (int i = 0; i < 16; i++) pc.mvp[i] = (i % 5 == 0) ? 1.0f : 0.0f;   // identity (verts already clip-space)
        if (subVerts > 0) {                                      // Layer 2: draw the real menu geometry, per-quad colors
            // Draw in 6-vert (1 quad = 2 tris) chunks with a cycling palette + alpha, so individual UI
            // rectangles are distinguishable (a flat color makes overlaps an unreadable soup).
            static const float pal[6][3] = {{0.95f,0.35f,0.35f},{0.35f,0.85f,0.40f},{0.40f,0.55f,0.95f},
                                            {0.95f,0.85f,0.35f},{0.85f,0.45f,0.90f},{0.40f,0.90f,0.90f}};
            for (int q = 0; q*6 + 6 <= subVerts; q++) {
                pc.color[0]=pal[q%6][0]; pc.color[1]=pal[q%6][1]; pc.color[2]=pal[q%6][2]; pc.color[3]=0.55f;
                vkCmdPushConstants(g_cmd, g_menuLayout, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                vkCmdDraw(g_cmd, 6, 1, q * 6, 0);
            }
        } else if (!g_hudsheet) {                                // REX_MENUTEST: hardcoded validation rects
            const float cols[3][4] = {{1.0f,0.2f,0.2f,0.6f},{0.2f,1.0f,0.2f,0.6f},{0.3f,0.4f,1.0f,0.8f}};
            for (int q = 0; q < 3; q++) { memcpy(pc.color, cols[q], 16);
                vkCmdPushConstants(g_cmd, g_menuLayout, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
                vkCmdDraw(g_cmd, 6, 1, q * 6, 0); }
        }
        // disk-resource path (REX_TEXTEST / cont.37 REX_HUDSHEET): draw the textured quad — validates the
        // textured pipeline end to end (pos+uv input + sampler2D descriptor + SPTextured FS = tex(diffuse,uv)*color).
        if ((g_textest || g_hudsheet || g_menulive) && g_texPipe && g_texSet && g_texVB) {
            vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_texPipe);
            VkDeviceSize to = 0; vkCmdBindVertexBuffers(g_cmd, 0, 1, &g_texVB, &to);
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_texLayout, 0, 1, &g_texSet, 0, nullptr);
            MenuPC tpc{}; for (int i = 0; i < 16; i++) tpc.mvp[i] = (i % 5 == 0) ? 1.0f : 0.0f;   // identity (clip-space verts)
            tpc.color[0] = tpc.color[1] = tpc.color[2] = tpc.color[3] = 1.0f;                     // white tint
            vkCmdPushConstants(g_cmd, g_texLayout, VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tpc), &tpc);
            vkCmdDraw(g_cmd, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(g_cmd);
        static int s_maxCap = 0;   // capture the RICHEST frame seen (most submitted verts) — steady-state menu
        bool richer = (subVerts + texVerts) > s_maxCap;          // incl. Task #8 textured backdrop verts
        bool cap = (g_shotTarget && g_frame == g_shotTarget) || richer;
        if (richer) s_maxCap = subVerts + texVerts;
        if (cap) { EnsureCaptureBuffer();
            if (g_capBuf) {
                ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferImageCopy rgn{}; rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                rgn.imageExtent = {g_extent.width, g_extent.height, 1};
                vkCmdCopyImageToBuffer(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_capBuf, 1, &rgn);
                ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0);
            } else cap = false; }
        vkEndCommandBuffer(g_cmd);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_acquireSem; si.pWaitDstStageMask = &ws;
        si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_presentSem;
        vkQueueSubmit(g_queue, 1, &si, g_fence);
        if (cap) { vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX); WriteCapturePPM("/tmp/varianta_menu.ppm"); }
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_presentSem;
        pi.swapchainCount = 1; pi.pSwapchains = &g_swapchain; pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(g_queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(g_device); CreateSwapchain(); }
        if ((g_frame % 300) == 0) fprintf(stderr, "[render] GPU-build: menu-quad test frame %llu\n", (unsigned long long)g_frame);
        g_frame++;
        return true;
    }

    if (g_drawtest && g_testPipe && idx < g_framebuffers.size()) {
        // GPU build (piece 1): render the test triangle via the graphics pipeline. The render pass takes the
        // image UNDEFINED -> PRESENT_SRC, so no manual barriers. Proves render pass + pipeline + vkCmdDraw.
        VkClearValue cv{}; cv.color.float32[0]=0.06f; cv.color.float32[1]=0.06f; cv.color.float32[2]=0.10f; cv.color.float32[3]=1.0f;
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = g_renderPass; rp.framebuffer = g_framebuffers[idx];
        rp.renderArea.extent = g_extent; rp.clearValueCount = 1; rp.pClearValues = &cv;
        vkCmdBeginRenderPass(g_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_testPipe);
        VkViewport vp{0.0f, 0.0f, (float)g_extent.width, (float)g_extent.height, 0.0f, 1.0f};
        VkRect2D scs{{0, 0}, g_extent};
        vkCmdSetViewport(g_cmd, 0, 1, &vp); vkCmdSetScissor(g_cmd, 0, 1, &scs);
        vkCmdDraw(g_cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(g_cmd);
        bool cap = g_shotTarget && g_frame == g_shotTarget;   // REX_RENDER_SHOT=N -> capture frame N to PPM
        if (cap) {
            EnsureCaptureBuffer();
            if (g_capBuf) {
                ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
                VkBufferImageCopy rgn{};
                rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                rgn.imageExtent = {g_extent.width, g_extent.height, 1};
                vkCmdCopyImageToBuffer(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_capBuf, 1, &rgn);
                ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             VK_ACCESS_TRANSFER_READ_BIT, 0);
            } else cap = false;
        }
        vkEndCommandBuffer(g_cmd);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_acquireSem; si.pWaitDstStageMask = &ws;
        si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_presentSem;
        vkQueueSubmit(g_queue, 1, &si, g_fence);
        if (cap) { vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX); WriteCapturePPM("/tmp/varianta_shot.ppm"); }
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_presentSem;
        pi.swapchainCount = 1; pi.pSwapchains = &g_swapchain; pi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(g_queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(g_device); CreateSwapchain(); }
        if ((g_frame % 300) == 0) fprintf(stderr, "[render] GPU-build: test triangle frame %llu\n", (unsigned long long)g_frame);
        g_frame++;
        return true;
    }

    ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 0, VK_ACCESS_TRANSFER_WRITE_BIT);

    // Fill the swapchain image: the decoded intro movie if the decoder has produced a frame, else a
    // cycling clear color (increment 1 fallback — proves the present loop runs before any decode).
    EnsureVideoBuffer();
    bool haveVideo = g_vidBuf && PickAndFillVideo();   // builds BGRA from the freshest luma plane (CPU)
    if (haveVideo) g_vidFrame++;
    if (haveVideo) {
        VkBufferImageCopy rgn{};
        rgn.bufferRowLength = kVidW; rgn.bufferImageHeight = kVidH;
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        uint32_t cw = g_extent.width  < kVidW ? g_extent.width  : kVidW;
        uint32_t ch = g_extent.height < kVidH ? g_extent.height : kVidH;
        rgn.imageExtent = {cw, ch, 1};
        vkCmdCopyBufferToImage(g_cmd, g_vidBuf, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    } else {
        float t = (float)(g_frame % 180) / 180.0f;
        VkClearColorValue clear{};
        clear.float32[0] = 0.10f;
        clear.float32[1] = 0.10f + 0.40f * t;
        clear.float32[2] = 0.35f;
        clear.float32[3] = 1.0f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    }

    // Capture the shotTarget-th DECODED frame when video is available (decouples capture from the
    // render/decode timing race), else the shotTarget-th present (increment-1 fallback / no decode).
    bool capturing = g_shotTarget && (haveVideo ? (g_vidFrame == g_shotTarget)
                                                 : (g_vbufN.load() == 0 && g_frame == g_shotTarget));
    if (capturing) {
        // Diagnostic: also dump the exact guest buffer the render thread selected, so its layout can be
        // analyzed offline (distinguish a clean frame from a mid-overwrite/stride artifact).
        const uint32_t* bufs = g_vbufs.load();
        if (getenv("REX_RENDER_DUMPSEL") && bufs && g_vidLastSel >= 0 && g_base) {
            FILE* f = fopen("/tmp/selbuf.raw", "wb");
            if (f) { fwrite(g_base + bufs[g_vidLastSel], 1, 0x101440, f); fclose(f);
                     fprintf(stderr, "[render] dumped selected buf%d @0x%X -> /tmp/selbuf.raw\n", g_vidLastSel, bufs[g_vidLastSel]); }
        }
        EnsureCaptureBuffer();
        if (g_capBuf) {
            ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rgn.imageExtent = {g_extent.width, g_extent.height, 1};
            vkCmdCopyImageToBuffer(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_capBuf, 1, &rgn);
            ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT, 0);
        } else capturing = false;
    }
    if (!capturing) {
        ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                     VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    }
    vkEndCommandBuffer(g_cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_acquireSem; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_presentSem;
    vkQueueSubmit(g_queue, 1, &si, g_fence);
    if (capturing) { vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX); WriteCapturePPM("/tmp/varianta_shot.ppm"); }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_presentSem;
    pi.swapchainCount = 1; pi.pSwapchains = &g_swapchain; pi.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(g_queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(g_device); CreateSwapchain(); }

    if ((g_frame % 300) == 0)
        fprintf(stderr, "[render] presented frame %llu (guest swaps=%llu, video buf=%d/%d, distinct movie frames=%llu)\n",
                (unsigned long long)g_frame, (unsigned long long)g_swaps.load(), g_vidLastSel, g_vbufN.load(),
                (unsigned long long)g_vidDistinct);
    g_frame++;
    return true;
}

void RenderThreadMain() {
    if (!Init()) { fprintf(stderr, "[render] init failed — renderer disabled.\n"); g_running = false; return; }
    while (g_running.load()) {
        PumpEvents();
        if (!PresentOnce()) break;   // FIFO present paces this loop to vsync (~60Hz)
    }
    vkDeviceWaitIdle(g_device);
}

} // namespace

bool Enabled() { return g_enabled; }

void Present(uint32_t frontBufferGuestAddr) {
    if (!g_enabled) return;
    g_frontBuffer.store(frontBufferGuestAddr);   // published for increment 2 (present the real framebuffer)
    g_swaps.fetch_add(1);
    // Lazily start the dedicated render thread on the first VdSwap. The guest never blocks on present.
    bool expected = false;
    if (g_started.compare_exchange_strong(expected, true)) {
        g_running = true;
        g_thread = std::thread(RenderThreadMain);
        g_thread.detach();
    }
}

void PublishVideo(const uint32_t* guestBufAddrs, const uint32_t* guestBufSizes, int count) {
    if (!g_enabled) return;
    g_vbufs.store(guestBufAddrs);    // points at kernel.cpp's g_videoBufs[] (stable once captured)
    g_vsizes.store(guestBufSizes);   // and g_videoBufSz[]
    g_vbufN.store(count);
}

void SubmitMenuGeometry(const float* clipXY, int vertCount) {
    if (!g_enabled || vertCount <= 0) return;
    std::lock_guard<std::mutex> lk(g_menuGeomMtx);
    g_menuGeom.assign(clipXY, clipXY + (size_t)vertCount * 2);
    g_menuGeomVerts.store(vertCount);
    fprintf(stderr, "[render] GPU-build: menu geometry submitted (%d verts)\n", vertCount);
}

void SubmitTexturedGeometry(const float* posUV, int vertCount) {
    if (!g_enabled || vertCount <= 0) return;
    std::lock_guard<std::mutex> lk(g_texGeomMtx);
    g_texGeom.assign(posUV, posUV + (size_t)vertCount * 4);   // pos.xy + uv.xy per vertex
    g_texGeomVerts.store(vertCount);
    static int s_logged = 0;
    if (s_logged++ < 3) fprintf(stderr, "[render] disk-resource: textured geometry submitted (%d verts)\n", vertCount);
}

// cont.60: composite a live-decoded per-draw menu texture into the contact sheet (see rex_render.h).
void BlitMenuCell(const uint8_t* rgba, int w, int h, uint32_t base) {
    if (!g_menulive || !rgba || w <= 0 || h <= 0) return;
    std::lock_guard<std::mutex> lk(g_menuSheetMtx);
    if (g_menuSheet.empty()) {                                  // lazy-init: dark-grey background
        g_menuSheet.assign((size_t)MS_W * MS_H * 4, 0);
        for (size_t i = 0; i < g_menuSheet.size(); i += 4) { g_menuSheet[i]=g_menuSheet[i+1]=g_menuSheet[i+2]=30; g_menuSheet[i+3]=255; }
    }
    int cell = -1, n = g_menuCells.load();
    for (int i = 0; i < n; i++) if (g_menuBases[i] == base) { cell = i; break; }
    if (cell < 0) { if (n >= 8) return; g_menuBases[n] = base; cell = n; g_menuCells.store(n + 1); }
    int cx = (cell % MS_COLS) * MS_CELL, cy = (cell / MS_COLS) * MS_CELL;
    float s = std::min((float)MS_CELL / (float)w, (float)MS_CELL / (float)h);
    int dw = (int)(w * s), dh = (int)(h * s);
    for (int yy = 0; yy < dh; yy++) for (int xx = 0; xx < dw; xx++) {
        int sx = (int)(xx / s), sy = (int)(yy / s); if (sx >= w || sy >= h) continue;
        const uint8_t* sp = &rgba[((size_t)sy * w + sx) * 4];
        uint8_t* dp = &g_menuSheet[((size_t)(cy + yy) * MS_W + (cx + xx)) * 4];
        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2]; dp[3] = 255;
    }
}

} // namespace rex_render
