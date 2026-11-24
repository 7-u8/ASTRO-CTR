// Overlay rendering: one render pass (load, draw ImGui, store) per presented image, submitted on the game's
// present queue between the game's wait semaphores and the present itself.
#include "overlay.h"

#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"

#include "../config/config.h"
#include "../diagnostics/log.h"
#include "menu.h"
#include "gamepad_view.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace astro {

namespace {

constexpr UINT kToggleKey = VK_F8;          // Insert is the host's own (bbport) menu
constexpr double kToastSeconds = 8.0;

struct Swap {
    DeviceFns* d = nullptr;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t min_images = 2;
    bool built = false, failed = false;
    VkRenderPass rp = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> fbs;
    std::vector<VkCommandBuffer> cbs;
    std::vector<VkFence> fences;
    std::vector<VkSemaphore> sems;
};

std::recursive_mutex g_mu;                          // ImGui state: render thread + window thread
std::unordered_map<VkSwapchainKHR, Swap*> g_swaps;
DeviceFns* g_imgui_dev = nullptr;                   // device the ImGui Vulkan backend is bound to
VkFormat g_imgui_format = VK_FORMAT_UNDEFINED;
bool g_ctx = false;
HWND g_hooked = nullptr;
WNDPROC g_old_proc = nullptr;
bool g_menu_open = false;
bool g_ready_signalled = false;
double g_toast_until = 0.0;
bool g_prev_key = false;
OverlayDiag g_diag;

double Now() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

void SignalNamed(const wchar_t* name) {
    HANDLE e = CreateEventW(nullptr, TRUE, FALSE, name);
    if (e) { ::SetEvent(e); /* kept open on purpose: the event lives as long as the game */ }
}

void WriteStatus(const std::string& line) {
    std::FILE* f = _wfopen((DefaultConfigPath().parent_path() / "status.txt").wstring().c_str(), L"a");
    if (!f) return;
    std::fprintf(f, "%lu %s\n", GetCurrentProcessId(), line.c_str());
    std::fclose(f);
}

bool IsInputMessage(UINT m) {
    return (m >= WM_KEYFIRST && m <= WM_KEYLAST) || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_INPUT;
}

LRESULT CALLBACK HookProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if ((m == WM_KEYDOWN || m == WM_SYSKEYDOWN) && w == kToggleKey) {
        if (!(l & (1 << 30))) {   // ignore auto-repeat
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            g_menu_open = !g_menu_open;
            Logger::Instance().Info(g_menu_open ? "menu opened" : "menu closed");
        }
        return 0;
    }
    if (g_menu_open) {
        {
            std::lock_guard<std::recursive_mutex> lk(g_mu);
            if (g_ctx) ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        }
        if (IsInputMessage(m)) return 0;   // the game does not see keyboard/mouse while the menu is open
    }
    return CallWindowProcW(g_old_proc, h, m, w, l);
}

void InstallHook(HWND hwnd) {
    if (g_hooked == hwnd) return;
    g_old_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookProc)));
    if (g_old_proc) {
        g_hooked = hwnd;
        g_diag.hook_installed = true;
        Logger::Instance().Info("window input hook installed (toggle key F8)");
    } else {
        Logger::Instance().Warn("could not hook the game window (error " + std::to_string(GetLastError()) + "); F8 is polled instead");
        g_hooked = hwnd;   // do not retry every frame
    }
}

PFN_vkVoidFunction ImGuiLoader(const char* name, void* user) {
    auto* d = static_cast<DeviceFns*>(user);
    if (!strcmp(name, "vkAllocateCommandBuffers")) return reinterpret_cast<PFN_vkVoidFunction>(&LayerAllocateCommandBuffers);
    PFN_vkVoidFunction f = d->gdpa(d->device, name);
    if (!f) f = d->gipa(d->instance, name);
    return f;
}

void CheckVk(VkResult r) {
    if (r != VK_SUCCESS) Logger::Instance().Error("ImGui Vulkan backend error " + std::to_string(int(r)));
}

void DestroySwapResources(Swap* s) {
    DeviceFns* d = s->d;
    if (!s->built) return;
    d->WaitForFences(d->device, uint32_t(s->fences.size()), s->fences.data(), VK_TRUE, 2000000000ull);
    for (auto f : s->fences) d->DestroyFence(d->device, f, nullptr);
    for (auto x : s->sems) d->DestroySemaphore(d->device, x, nullptr);
    for (auto fb : s->fbs) d->DestroyFramebuffer(d->device, fb, nullptr);
    for (auto v : s->views) d->DestroyImageView(d->device, v, nullptr);
    if (!s->cbs.empty()) d->FreeCommandBuffers(d->device, s->pool, uint32_t(s->cbs.size()), s->cbs.data());
    if (s->pool) d->DestroyCommandPool(d->device, s->pool, nullptr);
    if (s->rp) d->DestroyRenderPass(d->device, s->rp, nullptr);
    s->built = false;
}

