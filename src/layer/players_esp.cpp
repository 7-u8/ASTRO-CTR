// PLAYERS ESP. Addresses are eboot addresses + 0x800000000, the same in the EU and USA 1.21 builds; each one was
// read live in the game on 2026-11-23 (USA 1.21, Comp Hub host) before it was used here:
//   RacerMgr::_Instance 0x2ad4b70 -> +0x10 _racers (list: count +0xc, items +0x20)
//   racer +0x20 = handle of the kart entity (flags at +0, object at +0x28); CRacer fields from the game's own metadata (2026-11-23):
//     +0x10 _player (set only on your own racer; the _racers order is NOT stable), +0x40 _isAiControlled (bool)
//   entity +0x18 components (list), +0x20 position (x, y, z; z up; inches), +0x60 velocity (inches/s)
//   component type: vtable slot 0 = getMeta = "mov rax, [rip+X]; ret", X = the type's meta global
//   kart controller (0x2ac4580): +0x210 ground normal z, +0x214 on ground, +0x230 surface handle, +0x248 air time
//   boost (0x2ac4018): +0x28 active boosts (list), +0x38 boost speed; boost item: +0x10 data (+0x1c duration),
//     +0x18 elapsed, +0x28 extra, +0x38 start tick, +0x48 finished flag; ticks: *(0x2ab4100)+0x20, 8192 per second
//   wall (0x2ac4428): +0x54 touching wall; wheel visuals (0x2ac5438): +0x130 wetness
//   camera: *(0x2ab2538) +0x20 +0x20 [0] = camera system; +0x200 view-projection (4 rows of 4 floats)
#include "players_esp.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "imgui.h"

