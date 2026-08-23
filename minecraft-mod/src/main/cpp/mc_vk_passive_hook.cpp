#include "mc_vk_passive_hook.hpp"

#include <dlfcn.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define LOG_TAG "LSFG-VK"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Function pointer typedefs
using PFN_vkCreateSwapchainKHR = VkResult(VKAPI_PTR *)(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain);

using PFN_vkGetSwapchainImagesKHR = VkResult(VKAPI_PTR *)(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t *pSwapchainImageCount,
    VkImage *pSwapchainImages);

using PFN_vkAcquireNextImageKHR = VkResult(VKAPI_PTR *)(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t *pImageIndex);

using PFN_vkAcquireNextImage2KHR = VkResult(VKAPI_PTR *)(
    VkDevice device,
    const VkAcquireNextImageInfoKHR *pAcquireInfo,
    uint32_t *pImageIndex);

using PFN_vkQueuePresentKHR = VkResult(VKAPI_PTR *)(
    VkQueue queue,
    const VkPresentInfoKHR *pPresentInfo);

// Real Vulkan pointers
void *g_vulkanLibHandle = nullptr;
PFN_vkGetInstanceProcAddr g_realGipa = nullptr;
PFN_vkGetDeviceProcAddr g_realGdpa = nullptr;

PFN_vkCreateSwapchainKHR g_realCreateSwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR g_realGetSwapchainImagesKHR = nullptr;
PFN_vkAcquireNextImageKHR g_realAcquireNextImageKHR = nullptr;
PFN_vkAcquireNextImage2KHR g_realAcquireNextImage2KHR = nullptr;
PFN_vkQueuePresentKHR g_realQueuePresentKHR = nullptr;

// Diagnostic state flags & bounded frame counters
std::atomic<bool> g_vulkanObserved{false};
std::atomic<bool> g_gipaLogged{false};
std::atomic<bool> g_gdpaLogged{false};
std::atomic<bool> g_swapchainLogged{false};
std::atomic<int> g_acquireLogCount{0};
std::atomic<int> g_presentLogCount{0};

constexpr int kMaxFrameLogSamples = 10;

uint64_t get_time_ns() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
}

const char *present_mode_to_string(VkPresentModeKHR mode) {
    switch (mode) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default: return "OTHER";
    }
}

const char *format_to_string(VkFormat format) {
    switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        default: return "OTHER_FORMAT";
    }
}

void ensure_real_vulkan_loaded() {
    if (g_realGipa && g_realGdpa) return;

    // 1. Check if Amethyst exported VULKAN_PTR in environment
    const char *vulkanPtrEnv = std::getenv("VULKAN_PTR");
    if (vulkanPtrEnv != nullptr && *vulkanPtrEnv != '\0') {
        char *end = nullptr;
        unsigned long ptrVal = std::strtoul(vulkanPtrEnv, &end, 16);
        if (ptrVal != 0) {
            g_vulkanLibHandle = reinterpret_cast<void *>(ptrVal);
            LOGI("[LSFG/VK] Discovered Amethyst VULKAN_PTR: %p", g_vulkanLibHandle);
        }
    }

    // 2. If not found or null, open libvulkan.so directly
    if (g_vulkanLibHandle == nullptr) {
        g_vulkanLibHandle = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        LOGI("[LSFG/VK] Opened libvulkan.so: %p", g_vulkanLibHandle);
    }

    if (g_vulkanLibHandle != nullptr) {
        g_realGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(g_vulkanLibHandle, "vkGetInstanceProcAddr"));
        g_realGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            dlsym(g_vulkanLibHandle, "vkGetDeviceProcAddr"));
    }

    // Fallback to RTLD_DEFAULT if still null
    if (g_realGipa == nullptr) {
        g_realGipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
            dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr"));
    }
    if (g_realGdpa == nullptr) {
        g_realGdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
            dlsym(RTLD_DEFAULT, "vkGetDeviceProcAddr"));
    }

    LOGI("[LSFG/VK] Real GIPA=%p, Real GDPA=%p", g_realGipa, g_realGdpa);
}

