#include "zone_light.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../config/config.h"
#include "../config/json.h"
#include "../diagnostics/log.h"
#include "switches.h"

namespace astro {

namespace {

// The map's lighting lives in an object of the zone lighting class (vtable 0x8027b... seen 2026-11-01: 0x80273ece8 is
// the one the renderer uses; writing its fog colour turned the fog of Deep Sea Driving red, saturation -100 turned
// Electron Avenue grey). Field offsets come from the game's own igMetaField records (read live 2026-11-01):
//   +0x0c..+0x5f  one "state" byte per field (0 or 1)        +0x80 _sunColor (4 floats)   +0x90 _sunIntensity
//   +0x2ac/+0x300/+0x354 _ambientDiffuseColor Red/Green/Blue (21 floats each, a gradient)
//   +0x3e0 _fogDensity   +0x3f0 _fogColor (4 floats)   +0x468 _vignetteStrength
//   +0x474 exposure  +0x478 hue  +0x47c saturation  +0x480 brightness  +0x484 contrast (percent-like, 0 = neutral)
//   +0x490 _colorGlobalTint (4 floats, 1 1 1 1 = neutral)
// A level change frees the object and the memory is reused at once, so a write happens only right after checking the
// object is still there (vtable + state bytes), and only over the game's value or the value written last.
constexpr std::uint64_t kVtable = 0x80273ece8ull;
constexpr std::uint64_t kLo = 0x1000000000ull, kHi = 0x1100000000ull;

struct Options {
    bool fog_on = false, sun_on = false, amb_on = false, grade_on = false, tint_on = false;
    float fog[3] = {1, 1, 1}, fog_density = 100;   // density: percent of the game's
    float sun[3] = {1, 1, 1}, sun_power = 100;     // percent of the game's
    float amb[3] = {1, 1, 1};                      // multiplies the ambient light (white = unchanged)
    float exposure = 0, hue = 0, sat = 0, bright = 0, contrast = 0, vignette = 0;   // added to the game's
    float tint[3] = {1, 1, 1};
    bool wl_on = false;                            // "light colour of the whole zone": one colour for fog, sun, ambient, sky gradient, sun shafts
    float wl[3] = {1, 0, 0};
    bool Any() const { return fog_on || sun_on || amb_on || grade_on || tint_on || wl_on; }
};

struct Field { std::uint32_t off, n; };
const Field kFields[] = {{0x3e0, 1}, {0x3f0, 3}, {0x80, 3}, {0x90, 1}, {0x2ac, 21}, {0x300, 21}, {0x354, 21}, {0x468, 1},
                         {0x474, 1}, {0x478, 1}, {0x47c, 1}, {0x480, 1}, {0x484, 1}, {0x490, 3},
                         {0xa4, 21}, {0xf8, 21}, {0x14c, 21}, {0x1c0, 3}};   // sky gradient red/green/blue, sun shafts colour
constexpr size_t kNF = sizeof kFields / sizeof kFields[0];

struct Target {
    std::uint64_t obj;
    std::vector<float> orig[kNF], last[kNF];   // the game's value of each field, and what was written last
};
std::vector<Target> g_targets;
std::thread g_thread;
std::atomic<bool> g_stop{false};
bool g_applied = false;

bool Read(std::uint64_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
bool Write(std::uint64_t a, const void* in, size_t n) {
    JournalWrite("light", a, in, n);
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), in, n, &put) && put == n;
}

// the object is (still) a zone lighting object
bool Verified(std::uint64_t o) {
    unsigned char d[0x60];
    if (!Read(o, d, sizeof d)) return false;
    std::uint64_t vt;
    std::memcpy(&vt, d, 8);
    if (vt != kVtable) return false;
    for (int i = 0x0c; i < 0x60; ++i) if (d[i] > 1) return false;
    return true;
}

std::vector<float> ReadField(std::uint64_t o, const Field& f) {
    std::vector<float> v(f.n);
    if (!Read(o + f.off, v.data(), f.n * 4)) v.clear();
    return v;
}

void Scan() {
    std::vector<Target> found;
    std::vector<unsigned char> buf;
    MEMORY_BASIC_INFORMATION m;
    for (std::uint64_t a = kLo; a < kHi && !g_stop && VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m); a = std::uint64_t(m.BaseAddress) + m.RegionSize) {
        if (m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE) continue;
        const std::uint64_t base = std::uint64_t(m.BaseAddress), size = m.RegionSize;
        for (std::uint64_t off = 0; off < size; off += (16u << 20)) {
            const size_t n = size_t(std::min<std::uint64_t>(16u << 20, size - off));
            buf.resize(n);
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + off), buf.data(), n, &got)) continue;
            for (size_t i = 0; i + 8 <= got; i += 8) {
                std::uint64_t v;
                std::memcpy(&v, &buf[i], 8);
                if (v != kVtable || !Verified(base + off + i)) continue;
                Target t;
                t.obj = base + off + i;
                bool ok = true;
                for (size_t k = 0; k < kNF && ok; ++k) {
                    t.orig[k] = ReadField(t.obj, kFields[k]);
                    ok = t.orig[k].size() == kFields[k].n && std::all_of(t.orig[k].begin(), t.orig[k].end(), [](float x) { return std::isfinite(x); });
                    t.last[k] = t.orig[k];
                }
                if (ok) found.push_back(std::move(t));
            }
        }
    }
    g_targets = std::move(found);
    Logger::Instance().Info("maps light: " + std::to_string(g_targets.size()) + " lighting objects");
}

bool StillThere() {
    if (g_targets.empty()) return false;
    for (auto& t : g_targets) if (!Verified(t.obj)) return false;
    return true;
}

