#pragma once
// Overlay side of the layer: per-swapchain render resources, the ImGui context and the window hook.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <vector>

namespace astro {

// The next layer's / driver's device functions, captured in vkCreateDevice.
struct DeviceFns {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    PFN_vkSetDeviceLoaderData SetDeviceLoaderData = nullptr;
    std::vector<bool> graphics_family;
#define FN(name) PFN_vk##name name = nullptr
    FN(DestroyDevice); FN(GetDeviceQueue); FN(GetDeviceQueue2); FN(DeviceWaitIdle);
    FN(CreateSwapchainKHR); FN(DestroySwapchainKHR); FN(GetSwapchainImagesKHR); FN(QueuePresentKHR); FN(QueueSubmit);
    FN(CreateImageView); FN(DestroyImageView); FN(CreateFramebuffer); FN(DestroyFramebuffer);
    FN(CreateRenderPass); FN(DestroyRenderPass); FN(CreateCommandPool); FN(DestroyCommandPool);
    FN(AllocateCommandBuffers); FN(FreeCommandBuffers); FN(BeginCommandBuffer); FN(EndCommandBuffer);
    FN(ResetCommandBuffer); FN(CmdBeginRenderPass); FN(CmdEndRenderPass);
    FN(CreateFence); FN(DestroyFence); FN(WaitForFences); FN(ResetFences);
    FN(CreateSemaphore); FN(DestroySemaphore);
#undef FN
};

VKAPI_ATTR VkResult VKAPI_CALL LayerAllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out);

void Overlay_EnsureLog();
void Overlay_SignalLayerLoaded();
void Overlay_OnSwapchainCreated(DeviceFns* d, VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR& ci);
void Overlay_OnSwapchainDestroyed(DeviceFns* d, VkSwapchainKHR sc);
void Overlay_OnDeviceDestroyed(DeviceFns* d);
// Draws the overlay on the image about to be presented. Returns the semaphore the present must wait on,
// or VK_NULL_HANDLE when nothing was drawn (the original present info is then used unchanged).
VkSemaphore Overlay_OnPresent(DeviceFns* d, VkQueue queue, uint32_t family, HWND hwnd, VkSwapchainKHR sc, uint32_t image,
                              uint32_t wait_count, const VkSemaphore* waits);

// Shared with the menu (diagnostics tab).
struct OverlayDiag {
    bool hook_installed = false;
    unsigned swap_w = 0, swap_h = 0;
    int swap_format = 0;
    unsigned long long frames = 0;
};

}  // namespace astro
