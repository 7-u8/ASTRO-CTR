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
#include <thread>

#include "imgui.h"

#include "../config/config.h"
#include "../core/controller.h"
#include "../diagnostics/log.h"
#include "nitro_backend.h"

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

ImVec4 Col(const Rgba& c) { return ImVec4(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f); }
Rgba FromCol(const float* f) {
    auto b = [](float v) { return std::uint8_t(v <= 0 ? 0 : v >= 1 ? 255 : int(v * 255.f + 0.5f)); };
    return Rgba{b(f[0]), b(f[1]), b(f[2]), b(f[3])};
}

// ---------------- colour helpers ----------------
struct Hsv { float h, s, v; };
Hsv ToHsv(const Rgba& c) {
    float r = c.r / 255.f, g2 = c.g / 255.f, b = c.b / 255.f, h, s, v;
    ImGui::ColorConvertRGBtoHSV(r, g2, b, h, s, v);
    return {h, s, v};
}
Rgba FromHsv(float h, float s, float v) {
    h -= std::floor(h);
    float r, g2, b;
    ImGui::ColorConvertHSVtoRGB(h, std::clamp(s, 0.f, 1.f), std::clamp(v, 0.f, 1.f), r, g2, b);
    const float f[4] = {r, g2, b, 1};
    return FromCol(f);
}
Rgba Mix(const Rgba& a, const Rgba& b, float t) {
    auto m = [t](std::uint8_t x, std::uint8_t y) { return std::uint8_t(std::lround(x + (y - x) * t)); };
    return Rgba{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}
Rgba Scale(const Rgba& a, float k) {
    auto m = [k](std::uint8_t x) { return std::uint8_t(std::clamp(std::lround(x * k), 0L, 255L)); };
    return Rgba{m(a.r), m(a.g), m(a.b), 255};
}

// ---------------- kart paint patterns ----------------
// A slot = tint (layer 0, the sheen) + a 6-colour ramp (layers 1..6, the base colour). A pattern builds the 7
// layers from one colour. These are ASTRO's own looks made from that structure, not copies of the game's schemes.
// (names chosen not to look like the game's own paint types such as Electron or Atomic: these are colour layouts only)
const char* kPatterns[] = {"Solid", "Glow", "Icy", "Metal", "Duo-tone", "Pastel", "Dark", "Rainbow layers"};
void MakePattern(int p, const Rgba& c, Rgba out[kPaintLayers]) {
    const Hsv h = ToHsv(c);
    const Rgba white{255, 255, 255, 255}, black{0, 0, 0, 255};
    switch (p) {
    default:   // Solid
        for (int i = 0; i < kPaintLayers; ++i) out[i] = c;
        break;
    case 1: {  // Electron: deep base, bright electric highlights, a cyan-shifted glow
        const Rgba elec = FromHsv(h.h + 0.08f, 0.85f, 1.0f);
        Rgba r[] = {elec, Scale(c, 0.35f), c, white, elec, Scale(c, 0.6f), FromHsv(h.h - 0.05f, 1.0f, 1.0f)};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 2: {  // Crystal: pale, icy, glassy
        Rgba r[] = {white, Mix(c, white, 0.55f), Mix(c, white, 0.8f), Mix(c, white, 0.35f), white, Mix(c, white, 0.65f),
                    Mix(FromHsv(h.h + 0.1f, 0.4f, 1.0f), white, 0.4f)};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 3: {  // Chrome: metallic greys with a hint of the colour
        auto g = [&](float v) { return Mix(Rgba{std::uint8_t(v * 255), std::uint8_t(v * 255), std::uint8_t(v * 255), 255}, c, 0.18f); };
        Rgba r[] = {white, g(0.25f), g(0.95f), g(0.5f), g(1.0f), g(0.3f), g(0.75f)};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 4: {  // Neon: saturated colour with its complement
        const Rgba sat = FromHsv(h.h, 1.0f, 1.0f), comp = FromHsv(h.h + 0.5f, 1.0f, 1.0f);
        Rgba r[] = {comp, sat, sat, comp, sat, comp, sat};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 5: {  // Pearl: white with soft pastel shifts
        Rgba r[] = {white, white, Mix(c, white, 0.7f), Mix(FromHsv(h.h + 0.08f, 0.5f, 1), white, 0.6f), white,
                    Mix(FromHsv(h.h - 0.08f, 0.5f, 1), white, 0.6f), Mix(c, white, 0.85f)};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 6: {  // Galaxy: dark space with bright colour and magenta
        Rgba r[] = {FromHsv(h.h + 0.15f, 1, 1), Mix(black, c, 0.25f), c, FromHsv(0.83f, 0.9f, 0.9f), black, Scale(c, 0.6f), white};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 7:    // Spectrum: the colour wheel across the layers, starting at the chosen colour
        for (int i = 0; i < kPaintLayers; ++i) out[i] = FromHsv(h.h + i / float(kPaintLayers), std::max(h.s, 0.7f), std::max(h.v, 0.8f));
        break;
    }
}

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
    g.ctl = std::make_unique<VisualController>(std::unique_ptr<IVisualBackend>(CreateGuestBackend()));
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
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            while (!g_rainbow_stop) {
                Sleep(50);
                std::lock_guard<std::recursive_mutex> l(g_mu);
                if (g.loaded && g.ctl && !g.base.fx.rainbow.empty()) Push();
            }
        });
    }
}

void StatusLine(bool enabled, bool ok) {
    if (!enabled) { ImGui::TextDisabled("off - the game's original colour"); return; }
    if (ok) ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1), "applied in game");
    else ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1), "NOT applied: this effect is not connected to the game yet");
}

