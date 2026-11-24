#include "wheel_editor.h"
#include "switches.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../config/wheel_protocol.h"
#include "../diagnostics/log.h"

namespace astro {

namespace {

// A texture's BC1 payload is copied into game memory (a region of its own, exactly as big as the payload rounded up to
// pages, read-only) and the emulator uploads it again when it is written to (seen in game 2026-10-31: swapping red/blue
// in KartLimited4_Wheels_C at 0x1089160000 turned the cyan lines yellow). The memory copy can differ from the file in
// a few places and the race loads the textures again (the game puts its own colours back), so a texture is recognised
// by most of its probe windows, tracked per address, and its real contents are compared with what it should hold, every
// time.
constexpr uintptr_t kPage = 0x1000;

bool ReadSelf(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
// A texture sits in a read-only page of its own (host protection). The emulator watches those pages: a plain store
// from inside the game process faults, it marks the texture changed and uploads it again. WriteProcessMemory does not
// go through that path (the page stayed unchanged), so the texture is written with an ordinary copy.
bool WriteSelf(uintptr_t a, const void* in, size_t n) {
    JournalWrite("wheels", a, in, n);
    std::memcpy(reinterpret_cast<void*>(a), in, n);
    return true;
}

struct Live {
    const wheel::Texture* tex = nullptr;
    std::vector<std::uint8_t> orig;     // the game's own payload (as found in memory)
    std::vector<std::uint8_t> want;     // the recoloured payload for want_key
    std::string want_key;
    std::vector<std::uint8_t> last;     // what was last written
    std::vector<std::uint8_t> cur;      // scratch
    std::vector<size_t> samples;        // up to 3 windows of 64 bytes where the wanted bytes differ from the game's own
    std::string samples_key;            // which wanted state the samples belong to
};

// windows of 64 bytes (one per third of the payload) where a and b differ: checking only these tells whether memory
// holds b, without reading megabytes every 300 ms (a 2048x2048 snow texture is 5.6 MB)
std::vector<size_t> DiffWindows(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    std::vector<size_t> out;
    const size_t n = std::min(a.size(), b.size());
    for (int third = 0; third < 3; ++third) {
        const size_t lo = n * third / 3 / 64 * 64, hi = n * (third + 1) / 3 / 64 * 64;
        for (size_t o = lo; o + 64 <= hi && o + 64 <= n; o += 64)
            if (std::memcmp(a.data() + o, b.data() + o, 64)) { out.push_back(o); break; }
    }
    return out;
}

// regions already examined: base -> size, a signature of 3 windows, when, and the texture found there (or null)
struct Seen { size_t size; std::uint64_t sig; ULONGLONG t; const wheel::Texture* tex; };
std::map<uintptr_t, Seen> g_seen;
bool RegionSig(uintptr_t base, size_t size, std::uint64_t& sig) {
    std::uint8_t w[64];
    sig = 0xcbf29ce484222325ull;
    for (uintptr_t o : {uintptr_t(0x100), uintptr_t(size / 2 / 64 * 64), uintptr_t(size - 64)}) {
        if (!ReadSelf(base + o, w, 64)) return false;
        for (int i = 0; i < 64; i += 8) { std::uint64_t x; std::memcpy(&x, w + i, 8); sig = (sig ^ x) * 0x100000001b3ull; }
    }
    return true;
}

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::mutex g_mu;
std::vector<wheel::Wheel> g_catalog;
std::map<uintptr_t, Live> g_live;                  // address -> texture there
std::map<std::string, RecolorParams> g_edits;      // texture id (or tint id) -> edit

// effect tint colours (see wheel::Tint): 16 bytes found in memory, each copy tracked and rewritten
struct TintLive {
    std::string id;
    std::uint8_t orig[16];
    std::uint8_t last[16];
    bool wrote = false;
};
std::map<uintptr_t, TintLive> g_tints;
std::unordered_map<std::uint64_t, std::vector<const wheel::Tint*>> g_tint_by_prefix;

std::filesystem::path DllDir() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&DllDir), &m);
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(m, p, MAX_PATH);
    return std::filesystem::path(p).parent_path();
}

