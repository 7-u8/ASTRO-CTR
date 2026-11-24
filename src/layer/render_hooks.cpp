#include "render_hooks.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../config/config.h"
#include "../config/json.h"
#include "../diagnostics/log.h"
#include "overlay.h"

namespace astro {

namespace {

// the next layer's command functions, per dispatch key (device and its command buffers share it)
struct CmdFns {
    PFN_vkCmdBeginRendering BeginRendering = nullptr;
    PFN_vkCmdBeginRenderingKHR BeginRenderingKHR = nullptr;
    PFN_vkCmdEndRendering EndRendering = nullptr;
    PFN_vkCmdEndRenderingKHR EndRenderingKHR = nullptr;
    PFN_vkCmdBeginRenderPass BeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass EndRenderPass = nullptr;
    PFN_vkCmdDraw Draw = nullptr;
    PFN_vkCmdDrawIndexed DrawIndexed = nullptr;
    PFN_vkCmdDrawIndirect DrawIndirect = nullptr;
    PFN_vkCmdDrawIndexedIndirect DrawIndexedIndirect = nullptr;
    PFN_vkCmdDrawIndirectCount DrawIndirectCount = nullptr;
    PFN_vkCmdDrawIndexedIndirectCount DrawIndexedIndirectCount = nullptr;
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkCmdBindPipeline BindPipeline = nullptr;
};
using Key = void*;
template <typename T> Key KeyOf(T h) { return *reinterpret_cast<void**>(h); }

std::mutex g_mu;
std::unordered_map<Key, CmdFns*> g_fns;
thread_local Key t_key = nullptr;
thread_local CmdFns* t_fns = nullptr;

CmdFns* Fns(VkCommandBuffer cb) {
    const Key k = KeyOf(cb);
    if (k != t_key) {   // one lookup per thread and device, not per draw
        std::lock_guard<std::mutex> l(g_mu);
        auto it = g_fns.find(k);
        t_fns = it == g_fns.end() ? nullptr : it->second;
        t_key = k;
    }
    return t_fns;
}

// options (written by the ASTRO app): pass kinds whose draws are dropped
std::mutex g_opt_mu;
std::vector<std::string> g_skip;
std::atomic<bool> g_any_skip{false};
// texture detail: samplers created from now on skip this many mip levels (0 = full detail) and lose anisotropic filtering
std::atomic<int> g_tex_level{0};
std::atomic<bool> g_aniso_off{false};
std::atomic<unsigned> g_samplers{0}, g_samplers_changed{0};
// engine render settings (EU and USA 1.21, same address): the object the developers' render menu edits = *(*(0x802a7f420) + 0x1d8)
// (Ghidra FUN_00b48b30 / FUN_00b40260; seen in game 2026-10-31: Show VFX off removed the effects).
// -1 = leave the game's own value
std::atomic<int> g_vfx{-1}, g_bloom{-1}, g_mblur{-1}, g_defocus{-1};   // created since the game started / changed by ASTRO

// ---- shaders ----
// Every graphics pipeline gets a fingerprint from the code of its shaders, the same in every game session (the
// emulator builds the same SPIR-V for the same PS4 shader). Grass (GrassOutdoor), water (FlowWater) and the effects
// each have their own shaders, so a fingerprint names them across runs.
std::mutex g_sh_mu;
std::unordered_map<VkShaderModule, std::uint64_t> g_module_hash;
std::unordered_map<VkPipeline, std::uint64_t> g_pipe_hash;
// options: shaders whose draws are dropped, and shaders whose instanced draws keep only a share of their instances
using HashSet = std::unordered_set<std::uint64_t>;
using KeepMap = std::unordered_map<std::uint64_t, int>;   // percent kept
std::shared_ptr<const HashSet> g_hide_shaders = std::make_shared<HashSet>();
std::shared_ptr<const KeepMap> g_keep = std::make_shared<KeepMap>();

std::uint64_t Fnv(const void* p, size_t n, std::uint64_t h = 0xcbf29ce484222325ull) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

// the pipeline bound on this thread's command buffer, and per-shader counts until the pass ends
thread_local std::uint64_t t_pipe = 0;
struct ShCount { unsigned long long draws = 0, instances = 0, hidden = 0; std::string pass; };
thread_local std::unordered_map<std::uint64_t, ShCount> t_sh;
std::unordered_map<std::uint64_t, ShCount> g_sh_stats;

// the pass being recorded on this thread
thread_local VkCommandBuffer t_cb = VK_NULL_HANDLE;
thread_local bool t_skip = false;
thread_local unsigned t_draws = 0;
thread_local std::string t_kind;

// statistics of the current measuring window: pass kind -> (passes, draws, skipped draws)
struct Count { unsigned long long passes = 0, draws = 0, skipped = 0; };
std::map<std::string, Count> g_stats;
unsigned long long g_frames = 0;
ULONGLONG g_window_start = 0, g_options_read = 0;

std::string Kind(uint32_t w, uint32_t h, uint32_t colours, bool depth) {
    char b[64];
    std::snprintf(b, sizeof b, "%ux%u c%u%s", w, h, colours, depth ? " depth" : "");
    return b;
}

bool Skipped(const std::string& kind) {
    if (!g_any_skip.load()) return false;
    std::lock_guard<std::mutex> l(g_opt_mu);
    return std::find(g_skip.begin(), g_skip.end(), kind) != g_skip.end();
}

void BeginPass(VkCommandBuffer cb, uint32_t w, uint32_t h, uint32_t colours, bool depth) {
    t_cb = cb;
    t_draws = 0;
    t_kind = Kind(w, h, colours, depth);
    t_skip = Skipped(t_kind);
}
void EndPass() {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& [h, c] : t_sh) {
        ShCount& g = g_sh_stats[h];
        g.draws += c.draws;
        g.instances += c.instances;
        g.hidden += c.hidden;
        if (g.pass.empty()) g.pass = t_kind;
    }
    t_sh.clear();
    Count& c = g_stats[t_kind + (t_skip ? " [skipped]" : "")];
    ++c.passes;
    (t_skip ? c.skipped : c.draws) += t_draws;
    t_cb = VK_NULL_HANDLE;
    t_skip = false;
}
// a draw inside a pass: dropped when its pass kind or its shader is hidden, or (for a reduced shader) when it falls
// outside the share kept: the game draws its effects one draw each (seen in a race 2026-10-31: instances = draws), so
// keeping K% of a shader's draws keeps K% of its effects. The choice is spread evenly over the shader's draws of the pass.
bool Drop(VkCommandBuffer cb, std::uint32_t instances = 1) {
    if (cb != t_cb) return false;
    ++t_draws;
    ShCount& c = t_sh[t_pipe];
    bool hide = t_skip || std::atomic_load(&g_hide_shaders)->count(t_pipe) > 0;
    if (!hide) {
        auto keep = std::atomic_load(&g_keep);
        auto it = keep->find(t_pipe);
        if (it != keep->end()) {
            const unsigned long long n = c.draws + c.hidden;   // this shader's draws so far in the pass
            hide = (n + 1) * unsigned(it->second) / 100 == n * unsigned(it->second) / 100;
        }
    }
    if (hide) ++c.hidden;
    else { ++c.draws; c.instances += instances; }
    return hide;
}

void BeginRenderingCommon(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    const bool depth = ri->pDepthAttachment && ri->pDepthAttachment->imageView != VK_NULL_HANDLE;
    BeginPass(cb, ri->renderArea.extent.width, ri->renderArea.extent.height, ri->colorAttachmentCount, depth);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    BeginRenderingCommon(cb, ri);
    Fns(cb)->BeginRendering(cb, ri);
}
VKAPI_ATTR void VKAPI_CALL CmdBeginRenderingKHR(VkCommandBuffer cb, const VkRenderingInfo* ri) {
    BeginRenderingCommon(cb, ri);
    Fns(cb)->BeginRenderingKHR(cb, ri);
}
VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer cb) {
    if (cb == t_cb) EndPass();
    Fns(cb)->EndRendering(cb);
}
VKAPI_ATTR void VKAPI_CALL CmdEndRenderingKHR(VkCommandBuffer cb) {
    if (cb == t_cb) EndPass();
    Fns(cb)->EndRenderingKHR(cb);
}
// classic render passes: counted by size only (their attachments are not known here), never skipped
VKAPI_ATTR void VKAPI_CALL CmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo* bi, VkSubpassContents c) {
    t_cb = cb;
    t_draws = 0;
    t_skip = false;
    t_kind = Kind(bi->renderArea.extent.width, bi->renderArea.extent.height, 99, false) + " (render pass)";
    Fns(cb)->BeginRenderPass(cb, bi, c);
}
VKAPI_ATTR void VKAPI_CALL CmdEndRenderPass(VkCommandBuffer cb) {
    if (cb == t_cb) EndPass();
    Fns(cb)->EndRenderPass(cb);
}