bool BuildSwap(Swap* s, VkSwapchainKHR sc, uint32_t family) {
    DeviceFns* d = s->d;
    VkDevice dev = d->device;
    uint32_t n = 0;
    if (d->GetSwapchainImagesKHR(dev, sc, &n, nullptr) != VK_SUCCESS || !n) return false;
    s->images.resize(n);
    d->GetSwapchainImagesKHR(dev, sc, &n, s->images.data());

    VkAttachmentDescription att{};
    att.format = s->format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;            // keep the game's frame
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.srcAccessMask = 0;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if (d->CreateRenderPass(dev, &rpi, nullptr, &s->rp) != VK_SUCCESS) return false;

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family;
    if (d->CreateCommandPool(dev, &pci, nullptr, &s->pool) != VK_SUCCESS) return false;
    s->cbs.resize(n);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = s->pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = n;
    if (LayerAllocateCommandBuffers(dev, &ai, s->cbs.data()) != VK_SUCCESS) { s->cbs.clear(); return false; }

    s->built = true;   // from here DestroySwapResources can clean up partial state
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s->images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = s->format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v;
        if (d->CreateImageView(dev, &vi, nullptr, &v) != VK_SUCCESS) return false;
        s->views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = s->rp;
        fi.attachmentCount = 1;
        fi.pAttachments = &v;
        fi.width = s->extent.width;
        fi.height = s->extent.height;
        fi.layers = 1;
        VkFramebuffer fb;
        if (d->CreateFramebuffer(dev, &fi, nullptr, &fb) != VK_SUCCESS) return false;
        s->fbs.push_back(fb);
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkFence fe;
        if (d->CreateFence(dev, &fci, nullptr, &fe) != VK_SUCCESS) return false;
        s->fences.push_back(fe);
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore se;
        if (d->CreateSemaphore(dev, &sci, nullptr, &se) != VK_SUCCESS) return false;
        s->sems.push_back(se);
    }
    Logger::Instance().Info("overlay resources ready: " + std::to_string(n) + " images " + std::to_string(s->extent.width) + "x" +
                            std::to_string(s->extent.height) + " format " + std::to_string(int(s->format)));
    return true;
}

void ShutdownImGuiVulkan() {
    if (!g_imgui_dev) return;
    g_imgui_dev->DeviceWaitIdle(g_imgui_dev->device);
    ImGui_ImplVulkan_Shutdown();
    g_imgui_dev = nullptr;
    g_imgui_format = VK_FORMAT_UNDEFINED;
}

bool InitImGui(Swap* s, VkQueue queue, uint32_t family, HWND hwnd) {
    if (!g_ctx) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NavEnableKeyboard;
        AstroUi_Setup();   // the ASTRO app look and fonts (the F8 menu is the app)
        GamepadView_Setup();   // the game's button art, for the gamepad view of the MISC page
        if (!ImGui_ImplWin32_Init(hwnd)) { Logger::Instance().Error("ImGui Win32 init failed"); return false; }
        g_ctx = true;
    }
    if (g_imgui_dev && (g_imgui_dev != s->d || g_imgui_format != s->format)) ShutdownImGuiVulkan();
    if (g_imgui_dev) return true;

    DeviceFns* d = s->d;
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, &ImGuiLoader, d)) {
        Logger::Instance().Error("ImGui could not load Vulkan functions");
        return false;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_1;
    ii.Instance = d->instance;
    ii.PhysicalDevice = d->physical;
    ii.Device = d->device;
    ii.QueueFamily = family;
    ii.Queue = queue;
    ii.DescriptorPoolSize = 16;
    ii.MinImageCount = s->min_images < 2 ? 2 : s->min_images;
    ii.ImageCount = uint32_t(s->images.size()) < ii.MinImageCount ? ii.MinImageCount : uint32_t(s->images.size());
    ii.PipelineInfoMain.RenderPass = s->rp;
    ii.PipelineInfoMain.Subpass = 0;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.CheckVkResultFn = &CheckVk;
    if (!ImGui_ImplVulkan_Init(&ii)) { Logger::Instance().Error("ImGui Vulkan init failed"); return false; }
    g_imgui_dev = d;
    g_imgui_format = s->format;
    Logger::Instance().Info("ImGui " IMGUI_VERSION " ready on the game's present queue (family " + std::to_string(family) + ")");
    return true;
}

}  // namespace

void Overlay_EnsureLog() {
    static bool once = false;
    if (once) return;
    once = true;
    auto dir = DefaultConfigPath().parent_path();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    Logger::Instance().Open(dir / "astro_ctr.log");
    // which DLL the game really loaded (the Vulkan loader may take another registered copy): its path and build time
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Overlay_EnsureLog), &self);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(self, path, MAX_PATH);
    Logger::Instance().Info("---- ASTRO CTR layer loaded in sacredfire.exe (pid " + std::to_string(GetCurrentProcessId()) + "), build " +
                            __DATE__ + " " + __TIME__ + ", " + std::filesystem::path(path).string());
}

void Overlay_SignalLayerLoaded() {
    SignalNamed(L"Local\\ASTRO_CTR_LayerLoaded");
    WriteStatus("layer_loaded");
}