void LoadCatalog() {
    g_catalog = wheel::LoadCatalog(DllDir() / "wheel_catalog.json");
    for (auto& sky : wheel::LoadCatalog(DllDir() / "sky_catalog.json")) g_catalog.push_back(std::move(sky));   // MAPS page skies
    for (auto& snow : wheel::LoadCatalog(DllDir() / "snow_catalog.json")) g_catalog.push_back(std::move(snow));   // MAPS page snow and ice
    for (auto& pad : wheel::LoadCatalog(DllDir() / "pad_catalog.json")) g_catalog.push_back(std::move(pad));   // MAPS page speed pads
    for (auto& itm : wheel::LoadCatalog(DllDir() / "items_catalog.json")) g_catalog.push_back(std::move(itm));   // ITEMS page
    for (auto& drv : wheel::LoadCatalog(DllDir() / "driver_catalog.json")) g_catalog.push_back(std::move(drv));   // MAPS page: my character
    for (auto& roo : wheel::LoadCatalog(DllDir() / "roo_catalog.json")) g_catalog.push_back(std::move(roo));   // MAPS page Roo's Tubes
    for (auto& lava : wheel::LoadCatalog(DllDir() / "lava_catalog.json")) g_catalog.push_back(std::move(lava));   // MAPS page lava, fire, volcanic rock
    g_tint_by_prefix.clear();
    for (auto& w : g_catalog)
        for (auto& t : w.tints) {
            std::uint64_t k;
            std::memcpy(&k, t.bytes.data(), 8);
            auto& v = g_tint_by_prefix[k];
            if (std::none_of(v.begin(), v.end(), [&](const wheel::Tint* x) { return x->id == t.id; })) v.push_back(&t);
        }
    Logger::Instance().Info("wheels: catalogue of " + std::to_string(g_catalog.size()) + " wheels loaded");
}

// the share of equal 8-byte words
double Similar(const std::uint8_t* a, const std::uint8_t* b, size_t n) {
    size_t eq = 0;
    const size_t tot = n / 8;
    for (size_t i = 0; i + 8 <= n; i += 8) eq += !std::memcmp(a + i, b + i, 8);
    return tot ? double(eq) / double(tot) : 0;
}
// how many of the texture's probe windows match the region (-1: too few to be this texture)
int ProbeScore(const std::uint8_t* region, size_t region_len, const wheel::Texture& t) {
    size_t ok = 0;
    for (auto& [off, bytes] : t.probes)
        if (off + 64 <= region_len && !std::memcmp(region + off, bytes.data(), 64)) ++ok;
    return ok * 10 >= t.probes.size() * 9 ? int(ok) : -1;   // 90% of the windows (never flat ones, see the catalogue builder)
}
// still the texture we know: close to the game's own bytes or to the ones we wrote
bool StillThere(uintptr_t addr, Live& l) {
    l.cur.resize(l.orig.size());
    if (!ReadSelf(addr, l.cur.data(), l.cur.size())) return false;
    return Similar(l.cur.data(), l.orig.data(), l.cur.size()) >= 0.9 ||
           (!l.last.empty() && Similar(l.cur.data(), l.last.data(), l.cur.size()) >= 0.9);
}

std::string ParamsKey(const RecolorParams& p) {
    return FormatHexColor(p.target) + FormatHexColor(p.grey_target) + std::to_string(p.strength) + "|" + std::to_string(p.brightness) +
           (p.colours ? "c" : "") + (p.greys ? "g" : "") + (p.blacks ? "b" + FormatHexColor(p.black_target) : "");
}