namespace astro {
namespace {

constexpr std::uint64_t kG = 0x800000000ull;
constexpr std::uint64_t kRacerMgr = kG + 0x2ad4b70, kCameraMgr = kG + 0x2ab2538, kClock = kG + 0x2ab4100;
constexpr std::uint64_t kMetaKart = kG + 0x2ac4580, kMetaBoost = kG + 0x2ac4018, kMetaWall = kG + 0x2ac4428,
                        kMetaWheel = kG + 0x2ac5438;
constexpr float kInchToKmh = 0.0254f * 3.6f;

struct Options {
    bool on = false;
    bool box = true, line = false, mine = true;
    bool speed = true, air = true, boost = true, angle = true, surface = true, wall = true, wet = true;
    float max_km = 0.4f;   // farther karts are not labelled (0.4 km ~ the length of a straight)
};
Options o;

bool Rd(std::uint64_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
std::uint64_t Q(std::uint64_t a) { std::uint64_t v = 0; return Rd(a, &v, 8) ? v : 0; }
std::uint32_t U(std::uint64_t a) { std::uint32_t v = 0; return Rd(a, &v, 4) ? v : 0; }
float F(std::uint64_t a) { float v = 0; return Rd(a, &v, 4) && std::isfinite(v) ? v : 0.f; }
bool B(std::uint64_t a) { std::uint8_t v = 0; return Rd(a, &v, 1) && v; }
bool Heap(std::uint64_t p) { return p >= 0x1000000000ull && p < 0x8000000000ull; }

// igObjectList: count +0xc, items +0x20
int List(std::uint64_t l, std::uint64_t* out, int cap) {
    if (!Heap(l)) return 0;
    const int n = int(U(l + 0xc));
    const std::uint64_t d = Q(l + 0x20);
    if (n <= 0 || n > 4096 || !Heap(d)) return 0;
    const int m = std::min(n, cap);
    return Rd(d, out, size_t(m) * 8) ? m : 0;
}

// the meta global a component's getMeta returns, cached per vtable
std::uint64_t MetaOf(std::uint64_t comp) {
    static std::unordered_map<std::uint64_t, std::uint64_t> cache;
    const std::uint64_t vt = Q(comp);
    if (!vt) return 0;
    auto it = cache.find(vt);
    if (it != cache.end()) return it->second;
    std::uint64_t meta = 0;
    const std::uint64_t fn = Q(vt);
    std::uint8_t b[8] = {};
    if (fn && Rd(fn, b, 8) && b[0] == 0x48 && b[1] == 0x8b && b[2] == 0x05 && b[7] == 0xc3) {
        std::int32_t disp;
        std::memcpy(&disp, b + 3, 4);
        meta = fn + 7 + std::int64_t(disp);
    }
    if (cache.size() < 4096) cache[vt] = meta;
    return meta;
}

struct Kart {
    std::uint64_t entity = 0, kart = 0, boost = 0, wall = 0, wheel = 0;
    float pos[3] = {}, vel[3] = {};
};

bool ReadKart(std::uint64_t racer, Kart& k) {
    const std::uint64_t h = Q(racer + 0x20);
    if (!Heap(h)) return false;
    const std::uint32_t flags = U(h);
    if (flags & 0x30000000u) return false;   // indirect handle (not seen for karts)
    k.entity = Q(h + 0x28);
    if (!Heap(k.entity)) return false;
    std::uint64_t comps[96];
    const int n = List(Q(k.entity + 0x18), comps, 96);
    for (int i = 0; i < n; ++i) {
        const std::uint64_t m = Heap(comps[i]) ? MetaOf(comps[i]) : 0;
        if (m == kMetaKart) k.kart = comps[i];
        else if (m == kMetaBoost) k.boost = comps[i];
        else if (m == kMetaWall) k.wall = comps[i];
        else if (m == kMetaWheel) k.wheel = comps[i];
    }
    if (!k.kart) return false;   // not a kart
    return Rd(k.entity + 0x20, k.pos, 12) && Rd(k.entity + 0x60, k.vel, 12) && std::isfinite(k.pos[0]);
}

// view-projection of the first camera system
bool Camera(float m[16]) {
    const std::uint64_t mgr = Q(kCameraMgr);
    const std::uint64_t sys = Heap(mgr) ? Q(Q(Q(mgr + 0x20) + 0x20)) : 0;
    return Heap(sys) && Rd(sys + 0x200, m, 64) && std::isfinite(m[15]);
}

// game rows: clip = x*row0 + y*row1 + z*row2 + row3; screen from the camera's viewport (left 0 / top 0)
bool Project(const float m[16], const float p[3], ImVec2* s, float* depth) {
    float c[4];
    for (int j = 0; j < 4; ++j) c[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
    if (c[3] < 1.0f) return false;   // behind the camera or on it
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    s->x = (c[0] / c[3] + 1.f) * 0.5f * ds.x;
    s->y = (1.f - c[1] / c[3]) * 0.5f * ds.y;
    *depth = c[3];
    return s->x > -ds.x && s->x < 2 * ds.x && s->y > -ds.y && s->y < 2 * ds.y;
}

// seconds of boost left and spent (the formula of the game's Inspect Kart, without its rarely used extra term)
void BoostTimes(std::uint64_t boost, int* active, float* left, float* spent) {
    std::uint64_t items[16];
    const int n = List(Q(boost + 0x28), items, 16);
    *active = 0; *left = 0; *spent = 0;
    const std::uint32_t now = U(Q(kClock) + 0x20);
    std::uint32_t first = 0xffffffffu;
    for (int i = 0; i < n; ++i) {
        if (!Heap(items[i]) || B(items[i] + 0x48)) continue;
        ++*active;
        const float t = F(Q(items[i] + 0x10) + 0x1c) + F(items[i] + 0x28) - F(items[i] + 0x18);
        *left = std::max(*left, t);
        first = std::min(first, U(items[i] + 0x38));
    }
    if (*active && first != 0xffffffffu && now >= first) *spent = float(now - first) / 8192.f;
}

std::string Surface(std::uint64_t kart) {
    if (!B(kart + 0x214)) return "in the air";
    const std::uint64_t h = Q(kart + 0x230);
    if (!Heap(h) || (U(h) & 0x10000000u)) return "-";
    char s[48] = {};
    const std::uint64_t p = Q(h + 8);
    if (!p || !Rd(p, s, sizeof s - 1)) return "-";
    for (char& c : s) if (c && (c < 32 || c > 126)) return "-";
    return s[0] ? s : "-";
}

void Label(ImDrawList* dl, ImVec2 at, const char* text, ImU32 col) {
    const ImVec2 sz = ImGui::CalcTextSize(text);
    const ImVec2 a(at.x - sz.x * 0.5f - 6, at.y - sz.y - 6);
    dl->AddRectFilled(a, ImVec2(a.x + sz.x + 12, at.y), IM_COL32(0, 0, 0, 160), 5);
    dl->AddRect(a, ImVec2(a.x + sz.x + 12, at.y), col, 5);
    dl->AddText(ImVec2(a.x + 6, a.y + 3), IM_COL32(255, 255, 255, 255), text);
}

int g_shown = 0, g_racers = 0;
bool g_online = false, g_camera = false;

}  // namespace

bool PlayersEsp_Enabled() { return o.on; }

void PlayersEsp_Draw() {
    g_shown = g_racers = 0;
    const std::uint64_t mgr = Q(kRacerMgr);
    if (!Heap(mgr)) return;
    std::uint64_t racers[16];
    g_racers = List(Q(mgr + 0x10), racers, 16);
    std::uint64_t net[16];
    g_online = List(Q(mgr + 0x28), net, 16) > 0;   // _racerNetInfos: an online race
    float m[16];
    g_camera = Camera(m);
    if (!g_camera) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    for (int i = 0; i < g_racers; ++i) {
        const bool me = Heap(racers[i]) && Heap(Q(racers[i] + 0x10));
        const bool bot = Heap(racers[i]) && B(racers[i] + 0x40);
        // only your kart and AI karts: other human players are never shown (Comp Hub races list bots as net racers too)
        if (!me && !bot) continue;
        if (me && !o.mine) continue;
        Kart k;
        if (!Heap(racers[i]) || !ReadKart(racers[i], k)) continue;
        ImVec2 base, top;
        float d0, d1;
        const float up[3] = {k.pos[0], k.pos[1], k.pos[2] + 45.f};   // ~1.1 m above the kart's origin
        if (!Project(m, k.pos, &base, &d0) || !Project(m, up, &top, &d1)) continue;
        const float speed = std::sqrt(k.vel[0] * k.vel[0] + k.vel[1] * k.vel[1] + k.vel[2] * k.vel[2]) * kInchToKmh;
        const ImU32 col = me ? IM_COL32(255, 140, 30, 255) : IM_COL32(80, 200, 255, 255);
        const float hgt = std::max(8.f, base.y - top.y);
        if (o.box) dl->AddRect(ImVec2(base.x - hgt * 0.8f, top.y), ImVec2(base.x + hgt * 0.8f, base.y), col, 3.f, 0, 2.f);
        if (o.line && !me) dl->AddLine(ImVec2(ds.x * 0.5f, ds.y), base, col, 1.5f);
        if (d0 * 0.0000254f > o.max_km) { ++g_shown; continue; }   // far: box only (w ~ distance in inches)

        char buf[512];
        int n = me ? std::snprintf(buf, sizeof buf, "YOU") : std::snprintf(buf, sizeof buf, "CPU %d", i + 1);
        auto add = [&](const char* fmt, auto... v) {
            if (n < int(sizeof buf) - 1) n += std::snprintf(buf + n, sizeof buf - n, fmt, v...);
        };
        if (o.speed) add("\n%.0f km/h", speed);
        if (o.boost && k.boost) {
            int act; float left, spent;
            BoostTimes(k.boost, &act, &left, &spent);
            if (act) add("\nBOOST %.0f km/h  left %.2f s  (%.2f s in)", F(k.boost + 0x38) * kInchToKmh, left, spent);
            else add("\nboost: -");
        }
        if (o.air) { const float a = F(k.kart + 0x248); if (a > 0.f) add("\nair %.2f s", a); }
        if (o.angle) add("\nground %.1f deg", std::acos(std::clamp(F(k.kart + 0x210), -1.f, 1.f)) * 57.29578f);
        if (o.surface) add("\nsurface %s", Surface(k.kart).c_str());
        if (o.wall && k.wall && B(k.wall + 0x54)) add("\nWALL");
        if (o.wet && k.wheel) { const float w = F(k.wheel + 0x130); if (w > 0.001f) add("\nwet %.2f", w); }
        Label(dl, ImVec2(base.x, top.y - 4), buf, col);
        ++g_shown;
    }
}

void PlayersEsp_Tab() {
    ImGui::Checkbox("Show karts on the screen (ESP)", &o.on);
    ImGui::BeginDisabled(!o.on);
    ImGui::Checkbox("Box", &o.box);
    ImGui::SameLine();
    ImGui::Checkbox("Line to each kart", &o.line);
    ImGui::SameLine();
    ImGui::Checkbox("My kart too", &o.mine);
    ImGui::SeparatorText("Information above each kart");
    ImGui::Checkbox("Speed", &o.speed);
    ImGui::SameLine();
    ImGui::Checkbox("Boost (speed, time left)", &o.boost);
    ImGui::SameLine();
    ImGui::Checkbox("Air time", &o.air);
    ImGui::Checkbox("Ground angle", &o.angle);
    ImGui::SameLine();
    ImGui::Checkbox("Surface", &o.surface);
    ImGui::SameLine();
    ImGui::Checkbox("Touching wall", &o.wall);
    ImGui::SameLine();
    ImGui::Checkbox("Wetness", &o.wet);
    ImGui::SliderFloat("Label distance", &o.max_km, 0.05f, 2.0f, "%.2f km");
    ImGui::EndDisabled();
    ImGui::Separator();
    if (!o.on) ImGui::TextDisabled("off");
    else if (!g_camera) ImGui::TextColored(ImVec4(1, 0.8f, 0.3f, 1), "waiting for the game camera");
    else ImGui::Text("racers: %d   shown: %d", g_racers, g_shown);
    ImGui::TextDisabled("Shown: your kart and AI karts. Other human players are never shown.");
    ImGui::TextDisabled("Read only: nothing is written to the game.");
}

}  // namespace astro
