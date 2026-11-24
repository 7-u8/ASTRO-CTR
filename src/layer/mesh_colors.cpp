#include "mesh_colors.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../config/config.h"
#include "../config/json.h"
#include "../diagnostics/log.h"
#include "switches.h"

namespace astro {
namespace {

// The TNT and Nitro crates are grey wood coloured by their vertex colours (TNT ff2020ff red + fff220ff yellow, Nitro
// 54c456ff green + fffd43ff yellow, bytes r g b a). Their vertex buffers are in read-only pages of the game, like the
// textures (found once each in a race, 2026-11-01): written with an ordinary copy, the emulator uploads them again
// (same path as wheel_editor.cpp). A buffer is found by its 64-byte probe; the places of each colour are remembered
// when it is found, and a place is only written while it holds the game's colour or the one written last.
struct Colour { std::string key; std::uint32_t orig = 0; };
struct Mesh { std::string name; std::vector<unsigned char> probe; std::vector<Colour> colours; };
struct Found {
    const Mesh* mesh = nullptr;
    uintptr_t base = 0, size = 0;
    std::map<std::string, std::vector<uintptr_t>> places;   // colour key -> addresses
    std::map<std::string, std::uint32_t> last;               // colour key -> written last
};

// Material colours (potions, shield domes): 3 or 4 floats in read-write memory, each one right after a colour object
// header whose first qword is the same pointer for all of them (seen 2026-11-01: 16 or 12 bytes before the floats).
// The same float values also sit in ~130 unrelated places without that pointer: those are never touched.
struct Vec { std::string key; std::vector<unsigned char> orig; };
struct Place { uintptr_t at; const Vec* vec; std::vector<unsigned char> last; };
std::vector<Vec> g_vecs;
std::vector<Place> g_places;
std::uint64_t g_header = 0;   // the pointer in front of every material colour (found at run time)
std::vector<Mesh> g_meshes;
std::vector<Found> g_found;
std::thread g_thread;
std::atomic<bool> g_stop{false};

std::filesystem::path DllDir() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DllDir), &m);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(m, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}
std::string ReadAll(const std::filesystem::path& p) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string s;
    char b[4096];
    DWORD n = 0;
    while (ReadFile(h, b, sizeof b, &n, nullptr) && n) s.append(b, n);
    CloseHandle(h);
    return s;
}
std::vector<unsigned char> Hex(const std::string& h) {
    std::vector<unsigned char> v;
    for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back(static_cast<unsigned char>(std::stoul(h.substr(i, 2), nullptr, 16)));
    return v;
}
std::uint32_t Rgba(const std::string& h) {   // "rrggbbaa" -> the 4 bytes as they sit in memory
    const auto v = Hex(h);
    std::uint32_t x = 0;
    if (v.size() == 4) std::memcpy(&x, v.data(), 4);
    return x;
}
bool ReadSelf(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
void WriteSelf(uintptr_t a, std::uint32_t v) {
    JournalWrite("mesh", a, &v, 4);
    std::memcpy(reinterpret_cast<void*>(a), &v, 4);   // an ordinary store: the emulator sees it and uploads the buffer
}

void LoadCatalog() {
    auto root = json::Parse(ReadAll(DllDir() / "mesh_catalog.json"));
    if (!root) return;
    const json::Value* arr = root->Find("meshes");
    if (arr && arr->type == json::Value::Type::Array && arr->a)
    for (auto& e : *arr->a) {
        Mesh m;
        if (const json::Value* v = e.Find("name"); v && v->type == json::Value::Type::String) m.name = v->s;
        if (const json::Value* v = e.Find("probe"); v && v->type == json::Value::Type::String) m.probe = Hex(v->s);
        if (const json::Value* c = e.Find("colours"); c && c->type == json::Value::Type::Array && c->a)
            for (auto& x : *c->a) {
                const json::Value* k = x.Find("key");
                const json::Value* r = x.Find("rgba");
                if (k && r && k->type == json::Value::Type::String && r->type == json::Value::Type::String) m.colours.push_back({k->s, Rgba(r->s)});
            }
        if (m.probe.size() == 64 && !m.colours.empty()) g_meshes.push_back(std::move(m));
    }
    if (const json::Value* va = root->Find("vectors"); va && va->type == json::Value::Type::Array && va->a)
        for (auto& e : *va->a) {
            const json::Value* k = e.Find("key");
            const json::Value* v = e.Find("value");
            if (!k || !v || k->type != json::Value::Type::String || v->type != json::Value::Type::String) continue;
            const auto b = Hex(v->s);
            if (b.size() != 12 && b.size() != 16) continue;
            g_vecs.push_back({k->s, b});
        }
}

// what the app asks for: {"tnt_body": "rrggbbaa", ...}; a key that is missing = the game's own colour
std::map<std::string, std::uint32_t> ReadEdits() {
    std::map<std::string, std::uint32_t> e;
    auto root = json::Parse(ReadAll(DefaultConfigPath().parent_path() / "mesh_edits.json"));
    if (!root || root->type != json::Value::Type::Object || !root->o) return e;
    for (auto& [k, v] : *root->o)
        if (v.type == json::Value::Type::String && v.s.size() == 8) e[k] = Rgba(v.s);
    return e;
}

// read-only regions under 4 MB that hold a mesh's probe (each crate's buffer was in one of 0x18000 / 0x20000 bytes)
void Scan() {
    const ULONGLONG t0 = GetTickCount64();
    std::vector<Found> found;
    std::vector<unsigned char> buf;
    MEMORY_BASIC_INFORMATION m{};
    for (uintptr_t a = 0x1000000000ull; a < 0x8000000000ull && !g_stop; a = uintptr_t(m.BaseAddress) + m.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
        // crate vertex buffers were regions of 0x18000 / 0x20000 bytes: bigger ones (textures) are not read
        if (m.State != MEM_COMMIT || m.Protect != PAGE_READONLY || m.RegionSize > (1u << 20)) continue;
        buf.resize(m.RegionSize);
        if (!ReadSelf(uintptr_t(m.BaseAddress), buf.data(), buf.size())) continue;
        for (const Mesh& mesh : g_meshes) {
            // the probe's colour dwords are skipped: once written they hold ASTRO's colours
            auto is_colour = [&](size_t i) {
                for (const Colour& c : mesh.colours) if (std::memcmp(mesh.probe.data() + i, &c.orig, 4) == 0) return true;
                return false;
            };
            size_t key = 0;
            while (key < 64 && is_colour(key)) key += 4;
            if (key >= 64) continue;
            bool hit = false;
            for (size_t o = 0; o + 64 <= buf.size() && !hit; o += 4) {
                if (std::memcmp(buf.data() + o + key, mesh.probe.data() + key, 4)) continue;
                bool ok = true;
                for (size_t i = 0; i < 64 && ok; i += 4)
                    if (!is_colour(i) && std::memcmp(buf.data() + o + i, mesh.probe.data() + i, 4)) ok = false;
                hit = ok;
            }
            if (!hit) continue;
            Found f;
            f.mesh = &mesh;
            f.base = uintptr_t(m.BaseAddress);
            f.size = m.RegionSize;
            for (const Colour& c : mesh.colours)
                for (size_t o = 0; o + 4 <= buf.size(); o += 4)
                    if (std::memcmp(buf.data() + o, &c.orig, 4) == 0) f.places[c.key].push_back(f.base + o);
            found.push_back(std::move(f));
        }
    }
    // material colours: every copy of each value in the read-write heap, kept only behind the colour header pointer
    if (!g_vecs.empty()) {
        struct Cand { uintptr_t at; const Vec* vec; std::uint64_t q16, q12; };
        std::vector<Cand> cands;
        std::unordered_map<std::uint32_t, std::vector<const Vec*>> by_first;   // first 4 bytes of each value
        for (const Vec& v : g_vecs) {
            std::uint32_t f;
            std::memcpy(&f, v.orig.data(), 4);
            by_first[f].push_back(&v);
        }
        for (uintptr_t a = 0x1000000000ull; a < 0x1100000000ull && !g_stop; a = uintptr_t(m.BaseAddress) + m.RegionSize) {
            if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
            if (m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE) continue;
            for (uintptr_t off = 0; off < m.RegionSize; off += (16u << 20)) {
                const size_t n = size_t(std::min<uintptr_t>(16u << 20, m.RegionSize - off));
                buf.resize(n);
                if (!ReadSelf(uintptr_t(m.BaseAddress) + off, buf.data(), n)) continue;
                // one pass over the chunk for all values: a std::search per value took ~45 s per scan (log 2026-11-01)
                for (size_t o = 16; o + 16 <= n; o += 4) {
                    std::uint32_t first;
                    std::memcpy(&first, buf.data() + o, 4);
                    auto hit = by_first.find(first);
                    if (hit == by_first.end()) continue;
                    for (const Vec* v : hit->second) {
                        if (std::memcmp(buf.data() + o, v->orig.data(), v->orig.size())) continue;
                        Cand c{uintptr_t(m.BaseAddress) + off + o, v, 0, 0};
                        std::memcpy(&c.q16, buf.data() + o - 16, 8);
                        std::memcpy(&c.q12, buf.data() + o - 12, 8);
                        cands.push_back(c);
                    }
                }
            }
        }
        // the header pointer: the heap pointer most often found in front of the candidates
        std::map<std::uint64_t, int> votes;
        for (const Cand& c : cands)
            for (std::uint64_t q : {c.q16, c.q12})
                if (q >= 0x1000000000ull && q < 0x1100000000ull) ++votes[q];
        for (auto& [q, n] : votes) if (n >= 3 && (!g_header || n > votes[g_header])) g_header = q;
        std::vector<Place> places;
        for (const Cand& c : cands) {
            if (!g_header || (c.q16 != g_header && c.q12 != g_header)) continue;
            Place pl{c.at, c.vec, c.vec->orig};
            for (const Place& old : g_places) if (old.at == c.at) pl.last = old.last;
            places.push_back(pl);
        }
        // places already written keep being tracked while they still hold what was written
        for (const Place& old : g_places) {
            if (std::any_of(places.begin(), places.end(), [&](const Place& q) { return q.at == old.at; })) continue;
            std::vector<unsigned char> cur(old.last.size());
            if (ReadSelf(old.at, cur.data(), cur.size()) && cur == old.last) places.push_back(old);
        }
        if (places.size() != g_places.size()) Logger::Instance().Info("mesh: " + std::to_string(places.size()) + " material colours found");
        g_places = std::move(places);
    }
    // a buffer already tracked keeps what was written there
    for (Found& f : found)
        for (const Found& old : g_found)
            if (old.base == f.base && old.mesh == f.mesh) { f.places = old.places; f.last = old.last; }
    if (found.size() != g_found.size()) Logger::Instance().Info("mesh: " + std::to_string(found.size()) + " crate buffers found");
    g_found = std::move(found);
    if (GetTickCount64() - t0 > 300) Logger::Instance().Info("mesh: slow scan " + std::to_string(GetTickCount64() - t0) + " ms");
}

bool StillThere(const Found& f) {
    unsigned char w[64];
    // the probe sits where the first colour is: if it is ours (written), compare the rest of the window only loosely
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(reinterpret_cast<void*>(f.base), &m, sizeof m) || m.State != MEM_COMMIT || m.RegionSize != f.size) return false;
    return ReadSelf(f.base, w, 4);
}

