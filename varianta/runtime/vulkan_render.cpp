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

#define VKCHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "[render] %s = %d\n", #x, (int)_r); return false; } } while (0)

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

    // Increment 1: cycling clear color so the window visibly updates (proves the present loop runs).
    float t = (float)(g_frame % 180) / 180.0f;
    VkClearColorValue clear{};
    clear.float32[0] = 0.10f;
    clear.float32[1] = 0.10f + 0.40f * t;
    clear.float32[2] = 0.35f;
    clear.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    ImageBarrier(g_cmd, g_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                 VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    vkEndCommandBuffer(g_cmd);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_acquireSem; si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1; si.pCommandBuffers = &g_cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &g_presentSem;
    vkQueueSubmit(g_queue, 1, &si, g_fence);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &g_presentSem;
    pi.swapchainCount = 1; pi.pSwapchains = &g_swapchain; pi.pImageIndices = &idx;
    VkResult pr = vkQueuePresentKHR(g_queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(g_device); CreateSwapchain(); }

    if ((g_frame % 300) == 0)
        fprintf(stderr, "[render] presented frame %llu (guest swaps=%llu)\n",
                (unsigned long long)g_frame, (unsigned long long)g_swaps.load());
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

} // namespace rex_render