VKAPI_ATTR void VKAPI_CALL CmdDraw(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!Drop(cb, b)) Fns(cb)->Draw(cb, a, b, c, d);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexed(VkCommandBuffer cb, uint32_t a, uint32_t b, uint32_t c, int32_t d, uint32_t e) {
    if (!Drop(cb, b)) Fns(cb)->DrawIndexed(cb, a, b, c, d, e);
}
VKAPI_ATTR void VKAPI_CALL CmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (bp == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        std::lock_guard<std::mutex> l(g_sh_mu);
        auto it = g_pipe_hash.find(p);
        t_pipe = it == g_pipe_hash.end() ? 0 : it->second;
    }
    Fns(cb)->BindPipeline(cb, bp, p);
}
CmdFns* DevFns(VkDevice dev) {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_fns.find(KeyOf(dev));
    return it == g_fns.end() ? nullptr : it->second;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo* ci, const VkAllocationCallbacks* a, VkShaderModule* out) {
    VkResult r = DevFns(dev)->CreateShaderModule(dev, ci, a, out);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> l(g_sh_mu);
        g_module_hash[*out] = Fnv(ci->pCode, ci->codeSize);
    }
    return r;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n, const VkGraphicsPipelineCreateInfo* ci,
                                                       const VkAllocationCallbacks* a, VkPipeline* out) {
    VkResult r = DevFns(dev)->CreateGraphicsPipelines(dev, cache, n, ci, a, out);
    std::lock_guard<std::mutex> l(g_sh_mu);
    for (uint32_t i = 0; i < n; ++i) {
        if (!out[i]) continue;
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t s = 0; s < ci[i].stageCount; ++s) {
            const VkPipelineShaderStageCreateInfo& st = ci[i].pStages[s];
            std::uint64_t mh = 0;
            auto it = g_module_hash.find(st.module);
            if (it != g_module_hash.end()) mh = it->second;
            else   // shader code given inline (VK_KHR_maintenance5)
                for (auto* p = static_cast<const VkBaseInStructure*>(st.pNext); p; p = p->pNext)
                    if (p->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO) {
                        auto* m = reinterpret_cast<const VkShaderModuleCreateInfo*>(p);
                        mh = Fnv(m->pCode, m->codeSize);
                    }
            h = Fnv(&mh, sizeof mh, Fnv(&st.stage, sizeof st.stage, h));
        }
        g_pipe_hash[out[i]] = h;
    }
    return r;
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    if (!Drop(cb)) Fns(cb)->DrawIndirect(cb, b, o, n, s);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) {
    if (!Drop(cb)) Fns(cb)->DrawIndexedIndirect(cb, b, o, n, s);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s) {
    if (!Drop(cb)) Fns(cb)->DrawIndirectCount(cb, b, o, cbuf, co, m, s);
}
VKAPI_ATTR void VKAPI_CALL CmdDrawIndexedIndirectCount(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkBuffer cbuf, VkDeviceSize co, uint32_t m, uint32_t s) {
    if (!Drop(cb)) Fns(cb)->DrawIndexedIndirectCount(cb, b, o, cbuf, co, m, s);
}