// Colour picker bound to one config colour, with an "RGB" (rainbow) toggle when id is given.
// Returns true when the user changed the colour or the toggle.
bool ColorRow(const char* label, Rgba& c, const char* id = nullptr) {
    bool changed = false;
    if (id) {
        ImGui::PushID(id);
        bool on = Rainbow(id);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, Col(FromHsv(float((Now() - g.t0) * g.base.fx.rainbow_speed), 0.9f, 0.9f)));
        if (ImGui::SmallButton("RGB")) { SetRainbow(id, !on); changed = true; }
        if (on) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rainbow: cycle this colour through every colour");
        ImGui::PopID();
        ImGui::SameLine();
    }
    float f[4] = {c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.0f};
    const ImGuiColorEditFlags fl = ImGuiColorEditFlags_DisplayHex | ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoAlpha;
    if (ImGui::ColorEdit4(label, f, fl)) {
        Rgba n = FromCol(f);
        n.a = 255;
        if (!(n == c)) { c = n; changed = true; }
    }
    return changed;
}

void Changed() {
    Push();
    g.dirty = true;
}

void RainbowSpeed() {
    if (g.base.fx.rainbow.empty()) return;
    if (ImGui::SliderFloat("Rainbow speed", &g.base.fx.rainbow_speed, 0.02f, 2.0f, "%.2f cycles/s", ImGuiSliderFlags_Logarithmic)) Changed();
}

void NitroTab() {
    Config& cfg = g.base;
    bool ch = false;
    g.info = g.ctl->Backend().Probe();
    if (g.info.status == BackendStatus::Ready && !g.last.normal_ok) Push();   // search just finished
    const ImVec4 st = g.info.status == BackendStatus::Ready ? ImVec4(0.35f, 0.9f, 0.45f, 1)
                    : g.info.status == BackendStatus::Error ? ImVec4(1, 0.35f, 0.3f, 1) : ImVec4(1, 0.8f, 0.3f, 1);
    ImGui::TextColored(st, "%s", g.info.detail.c_str());
    ImGui::SeparatorText("Nitro flame (normal and blue boost)");
    ch |= ImGui::Checkbox("Custom colours##n", &cfg.normal.enabled);
    ImGui::BeginDisabled(!cfg.normal.enabled);
    ch |= ColorRow("Colour 1 (game: orange)##n", cfg.normal.color, "n1");
    ch |= ColorRow("Colour 2 (game: red)##n", cfg.normal.color2, "n2");
    ch |= ColorRow("Colour 3 (game: pink)##n", cfg.normal.color3, "n3");
    ch |= ImGui::SliderFloat("Brightness##n", &cfg.normal.brightness, 0.25f, 8.0f, "x%.2f", ImGuiSliderFlags_Logarithmic);
    ch |= ColorRow("Smoke##n", cfg.normal.smoke, "smoke");
    ImGui::TextDisabled("Smoke keeps its own dark brightness; the colour sets its tint.");
    RainbowSpeed();
    ImGui::EndDisabled();
    StatusLine(cfg.normal.enabled, g.last.normal_ok);
    if (ImGui::Button("Reset nitro flame")) {
        cfg.normal = NitroNormal{};
        for (const char* id : {"n1", "n2", "n3", "smoke"}) SetRainbow(id, false);
        ch = true;
    }

    ImGui::SeparatorText("Blue boost only");
    ImGui::TextWrapped("The game draws the blue boost with the same colour curves as the normal boost, so the colours "
                       "above change both. A separate blue-boost colour needs the switch the game uses at run time; "
                       "it has not been found yet.");
    if (ch) Changed();
}

