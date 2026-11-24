#include "mod_engine.h"
#include "switches.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "../config/mods_protocol.h"
#include "../diagnostics/log.h"

namespace astro {

namespace {

// The game's code runs natively from read+execute pages (seen in game 2026-10-31: protection 0x20 at 0x80120b830).
// A patch lifts the protection of its bytes for the store, puts it back and flushes the instruction cache. Patches are
// a few bytes (a jump or nops) written whole; a code cave is written before the jump that leads into it.

std::thread g_thread;
std::atomic<bool> g_stop{false};
std::map<std::uint64_t, std::vector<std::uint8_t>> g_orig;   // guest address -> the game's own bytes (once known)

bool Read(std::uint64_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
bool Write(std::uint64_t a, const void* in, size_t n) {
    JournalWrite("mods", a, in, n);
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(a), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(a), in, n);
    DWORD dummy;
    VirtualProtect(reinterpret_cast<void*>(a), n, old, &dummy);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(a), n);
    return true;
}

// memory at the signature's place matches it (wildcards match anything); `patched` lets the patch's own bytes count
bool SigMatches(const mods::Line& l, std::uint64_t at, bool patched) {
    if (l.sig.empty()) return false;
    const std::uint64_t start = at - l.sig_off;
    std::vector<std::uint8_t> cur(l.sig.size());
    if (!Read(start, cur.data(), cur.size())) return false;
    for (size_t i = 0; i < cur.size(); ++i) {
        if (patched && i >= l.sig_off && i < l.sig_off + l.value.size()) {
            if (cur[i] != l.value[i - l.sig_off]) return false;
        } else if (l.sig[i] >= 0 && cur[i] != l.sig[i]) return false;
    }
    return true;
}
// the game's own bytes under a patch, from its signature (when the signature covers them without wildcards)
bool OrigFromSig(const mods::Line& l, std::vector<std::uint8_t>& out) {
    if (l.sig.size() < l.sig_off + l.value.size()) return false;
    out.clear();
    for (size_t i = 0; i < l.value.size(); ++i) {
        const int b = l.sig[l.sig_off + i];
        if (b < 0) return false;
        out.push_back(std::uint8_t(b));
    }
    return true;
}

// turns one mod on or off; returns its state
mods::Status Set(const mods::Mod& m, bool on) {
    // check every line first: nothing is written unless the whole mod can be
    struct Step { std::uint64_t at; const mods::Line* l; bool is_on; };
    std::vector<Step> steps;
    for (const mods::Line& l : m.lines) {
        const std::uint64_t at = l.addr + mods::kGuestDelta;
        std::vector<std::uint8_t> cur(l.value.size());
        if (!Read(at, cur.data(), cur.size())) return {"error", "address not readable"};
        const bool is_on = cur == l.value;
        if (!is_on && !l.sig.empty() && !SigMatches(l, at, false))
            return {"mismatch", "the game's code is not what this mod expects (another version or another mod)"};
        if (is_on && !l.sig.empty() && !SigMatches(l, at, true)) return {"mismatch", "the patched code does not match"};
        if (!g_orig.count(at)) {
            std::vector<std::uint8_t> o;
            if (OrigFromSig(l, o)) g_orig[at] = o;
            else if (!is_on) g_orig[at] = cur;   // unpatched now: these are the game's bytes
        }
        steps.push_back({at, &l, is_on});
    }
    if (on) {
        for (auto& s : steps)   // in file order: a code cave comes before the jump into it
            if (!s.is_on && !Write(s.at, s.l->value.data(), s.l->value.size())) return {"error", "write failed"};
        return {"on", ""};
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it) {   // reverse order: the jump goes before its cave
        if (!it->is_on) continue;
        auto o = g_orig.find(it->at);
        if (o == g_orig.end()) return {"pending", "turns off at the next game start"};
    }
    for (auto it = steps.rbegin(); it != steps.rend(); ++it)
        if (it->is_on && !Write(it->at, g_orig[it->at].data(), g_orig[it->at].size())) return {"error", "write failed"};
    return {"off", ""};
}

void ApplyAll() {
    std::vector<mods::Mod> list;
    std::map<std::string, bool> on;
    if (!mods::LoadLive(list, on)) return;
    std::map<std::string, mods::Status> status;
    for (const mods::Mod& m : list) {
        status[m.id] = Set(m, on[m.id]);
        Logger::Instance().Info("mods: " + m.id + " -> " + status[m.id].state + (status[m.id].detail.empty() ? "" : " (" + status[m.id].detail + ")"));
    }
    mods::SaveStatus(status);
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, mods::kModsStatusEvent)) { SetEvent(e); CloseHandle(e); }
}

void Run() {
    NameThread(L"astro:mods");
    HANDLE apply = CreateEventW(nullptr, FALSE, FALSE, mods::kModsApplyEvent);
    CreateEventW(nullptr, FALSE, FALSE, mods::kModsStatusEvent);   // so OpenEventW finds it before the app runs
    ApplyAll();   // the state saved by the app (the launcher applied the same patches at start)
    while (!g_stop) {
        if (apply && WaitForSingleObject(apply, 200) == WAIT_OBJECT_0) ApplyAll();
    }
    if (apply) CloseHandle(apply);
}

}  // namespace

void ModEngine_Start() {
    if (g_thread.joinable()) return;
    g_stop = false;
    g_thread = std::thread(Run);
}

void ModEngine_Stop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

}  // namespace astro