// Textures are read through samplers; a sampler whose minimum LOD is N never reads the N finest mip levels, so the GPU
// reads (and caches) textures at 1/2^N of their size. Single-level images (render targets) clamp to their only level.
// Samplers are created when textures are first used, so a change shows on what loads next (a new race).
VKAPI_ATTR VkResult VKAPI_CALL CreateSampler(VkDevice dev, const VkSamplerCreateInfo* ci, const VkAllocationCallbacks* alloc, VkSampler* out) {
    CmdFns* f;
    { std::lock_guard<std::mutex> l(g_mu); auto it = g_fns.find(KeyOf(dev)); f = it == g_fns.end() ? nullptr : it->second; }
    VkSamplerCreateInfo info = *ci;
    const int lv = g_tex_level.load();
    ++g_samplers;
    if (lv > 0 && info.maxLod > 0.0f) {   // samplers that may use mips (maxLod 0: render targets, UI)
        info.minLod = std::max(info.minLod, float(lv));
        info.maxLod = std::max(info.maxLod, info.minLod);
        ++g_samplers_changed;
    }
    if (g_aniso_off.load()) { info.anisotropyEnable = VK_FALSE; info.maxAnisotropy = 1.0f; }
    return f->CreateSampler(dev, &info, alloc, out);
}