void VehicleTab() {
    Config& cfg = g.base;
    bool ch = false;
    // the underglow is found by the same background search: retry until it has been applied
    if (cfg.champion.enabled && !g.last.champion_ok) Push();
    ImGui::SeparatorText("Champion kart underglow");
    ch |= ImGui::Checkbox("Custom underglow", &cfg.champion.enabled);
    ImGui::BeginDisabled(!cfg.champion.enabled);
    ch |= ColorRow("Colour##c", cfg.champion.lower_region, "glow");
    ImGui::EndDisabled();
    if (cfg.champion.enabled && !g.last.champion_ok) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "searching for the underglow in memory (takes ~10-20 s; show the Champion kart)");
    else StatusLine(cfg.champion.enabled, g.last.champion_ok);
    ImGui::TextDisabled("After the game reloads the kart it can show blue for a few seconds.");
    if (ImGui::Button("Reset underglow")) { cfg.champion = Champion{}; SetRainbow("glow", false); ch = true; }

    ImGui::SeparatorText("My kart paint");
    if (cfg.paint.enabled && !g.last.paint_ok) Push();   // retry until the kart's colours are found
    Rgba orig[kPaintSlots][kPaintLayers];
    const bool known = g.ctl->Backend().KartPaintOriginal(&orig[0][0]);
    auto use_kart_colours = [&] { std::memcpy(cfg.paint.layer, orig, sizeof orig); ch = true; };
    bool enable = cfg.paint.enabled;
    if (ImGui::Checkbox("Custom kart paint", &enable)) {
        cfg.paint.enabled = enable;
        // start from the kart's own colours, so only what the user changes looks different
        if (enable && known && !g.paint_touched) use_kart_colours();
        ch = true;
    }
    if (cfg.paint.enabled && known && !g.paint_seeded && !g.paint_touched) { use_kart_colours(); g.paint_seeded = true; }

    ImGui::BeginDisabled(!cfg.paint.enabled);
    // whole kart: one colour, or a pattern built from it
    if (ColorRow("Whole kart##p", g.paint_all, "kart")) {
        for (auto& sl : cfg.paint.layer) for (auto& c : sl) c = g.paint_all;
        g.paint_touched = ch = true;
    }
    ImGui::SetNextItemWidth(140);
    ImGui::Combo("Colour layout##patall", &g.pattern_all, kPatterns, IM_ARRAYSIZE(kPatterns));
    ImGui::SameLine();
    if (ImGui::Button("Pattern on whole kart")) {
        for (int s = 0; s < kPaintSlots; ++s) MakePattern(g.pattern_all, Mix(g.paint_all, FromHsv(ToHsv(g.paint_all).h + s * 0.06f, 1, 1), 0.15f * s), cfg.paint.layer[s]);
        g.paint_touched = ch = true;
    }
    static const char* kSlotName[kPaintSlots] = {"Slot 1 - main body", "Slot 2", "Slot 3"};
    for (int s = 0; s < kPaintSlots; ++s) {
        ImGui::PushID(s);
        if (ImGui::CollapsingHeader(kSlotName[s])) {
            const std::string sid = "s" + std::to_string(s + 1);
            Rgba whole = cfg.paint.layer[s][1];
            if (ColorRow("Whole slot", whole, sid.c_str())) {
                for (auto& c : cfg.paint.layer[s]) c = whole;
                g.paint_touched = ch = true;
            }
            ImGui::SetNextItemWidth(140);
            ImGui::Combo("##pat", &g.pattern[s], kPatterns, IM_ARRAYSIZE(kPatterns));
            ImGui::SameLine();
            if (ImGui::Button("Apply pattern")) { MakePattern(g.pattern[s], whole, cfg.paint.layer[s]); g.paint_touched = ch = true; }
            for (int l = 0; l < kPaintLayers; ++l) {
                char label[32];
                if (l == 0) std::snprintf(label, sizeof label, "Tint (sheen)");
                else std::snprintf(label, sizeof label, "Colour layer %d", l);
                const std::string lid = sid + "l" + std::to_string(l);
                if (ColorRow(label, cfg.paint.layer[s][l], lid.c_str())) g.paint_touched = ch = true;
            }
        }
        ImGui::PopID();
    }
    RainbowSpeed();
    if (ImGui::Button("Back to my kart's colours") && known) { use_kart_colours(); g.paint_touched = false; }
    ImGui::EndDisabled();
    if (cfg.paint.enabled && !g.last.paint_ok)
        ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "waiting for your kart (show it in the garage)");
    else StatusLine(cfg.paint.enabled, g.last.paint_ok);
    if (ImGui::Button("Reset kart paint")) {
        cfg.paint.enabled = false;
        std::string keep;   // drop every kart rainbow
        for (const char* id : {"n1", "n2", "n3", "smoke", "glow"}) if (Rainbow(id)) keep += (keep.empty() ? "" : ",") + std::string(id);
        cfg.fx.rainbow = keep;
        g.paint_touched = false;
        ch = true;
    }
    if (ch) Changed();
}

