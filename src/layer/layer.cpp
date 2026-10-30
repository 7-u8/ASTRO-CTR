// ASTRO CTR Vulkan layer (VK_LAYER_ASTRO_ctr). Loaded by the Vulkan loader because the ASTRO launcher sets
// VK_ADD_LAYER_PATH + VK_INSTANCE_LAYERS: nothing is injected and no game file is changed. The layer only adds
// one render pass on top of each presented swapchain image to draw the ASTRO menu. In any process other than
// sacredfire.exe every call is passed straight through.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../diagnostics/log.h"
#include "overlay.h"

#define LAYER_EXPORT extern "C" __declspec(dllexport)

namespace astro {

namespace {

using Key = void*;
template <typename T> Key KeyOf(T h) { return *reinterpret_cast<void**>(h); }

struct InstData {
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkCreateWin32SurfaceKHR CreateWin32SurfaceKHR = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetQueueFamilyProps = nullptr;
};

std::mutex g_mu;
std::unordered_map<Key, InstData*> g_inst;            // instance / physical device key -> data
std::unordered_map<Key, DeviceFns*> g_dev;            // device / queue / command buffer key -> data
std::unordered_map<VkQueue, uint32_t> g_queue_family;
std::unordered_map<VkSwapchainKHR, VkDevice> g_swap_dev;
HWND g_hwnd = nullptr;
bool g_active = false;   // true only inside sacredfire.exe

bool IsHostProcess() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return _wcsicmp(name, L"sacredfire.exe") == 0;
}

// The ASTRO window holds this mutex; without it the layer passes everything through (leftover registration).
bool WatcherRunning() {
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Local\\ASTRO_CTR_Watcher");
    if (!m) return false;
    CloseHandle(m);
    return true;
}

InstData* FindInst(Key k) { std::lock_guard<std::mutex> l(g_mu); auto it = g_inst.find(k); return it == g_inst.end() ? nullptr : it->second; }
DeviceFns* FindDev(Key k) { std::lock_guard<std::mutex> l(g_mu); auto it = g_dev.find(k); return it == g_dev.end() ? nullptr : it->second; }

template <typename T> T* ChainInfo(const void* pnext, VkStructureType type, VkLayerFunction fn) {
    auto* p = static_cast<T*>(const_cast<void*>(pnext));
    while (p && !(p->sType == type && p->function == fn)) p = static_cast<T*>(const_cast<void*>(p->pNext));
    return p;
}

// ---------------- instance ----------------

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkInstance* out) {
    auto* link = ChainInfo<VkLayerInstanceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO);
    if (!link) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    VkResult r = create(ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    auto* d = new InstData;
    d->inst = *out;
    d->gipa = gipa;
    d->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*out, "vkDestroyInstance"));
    d->CreateWin32SurfaceKHR = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(gipa(*out, "vkCreateWin32SurfaceKHR"));
    d->GetQueueFamilyProps = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(gipa(*out, "vkGetPhysicalDeviceQueueFamilyProperties"));
    { std::lock_guard<std::mutex> l(g_mu); g_inst[KeyOf(*out)] = d; }
    if (g_active) {
        Overlay_EnsureLog();
        Logger::Instance().Info(std::string("vkCreateInstance ok, app '") +
                                (ci->pApplicationInfo && ci->pApplicationInfo->pApplicationName ? ci->pApplicationInfo->pApplicationName : "?") + "'");
        Overlay_SignalLayerLoaded();
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance inst, const VkAllocationCallbacks* alloc) {
    InstData* d = FindInst(KeyOf(inst));
    if (!d) return;
    { std::lock_guard<std::mutex> l(g_mu); g_inst.erase(KeyOf(inst)); }
    d->DestroyInstance(inst, alloc);
    delete d;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateWin32SurfaceKHR(VkInstance inst, const VkWin32SurfaceCreateInfoKHR* ci, const VkAllocationCallbacks* alloc, VkSurfaceKHR* out) {
    InstData* d = FindInst(KeyOf(inst));
    VkResult r = d->CreateWin32SurfaceKHR(inst, ci, alloc, out);
    if (r == VK_SUCCESS && g_active) {
        g_hwnd = ci->hwnd;
        Logger::Instance().Info("game window found (vkCreateWin32SurfaceKHR)");
    }
    return r;
}

// ---------------- device ----------------

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, const VkAllocationCallbacks* alloc, VkDevice* out) {
    InstData* inst = FindInst(KeyOf(pd));
    auto* link = ChainInfo<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);
    auto* cb = ChainInfo<VkLayerDeviceCreateInfo>(ci->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LOADER_DATA_CALLBACK);
    if (!link || !inst) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;
    auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->inst, "vkCreateDevice"));
    VkResult r = create(pd, ci, alloc, out);
    if (r != VK_SUCCESS) return r;

    auto* d = new DeviceFns;
    d->device = *out;
    d->physical = pd;
    d->instance = inst->inst;
    d->gipa = inst->gipa;
    d->gdpa = gdpa;
    d->SetDeviceLoaderData = cb ? cb->u.pfnSetDeviceLoaderData : nullptr;