// Finds the catalogue textures loaded in memory. Each loaded texture is a region of its own, read-only, exactly as big
// as its payload (KartLimited4_Wheels_C = 0x2b000 bytes at 0x1089160000), so only regions of those sizes are read: a
// quick walk, not a search of the whole heap.
void Scan() {
    const auto t0 = GetTickCount64();
    std::unordered_map<size_t, std::vector<const wheel::Texture*>> by_size;
    for (auto& w : g_catalog)
        for (auto& t : w.textures) {
            auto& v = by_size[(t.len + kPage - 1) & ~(kPage - 1)];
            if (std::none_of(v.begin(), v.end(), [&](const wheel::Texture* x) { return x->id == t.id; })) v.push_back(&t);
        }
    std::vector<std::pair<uintptr_t, const wheel::Texture*>> found;
    std::vector<uintptr_t> visited;
    std::vector<std::uint8_t> buf;
    MEMORY_BASIC_INFORMATION m{};
    for (uintptr_t a = 0x1000000000ull; a < 0x8000000000ull && !g_stop; a = uintptr_t(m.BaseAddress) + m.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
        // textures are read-only regions (seen in game 2026-10-31); a writable region is a buffer, never written to
        if (m.State != MEM_COMMIT || m.Protect != PAGE_READONLY) continue;
        auto cand = by_size.find(m.RegionSize);
        if (cand == by_size.end()) continue;
        // a region seen before with the same 3-window signature is not read again (full re-read every 2 minutes):
        // reading every region of a texture-sized class every 3 s was megabytes of memory traffic for nothing
        std::uint64_t sig = 0;
        const uintptr_t base = uintptr_t(m.BaseAddress);
        if (!RegionSig(base, m.RegionSize, sig)) continue;
        const ULONGLONG now = GetTickCount64();
        visited.push_back(base);
        auto sn = g_seen.find(base);
        if (sn != g_seen.end() && sn->second.size == m.RegionSize && sn->second.sig == sig && now - sn->second.t < 120000) {
            if (sn->second.tex) found.push_back({base, sn->second.tex});
            continue;
        }
        buf.resize(m.RegionSize);
        if (!ReadSelf(base, buf.data(), buf.size())) continue;
        const wheel::Texture* best = nullptr;
        int best_score = -1;
        for (const wheel::Texture* t : cand->second) {   // variants that share most pixels: the best match wins
            const int sc = ProbeScore(buf.data(), buf.size(), *t);
            if (sc > best_score) { best = t; best_score = sc; }
        }
        g_seen[base] = {m.RegionSize, sig, now, best};
        if (best) found.push_back({base, best});
    }
    for (auto it = g_seen.begin(); it != g_seen.end();)   // regions that are gone are forgotten
        if (std::find(visited.begin(), visited.end(), it->first) == visited.end()) it = g_seen.erase(it); else ++it;
    std::lock_guard<std::mutex> l(g_mu);
    for (auto it = g_live.begin(); it != g_live.end();) {   // gone (unloaded or reused)
        if (StillThere(it->first, it->second)) ++it;
        else { it = g_live.erase(it); NoteLoad(); }   // a texture went away: a race or menu is loading
    }
    size_t added = 0;
    for (auto& [addr, tex] : found) {
        if (g_live.count(addr)) continue;
        Live lv;
        lv.tex = tex;
        lv.orig.resize(tex->len);
        if (!ReadSelf(addr, lv.orig.data(), tex->len)) continue;
        g_live[addr] = std::move(lv);
        ++added;
    }
    std::set<std::string> ids;
    for (auto& [addr, lv] : g_live) ids.insert(lv.tex->id);
    wheel::SaveFound(std::vector<std::string>(ids.begin(), ids.end()));
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, wheel::kWheelScannedEvent)) { SetEvent(e); CloseHandle(e); }
    if (added) {
        std::string list;
        for (auto& [addr, lv] : g_live) { char b[32]; std::snprintf(b, sizeof b, "%llx", (unsigned long long)addr); list += " " + lv.tex->name + "@" + b; }
        Logger::Instance().Info("wheels: tracking:" + list);
    }
    if (added) Logger::Instance().Info("wheels: " + std::to_string(g_live.size()) + " texture copies tracked (" + std::to_string(added) +
                                       " new), " + std::to_string(GetTickCount64() - t0) + " ms");
}