void SettingsTab() {
    Config& cfg = g.base;
    bool ch = false;
    ch |= ImGui::SliderFloat("Menu size", &cfg.ui.scale, 0.75f, 2.0f, "%.2f");
    if (ImGui::Checkbox("Debug log", &cfg.ui.debug_logging)) { Logger::Instance().SetDebug(cfg.ui.debug_logging); ch = true; }
    if (ch) g.dirty = true;
    ImGui::Separator();
    if (ImGui::Button(g.dirty ? "Save settings *" : "Save settings")) {
        std::string err;
        if (SaveConfig(DefaultConfigPath(), g.base, &err)) { g.save_msg = "saved"; g.dirty = false; }
        else g.save_msg = "save FAILED: " + err;
        Logger::Instance().Info("config " + g.save_msg);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload from file")) { g.ctl->Shutdown(); g.loaded = false; EnsureLoaded(); g.dirty = false; }
    if (!g.save_msg.empty()) ImGui::TextUnformatted(g.save_msg.c_str());
    ImGui::TextDisabled("%s", DefaultConfigPath().string().c_str());
    ImGui::SeparatorText("Credits");
    ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.15f, 1), "ASTRO CTR " ASTRO_VERSION " by ABU 3RB");
    ImGui::TextUnformatted("TikTok: _tmo   |   Instagram: 7.u8   |   Discord: 7_ug");
}

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

void Menu_ApplyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8;
    s.FrameRounding = 5;
    s.GrabRounding = 5;
    s.WindowPadding = ImVec2(14, 12);
    ImVec4* c = s.Colors;
    const ImVec4 accent(0.95f, 0.45f, 0.10f, 1.0f);   // ASTRO orange
    c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 0.94f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.55f, 0.22f, 0.04f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.20f, 0.12f, 0.08f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.75f, 0.32f, 0.06f, 1.0f);
    c[ImGuiCol_TabHovered] = accent;
    c[ImGuiCol_CheckMark] = accent;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_Button] = ImVec4(0.30f, 0.16f, 0.07f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.65f, 0.28f, 0.06f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.14f, 0.14f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.45f, 0.20f, 0.06f, 1.0f);
}

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
    std::lock_guard<std::recursive_mutex> l(g_mu);
    EnsureLoaded();
    ImGui::GetStyle().FontScaleMain = g.base.ui.scale;
    ImGui::SetNextWindowSize(ImVec2(520, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("ASTRO CTR " ASTRO_VERSION "   (F8)###astro", open)) {
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Nitro Color Studio")) { NitroTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Vehicle Studio")) { VehicleTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings")) { SettingsTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Diagnostics")) { DiagTab(diag); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void Menu_Shutdown() {
    g_rainbow_stop = true;
    if (g_rainbow.joinable()) g_rainbow.join();
    std::lock_guard<std::recursive_mutex> l(g_mu);
    if (!g.ctl) return;
    g.ctl->Shutdown();
    Logger::Instance().Info("menu shut down; mod changes restored");
    g = State{};
}

}  // namespace astro