// what a field should hold, from the game's value and the options
std::vector<float> Want(size_t k, const std::vector<float>& g, const Options& o) {
    std::vector<float> w = g;
    const std::uint32_t off = kFields[k].off;
    auto add = [&](bool on, float d) { if (on) w[0] = g[0] + d; };
    if (o.wl_on) {   // the whole zone's light in one colour (measured in Roo's Tubes 2026-11-01: sun, fog, ambient, sky gradient, sun shafts)
        switch (off) {
        case 0x3f0: case 0x80: case 0x1c0: for (int i = 0; i < 3; ++i) w[i] = o.wl[i]; return w;
        case 0x2ac: case 0xa4: std::fill(w.begin(), w.end(), o.wl[0]); return w;
        case 0x300: case 0xf8: std::fill(w.begin(), w.end(), o.wl[1]); return w;
        case 0x354: case 0x14c: std::fill(w.begin(), w.end(), o.wl[2]); return w;
        }
    }
    switch (off) {
    case 0x3e0: if (o.fog_on) w[0] = g[0] * o.fog_density / 100.f; break;
    case 0x3f0: if (o.fog_on) for (int i = 0; i < 3; ++i) w[i] = o.fog[i]; break;
    case 0x80: if (o.sun_on) for (int i = 0; i < 3; ++i) w[i] = o.sun[i]; break;
    case 0x90: if (o.sun_on) w[0] = g[0] * o.sun_power / 100.f; break;
    case 0x2ac: case 0x300: case 0x354:
        if (o.amb_on) { const float m = o.amb[off == 0x2ac ? 0 : off == 0x300 ? 1 : 2] * 2.f; for (float& x : w) x *= m; }   // white x1, black x0
        break;
    case 0x468: if (o.grade_on) w[0] = std::clamp(g[0] + o.vignette, 0.f, 1.f); break;
    case 0x474: add(o.grade_on, o.exposure); break;
    case 0x478: add(o.grade_on, o.hue); break;
    case 0x47c: add(o.grade_on, o.sat); break;
    case 0x480: add(o.grade_on, o.bright); break;
    case 0x484: add(o.grade_on, o.contrast); break;
    case 0x490: if (o.tint_on) for (int i = 0; i < 3; ++i) w[i] = o.tint[i]; break;
    }
    return w;
}

void Apply(const Options& o) {
    for (auto& t : g_targets) {
        for (size_t k = 0; k < kNF; ++k) {
            std::vector<float> cur = ReadField(t.obj, kFields[k]);
            if (cur.size() != kFields[k].n) continue;
            if (cur != t.orig[k] && cur != t.last[k]) {
                // the game changed it (another zone of the map): that is its value now, if the object is still there
                if (!Verified(t.obj) || !std::all_of(cur.begin(), cur.end(), [](float x) { return std::isfinite(x); })) continue;
                t.orig[k] = cur;
                t.last[k] = cur;
            }
            const std::vector<float> want = Want(k, t.orig[k], o);
            if (want == cur) continue;
            if (!Verified(t.obj)) break;   // checked right before the write
            if (Write(t.obj + kFields[k].off, want.data(), want.size() * 4)) t.last[k] = want;
        }
    }
}

Options ReadOptions() {
    Options o;
    std::FILE* f = _wfopen((DefaultConfigPath().parent_path() / "maps.json").c_str(), L"rb");
    if (!f) return o;
    std::string s;
    char b[1024];
    for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
    std::fclose(f);
    auto root = json::Parse(s);
    if (!root) return o;
    auto flag = [&](const char* k, bool& v) { if (const json::Value* x = root->Find(k); x && x->type == json::Value::Type::Bool) v = x->b; };
    auto num = [&](const char* k, float& v) { if (const json::Value* x = root->Find(k); x && x->type == json::Value::Type::Number) v = float(x->n); };
    auto rgb = [&](const char* k, float* v) {
        if (const json::Value* a = root->Find(k); a && a->type == json::Value::Type::Array && a->a && a->a->size() >= 3)
            for (int i = 0; i < 3; ++i) if ((*a->a)[i].type == json::Value::Type::Number) v[i] = float((*a->a)[i].n);
    };
    flag("fog_on", o.fog_on); rgb("fog_color", o.fog); num("fog_density", o.fog_density);
    flag("sun_on", o.sun_on); rgb("sun_color", o.sun); num("sun_power", o.sun_power);
    flag("amb_on", o.amb_on); rgb("amb_color", o.amb);
    flag("grade_on", o.grade_on);
    num("exposure", o.exposure); num("hue", o.hue); num("saturation", o.sat); num("brightness", o.bright); num("contrast", o.contrast);
    num("vignette", o.vignette);
    flag("tint_on", o.tint_on); rgb("tint_color", o.tint);
    flag("wl_on", o.wl_on); rgb("wl_color", o.wl);
    return o;
}

void Run() {
    NameThread(L"astro:light");
    ULONGLONG last_scan = 0;
    bool nothing = false;
    while (!g_stop) {
        Sleep(500);
        const Options o = ReadOptions();
        if (!o.Any() && !g_applied) continue;
        if (!StillThere()) {
            // a new map: find its lighting again (a heap pass, about a second); less often while nothing is found
            const ULONGLONG now = GetTickCount64();
            if (now - last_scan < (Loading() ? 1500u : nothing && last_scan ? 10000u : 3000u)) continue;
            last_scan = now;
            Scan();
            nothing = g_targets.empty();
            if (nothing) continue;
        }
        Apply(o);
        g_applied = o.Any();
    }
}

}  // namespace

void ZoneLight_Start() {
    if (g_thread.joinable()) return;
    g_stop = false;
    g_thread = std::thread(Run);
}

void ZoneLight_Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace astro