// makes every tracked copy hold what it should: the recoloured payload where there is an edit, the game's own
// where there is none. The memory is read and compared every time (the game puts its colours back on a new race).
void Apply() {
    std::lock_guard<std::mutex> l(g_mu);
    size_t fixed = 0;
    for (auto& [addr, lv] : g_live) {
        auto e = g_edits.find(lv.tex->id);
        const std::vector<std::uint8_t>* target = &lv.orig;
        if (e != g_edits.end()) {
            const std::string key = ParamsKey(e->second);
            if (key != lv.want_key || lv.want.size() != lv.orig.size()) {
                lv.want.resize(lv.orig.size());
                if (lv.tex->rgba) RecolorRgba8(e->second, lv.orig.data(), lv.want.data(), lv.want.size());
                else if (lv.tex->bc3) RecolorBc3(e->second, lv.orig.data(), lv.want.data(), lv.want.size());
                else RecolorBc1(e->second, lv.orig.data(), lv.want.data(), lv.want.size());
                lv.want_key = key;
            }
            target = &lv.want;
        }
        // nothing was ever written here and nothing is wanted: the memory is the game's own
        if (e == g_edits.end() && lv.last.empty()) continue;
        // cheap check: 3 windows where the wanted bytes differ from the game's. All equal = already as wanted.
        const std::string skey = e != g_edits.end() ? lv.want_key : std::string("restore");
        if (lv.samples_key != skey) {
            lv.samples = DiffWindows(e != g_edits.end() ? lv.want : lv.last, lv.orig);
            lv.samples_key = skey;
        }
        if (!lv.samples.empty()) {
            bool as_wanted = true;
            std::uint8_t w[64];
            for (size_t o : lv.samples)
                if (!ReadSelf(addr + o, w, 64) || std::memcmp(w, target->data() + o, 64)) { as_wanted = false; break; }
            if (as_wanted) continue;
        }
        lv.cur.resize(lv.orig.size());
        if (!ReadSelf(addr, lv.cur.data(), lv.cur.size())) continue;
        if (lv.cur == *target) continue;
        // only over what is still this texture: close to the game's own bytes or to the ones we wrote
        if (Similar(lv.cur.data(), lv.orig.data(), lv.cur.size()) < 0.9 &&
            (lv.last.empty() || Similar(lv.cur.data(), lv.last.data(), lv.cur.size()) < 0.9)) continue;
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(reinterpret_cast<void*>(addr), &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect != PAGE_READONLY ||
            uintptr_t(mi.BaseAddress) != addr || mi.RegionSize != ((lv.orig.size() + kPage - 1) & ~(kPage - 1))) continue;   // moved or freed
        if (WriteSelf(addr, target->data(), target->size())) { lv.last = *target; lv.samples_key.clear(); ++fixed; }
    }
    if (fixed) Logger::Instance().Info("wheels: " + std::to_string(fixed) + " texture copies written");
}

