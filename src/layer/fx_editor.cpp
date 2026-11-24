#include "fx_editor.h"
#include "switches.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../config/fx_protocol.h"
#include "../config/json.h"
#include "../diagnostics/log.h"

namespace astro {

namespace {

// plain in-process reads and writes (writes from another process do not show in the game)
bool ReadSelf(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
bool WriteSelf(uintptr_t a, const void* in, size_t n) {
    JournalWrite("fx", a, in, n);
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), in, n, &put) && put == n;
}

using fx::Color4;

// A live igVfxColorOperator: vtable pointer at +0 (EU and USA 1.21 eboots: 0x802461dc0), 0x1f at +0xc, a mode field at
// +0x10 and the colour (4 floats) at +0x20. Same layout as the object in the .igz file (seen in game 2026-10-31:
// the Champion underglow operator and the kart shadows). The decals copy the colour when they are created.
constexpr unsigned char kVtable[8] = {0xc0, 0x1d, 0x46, 0x02, 0x08, 0, 0, 0};
constexpr uintptr_t kColorAt = 0x20;
constexpr uintptr_t kNearSameFile = 0x4000;   // operators of one loaded effect file sit close together

struct Track { std::string id; Color4 orig; };

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::mutex g_mu;
std::unordered_map<std::string, std::vector<std::string>> g_catalog;   // "field|r|g|b|a" (x10000) -> effect names
std::map<uintptr_t, Track> g_tracked;
std::map<std::string, Color4> g_edits;     // what the app asked for
std::map<std::string, Color4> g_written;   // what was last written per item

std::filesystem::path DllDir() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DllDir), &m);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(m, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

void LoadCatalog() {
    g_catalog.clear();
    std::ifstream in(DllDir() / "vfx_catalog.json", std::ios::binary);
    if (!in) { Logger::Instance().Warn("effects: vfx_catalog.json not found next to the DLL; effects stay unnamed"); return; }
    std::stringstream ss;
    ss << in.rdbuf();
    auto root = json::Parse(ss.str());
    if (!root || root->type != json::Value::Type::Object || !root->o) return;
    for (auto& [k, v] : *root->o) {
        if (v.type != json::Value::Type::Array || !v.a) continue;
        auto& names = g_catalog[k];
        for (auto& n : *v.a) if (n.type == json::Value::Type::String) names.push_back(n.s);
    }
    Logger::Instance().Info("effects: catalogue of " + std::to_string(g_catalog.size()) + " colours loaded");
}

std::string Key(uint32_t field, const Color4& c) {
    std::string k = std::to_string(field);
    for (float f : c) k += "|" + std::to_string(std::lround(double(f) * 10000));
    return k;
}

bool IsOperator(uintptr_t a) {
    unsigned char h[16];
    uint32_t tag;
    return ReadSelf(a, h, 16) && !std::memcmp(h, kVtable, 8) && (std::memcpy(&tag, h + 12, 4), tag == 0x1f);
}

// every live colour operator (address order)
std::vector<uintptr_t> FindOperators() {
    std::vector<uintptr_t> out;
    std::vector<unsigned char> buf;
    MEMORY_BASIC_INFORMATION m{};
    for (uintptr_t a = 0x1000000000ull; a < 0x8000000000ull && !g_stop; a = uintptr_t(m.BaseAddress) + m.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
        if (m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE) continue;
        const uintptr_t end = uintptr_t(m.BaseAddress) + m.RegionSize;
        for (uintptr_t p = uintptr_t(m.BaseAddress); p < end; p += (16u << 20)) {
            const size_t n = std::min<size_t>(16u << 20, end - p);
            buf.resize(n);
            if (!ReadSelf(p, buf.data(), n)) continue;
            for (size_t i = 0; i + 48 <= n; i += 8) {
                if (std::memcmp(&buf[i], kVtable, 8)) continue;
                uint32_t tag;
                std::memcpy(&tag, &buf[i + 12], 4);
                if (tag == 0x1f) out.push_back(p + i);
            }
        }
    }
    return out;
}

// Finds the operators, names new ones, forgets gone ones, publishes the list when asked.
void Scan(bool publish) {
    const auto t0 = GetTickCount64();
    const std::vector<uintptr_t> ops = FindOperators();
    std::lock_guard<std::mutex> l(g_mu);
    struct Op { uintptr_t a; uint32_t field; Color4 c; const std::vector<std::string>* names; };
    std::vector<Op> found;
    for (uintptr_t a : ops) {
        unsigned char d[48];
        if (!ReadSelf(a, d, 48)) continue;
        Op o{a, 0, {}, nullptr};
        std::memcpy(&o.field, d + 16, 4);
        std::memcpy(o.c.data(), d + kColorAt, 16);
        auto it = g_catalog.find(Key(o.field, o.c));
        o.names = it == g_catalog.end() ? nullptr : &it->second;
        found.push_back(o);
    }
    // operators of one effect file lie together: the effect is the name common to the whole group
    std::map<uintptr_t, Track> next;
    for (size_t i = 0; i < found.size();) {
        size_t j = i + 1;
        while (j < found.size() && found[j].a - found[j - 1].a <= kNearSameFile) ++j;
        std::set<std::string> common;
        bool first = true;
        for (size_t k = i; k < j; ++k) {
            if (!found[k].names) continue;
            std::set<std::string> s(found[k].names->begin(), found[k].names->end());
            if (first) { common = s; first = false; }
            else {
                std::set<std::string> x;
                std::set_intersection(common.begin(), common.end(), s.begin(), s.end(), std::inserter(x, x.end()));
                common = x;
            }
        }
        for (size_t k = i; k < j; ++k) {
            const Op& o = found[k];
            auto old = g_tracked.find(o.a);
            if (old != g_tracked.end()) {   // still ours: keep its original (it may hold the edited colour now)
                auto w = g_written.find(old->second.id);
                if (o.c == old->second.orig || (w != g_written.end() && o.c == w->second)) { next[o.a] = old->second; continue; }
            }
            std::vector<std::string> names;
            if (!common.empty()) names.assign(common.begin(), common.end());
            else if (o.names) names = *o.names;
            const std::string id = Key(o.field, o.c) + "@" + (names.empty() ? std::string("?") : names.front());
            next[o.a] = Track{id, o.c};
        }
        i = j;
    }
    g_tracked.swap(next);
    if (publish) {
        std::map<std::string, fx::Item> items;
        for (auto& [a, t] : g_tracked) {
            fx::Item& it = items[t.id];
            if (it.id.empty()) {
                it.id = t.id;
                it.orig = t.orig;
                const size_t at = t.id.find('@');
                auto c = g_catalog.find(t.id.substr(0, at));
                if (c != g_catalog.end()) it.names = c->second;
                // the group's best name first
                const std::string best = t.id.substr(at + 1);
                auto b = std::find(it.names.begin(), it.names.end(), best);
                if (b != it.names.end()) std::rotate(it.names.begin(), b, b + 1);
            }
            ++it.count;
        }
        std::vector<fx::Item> list;
        for (auto& [id, it] : items) list.push_back(it);
        fx::SaveItems(list);
        if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, fx::kFxScannedEvent)) { SetEvent(e); CloseHandle(e); }
        Logger::Instance().Info("effects: " + std::to_string(ops.size()) + " colour operators, " + std::to_string(list.size()) +
                                " items, " + std::to_string(GetTickCount64() - t0) + " ms");
    }
}