bool HeapPtr(std::uint64_t p) { return p >= 0x1000000000ull && p < 0x8000000000ull; }
bool ReadMem(std::uint64_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
// writes the wanted engine settings (once a second): only through a pointer chain that looks right
void ApplyEngineSettings() {
    const int vfx = g_vfx.load(), bloom = g_bloom.load(), mblur = g_mblur.load(), defocus = g_defocus.load();
    if (vfx < 0 && bloom < 0 && mblur < 0 && defocus < 0) return;
    std::uint64_t renderer = 0, settings = 0;
    std::int32_t shells = 0;
    float far_clip = 0;
    if (!ReadMem(0x802a7f420ull, &renderer, 8) || !HeapPtr(renderer)) return;
    if (!ReadMem(renderer + 0x1d8, &settings, 8) || !HeapPtr(settings)) return;
    // the object is the render settings: Max Fur Shells is 1..7 and the far clip a positive distance
    if (!ReadMem(settings + 0x178, &shells, 4) || shells < 1 || shells > 7) return;
    if (!ReadMem(settings + 0x130, &far_clip, 4) || !(far_clip > 1.0f && far_clip < 1e7f)) return;
    auto put = [&](std::uint64_t a, int v) {
        if (v < 0) return;
        std::uint8_t cur = 0, want = std::uint8_t(v);
        if (ReadMem(a, &cur, 1) && cur != want) *reinterpret_cast<volatile std::uint8_t*>(a) = want;
    };
    put(settings + 0x121, vfx);
    put(settings + 0x71, bloom);
    put(settings + 0x70, mblur);
    put(settings + 0x58, defocus);
    // Show VFX also lives in the effect manager (the menu's apply function copies it there)
    if (vfx >= 0) {
        std::uint64_t mgr = 0, inner = 0;
        if (ReadMem(0x802adf6c0ull, &mgr, 8) && HeapPtr(mgr) && ReadMem(mgr + 0x10, &inner, 8) && HeapPtr(inner)) put(inner + 0xe80, vfx);
    }
}

void ReadOptions() {
    std::FILE* f = _wfopen((DefaultConfigPath().parent_path() / "optimize.json").c_str(), L"rb");
    if (!f) return;
    std::string s;
    char b[1024];
    for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
    std::fclose(f);
    auto root = json::Parse(s);
    if (!root) return;
    if (const json::Value* v = root->Find("texture_level"); v && v->type == json::Value::Type::Number && int(v->n) != g_tex_level.load()) {
        g_tex_level = int(v->n);
        Logger::Instance().Info("optimize: texture level " + std::to_string(g_tex_level.load()) + " (samplers created so far: " +
                                std::to_string(g_samplers.load()) + ")");
    }
    auto tri = [&](const char* k) { const json::Value* v = root->Find(k); return v && v->type == json::Value::Type::Bool ? int(v->b) : -1; };
    g_vfx = tri("vfx");
    g_bloom = tri("bloom");
    g_mblur = tri("motion_blur");
    g_defocus = tri("depth_blur");
    if (const json::Value* v = root->Find("aniso_off"); v && v->type == json::Value::Type::Bool) g_aniso_off = v->b;
    auto hide = std::make_shared<HashSet>();
    if (const json::Value* a = root->Find("hide_shaders"); a && a->type == json::Value::Type::Array && a->a)
        for (auto& v : *a->a) if (v.type == json::Value::Type::String) hide->insert(std::stoull(v.s, nullptr, 16));
    auto keep = std::make_shared<KeepMap>();
    if (const json::Value* o = root->Find("keep_instances"); o && o->type == json::Value::Type::Object && o->o)
        for (auto& [k, v] : *o->o) if (v.type == json::Value::Type::Number) (*keep)[std::stoull(k, nullptr, 16)] = std::clamp(int(v.n), 0, 100);
    std::atomic_store(&g_hide_shaders, std::shared_ptr<const HashSet>(hide));
    std::atomic_store(&g_keep, std::shared_ptr<const KeepMap>(keep));
    std::vector<std::string> skip;
    if (const json::Value* a = root->Find("skip"); a && a->type == json::Value::Type::Array && a->a)
        for (auto& v : *a->a) if (v.type == json::Value::Type::String) skip.push_back(v.s);
    std::lock_guard<std::mutex> l(g_opt_mu);
    if (skip != g_skip) {
        std::string list;
        for (auto& k : skip) list += " [" + k + "]";
        Logger::Instance().Info("optimize: passes hidden:" + (list.empty() ? std::string(" none") : list));
    }
    g_skip = skip;
    g_any_skip = !g_skip.empty();
}

void WriteStats() {
    std::string out = "{\n  \"frames\": " + std::to_string(g_frames) + ",\n  \"samplers\": " + std::to_string(g_samplers.load()) +
                      ",\n  \"samplers_changed\": " + std::to_string(g_samplers_changed.load()) + ",\n  \"passes\": [\n";
    bool first = true;
    for (auto& [kind, c] : g_stats) {
        char b[256];
        const double f = g_frames ? double(g_frames) : 1.0;
        std::snprintf(b, sizeof b, "%s    {\"kind\": \"%s\", \"per_frame\": %.1f, \"draws_per_frame\": %.1f, \"skipped_per_frame\": %.1f}",
                      first ? "" : ",\n", kind.c_str(), c.passes / f, c.draws / f, c.skipped / f);
        out += b;
        first = false;
    }
    out += "\n  ],\n  \"shaders\": [\n";
    std::vector<std::pair<std::uint64_t, ShCount>> sh(g_sh_stats.begin(), g_sh_stats.end());
    std::sort(sh.begin(), sh.end(), [](auto& a, auto& b) { return a.second.draws + a.second.hidden > b.second.draws + b.second.hidden; });
    first = true;
    for (size_t i = 0; i < sh.size() && i < 60; ++i) {
        char b[256];
        const double f = g_frames ? double(g_frames) : 1.0;
        std::snprintf(b, sizeof b, "%s    {\"id\": \"%016llx\", \"draws\": %.1f, \"instances\": %.1f, \"hidden\": %.1f, \"pass\": \"%s\"}",
                      first ? "" : ",\n", (unsigned long long)sh[i].first, sh[i].second.draws / f, sh[i].second.instances / f,
                      sh[i].second.hidden / f, sh[i].second.pass.c_str());
        out += b;
        first = false;
    }
    out += "\n  ]\n}\n";
    const auto dir = DefaultConfigPath().parent_path();
    const auto tmp = dir / "render_stats.json.tmp";
    if (std::FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        std::fputs(out.c_str(), f);
        std::fclose(f);
        MoveFileExW(tmp.c_str(), (dir / "render_stats.json").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

}  // namespace

PFN_vkVoidFunction RenderHooks_Intercept(const char* n) {
#define HOOK(name) if (!std::strcmp(n, "vk" #name)) return reinterpret_cast<PFN_vkVoidFunction>(&name)
    HOOK(CmdBeginRendering); HOOK(CmdBeginRenderingKHR); HOOK(CmdEndRendering); HOOK(CmdEndRenderingKHR);
    HOOK(CmdBeginRenderPass); HOOK(CmdEndRenderPass);
    HOOK(CmdDraw); HOOK(CmdDrawIndexed); HOOK(CmdDrawIndirect); HOOK(CmdDrawIndexedIndirect);
    HOOK(CmdDrawIndirectCount); HOOK(CmdDrawIndexedIndirectCount); HOOK(CreateSampler);
    HOOK(CmdBindPipeline); HOOK(CreateShaderModule); HOOK(CreateGraphicsPipelines);
#undef HOOK
    return nullptr;
}

void RenderHooks_OnDevice(DeviceFns* d) {
    auto* f = new CmdFns;
#define LOAD(field, name) f->field = reinterpret_cast<decltype(f->field)>(d->gdpa(d->device, name))
    LOAD(BeginRendering, "vkCmdBeginRendering");
    LOAD(BeginRenderingKHR, "vkCmdBeginRenderingKHR");
    LOAD(EndRendering, "vkCmdEndRendering");
    LOAD(EndRenderingKHR, "vkCmdEndRenderingKHR");
    LOAD(BeginRenderPass, "vkCmdBeginRenderPass");
    LOAD(EndRenderPass, "vkCmdEndRenderPass");
    LOAD(Draw, "vkCmdDraw");
    LOAD(DrawIndexed, "vkCmdDrawIndexed");
    LOAD(DrawIndirect, "vkCmdDrawIndirect");
    LOAD(DrawIndexedIndirect, "vkCmdDrawIndexedIndirect");
    LOAD(DrawIndirectCount, "vkCmdDrawIndirectCount");
    LOAD(DrawIndexedIndirectCount, "vkCmdDrawIndexedIndirectCount");
    LOAD(CreateSampler, "vkCreateSampler");
    LOAD(CreateShaderModule, "vkCreateShaderModule");
    LOAD(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
    LOAD(BindPipeline, "vkCmdBindPipeline");
#undef LOAD
    std::lock_guard<std::mutex> l(g_mu);
    g_fns[KeyOf(d->device)] = f;
}

void RenderHooks_OnPresent() {
    const ULONGLONG now = GetTickCount64();
    if (now - g_options_read >= 1000) { g_options_read = now; ReadOptions(); ApplyEngineSettings(); }
    std::lock_guard<std::mutex> l(g_mu);
    ++g_frames;
    if (!g_window_start) g_window_start = now;
    if (now - g_window_start >= 5000) {   // every 5 seconds: the average frame of the window
        WriteStats();
        g_stats.clear();
        g_sh_stats.clear();
        g_frames = 0;
        g_window_start = now;
    }
}

}  // namespace astro
