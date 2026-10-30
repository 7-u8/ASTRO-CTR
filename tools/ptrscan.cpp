// Pointer scanner for the running game (sacredfire.exe). Walks backwards from target addresses: level 1 = every
// 8-byte value in guest memory that points at or up to max_offset bytes before a target, level 2 = values pointing
// at those, and so on. A chain ends when its pointer lives inside the game image (static: same address every run).
// usage: ptrscan <max_depth> <max_offset hex> <target hex> [<target hex> ...]
// output: one line per static chain:  [image+0xNNN] +off1 -> +off2 -> ... -> target
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct Node { uint64_t addr; int parent; uint32_t off; };   // addr holds a pointer to (child addr - off)

static constexpr uint64_t kGuestLo = 0x800000000ull, kGuestHi = 0x8000000000ull;
static constexpr uint64_t kImageLo = 0x800000000ull, kImageHi = 0x804000000ull;   // eboot image (EU 1.21, < 64 MB)

static DWORD FindPid() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e))
        if (!_wcsicmp(e.szExeFile, L"sacredfire.exe")) pid = e.th32ProcessID;
    CloseHandle(s);
    return pid;
}

int main(int argc, char** argv) {
    if (argc < 4) { std::puts("usage: ptrscan <max_depth> <max_offset hex> <target hex>..."); return 2; }
    const int depth = std::atoi(argv[1]);
    const uint64_t max_off = std::strtoull(argv[2], nullptr, 16);
    DWORD pid = FindPid();
    if (!pid) { std::puts("game not running"); return 1; }
    HANDLE h = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) { std::puts("cannot open the game process"); return 1; }

    // regions to scan
    std::vector<std::pair<uint64_t, uint64_t>> regions;
    MEMORY_BASIC_INFORMATION m{};
    for (uint64_t a = kGuestLo; a < kGuestHi && VirtualQueryEx(h, (void*)a, &m, sizeof m); a = (uint64_t)m.BaseAddress + m.RegionSize)
        if (m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (m.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
            regions.push_back({(uint64_t)m.BaseAddress, m.RegionSize});

    std::vector<Node> nodes;
    std::vector<int> level;   // node indices of the current level
    for (int i = 3; i < argc; ++i) { nodes.push_back({std::strtoull(argv[i], nullptr, 16), -1, 0}); level.push_back(int(nodes.size()) - 1); }

    std::vector<uint8_t> buf;
    int printed = 0;
    for (int d = 1; d <= depth && !level.empty(); ++d) {
        // sorted targets of this level
        std::vector<std::pair<uint64_t, int>> tg;
        for (int i : level) tg.push_back({nodes[i].addr, i});
        std::sort(tg.begin(), tg.end());
        std::vector<int> next;
        size_t found = 0;
        for (auto [base, size] : regions) {
            for (uint64_t off = 0; off < size; off += (32u << 20)) {
                size_t n = (size_t)std::min<uint64_t>(32u << 20, size - off);
                buf.resize(n);
                SIZE_T got = 0;
                if (!ReadProcessMemory(h, (void*)(base + off), buf.data(), n, &got)) continue;
                for (size_t i = 0; i + 8 <= got; i += 8) {
                    uint64_t v;
                    std::memcpy(&v, &buf[i], 8);
                    if (v < kGuestLo || v >= kGuestHi) continue;
                    // targets t with v <= t <= v + max_off
                    auto it = std::lower_bound(tg.begin(), tg.end(), std::make_pair(v, -1));
                    for (; it != tg.end() && it->first - v <= max_off; ++it) {
                        const uint64_t at = base + off + i;
                        nodes.push_back({at, it->second, uint32_t(it->first - v)});
                        const int id = int(nodes.size()) - 1;
                        ++found;
                        if (at >= kImageLo && at < kImageHi) {
                            // static: print the chain
                            std::string s;
                            char t[64];
                            std::snprintf(t, sizeof t, "[image+0x%llx]", (unsigned long long)(at - kImageLo));
                            s = t;
                            for (int c = id; nodes[c].parent >= 0; c = nodes[c].parent) {
                                std::snprintf(t, sizeof t, " +0x%x ->", nodes[c].off);
                                s += t;
                            }
                            int root = id;
                            while (nodes[root].parent >= 0) root = nodes[root].parent;
                            std::snprintf(t, sizeof t, " 0x%llx", (unsigned long long)nodes[root].addr);
                            s += t;
                            std::printf("depth %d: %s\n", d, s.c_str());
                            if (++printed > 200) return 0;
                        } else if (next.size() < 20000) {
                            next.push_back(id);
                        }
                    }
                }
            }
        }
        std::printf("level %d: %zu pointers (%zu kept for the next level)\n", d, found, next.size());
        std::fflush(stdout);
        level.swap(next);
    }
    CloseHandle(h);
    return 0;
}