// writes every tracked colour: the app's edit, or the original when the edit was removed. A place is written only
// while it is still an operator holding its original or our last value (the game may have reused the memory).
void Apply() {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto it = g_tracked.begin(); it != g_tracked.end();) {
        const uintptr_t a = it->first;
        const Track& t = it->second;
        Color4 cur;
        auto w = g_written.find(t.id);
        if (!IsOperator(a) || !ReadSelf(a + kColorAt, cur.data(), 16) || !(cur == t.orig || (w != g_written.end() && cur == w->second))) {
            it = g_tracked.erase(it);
            continue;
        }
        auto e = g_edits.find(t.id);
        const Color4 want = e != g_edits.end() ? e->second : t.orig;
        if (!(cur == want)) WriteSelf(a + kColorAt, want.data(), 16);
        ++it;
    }
    for (auto& [id, c] : g_edits) g_written[id] = c;   // also for items not loaded right now
}

void Run() {
    NameThread(L"astro:fx");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    LoadCatalog();
    HANDLE scan = CreateEventW(nullptr, FALSE, FALSE, fx::kFxScanEvent);
    HANDLE apply = CreateEventW(nullptr, FALSE, FALSE, fx::kFxApplyEvent);
    CreateEventW(nullptr, FALSE, FALSE, fx::kFxScannedEvent);   // so OpenEventW finds it even before the app runs
    { std::lock_guard<std::mutex> l(g_mu); g_edits = fx::LoadEdits(); }
    ULONGLONG next_scan = GetTickCount64() + 8000;   // saved edits: first pass once the game has loaded a bit
    for (int tick = 0; !g_stop; ++tick) {
        Sleep(100);
        bool have_edits;
        if (apply && WaitForSingleObject(apply, 0) == WAIT_OBJECT_0) {
            std::lock_guard<std::mutex> l(g_mu);
            g_edits = fx::LoadEdits();
        }
        { std::lock_guard<std::mutex> l(g_mu); have_edits = !g_edits.empty() || !g_written.empty(); }
        const bool asked = scan && WaitForSingleObject(scan, 0) == WAIT_OBJECT_0;
        // effects load and unload with tracks and menus: look again every 10 s while something is edited
        if (asked || (have_edits && GetTickCount64() >= next_scan)) {
            if (asked) LoadCatalog();   // a newer catalogue next to the DLL is used without restarting the game
            Scan(asked);
            next_scan = GetTickCount64() + (Loading() ? 2000 : 10000);
        }
        if (have_edits && tick % 2 == 0) Apply();
    }
    if (scan) CloseHandle(scan);
    if (apply) CloseHandle(apply);
}

}  // namespace

void FxEditor_Start() {
    if (g_thread.joinable()) return;
    g_stop = false;
    g_thread = std::thread(Run);
}

void FxEditor_Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> l(g_mu);
    g_edits.clear();
    size_t n = 0;
    for (auto& [a, t] : g_tracked) {
        Color4 cur;
        auto w = g_written.find(t.id);
        if (w != g_written.end() && IsOperator(a) && ReadSelf(a + kColorAt, cur.data(), 16) && cur == w->second) {
            WriteSelf(a + kColorAt, t.orig.data(), 16);
            ++n;
        }
    }
    g_tracked.clear();
    g_written.clear();
    Logger::Instance().Info("effects: " + std::to_string(n) + " colours restored");
}

}  // namespace astro
