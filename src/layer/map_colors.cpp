#include "map_colors.h"
#include "switches.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../config/config.h"
#include "../config/json.h"
#include "../diagnostics/log.h"

namespace astro {

namespace {

// The map's water is a FlowWater material (seen in Crash Cove 2026-10-31). Its values are shader-constant objects:
//   vector value (vtable 0x8027b0e08): +0x10 the constant (its name at +0x10), +0x20 the value (4 floats)
//   float value  (vtable 0x8027b1448): +0x10 the constant, +0x18 the value
// in this order: deepWaterColor, churnWaterColor, flowProperties (0x30 apart), then the floats (absorbtion ...).
// When the level loads they are packed into the material's constant block that the GPU reads from:
//   deepWaterColor, churnWaterColor, flowProperties, choppy params, absorbtion (16 bytes apart).
// Writing the packed block changes the water at once; the objects are written too so a race restart keeps it.
// absorbtion is how fast the water hides what is under it (0.01 in Crash Cove: clear; 1.0: solid colour).
constexpr std::uint64_t kVecVtable = 0x8027b0e08ull, kFloatVtable = 0x8027b1448ull;
constexpr std::uint64_t kLo = 0x1000000000ull, kHi = 0x1100000000ull;
constexpr float kOpaqueAbsorbtion = 1.0f;

std::thread g_thread;
std::atomic<bool> g_stop{false};

struct Options { bool color_on = false, opaque = false; float rgb[3] = {0.22f, 0.04f, 0.32f}; };

// one water material: its objects and every packed copy of its values
struct Water {
    std::uint64_t deep_obj = 0, absorb_obj = 0;
    float deep_orig[4] = {}, absorb_orig = 0;
    unsigned char pattern[32] = {};          // churnWaterColor + flowProperties: finds the packed copies
    std::vector<std::uint64_t> packed;       // address of churnWaterColor in each copy
};
std::vector<Water> g_water;
bool g_applied = false;   // ASTRO's values are in the game (so turning off puts the originals back)

bool Read(std::uint64_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
bool Write(std::uint64_t a, const void* in, size_t n) {
    JournalWrite("maps", a, in, n);
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), in, n, &put) && put == n;
}
std::uint64_t Q(std::uint64_t a) { std::uint64_t v = 0; Read(a, &v, 8); return v; }

std::string ConstName(std::uint64_t value_obj) {
    const std::uint64_t c = Q(value_obj + 0x10);
    if (c < kLo || c >= kHi) return {};
    const std::uint64_t s = Q(c + 0x10);
    if (s < kLo || s >= kHi) return {};
    char b[64] = {};
    if (!Read(s, b, sizeof b - 1)) return {};
    return b;
}
bool IsValue(std::uint64_t o, std::uint64_t vtable) { return Q(o) == vtable && (Q(o + 8) & 0xffff) == 1; }

// every committed read-write region of the game's heap, read in pieces
template <class F> void ForEachChunk(F&& f) {
    std::vector<unsigned char> buf;
    MEMORY_BASIC_INFORMATION m;
    for (std::uint64_t a = kLo; a < kHi && !g_stop && VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m); a = std::uint64_t(m.BaseAddress) + m.RegionSize) {
        if (m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE) continue;
        const std::uint64_t base = std::uint64_t(m.BaseAddress), size = m.RegionSize;
        for (std::uint64_t off = 0; off < size; off += (16u << 20)) {
            const size_t n = size_t(std::min<std::uint64_t>(16u << 20, size - off) + (off + (16u << 20) < size ? 64 : 0));
            buf.resize(n);
            SIZE_T got = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(base + off), buf.data(), n, &got) || got < 64) continue;
            f(base + off, buf.data(), size_t(got));
        }
    }
}

void Scan() {
    std::vector<Water> found;
    // 1. the materials: a churnWaterColor value followed by flowProperties
    ForEachChunk([&](std::uint64_t at, const unsigned char* d, size_t n) {
        for (size_t i = 0; i + 0x60 <= n; i += 8) {
            std::uint64_t v;
            std::memcpy(&v, d + i, 8);
            if (v != kVecVtable) continue;
            const std::uint64_t o = at + i;
            if (ConstName(o) != "flowwater__churnWaterColor" || !IsValue(o - 0x30, kVecVtable) || ConstName(o - 0x30) != "flowwater__deepWaterColor" ||
                !IsValue(o + 0x30, kVecVtable) || ConstName(o + 0x30) != "flowwater__flowProperties")
                continue;
            Water w;
            w.deep_obj = o - 0x30;
            if (!Read(o + 0x20, w.pattern, 16) || !Read(o + 0x50, w.pattern + 16, 16) || !Read(w.deep_obj + 0x20, w.deep_orig, 16)) continue;
            for (std::uint64_t f = o + 0x60; f < o + 0x300; f += 0x20)
                if (IsValue(f, kFloatVtable) && ConstName(f) == "flowwater__absorbtion") { w.absorb_obj = f; Read(f + 0x18, &w.absorb_orig, 4); break; }
            found.push_back(w);
        }
    });
    // 2. their packed blocks: the pattern with the deep colour 16 bytes before it and absorbtion 0x30 after it.
    //    The GPU's frame copies of a block come and go every frame: only blocks still there a second later are kept
    //    (writing a copy that was already reused would change memory that is no longer the water).
    if (!found.empty())
        ForEachChunk([&](std::uint64_t at, const unsigned char* d, size_t n) {
            for (size_t i = 0x10; i + 0x40 <= n; i += 4)
                for (Water& w : found)
                    if (std::memcmp(d + i, w.pattern, 32) == 0 && std::memcmp(d + i - 0x10, w.deep_orig, 16) == 0 &&
                        std::memcmp(d + i + 0x30, &w.absorb_orig, 4) == 0) { w.packed.push_back(at + i); break; }
        });
    Sleep(1000);
    for (Water& w : found) {
        std::vector<std::uint64_t> stable;
        for (std::uint64_t p : w.packed) {
            unsigned char cur[0x40];
            if (Read(p - 0x10, cur, sizeof cur) && std::memcmp(cur + 0x10, w.pattern, 32) == 0 && std::memcmp(cur, w.deep_orig, 16) == 0) stable.push_back(p);
        }
        w.packed = std::move(stable);
    }
    g_water = std::move(found);
    size_t copies = 0;
    for (auto& w : g_water) copies += w.packed.size();
    Logger::Instance().Info("maps: water materials " + std::to_string(g_water.size()) + ", packed copies " + std::to_string(copies));
}