void Overlay_OnSwapchainCreated(DeviceFns* d, VkSwapchainKHR sc, const VkSwapchainCreateInfoKHR& ci) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto* s = new Swap;
    s->d = d;
    s->format = ci.imageFormat;
    s->extent = ci.imageExtent;
    s->min_images = ci.minImageCount;
    g_swaps[sc] = s;
    g_diag.swap_w = ci.imageExtent.width;
    g_diag.swap_h = ci.imageExtent.height;
    g_diag.swap_format = int(ci.imageFormat);
    Logger::Instance().Info("swapchain " + std::to_string(ci.imageExtent.width) + "x" + std::to_string(ci.imageExtent.height) +
                            " format " + std::to_string(int(ci.imageFormat)));
}

void Overlay_OnSwapchainDestroyed(DeviceFns*, VkSwapchainKHR sc) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    auto it = g_swaps.find(sc);
    if (it == g_swaps.end()) return;
    DestroySwapResources(it->second);
    delete it->second;
    g_swaps.erase(it);
}

void Overlay_OnDeviceDestroyed(DeviceFns* d) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    for (auto it = g_swaps.begin(); it != g_swaps.end();) {
        if (it->second->d == d) { DestroySwapResources(it->second); delete it->second; it = g_swaps.erase(it); }
        else ++it;
    }
    if (g_imgui_dev == d) ShutdownImGuiVulkan();
    Menu_Shutdown();
}

VkSemaphore Overlay_OnPresent(DeviceFns* d, VkQueue queue, uint32_t family, HWND hwnd, VkSwapchainKHR sc, uint32_t image,
                              uint32_t wait_count, const VkSemaphore* waits) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (!hwnd) return VK_NULL_HANDLE;
    auto it = g_swaps.find(sc);
    if (it == g_swaps.end()) return VK_NULL_HANDLE;
    Swap* s = it->second;
    if (s->failed) return VK_NULL_HANDLE;
    if (!s->built && !BuildSwap(s, sc, family)) {
        Logger::Instance().Error("could not create overlay resources; the overlay is off for this swapchain");
        DestroySwapResources(s);
        s->failed = true;
        return VK_NULL_HANDLE;
    }
    if (image >= s->images.size()) return VK_NULL_HANDLE;
    if (!InitImGui(s, queue, family, hwnd)) { s->failed = true; return VK_NULL_HANDLE; }
    InstallHook(hwnd);
    if (!g_old_proc) {   // fallback when the window could not be hooked
        bool down = (GetAsyncKeyState(kToggleKey) & 0x8000) != 0 && GetForegroundWindow() == hwnd;
        if (down && !g_prev_key) g_menu_open = !g_menu_open;
        g_prev_key = down;
    }
    if (g_toast_until == 0.0) g_toast_until = Now() + kToastSeconds;
    const bool toast = Now() < g_toast_until;
    Menu_Tick();   // rainbow colours keep moving while the menu is closed
    const bool pad_view = GamepadView_Enabled();   // MISC page: the pad on the screen
    static bool was_open = false;
    if (was_open && !g_menu_open) AstroUi_Flush();   // closed: save what the pages still had pending
    was_open = g_menu_open;
    if (!g_menu_open && !toast && !pad_view && g_ready_signalled) return VK_NULL_HANDLE;   // nothing to draw: zero cost

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0)
        io.DisplayFramebufferScale = ImVec2(s->extent.width / io.DisplaySize.x, s->extent.height / io.DisplaySize.y);
    io.MouseDrawCursor = g_menu_open;
    ImGui::NewFrame();
    g_diag.frames++;
    if (pad_view) GamepadView_Draw();
    if (toast && !g_menu_open) Menu_DrawToast();
    if (g_menu_open) Menu_Draw(&g_menu_open, g_diag);
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();

    VkDevice dev = d->device;
    VkFence fence = s->fences[image];
    d->WaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    d->ResetFences(dev, 1, &fence);
    VkCommandBuffer cb = s->cbs[image];
    d->ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    d->BeginCommandBuffer(cb, &bi);
    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = s->rp;
    rb.framebuffer = s->fbs[image];
    rb.renderArea.extent = s->extent;
    d->CmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    d->CmdEndRenderPass(cb);
    d->EndCommandBuffer(cb);

    std::vector<VkPipelineStageFlags> stages(wait_count, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = wait_count;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = stages.data();
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s->sems[image];
    VkResult r = d->QueueSubmit(queue, 1, &si, fence);
    if (r != VK_SUCCESS) {
        Logger::Instance().Error("overlay submit failed (" + std::to_string(int(r)) + "); overlay turned off");
        s->failed = true;
        return VK_NULL_HANDLE;   // NOTE: the game's wait semaphores were not consumed; the original present still waits on them
    }
    if (!g_ready_signalled) {
        g_ready_signalled = true;
        SignalNamed(L"Local\\ASTRO_CTR_OverlayReady");
        WriteStatus("overlay_ready");
        Logger::Instance().Info("first overlay frame submitted");
    }
    return s->sems[image];
}

}  // namespace astro