#define LOAD(name) d->name = reinterpret_cast<PFN_vk##name>(gdpa(*out, "vk" #name))
    LOAD(DestroyDevice); LOAD(GetDeviceQueue); LOAD(GetDeviceQueue2); LOAD(DeviceWaitIdle);
    LOAD(CreateSwapchainKHR); LOAD(DestroySwapchainKHR); LOAD(GetSwapchainImagesKHR); LOAD(QueuePresentKHR); LOAD(QueueSubmit);
    LOAD(CreateImageView); LOAD(DestroyImageView); LOAD(CreateFramebuffer); LOAD(DestroyFramebuffer);
    LOAD(CreateRenderPass); LOAD(DestroyRenderPass); LOAD(CreateCommandPool); LOAD(DestroyCommandPool);
    LOAD(AllocateCommandBuffers); LOAD(FreeCommandBuffers); LOAD(BeginCommandBuffer); LOAD(EndCommandBuffer);
    LOAD(ResetCommandBuffer); LOAD(CmdBeginRenderPass); LOAD(CmdEndRenderPass);
    LOAD(CreateFence); LOAD(DestroyFence); LOAD(WaitForFences); LOAD(ResetFences);
    LOAD(CreateSemaphore); LOAD(DestroySemaphore);
#undef LOAD
    // queue families with graphics support (the overlay only draws on a graphics-capable present queue)
    uint32_t n = 0;
    inst->GetQueueFamilyProps(pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> fams(n);
    inst->GetQueueFamilyProps(pd, &n, fams.data());
    for (uint32_t i = 0; i < n; ++i) d->graphics_family.push_back((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0);
    { std::lock_guard<std::mutex> l(g_mu); g_dev[KeyOf(*out)] = d; }
    if (g_active) Logger::Instance().Info("vkCreateDevice ok (" + std::to_string(n) + " queue families)");
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice dev, const VkAllocationCallbacks* alloc) {
    DeviceFns* d = FindDev(KeyOf(dev));
    if (!d) return;
    if (g_active) Overlay_OnDeviceDestroyed(d);
    { std::lock_guard<std::mutex> l(g_mu); g_dev.erase(KeyOf(dev)); }
    d->DestroyDevice(dev, alloc);
    delete d;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice dev, uint32_t family, uint32_t index, VkQueue* out) {
    DeviceFns* d = FindDev(KeyOf(dev));
    d->GetDeviceQueue(dev, family, index, out);
    std::lock_guard<std::mutex> l(g_mu);
    g_queue_family[*out] = family;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2* info, VkQueue* out) {
    DeviceFns* d = FindDev(KeyOf(dev));
    d->GetDeviceQueue2(dev, info, out);
    std::lock_guard<std::mutex> l(g_mu);
    g_queue_family[*out] = info->queueFamilyIndex;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR* ci, const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
    DeviceFns* d = FindDev(KeyOf(dev));
    VkSwapchainCreateInfoKHR info = *ci;
    if (g_active) info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;   // always supported for swapchain images
    VkResult r = d->CreateSwapchainKHR(dev, &info, alloc, out);
    if (r == VK_SUCCESS && g_active) {
        { std::lock_guard<std::mutex> l(g_mu); g_swap_dev[*out] = dev; }
        Overlay_OnSwapchainCreated(d, *out, info);
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks* alloc) {
    DeviceFns* d = FindDev(KeyOf(dev));
    if (g_active && sc) {
        Overlay_OnSwapchainDestroyed(d, sc);
        std::lock_guard<std::mutex> l(g_mu);
        g_swap_dev.erase(sc);
    }
    d->DestroySwapchainKHR(dev, sc, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    DeviceFns* d = FindDev(KeyOf(queue));
    if (!g_active || info->swapchainCount != 1) return d->QueuePresentKHR(queue, info);
    uint32_t family = UINT32_MAX;
    {
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_queue_family.find(queue);
        if (it != g_queue_family.end()) family = it->second;
    }
    VkSemaphore done = VK_NULL_HANDLE;
    if (family < d->graphics_family.size() && d->graphics_family[family])
        done = Overlay_OnPresent(d, queue, family, g_hwnd, info->pSwapchains[0], info->pImageIndices[0],
                                 info->waitSemaphoreCount, info->pWaitSemaphores);
    if (done == VK_NULL_HANDLE) return d->QueuePresentKHR(queue, info);
    VkPresentInfoKHR p = *info;
    p.waitSemaphoreCount = 1;
    p.pWaitSemaphores = &done;
    return d->QueuePresentKHR(queue, &p);
}

// ---------------- proc addr ----------------

PFN_vkVoidFunction Intercept(const char* n) {
#define HOOK(name) if (!strcmp(n, "vk" #name)) return reinterpret_cast<PFN_vkVoidFunction>(&name)
    HOOK(CreateDevice); HOOK(DestroyDevice); HOOK(GetDeviceQueue); HOOK(GetDeviceQueue2);
    HOOK(CreateSwapchainKHR); HOOK(DestroySwapchainKHR); HOOK(QueuePresentKHR);
#undef HOOK
    return nullptr;
}

}  // namespace

// vkAllocateCommandBuffers for command buffers the layer itself creates (ours and ImGui's): they need the
// loader's dispatch pointer, or a layer below ours would not find its own data for them.
VKAPI_ATTR VkResult VKAPI_CALL LayerAllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo* ai, VkCommandBuffer* out) {
    DeviceFns* d = FindDev(KeyOf(dev));
    VkResult r = d->AllocateCommandBuffers(dev, ai, out);
    if (r == VK_SUCCESS && d->SetDeviceLoaderData)
        for (uint32_t i = 0; i < ai->commandBufferCount; ++i) d->SetDeviceLoaderData(dev, out[i]);
    return r;
}

}  // namespace astro