// this map's water is still loaded (a new level frees it). A block that no longer holds the water is dropped and never
// written again; the heap is searched again only when the map changed or no block is left.
bool StillThere() {
    if (g_water.empty()) return false;
    size_t blocks = 0;
    for (auto& w : g_water) {
        if (!IsValue(w.deep_obj, kVecVtable) || ConstName(w.deep_obj) != "flowwater__deepWaterColor") return false;
        w.packed.erase(std::remove_if(w.packed.begin(), w.packed.end(), [&](std::uint64_t p) {
                           unsigned char cur[32];
                           return !Read(p, cur, 32) || std::memcmp(cur, w.pattern, 32) != 0;
                       }),
                       w.packed.end());
        blocks += w.packed.size();
    }
    return blocks > 0;
}

void Apply(const Options& o) {
    for (auto& w : g_water) {
        float col[4] = {w.deep_orig[0], w.deep_orig[1], w.deep_orig[2], w.deep_orig[3]};
        if (o.color_on) { col[0] = o.rgb[0]; col[1] = o.rgb[1]; col[2] = o.rgb[2]; col[3] = 1.0f; }
        const float absorb = o.opaque ? kOpaqueAbsorbtion : w.absorb_orig;
        auto put = [](std::uint64_t a, const void* v, size_t n) {
            unsigned char cur[16];
            if (Read(a, cur, n) && std::memcmp(cur, v, n) != 0) Write(a, v, n);
        };
        put(w.deep_obj + 0x20, col, 16);
        if (w.absorb_obj) put(w.absorb_obj + 0x18, &absorb, 4);
        for (std::uint64_t p : w.packed) {
            unsigned char cur[32];   // checked again right before writing
            if (!Read(p, cur, 32) || std::memcmp(cur, w.pattern, 32) != 0) continue;
            put(p - 0x10, col, 16);
            put(p + 0x30, &absorb, 4);
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
    if (const json::Value* v = root->Find("water_color_on"); v && v->type == json::Value::Type::Bool) o.color_on = v->b;
    if (const json::Value* v = root->Find("water_opaque"); v && v->type == json::Value::Type::Bool) o.opaque = v->b;
    if (const json::Value* a = root->Find("water_color"); a && a->type == json::Value::Type::Array && a->a && a->a->size() >= 3)
        for (int i = 0; i < 3; ++i) if ((*a->a)[i].type == json::Value::Type::Number) o.rgb[i] = float((*a->a)[i].n);
    return o;
}

void WriteStatus() {
    size_t copies = 0;
    for (auto& w : g_water) copies += w.packed.size();
    char b[160];
    std::snprintf(b, sizeof b, "{\n  \"water_materials\": %zu,\n  \"water_copies\": %zu\n}\n", g_water.size(), copies);
    const auto dir = DefaultConfigPath().parent_path();
    const auto tmp = dir / "maps_status.json.tmp";
    if (std::FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        std::fputs(b, f);
        std::fclose(f);
        MoveFileExW(tmp.c_str(), (dir / "maps_status.json").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

void Run() {
    NameThread(L"astro:water");
    ULONGLONG last_scan = 0;
    bool nothing_found = false;   // the last search found no water block: search less often
    while (!g_stop) {
        Sleep(1000);
        const Options o = ReadOptions();
        const bool want = o.color_on || o.opaque;
        if (!want && !g_applied) continue;
        if (!StillThere()) {
            // a scan reads the whole heap (about a second): again after 3 s, or 15 s when the map has no such water
            const ULONGLONG now = GetTickCount64();
            if (now - last_scan < (Loading() ? 1500u : nothing_found && last_scan ? 15000u : 3000u)) continue;
            last_scan = now;
            Scan();
            WriteStatus();
            nothing_found = !StillThere();
            if (nothing_found) continue;
        }
        Apply(o);
        g_applied = want;
    }
}

}  // namespace

void MapColors_Start() {
    if (g_thread.joinable()) return;
    g_stop = false;
    g_thread = std::thread(Run);
}

void MapColors_Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace astro