// -----------------------------------------------------------------------------
// Passive Pass-Through Vulkan Wrappers
// -----------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL lsfg_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain)
{
    g_vulkanObserved.store(true, std::memory_order_relaxed);

    if (pCreateInfo != nullptr && !g_swapchainLogged.exchange(true)) {
        LOGI("[LSFG/VK] vkCreateSwapchainKHR observed:");
        LOGI("[LSFG/VK]   requested minImageCount: %u", pCreateInfo->minImageCount);
        LOGI("[LSFG/VK]   imageFormat: %s (%d)", format_to_string(pCreateInfo->imageFormat), pCreateInfo->imageFormat);
        LOGI("[LSFG/VK]   extent: %ux%u", pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height);
        LOGI("[LSFG/VK]   presentMode: %s (%d)", present_mode_to_string(pCreateInfo->presentMode), pCreateInfo->presentMode);
        LOGI("[LSFG/VK]   imageUsage: 0x%x", pCreateInfo->imageUsage);
        LOGI("[LSFG/VK]   compositeAlpha: 0x%x", pCreateInfo->compositeAlpha);
        LOGI("[LSFG/VK]   preTransform: 0x%x", pCreateInfo->preTransform);
    }

    ensure_real_vulkan_loaded();
    PFN_vkCreateSwapchainKHR realFunc = g_realCreateSwapchainKHR;
    if (realFunc == nullptr && g_realGdpa != nullptr) {
        realFunc = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
            g_realGdpa(device, "vkCreateSwapchainKHR"));
    }

    if (realFunc == nullptr) {
        LOGE("[LSFG/VK] Could not resolve real vkCreateSwapchainKHR!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // STRICT PASS-THROUGH: Forward create info UNCHANGED
    VkResult res = realFunc(device, pCreateInfo, pAllocator, pSwapchain);

    if (res == VK_SUCCESS && pSwapchain != nullptr && *pSwapchain != VK_NULL_HANDLE) {
        // Query actual swapchain image count once
        PFN_vkGetSwapchainImagesKHR getImages = g_realGetSwapchainImagesKHR;
        if (getImages == nullptr && g_realGdpa != nullptr) {
            getImages = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
                g_realGdpa(device, "vkGetSwapchainImagesKHR"));
        }
        if (getImages != nullptr) {
            uint32_t actualCount = 0;
            if (getImages(device, *pSwapchain, &actualCount, nullptr) == VK_SUCCESS) {
                LOGI("[LSFG/VK] Swapchain successfully created: handle=%p, actual allocated imageCount=%u",
                     *pSwapchain, actualCount);
            }
        }
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL lsfg_vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t *pImageIndex)
{
    g_vulkanObserved.store(true, std::memory_order_relaxed);

    ensure_real_vulkan_loaded();
    PFN_vkAcquireNextImageKHR realFunc = g_realAcquireNextImageKHR;
    if (realFunc == nullptr && g_realGdpa != nullptr) {
        realFunc = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
            g_realGdpa(device, "vkAcquireNextImageKHR"));
    }

    if (realFunc == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // STRICT PASS-THROUGH: Call real function unchanged
    VkResult res = realFunc(device, swapchain, timeout, semaphore, fence, pImageIndex);

    int count = g_acquireLogCount.fetch_add(1);
    if (count < kMaxFrameLogSamples) {
        uint32_t idx = (pImageIndex != nullptr) ? *pImageIndex : UINT32_MAX;
        LOGI("[LSFG/VK] AcquireNextImage #%d: swapchain=%p, imageIndex=%u, result=%d, timeNs=%" PRIu64,
             count + 1, swapchain, idx, res, get_time_ns());
        if (count + 1 == kMaxFrameLogSamples) {
            LOGI("[LSFG/VK] Reached %d acquire trace samples — stopping per-frame logging.", kMaxFrameLogSamples);
        }
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL lsfg_vkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR *pAcquireInfo,
    uint32_t *pImageIndex)
{
    g_vulkanObserved.store(true, std::memory_order_relaxed);

    ensure_real_vulkan_loaded();
    PFN_vkAcquireNextImage2KHR realFunc = g_realAcquireNextImage2KHR;
    if (realFunc == nullptr && g_realGdpa != nullptr) {
        realFunc = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
            g_realGdpa(device, "vkAcquireNextImage2KHR"));
    }

    if (realFunc == nullptr) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = realFunc(device, pAcquireInfo, pImageIndex);

    int count = g_acquireLogCount.fetch_add(1);
    if (count < kMaxFrameLogSamples) {
        uint32_t idx = (pImageIndex != nullptr) ? *pImageIndex : UINT32_MAX;
        VkSwapchainKHR sc = (pAcquireInfo != nullptr) ? pAcquireInfo->swapchain : VK_NULL_HANDLE;
        LOGI("[LSFG/VK] AcquireNextImage2 #%d: swapchain=%p, imageIndex=%u, result=%d, timeNs=%" PRIu64,
             count + 1, sc, idx, res, get_time_ns());
        if (count + 1 == kMaxFrameLogSamples) {
            LOGI("[LSFG/VK] Reached %d acquire trace samples — stopping per-frame logging.", kMaxFrameLogSamples);
        }
    }

    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL lsfg_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR *pPresentInfo)
{
    g_vulkanObserved.store(true, std::memory_order_relaxed);

    ensure_real_vulkan_loaded();
    PFN_vkQueuePresentKHR realFunc = g_realQueuePresentKHR;
    if (realFunc == nullptr && g_realGipa != nullptr) {
        // Fallback resolution
        realFunc = reinterpret_cast<PFN_vkQueuePresentKHR>(
            g_realGipa(VK_NULL_HANDLE, "vkQueuePresentKHR"));
    }

    int count = g_presentLogCount.fetch_add(1);
    if (count < kMaxFrameLogSamples) {
        uint32_t countImages = (pPresentInfo != nullptr) ? pPresentInfo->swapchainCount : 0;
        uint32_t firstIdx = (pPresentInfo != nullptr && pPresentInfo->pImageIndices != nullptr)
                                ? pPresentInfo->pImageIndices[0] : UINT32_MAX;
        LOGI("[LSFG/VK] QueuePresent #%d: queue=%p, swapchainCount=%u, imageIndex=%u, timeNs=%" PRIu64,
             count + 1, queue, countImages, firstIdx, get_time_ns());
        if (count + 1 == kMaxFrameLogSamples) {
            LOGI("[LSFG/VK] Reached %d present trace samples — stopping per-frame logging.", kMaxFrameLogSamples);
        }
    }

    if (realFunc == nullptr) {
        LOGE("[LSFG/VK] Could not resolve real vkQueuePresentKHR!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // STRICT PASS-THROUGH: Forward present info UNCHANGED
    return realFunc(queue, pPresentInfo);
}

} // namespace

namespace lsfg_mc {

void init_passive_vulkan_diagnostics() {
    ensure_real_vulkan_loaded();
    LOGI("[LSFG/VK] Passive Vulkan diagnostics layer initialized. Ready for Minecraft/Zink calls.");
}

bool is_vulkan_observed() {
    return g_vulkanObserved.load(std::memory_order_relaxed);
}

} // namespace lsfg_mc

// Forward declarations for entry points
extern "C" {
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lsfg_vkGetInstanceProcAddr(VkInstance instance, const char *pName);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lsfg_vkGetDeviceProcAddr(VkDevice device, const char *pName);
}

// -----------------------------------------------------------------------------
// Exported Vulkan Interception Entry Points
// -----------------------------------------------------------------------------

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lsfg_vkGetInstanceProcAddr(
    VkInstance instance,
    const char *pName)
{
    g_vulkanObserved.store(true, std::memory_order_relaxed);
    if (!g_gipaLogged.exchange(true)) {
        LOGI("[LSFG/VK] lsfg_vkGetInstanceProcAddr observed (called for %s)", pName ? pName : "<null>");
    }

    ensure_real_vulkan_loaded();

    if (pName == nullptr) return nullptr;

    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0 ||
        std::strcmp(pName, "lsfg_vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkGetInstanceProcAddr);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0 ||
        std::strcmp(pName, "lsfg_vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkGetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        LOGI("[LSFG/VK] Intercepted vkGetInstanceProcAddr -> vkCreateSwapchainKHR");
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkCreateSwapchainKHR);
    }
    if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0) {
        LOGI("[LSFG/VK] Intercepted vkGetInstanceProcAddr -> vkAcquireNextImageKHR");
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkAcquireNextImageKHR);
    }
    if (std::strcmp(pName, "vkAcquireNextImage2KHR") == 0) {
        LOGI("[LSFG/VK] Intercepted vkGetInstanceProcAddr -> vkAcquireNextImage2KHR");
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkAcquireNextImage2KHR);
    }
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) {
        LOGI("[LSFG/VK] Intercepted vkGetInstanceProcAddr -> vkQueuePresentKHR");
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkQueuePresentKHR);
    }

    if (g_realGipa != nullptr) {
        return g_realGipa(instance, pName);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL lsfg_vkGetDeviceProcAddr(
    VkDevice device,
    const char *pName)
{
    g_vulkanObserved.store(true, std::memory_order_relaxed);
    if (!g_gdpaLogged.exchange(true)) {
        LOGI("[LSFG/VK] lsfg_vkGetDeviceProcAddr observed (called for %s)", pName ? pName : "<null>");
    }

    ensure_real_vulkan_loaded();

    if (pName == nullptr) return nullptr;

    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0 ||
        std::strcmp(pName, "lsfg_vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkGetDeviceProcAddr);
    }
    if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) {
        LOGI("[LSFG/VK] Intercepted vkGetDeviceProcAddr -> vkCreateSwapchainKHR");
        if (g_realGdpa != nullptr && g_realCreateSwapchainKHR == nullptr) {
            g_realCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(
                g_realGdpa(device, "vkCreateSwapchainKHR"));
        }
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkCreateSwapchainKHR);
    }
    if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0) {
        if (g_realGdpa != nullptr && g_realGetSwapchainImagesKHR == nullptr) {
            g_realGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(
                g_realGdpa(device, "vkGetSwapchainImagesKHR"));
        }
    }
    if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0) {
        LOGI("[LSFG/VK] Intercepted vkGetDeviceProcAddr -> vkAcquireNextImageKHR");
        if (g_realGdpa != nullptr && g_realAcquireNextImageKHR == nullptr) {
            g_realAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(
                g_realGdpa(device, "vkAcquireNextImageKHR"));
        }
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkAcquireNextImageKHR);
    }
    if (std::strcmp(pName, "vkAcquireNextImage2KHR") == 0) {
        LOGI("[LSFG/VK] Intercepted vkGetDeviceProcAddr -> vkAcquireNextImage2KHR");
        if (g_realGdpa != nullptr && g_realAcquireNextImage2KHR == nullptr) {
            g_realAcquireNextImage2KHR = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(
                g_realGdpa(device, "vkAcquireNextImage2KHR"));
        }
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkAcquireNextImage2KHR);
    }
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) {
        LOGI("[LSFG/VK] Intercepted vkGetDeviceProcAddr -> vkQueuePresentKHR");
        if (g_realGdpa != nullptr && g_realQueuePresentKHR == nullptr) {
            g_realQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(
                g_realGdpa(device, "vkQueuePresentKHR"));
        }
        return reinterpret_cast<PFN_vkVoidFunction>(lsfg_vkQueuePresentKHR);
    }

    if (g_realGdpa != nullptr) {
        return g_realGdpa(device, pName);
    }
    return nullptr;
}

} // extern "C"
