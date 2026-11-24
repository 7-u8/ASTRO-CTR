// The ASTRO CTR in-game menu. Every change goes through VisualController -> IVisualBackend; the menu shows
// the backend's real answer for each property and never reports a change the game did not receive.
// g.base is what the user edits and saves; the controller receives g.base with the rainbow colours animated
// (Menu_Tick, every frame, also while the menu is closed).
#include "menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <vector>
#include <thread>

#include "imgui.h"

#include "../config/config.h"
#include "../config/presets.h"
#include "../core/controller.h"
#include "../core/paint_patterns.h"
#include "../diagnostics/log.h"
#include "fx_editor.h"
#include "map_colors.h"
#include "mesh_colors.h"
#include "zone_light.h"
#include "mod_engine.h"
#include "wheel_editor.h"
#include "nitro_backend.h"
#include "switches.h"
#include "players_esp.h"

namespace astro {

namespace {

constexpr unsigned long long kGuestBase = 0x800000000ull;   // sacredfire maps PS4 guest memory here

struct State {
    bool loaded = false;
    std::unique_ptr<VisualController> ctl;
    Config base;                  // the user's settings (saved); the controller gets them with rainbows applied
    BackendInfo info;
    ApplyReport last;
    bool dirty = false;
    std::string save_msg;
    std::string load_msg;
    bool paint_touched = false;   // the user edited a paint colour (do not overwrite it with the kart's own)
    bool paint_seeded = false;
    Rgba paint_all{255, 0, 255, 255};   // the "whole kart" picker
    int pattern_all = 1;
    int pattern[kPaintSlots] = {1, 1, 1};
    double t0 = 0;
};
State g;
// g.base and the controller are used by the menu (render thread) and by the rainbow thread
std::recursive_mutex g_mu;
std::thread g_rainbow;
std::atomic<bool> g_rainbow_stop{false};

// ---------------- rainbow ----------------
bool Rainbow(const std::string& id) {
    const std::string& r = g.base.fx.rainbow;
    for (size_t p = 0; (p = r.find(id, p)) != std::string::npos; p += id.size())
        if ((p == 0 || r[p - 1] == ',') && (p + id.size() == r.size() || r[p + id.size()] == ',')) return true;
    return false;
}
void SetRainbow(const std::string& id, bool on) {
    std::string out;
    size_t a = 0;
    const std::string& r = g.base.fx.rainbow;
    while (a <= r.size()) {
        size_t b = r.find(',', a);
        if (b == std::string::npos) b = r.size();
        std::string t = r.substr(a, b - a);
        if (!t.empty() && t != id) out += (out.empty() ? "" : ",") + t;
        a = b + 1;
    }
    if (on) out += (out.empty() ? "" : ",") + id;
    g.base.fx.rainbow = out;
}

double Now() {   // steady clock (thread safe; ImGui's clock is only for the render thread)
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// g.base with every rainbow colour replaced by its colour at time t (phase keeps neighbouring colours apart)
Config Effective(double t) {
    Config c = g.base;
    if (c.fx.rainbow.empty()) return c;
    const double cyc = t * c.fx.rainbow_speed;
    auto rb = [&](float phase) { return FromHsv(float(cyc) + phase, 1.0f, 1.0f); };
    if (Rainbow("n1")) c.normal.color = rb(0.0f);
    if (Rainbow("n2")) c.normal.color2 = rb(0.12f);
    if (Rainbow("n3")) c.normal.color3 = rb(0.24f);
    if (Rainbow("smoke")) c.normal.smoke = rb(0.5f);
    if (Rainbow("glow")) c.champion.lower_region = rb(0.0f);
    for (int s = 0; s < kPaintSlots; ++s)
        for (int l = 0; l < kPaintLayers; ++l) {
            const std::string slot = "s" + std::to_string(s + 1);
            if (Rainbow("kart")) c.paint.layer[s][l] = rb(s * 0.08f + l * 0.03f);
            else if (Rainbow(slot)) c.paint.layer[s][l] = rb(l * 0.03f);
            else if (Rainbow(slot + "l" + std::to_string(l))) c.paint.layer[s][l] = rb(l * 0.05f);
        }
    return c;
}

// paint layers never edited anywhere (all at their defaults): the menu then starts from the kart's own colours
bool PaintIsDefault(const KartPaint& p) {
    const KartPaint d;
    for (int s = 0; s < kPaintSlots; ++s)
        for (int l = 0; l < kPaintLayers; ++l)
            if (!(p.layer[s][l] == d.layer[s][l])) return false;
    return true;
}

void Push() {
    g.ctl->SetConfig(Effective(Now() - g.t0));
    g.last = g.ctl->Apply();
}

void EnsureLoaded() {
    if (g.loaded) return;
    g.loaded = true;
    g.t0 = Now();
    LoadResult lr = LoadConfig(DefaultConfigPath());
    g.base = lr.config;
    if (!Off("fx")) FxEditor_Start();           // effects editor of the ASTRO app
    if (!Off("wheels")) WheelEditor_Start();    // wheel editor of the ASTRO app
    if (!Off("mods")) ModEngine_Start();        // MODS page of the ASTRO app
    if (!Off("maps")) MapColors_Start();        // MAPS page of the ASTRO app
    if (!Off("mesh")) MeshColors_Start();       // ITEMS page: TNT / Nitro crate colours
    if (!Off("light")) ZoneLight_Start();       // MAPS page: fog, sun, ambient, colour grading
    g.ctl = std::make_unique<VisualController>(Off("backend") ? std::unique_ptr<IVisualBackend>(new UndiscoveredBackend)
                                                              : std::unique_ptr<IVisualBackend>(CreateGuestBackend()));
    g.info = g.ctl->Backend().Probe();
    Push();
    g.load_msg = lr.file_found ? (lr.recovered_from_error ? "config was damaged: defaults used" : "config loaded")
                               : "no config yet: defaults";
    for (auto& w : lr.warnings) Logger::Instance().Warn("config: " + w);
    Logger::Instance().Info("menu: " + g.load_msg + "; backend: " + g.info.detail);
    Logger::Instance().SetDebug(lr.config.ui.debug_logging);
    // rainbow colours are written from their own thread, about 20 times a second, so drawing never waits on the
    // game-memory writes (doing it on the render thread dropped the game to 3 FPS, 2026-10-30)
    if (!g_rainbow.joinable()) {
        g_rainbow_stop = false;
        g_rainbow = std::thread([] {
            NameThread(L"astro:config");
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            // the ASTRO app sets this event after writing config.json: apply the new file at once
            HANDLE pushed = CreateEventW(nullptr, FALSE, FALSE, kConfigPushedEvent);
            while (!g_rainbow_stop) {
                Sleep(50);
                std::lock_guard<std::recursive_mutex> l(g_mu);
                if (pushed && WaitForSingleObject(pushed, 0) == WAIT_OBJECT_0 && g.loaded && g.ctl) {
                    LoadResult lr = LoadConfig(DefaultConfigPath());
                    if (!lr.recovered_from_error) {
                        g.base = lr.config;
                        g.dirty = false;
                        g.paint_touched = g.base.paint.enabled && !PaintIsDefault(g.base.paint);   // app colours win; untouched: kart's own
                        Push();
                    }
                }
                if (g.loaded && g.ctl && !g.base.fx.rainbow.empty()) Push();
            }
            if (pushed) CloseHandle(pushed);
        });
    }
}

OverlayDiag g_diag;

void DiagTab(const OverlayDiag& d) {
    ImGui::Text("Layer: loaded in sacredfire.exe (pid %lu)", GetCurrentProcessId());
    ImGui::Text("Swapchain: %ux%u, format %d", d.swap_w, d.swap_h, d.swap_format);
    ImGui::Text("Input hook: %s", d.hook_installed ? "window hooked (F8)" : "polling F8");
    ImGui::Text("Overlay frames drawn: %llu", d.frames);
    MEMORY_BASIC_INFORMATION mbi{};
    bool guest = VirtualQuery(reinterpret_cast<void*>(kGuestBase), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT;
    ImGui::Text("Guest memory at 0x%llX: %s", kGuestBase, guest ? "present" : "not found");
    ImGui::Text("Engine backend: %s", g.info.detail.c_str());
    ImGui::Text("Config: %s", g.load_msg.c_str());
    ImGui::TextDisabled("Log: %s", Logger::Instance().Path().string().c_str());
}

}  // namespace

// Rainbow colours move on their own thread (see EnsureLoaded); nothing to do per frame.
void Menu_Tick() {}

void Menu_DrawToast() {
    { std::lock_guard<std::recursive_mutex> l(g_mu); EnsureLoaded(); }   // starts the memory search and applies the saved colours without opening the menu
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 20, vp->Pos.y + 20));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("##astro_toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                                ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.15f, 1), "ASTRO CTR");
    ImGui::SameLine();
    ImGui::TextUnformatted("by ABU 3RB - press F8 for the menu");
    ImGui::End();
}

void Menu_Draw(bool* open, const OverlayDiag& diag) {
    { std::lock_guard<std::recursive_mutex> l(g_mu); EnsureLoaded(); g_diag = diag; }
    AstroUi_Draw(open);   // every page of the ASTRO app (app/main.cpp, built into the layer)
}

void Menu_DiagPage() { DiagTab(g_diag); }

void Menu_Shutdown() {
    FxEditor_Stop();
    WheelEditor_Stop();
    ModEngine_Stop();
    MapColors_Stop();
    MeshColors_Stop();
    ZoneLight_Stop();
    g_rainbow_stop = true;
    if (g_rainbow.joinable()) g_rainbow.join();
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!g.ctl) return;
    g.ctl->Shutdown();
    Logger::Instance().Info("menu shut down; mod changes restored");
    g = State{};
}

}  // namespace astro
