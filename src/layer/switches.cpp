#include "switches.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

#include "../config/config.h"
#include "../diagnostics/log.h"

namespace astro {

namespace {
std::once_flag g_once;
std::set<std::string> g_off;
bool g_all = false;

void Load() {
    std::string s;
    if (std::FILE* f = _wfopen((DefaultConfigPath().parent_path() / "off.txt").c_str(), L"rb")) {
        char b[512];
        for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
        std::fclose(f);
    }
    std::string word;
    auto flush = [&] {
        if (word == "all") g_all = true;
        else if (!word.empty()) g_off.insert(word);
        word.clear();
    };
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c))) word += char(std::tolower(static_cast<unsigned char>(c)));
        else flush();
    }
    flush();
    if (g_all || !g_off.empty()) {
        std::string list = g_all ? " all" : "";
        for (auto& n : g_off) list += " " + n;
        Logger::Instance().Info("test run: parts switched off (off.txt):" + list);
    }
}
}  // namespace

bool Off(const char* name) {
    std::call_once(g_once, Load);
    return g_all || g_off.count(name) > 0;
}

}  // namespace astro

namespace astro {
namespace {
std::mutex g_jmu;
std::FILE* g_jf = nullptr;
ULONGLONG g_j0 = 0;
}  // namespace

void JournalWrite(const char* who, std::uintptr_t addr, const void* data, std::size_t n) {
    std::lock_guard<std::mutex> l(g_jmu);
    if (!g_jf) {
        const auto p = DefaultConfigPath().parent_path() / ("writes_" + std::to_string(GetCurrentProcessId()) + ".log");
        g_jf = _wfopen(p.c_str(), L"ab");
        g_j0 = GetTickCount64();
        if (!g_jf) return;
    }
    if (!g_jf) return;
    char head[96];
    std::snprintf(head, sizeof head, "%llu %s %llx %zu ", (unsigned long long)(GetTickCount64() - g_j0), who, (unsigned long long)addr, n);
    std::string line = head;
    if (n <= 64) {
        unsigned char old[64];
        SIZE_T got = 0;
        const bool ok = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(addr), old, n, &got) && got == n;
        auto hex = [&](const unsigned char* b) { std::string h; char x[3]; for (std::size_t i = 0; i < n; ++i) { std::snprintf(x, 3, "%02x", b[i]); h += x; } return h; };
        line += (ok ? hex(old) : std::string("unreadable")) + " -> " + hex(static_cast<const unsigned char*>(data));
    }
    line += "\n";
    std::fputs(line.c_str(), g_jf);
    std::fflush(g_jf);
}
}  // namespace astro

namespace astro {
void NameThread(const wchar_t* name) {
    using Fn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (fn) fn(GetCurrentThread(), name);
}
}  // namespace astro

namespace astro {
static std::atomic<unsigned long long> g_load_at{0};
void NoteLoad() { g_load_at = GetTickCount64(); }
bool Loading() { const unsigned long long t = g_load_at; return t && GetTickCount64() - t < 30000; }
}  // namespace astro
