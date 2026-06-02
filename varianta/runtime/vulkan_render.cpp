// Variant A — minimal from-scratch Vulkan renderer. INCREMENT 1: SDL window + Vulkan swapchain +
// clear-and-present. No PM4 translation yet — this establishes the host display/present pipeline that
// draws/shaders build on. Gated behind REX_RENDER=1.
//
// Architecture: a dedicated host RENDER THREAD owns the SDL window + all Vulkan objects and runs the
// present loop at vsync. The guest's VdSwap only publishes the current front-buffer address (atomic) and
// lazily starts the thread — it NEVER calls Vulkan/SDL or blocks on present, so a slow/stalled compositor
// can't stall the guest. Single frame-in-flight, FIFO present, clear via vkCmdClearColorImage.
#include "rex_render.h"
#include <vulkan/vulkan.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>

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
    return true;
}

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

} // namespace rex_render