// Finds the live copies of the wheels' tint colours: every 16-byte aligned slot of the writable heap is compared with
// the catalogue's colours (a lookup on its first 8 bytes). Only colours that have an edit are tracked.
void ScanTints() {
    const auto t0 = GetTickCount64();
    std::set<std::string> wanted;
    { std::lock_guard<std::mutex> l(g_mu); for (auto& [id, p] : g_edits) if (id.rfind("tint:", 0) == 0) wanted.insert(id); }
    if (wanted.empty()) return;
    std::vector<std::pair<uintptr_t, const wheel::Tint*>> found;
    std::vector<std::uint8_t> buf;
    // A hash lookup for every 16 bytes of the 4 GB heap cost ~70% of a core (250 million lookups per pass, measured
    // 2026-11-01 as -15 fps): a 64 KB table on the low 16 bits of the first 8 bytes rejects almost every slot first.
    std::vector<std::uint8_t> filt(65536, 0);
    for (auto& [k, v] : g_tint_by_prefix) filt[k & 0xffff] = 1;
    MEMORY_BASIC_INFORMATION m{};
    for (uintptr_t a = 0x1000000000ull; a < 0x8000000000ull && !g_stop; a = uintptr_t(m.BaseAddress) + m.RegionSize) {
        if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
        if (m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE) continue;
        const uintptr_t end = uintptr_t(m.BaseAddress) + m.RegionSize;
        for (uintptr_t p = uintptr_t(m.BaseAddress); p < end; p += (1u << 20)) {
            const size_t n = std::min<size_t>(1u << 20, end - p);
            buf.resize(n);
            if (!ReadSelf(p, buf.data(), n)) continue;
            for (size_t i = 0; i + 16 <= n; i += 16) {
                std::uint64_t k;
                std::memcpy(&k, &buf[i], 8);
                if (!filt[k & 0xffff]) continue;
                auto it = g_tint_by_prefix.find(k);
                if (it == g_tint_by_prefix.end()) continue;
                for (const wheel::Tint* t : it->second)
                    if (wanted.count(t->id) && !std::memcmp(&buf[i], t->bytes.data(), 16)) found.push_back({p + i, t});
            }
        }
    }
    std::lock_guard<std::mutex> l(g_mu);
    for (auto it = g_tints.begin(); it != g_tints.end();) {   // gone, or memory reused
        std::uint8_t cur[16];
        if (ReadSelf(it->first, cur, 16) && (!std::memcmp(cur, it->second.orig, 16) || (it->second.wrote && !std::memcmp(cur, it->second.last, 16)))) ++it;
        else it = g_tints.erase(it);
    }
    size_t added = 0;
    for (auto& [addr, t] : found) {
        if (g_tints.count(addr)) continue;
        TintLive tl{};
        tl.id = t->id;
        std::memcpy(tl.orig, t->bytes.data(), 16);
        g_tints[addr] = tl;
        ++added;
    }
    if (added) Logger::Instance().Info("wheels: " + std::to_string(g_tints.size()) + " effect tint copies tracked (" + std::to_string(added) +
                                       " new), " + std::to_string(GetTickCount64() - t0) + " ms");
}

void ApplyTints() {
    std::lock_guard<std::mutex> l(g_mu);
    for (auto& [addr, tl] : g_tints) {
        std::uint8_t want[16], cur[16];
        std::memcpy(want, tl.orig, 16);
        auto e = g_edits.find(tl.id);
        if (e != g_edits.end()) {
            const float v[4] = {e->second.target.r / 255.f, e->second.target.g / 255.f, e->second.target.b / 255.f, 1.0f};
            std::memcpy(want, v, 16);
        }
        if (!ReadSelf(addr, cur, 16) || !std::memcmp(cur, want, 16)) continue;
        // only over the game's own value or the one we wrote
        if (std::memcmp(cur, tl.orig, 16) && !(tl.wrote && !std::memcmp(cur, tl.last, 16))) continue;
        WriteSelf(addr, want, 16);
        std::memcpy(tl.last, want, 16);
        tl.wrote = true;
    }
}

// the Wheel Editor's edits and the MAPS page's sky edits (texture ids never overlap)
std::map<std::string, RecolorParams> AllEdits() {
    auto e = wheel::LoadEdits();
    for (auto& [id, p] : wheel::LoadEdits(wheel::SkyEditsPath())) e[id] = p;
    for (auto& [id, p] : wheel::LoadEdits(wheel::SnowEditsPath())) e[id] = p;
    for (auto& [id, p] : wheel::LoadEdits(wheel::PadEditsPath())) e[id] = p;
    for (auto& [id, p] : wheel::LoadEdits(wheel::LavaEditsPath())) e[id] = p;
    for (auto& [id, p] : wheel::LoadEdits(wheel::RooEditsPath())) e[id] = p;
    for (auto& [id, p] : wheel::LoadEdits(wheel::DriverEditsPath())) e[id] = p;
    for (auto& [id, p] : wheel::LoadEdits(wheel::ItemsEditsPath())) e[id] = p;
    return e;
}