void Apply(const std::map<std::string, std::uint32_t>& edits) {
    for (Place& pl : g_places) {
        const Vec& v = *pl.vec;
        std::vector<unsigned char> want = v.orig;
        if (auto e = edits.find(v.key); e != edits.end()) {
            const unsigned char* c = reinterpret_cast<const unsigned char*>(&e->second);   // bytes r g b a
            float o[3];
            std::memcpy(o, v.orig.data(), 12);
            const float k = std::max({o[0], o[1], o[2], 0.05f});   // keeps how bright this shade was
            const float f[3] = {c[0] / 255.f * k, c[1] / 255.f * k, c[2] / 255.f * k};
            std::memcpy(want.data(), f, 12);
        }
        std::vector<unsigned char> cur(want.size());
        if (!ReadSelf(pl.at, cur.data(), cur.size()) || cur == want) continue;
        if (cur != v.orig && cur != pl.last) continue;   // not the colour any more: left alone
        JournalWrite("mesh", pl.at, want.data(), want.size());
        SIZE_T put = 0;
        if (WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(pl.at), want.data(), want.size(), &put) && put == want.size()) pl.last = want;
    }
    for (Found& f : g_found) {
        for (const Colour& c : f.mesh->colours) {
            auto e = edits.find(c.key);
            const std::uint32_t want = e != edits.end() ? e->second : c.orig;
            auto l = f.last.find(c.key);
            const std::uint32_t last = l != f.last.end() ? l->second : c.orig;
            int written = 0;
            for (uintptr_t p : f.places[c.key]) {
                std::uint32_t cur = 0;
                if (!ReadSelf(p, &cur, 4) || cur == want) continue;
                if (cur != c.orig && cur != last) continue;   // not the colour any more: left alone
                WriteSelf(p, want);
                ++written;
            }
            f.last[c.key] = want;
            if (written) Logger::Instance().Info("mesh: " + f.mesh->name + " " + c.key + " " + std::to_string(written) + " vertex colours written");
        }
    }
}

void Run() {
    NameThread(L"astro:mesh");
    LoadCatalog();
    Logger::Instance().Info("mesh: " + std::to_string(g_meshes.size()) + " meshes in mesh_catalog.json");
    if (g_meshes.empty() && g_vecs.empty()) return;
    ULONGLONG last_scan = 0;
    bool any_written = false;
    while (!g_stop) {
        Sleep(1000);
        const auto edits = ReadEdits();
        if (edits.empty() && !any_written) continue;
        const ULONGLONG now = GetTickCount64();
        // the crates are loaded with the race: search every 5 s until both are found, then every 30 s (a new race)
        bool gone = g_found.size() < g_meshes.size();
        if (!g_vecs.empty() && g_places.empty()) gone = true;
        for (const Found& f : g_found) if (!StillThere(f)) gone = true;
        if (now - last_scan > (Loading() ? 2000u : gone ? 5000u : 30000u)) { last_scan = now; Scan(); }
        Apply(edits);
        any_written = !edits.empty();
    }
}

}  // namespace

void MeshColors_Start() {
    if (g_thread.joinable()) return;
    g_stop = false;
    g_thread = std::thread(Run);
}

void MeshColors_Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    Apply({});   // the game's own colours back
}

}  // namespace astro