using namespace astro;

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL astro_GetDeviceProcAddr(VkDevice dev, const char* n);

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL astro_GetInstanceProcAddr(VkInstance inst, const char* n) {
    if (!strcmp(n, "vkGetInstanceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&astro_GetInstanceProcAddr);
    if (!strcmp(n, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&astro_GetDeviceProcAddr);
    if (!strcmp(n, "vkCreateInstance")) return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
    if (!strcmp(n, "vkDestroyInstance")) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance);
    if (!strcmp(n, "vkCreateWin32SurfaceKHR")) return reinterpret_cast<PFN_vkVoidFunction>(&CreateWin32SurfaceKHR);
    if (auto f = Intercept(n)) return f;
    if (!inst) return nullptr;
    InstData* d = FindInst(KeyOf(inst));
    return d ? d->gipa(inst, n) : nullptr;
}

LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL astro_GetDeviceProcAddr(VkDevice dev, const char* n) {
    if (!strcmp(n, "vkGetDeviceProcAddr")) return reinterpret_cast<PFN_vkVoidFunction>(&astro_GetDeviceProcAddr);
    if (auto f = Intercept(n)) return f;
    DeviceFns* d = FindDev(KeyOf(dev));
    return d ? d->gdpa(dev, n) : nullptr;
}

LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL astro_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = &astro_GetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = &astro_GetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_active = IsHostProcess() && WatcherRunning();
    }
    return TRUE;
}