void Run() {
    NameThread(L"astro:wheels");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    LoadCatalog();
    HANDLE scan = CreateEventW(nullptr, FALSE, FALSE, wheel::kWheelScanEvent);
    HANDLE apply = CreateEventW(nullptr, FALSE, FALSE, wheel::kWheelApplyEvent);
    CreateEventW(nullptr, FALSE, FALSE, wheel::kWheelScannedEvent);   // so OpenEventW finds it before the app runs
    { std::lock_guard<std::mutex> l(g_mu); g_edits = AllEdits(); }
    ULONGLONG next_scan = GetTickCount64() + 5000;   // saved edits: first look once the game has loaded a bit
    ULONGLONG next_tint = GetTickCount64() + 6000;
    for (int tick = 0; !g_stop; ++tick) {
        Sleep(100);
        bool changed = false;
        if (apply && WaitForSingleObject(apply, 0) == WAIT_OBJECT_0) {
            std::lock_guard<std::mutex> l(g_mu);
            g_edits = AllEdits();
            changed = true;
        }
        bool have, missing = false;
        {
            std::lock_guard<std::mutex> l(g_mu);
            have = !g_edits.empty();
            for (auto& [id, p] : g_edits) {
                bool seen = false;
                for (auto& [addr, lv] : g_live) if (lv.tex->id == id) seen = true;
                if (!seen) missing = true;
            }
        }
        const bool asked = scan && WaitForSingleObject(scan, 0) == WAIT_OBJECT_0;
        // a race (or a menu) loads the wheel textures again: while something is edited, look every 3 seconds
        if (asked || (changed && missing) || (have && GetTickCount64() >= next_scan)) {
            Scan();
            next_scan = GetTickCount64() + (Loading() ? 1000 : 3000);
            changed = true;
        }
        if (changed || (tick % 3 == 0 && have)) Apply();
        // effect tints: a heap pass is a second of work, so only when asked or every 10 seconds while a tint is edited
        bool tint_edit = false;
        { std::lock_guard<std::mutex> l(g_mu); for (auto& [id, p] : g_edits) if (id.rfind("tint:", 0) == 0) tint_edit = true; }
        if (tint_edit && (asked || (changed && g_tints.empty()) || GetTickCount64() >= next_tint)) {
            ScanTints();
            next_tint = GetTickCount64() + 10000;
            changed = true;
        }
        if (tint_edit || !g_tints.empty()) if (changed || tick % 3 == 0) ApplyTints();
    }
    if (scan) CloseHandle(scan);
    if (apply) CloseHandle(apply);
}

}  // namespace

void WheelEditor_Start() {
    if (g_thread.joinable()) return;
    g_stop = false;
    g_thread = std::thread(Run);
}

void WheelEditor_Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
    std::lock_guard<std::mutex> l(g_mu);
    size_t n = 0;
    for (auto& [addr, lv] : g_live) {
        lv.cur.resize(lv.orig.size());
        if (!lv.last.empty() && ReadSelf(addr, lv.cur.data(), lv.cur.size()) && lv.cur == lv.last) {
            WriteSelf(addr, lv.orig.data(), lv.orig.size());
            ++n;
        }
    }
    for (auto& [addr, tl] : g_tints) {
        std::uint8_t cur[16];
        if (tl.wrote && ReadSelf(addr, cur, 16) && !std::memcmp(cur, tl.last, 16)) WriteSelf(addr, tl.orig, 16);
    }
    g_tints.clear();
    g_live.clear();
    Logger::Instance().Info("wheels: " + std::to_string(n) + " textures restored");
}

}  // namespace astro
