// ASTRO CTR V2: the ASTRO launcher in a terminal. It works like ASTRO_CTR.exe: open it before the game, keep it open,
// close it to turn ASTRO off. While it is open:
//  - the ASTRO Vulkan layer is registered for this Windows user (HKCU\Software\Khronos\Vulkan\ImplicitLayers) and the
//    "Local\ASTRO_CTR_Watcher" mutex exists (the layer stays inert without it); closing removes the registration
//  - a staged astro_ctr.new.dll replaces astro_ctr.dll while no game runs
//  - every DLL in autoload\ next to it is loaded into each game ASTRO connects to, once per game
// Everything else (colours, maps, players...) is the in-game F8 menu. The screen is the ASTRO art, coloured and animated.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

#include "astro_art.h"

namespace fs = std::filesystem;

namespace {

// ---------------- ASTRO: registration, game, autoload ----------------
const wchar_t* kRegKey = L"Software\\Khronos\\Vulkan\\ImplicitLayers";
std::wstring g_manifest;
std::atomic<bool> g_quit{false};

fs::path ExeDir() {
    wchar_t p[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    return fs::path(p).parent_path();
}

DWORD FindProcess(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    DWORD pid = 0;
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
        if (_wcsicmp(e.szExeFile, name) == 0) { pid = e.th32ProcessID; break; }
    CloseHandle(snap);
    return pid;
}

// only one ASTRO layer may be registered (the loader picks one of several with the same name), as in ASTRO_CTR.exe
bool Register() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    std::vector<std::wstring> stale;
    for (DWORD i = 0;; ++i) {
        wchar_t name[1024];
        DWORD len = 1024;
        if (RegEnumValueW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring n(name, len);
        if (n.find(L"VkLayer_ASTRO_ctr_implicit.json") != std::wstring::npos && _wcsicmp(n.c_str(), g_manifest.c_str()) != 0) stale.push_back(n);
    }
    for (auto& n : stale) RegDeleteValueW(k, n.c_str());
    DWORD zero = 0;   // 0 = enabled
    const bool ok = RegSetValueExW(k, g_manifest.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof zero) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}
void Unregister() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, g_manifest.c_str());
    RegCloseKey(k);
}

// LoadLibraryW in the game on a remote thread (the DLL injector of ASTRO_CTR.exe)
std::string Inject(DWORD pid, const fs::path& dll) {
    const std::wstring path = fs::absolute(dll).wstring();
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!proc) return "cannot open the game";
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    std::string out = "cannot write into the game";
    if (mem && WriteProcessMemory(proc, mem, path.c_str(), bytes, nullptr)) {
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
        if (HANDLE th = CreateRemoteThread(proc, nullptr, 0, load, mem, 0, nullptr)) {
            DWORD code = 0;
            if (WaitForSingleObject(th, 10000) == WAIT_TIMEOUT) out = "still starting";
            else { GetExitCodeThread(th, &code); out = code ? "loaded" : "refused by the game"; }
            CloseHandle(th);
        } else out = "cannot start the loader thread";
    }
    if (mem) VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return out;
}

// ---------------- what the screen shows ----------------
enum class State { Waiting, AlreadyRunning, Starting, NoLayer, Loaded, Ready };
State g_state = State::Waiting;
struct LogLine { std::string time, text; int kind; };   // kind: 0 info, 1 good, 2 warn
std::deque<LogLine> g_log;

void Log(const std::string& s, int kind = 0) {
    char t[16];
    std::time_t now = std::time(nullptr);
    std::strftime(t, sizeof t, "%H:%M:%S", std::localtime(&now));
    g_log.push_back({t, s, kind});
    while (g_log.size() > 5) g_log.pop_front();
}

struct Watch {
    HANDLE loaded = nullptr, ready = nullptr;
    DWORD pid = 0, stale = 0, injected = 0;
    ULONGLONG seen_at = 0, next = 0;
    fs::path dir;

    void Init(const fs::path& d) {
        dir = d;
        loaded = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ASTRO_CTR_LayerLoaded");
        ready = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ASTRO_CTR_OverlayReady");
        stale = FindProcess(L"sacredfire.exe");
        if (stale) { g_state = State::AlreadyRunning; Log("the game was already running without ASTRO: close it and start it again", 2); }
    }
    void InstallUpdate() {
        const fs::path staged = dir / L"astro_ctr.new.dll";
        std::error_code ec;
        if (fs::exists(staged, ec) && MoveFileExW(staged.c_str(), (dir / L"astro_ctr.dll").c_str(), MOVEFILE_REPLACE_EXISTING))
            Log("ASTRO update installed (astro_ctr.dll replaced)", 1);
    }
    void Poll() {
        const ULONGLONG now = GetTickCount64();
        if (now < next) return;
        next = now + 500;
        const DWORD running = FindProcess(L"sacredfire.exe");
        if (!running) {
            if (pid && g_state != State::Waiting) Log("game closed - waiting for the next start");
            if (pid || g_state != State::Waiting) { ResetEvent(loaded); ResetEvent(ready); }
            pid = stale = 0;
            g_state = State::Waiting;
            InstallUpdate();
            return;
        }
        if (running == stale) { g_state = State::AlreadyRunning; return; }
        if (pid != running) { pid = running; seen_at = now; Log("game found (pid " + std::to_string(pid) + ")"); }
        const State before = g_state;
        if (WaitForSingleObject(ready, 0) == WAIT_OBJECT_0) g_state = State::Ready;
        else if (WaitForSingleObject(loaded, 0) == WAIT_OBJECT_0) g_state = State::Loaded;
        else g_state = now - seen_at > 30000 ? State::NoLayer : State::Starting;
        if (g_state != before) {
            if (g_state == State::Loaded) Log("ASTRO layer loaded in the game", 1);
            if (g_state == State::Ready) Log("menu ready: press F8 in the game", 1);
            if (g_state == State::NoLayer) Log("the game started without ASTRO: close it and start it again", 2);
        }
        if ((g_state == State::Loaded || g_state == State::Ready) && injected != pid) {   // autoload\*.dll, once per game
            injected = pid;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(dir / "autoload", ec))
                if (e.path().extension() == ".dll") {
                    const std::string r = Inject(pid, e.path());
                    Log("autoload " + e.path().filename().string() + ": " + r, r == "loaded" ? 1 : 2);
                }
        }
    }
};

// ---------------- drawing ----------------
struct Rgb { int r, g, b; };
float Hash(int x, int y) {
    unsigned h = unsigned(x) * 374761393u + unsigned(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float(h ^ (h >> 16)) / 4294967295.f;
}
Rgb Hsv(float h, float s, float v) {
    h = h - std::floor(h);
    const float i = std::floor(h * 6), f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    float r, g, b;
    switch (int(i) % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return {int(r * 255), int(g * 255), int(b * 255)};
}
Rgb Mix(Rgb a, Rgb b, float k) {
    k = std::clamp(k, 0.f, 1.f);
    return {int(a.r + (b.r - a.r) * k), int(a.g + (b.g - a.g) * k), int(a.b + (b.b - a.b) * k)};
}
int Density(char c) {
    static const char* ramp = " .:-=+*#%@";
    const char* p = std::strchr(ramp, c);
    return p && c ? int(p - ramp) : 0;
}

struct Screen {
    std::string out;
    Rgb cur{-1, -1, -1};
    void Color(Rgb c) {
        if (c.r == cur.r && c.g == cur.g && c.b == cur.b) return;
        cur = c;
        char b[32];
        std::snprintf(b, sizeof b, "\x1b[38;2;%d;%d;%dm", c.r, c.g, c.b);
        out += b;
    }
    void Put(const std::string& s, Rgb c) { Color(c); out += s; }
};

std::vector<std::string> g_art;
std::vector<std::vector<bool>> g_star;   // a dot with no dense art next to it: a star, not part of the drawing
int g_art_w = 0;

void FindStars() {
    g_star.assign(g_art.size(), std::vector<bool>(g_art_w, false));
    auto at = [](int x, int y) { return y >= 0 && y < int(g_art.size()) && x >= 0 && x < int(g_art[y].size()) ? Density(g_art[y][x]) : 0; };
    for (int y = 0; y < int(g_art.size()); ++y)
        for (int x = 0; x < int(g_art[y].size()); ++x) {
            if (at(x, y) != 1) continue;
            bool alone = true;
            for (int dy = -1; dy <= 1 && alone; ++dy)
                for (int dx = -3; dx <= 3; ++dx) if (at(x + dx, y + dy) >= 3) { alone = false; break; }
            g_star[y][x] = alone;
        }
}

const Rgb kOrange{255, 90, 31}, kWhite{245, 245, 248}, kMuted{120, 120, 130}, kGood{80, 230, 120}, kWarn{255, 190, 60};

// one character of the art at (x, y) of the art, after time t
void ArtCell(Screen& s, int ax, int ay, int col, float t, char c) {
    const int d = Density(c);
    if (!d) { s.out += ' '; return; }
    // intro: the art appears from the left, each cell with a short glitch before it settles
    const float appear = (float(ax) / g_art_w) * 1.1f + Hash(ax, ay) * 0.35f;
    if (t < appear) { s.out += ' '; return; }
    if (t < appear + 0.12f) { s.Put(std::string(1, "@#%*+=-:"[int(Hash(ax, int(t * 40)) * 8) & 7]), kWhite); return; }
    if (d == 1 && g_star[ay][ax]) {   // dots in the empty space around the art: twinkling stars
        const float tw = std::sin(t * 2.2f + Hash(ax, ay) * 40.f) * 0.5f + 0.5f;
        const float v = 0.25f + 0.75f * tw * tw;
        const char ch = tw > 0.93f ? '+' : tw > 0.8f ? '*' : '.';
        s.Put(std::string(1, ch), Mix({40, 40, 60}, Mix(kOrange, kWhite, Hash(ay, ax)), v));
        return;
    }
    // colour: ASTRO orange on the left flowing to crimson and violet on the right, waves moving through it
    const float across = float(ax) / g_art_w, down = float(ay) / g_art.size();
    const float wave = std::sin(ax * 0.03f + ay * 0.11f - t * 1.1f) * 0.5f + 0.5f;
    const float hue = 0.075f - across * 0.29f + wave * 0.045f + std::sin(t * 0.3f) * 0.025f;   // orange .. violet
    // depth: the edges of the art fade into the dark (vignette), the whole art breathes slowly
    const float cx = across - 0.5f, cy = (down - 0.55f) * 0.8f;
    const float vignette = std::clamp(1.15f - (cx * cx + cy * cy) * 2.2f, 0.35f, 1.f);
    const float breathe = 0.93f + 0.07f * std::sin(t * 1.3f);
    Rgb c0 = Hsv(hue, 0.97f - d * 0.03f, std::min(1.f, (0.5f + d * 0.055f + wave * 0.08f) * vignette * breathe));
    // the densest parts glow white-hot in their core
    if (d >= 9) c0 = Mix(c0, {255, 236, 210}, 0.22f + 0.18f * wave);
    // a shine sweeping across every few seconds
    const float pos = std::fmod(t * 0.3f, 1.9f) - 0.45f;
    const float band = std::max(0.f, 1.f - std::fabs(across + down * 0.3f - pos) / 0.05f);
    c0 = Mix(c0, kWhite, band * band * 0.9f);
    (void)col;
    s.Put(std::string(1, c), c0);
}

// ---- the big title: block letters (ANSI Shadow): the blocks are lit, the box lines are the letters' shadow ----
const wchar_t* const kBanner[6] = {
    L" █████╗ ███████╗████████╗██████╗  ██████╗      ██████╗████████╗██████╗     ██╗   ██╗██████╗ ",
    L"██╔══██╗██╔════╝╚══██╔══╝██╔══██╗██╔═══██╗    ██╔════╝╚══██╔══╝██╔══██╗    ██║   ██║╚════██╗",
    L"███████║███████╗   ██║   ██████╔╝██║   ██║    ██║        ██║   ██████╔╝    ██║   ██║ █████╔╝",
    L"██╔══██║╚════██║   ██║   ██╔══██╗██║   ██║    ██║        ██║   ██╔══██╗    ╚██╗ ██╔╝██╔═══╝ ",
    L"██║  ██║███████║   ██║   ██║  ██║╚██████╔╝    ╚██████╗   ██║   ██║  ██║     ╚████╔╝ ███████╗",
    L"╚═╝  ╚═╝╚══════╝   ╚═╝   ╚═╝  ╚═╝ ╚═════╝      ╚═════╝   ╚═╝   ╚═╝  ╚═╝      ╚═══╝  ╚══════╝",
};

std::string Utf8(wchar_t w) {
    char b[8];
    const int n = WideCharToMultiByte(CP_UTF8, 0, &w, 1, b, sizeof b, nullptr, nullptr);
    return std::string(b, n > 0 ? n : 0);
}

void Banner(Screen& s, int cols, float t) {
    const int w = int(std::wcslen(kBanner[0]));
    if (cols < w + 2) {   // too narrow for the block letters: the plain title
        const std::string title = "A S T R O   C T R   V 2";
        s.out.append(std::max(0, (cols - int(title.size())) / 2), ' ');
        s.Put(title, kOrange);
        s.out += "\x1b[K\r\n";
        return;
    }
    const int pad = (cols - w) / 2;
    for (int y = 0; y < 6; ++y) {
        s.out.append(pad, ' ');
        for (int x = 0; x < w; ++x) {
            const wchar_t ch = kBanner[y][x];
            if (ch == L' ') { s.out += ' '; continue; }
            const float across = float(x) / w;
            if (ch == L'█') {
                // fire from the top (gold) down to crimson, the hue drifting across; a light sweeps through
                const float hue = 0.11f - y * 0.022f - across * 0.05f + std::sin(t * 0.8f + across * 3) * 0.012f;
                Rgb c = Hsv(hue, 0.9f, 1.f - y * 0.05f);
                const float sweep = std::fmod(t * 0.45f, 2.2f) - 0.6f;
                const float band = std::max(0.f, 1.f - std::fabs(across - sweep + y * 0.02f) / 0.06f);
                s.Put(Utf8(ch), Mix(c, kWhite, band * band));
            } else {
                s.Put(Utf8(ch), Mix({70, 22, 18}, {120, 40, 90}, across));   // the shadow: deep red to plum
            }
        }
        s.out += "\x1b[K\r\n";
    }
}

// a thin line with a light running along it
void Rule(Screen& s, int cols, int width, float t) {
    width = std::min(width, cols - 4);
    s.out.append((cols - width) / 2, ' ');
    const float p = std::fmod(t * 0.5f, 1.4f) - 0.2f;
    for (int x = 0; x < width; ++x) {
        const float a = float(x) / width;
        const float edge = std::min(1.f, std::min(a, 1 - a) * 6);   // fades out at both ends
        Rgb c = Mix({20, 20, 24}, Hsv(0.07f - a * 0.25f, 0.9f, 0.85f), edge);
        c = Mix(c, kWhite, std::max(0.f, 1.f - std::fabs(a - p) / 0.04f) * edge);
        s.Put(Utf8(L'━'), c);
    }
    s.out += "\x1b[K\r\n";
}

// a framed panel: rounded corners, the border's colours flowing around it
struct Panel {
    int left, width;
    float t;
    Rgb Border(int i, int perimeter) const {
        const float a = float(i) / perimeter;
        const float k = std::sin((a - t * 0.12f) * 6.2832f * 2) * 0.5f + 0.5f;
        return Mix({120, 30, 20}, Mix(kOrange, {200, 60, 200}, a), 0.35f + 0.65f * k);
    }
    void Edge(Screen& s, bool top) const {
        s.out.append(left, ' ');
        s.Put(Utf8(top ? L'╭' : L'╰'), Border(top ? 0 : width, width * 2));
        for (int x = 1; x < width - 1; ++x) s.Put(Utf8(L'─'), Border(top ? x : width * 2 - x, width * 2));
        s.Put(Utf8(top ? L'╮' : L'╯'), Border(width, width * 2));
        s.out += "\x1b[K\r\n";
    }
    void Line(Screen& s, int row, const std::string& text, Rgb c) const {
        s.out.append(left, ' ');
        s.Put(Utf8(L'│'), Border(width * 2 - row, width * 2));
        const int inner = width - 2;
        const std::string tx = text.size() > size_t(inner - 2) ? text.substr(0, inner - 2) : text;
        const int lp = (inner - int(tx.size())) / 2;
        s.out.append(lp, ' ');
        s.Put(tx, c);
        s.out.append(inner - lp - int(tx.size()), ' ');
        s.Put(Utf8(L'│'), Border(width + row, width * 2));
        s.out += "\x1b[K\r\n";
    }
};

void Centered(Screen& s, int cols, const std::string& text, Rgb c) {
    const int pad = std::max(0, (cols - int(text.size())) / 2);
    s.out.append(pad, ' ');
    s.Put(text, c);
    s.out += "\x1b[K\r\n";
}

void Draw(HANDLE out, float t) {
    CONSOLE_SCREEN_BUFFER_INFO bi{};
    GetConsoleScreenBufferInfo(out, &bi);
    const int cols = std::max(40, int(bi.srWindow.Right - bi.srWindow.Left + 1));
    const int rows = std::max(20, int(bi.srWindow.Bottom - bi.srWindow.Top + 1));
    const int panel = 22;   // lines under the art: title, line, credits, panel
    // the art at the largest size that fits: one terminal cell per art cell, or fewer (the densest of each block kept)
    const float k = std::max({1.f, float(g_art_w) / (cols - 1), float(g_art.size()) / std::max(4, rows - panel)});
    const int aw = int(g_art_w / k), ah = int(g_art.size() / k);
    const int left = std::max(0, (cols - aw) / 2);
    Screen s;
    s.out.reserve(size_t(cols) * rows * 12);
    // a new window size: the terminal re-wraps what is on the screen, so start from an empty screen
    static int last_cols = 0, last_rows = 0;
    if (cols != last_cols || rows != last_rows) { s.out += "\x1b[2J\x1b[3J"; last_cols = cols; last_rows = rows; }
    s.out += "\x1b[H";
    for (int y = 0; y < ah; ++y) {
        s.out.append(left, ' ');
        for (int x = 0; x < aw; ++x) {
            int bx = int(x * k), by = int(y * k);
            char best = ' ';
            for (int yy = by; yy < std::min(int(g_art.size()), int((y + 1) * k)); ++yy)
                for (int xx = bx; xx < int((x + 1) * k); ++xx) {
                    const char c = xx < int(g_art[yy].size()) ? g_art[yy][xx] : ' ';
                    if (Density(c) > Density(best)) { best = c; bx = xx; by = yy; }
                }
            ArtCell(s, bx, by, x, t, best);
        }
        s.out += "\x1b[K\r\n";
    }
    // title: block letters, a running line, the credits
    s.out += "\x1b[K\r\n";
    Banner(s, cols, t);
    Rule(s, cols, 96, t);
    Centered(s, cols, "by ABU 3RB      TikTok _tmo      Instagram 7.u8      Discord 7_ug      v" ASTRO_VERSION, kMuted);
    s.out += "\x1b[K\r\n";
    // state, in a framed panel
    static const char* spin[] = {"|", "/", "-", "\\"};
    const std::string sp = spin[int(t * 8) & 3];
    std::string st;
    Rgb sc = kWarn;
    switch (g_state) {
    case State::Waiting: st = sp + "  ASTRO IS ON  -  start the game (launcher -> Play) and keep this window open"; break;
    case State::AlreadyRunning: st = "!  The game was already running without ASTRO. Close it and start it again."; break;
    case State::Starting: st = sp + "  GAME FOUND  -  loading ASTRO..."; break;
    case State::NoLayer: st = "!  The game started without ASTRO. Close it and start it again."; break;
    case State::Loaded: st = sp + "  ASTRO LOADED  -  preparing the menu..."; sc = kGood; break;
    case State::Ready: st = "<>  CONNECTED  -  press F8 in the game  <>"; sc = kGood; break;
    }
    if (g_state == State::Ready) sc = Mix(kGood, kWhite, std::sin(t * 4) * 0.25f + 0.25f);
    const int pw = std::min(cols - 4, 100);
    const Panel p{(cols - pw) / 2, pw, t};
    p.Edge(s, true);
    p.Line(s, 1, st, sc);
    p.Line(s, 2, "", kMuted);
    for (int i = 0; i < 5; ++i) {
        if (i < int(g_log.size())) {
            const LogLine& l = g_log[i];
            p.Line(s, 3 + i, l.time + "   " + l.text, l.kind == 1 ? kGood : l.kind == 2 ? kWarn : kMuted);
        } else p.Line(s, 3 + i, "", kMuted);
    }
    p.Edge(s, false);
    Centered(s, cols, "close this window (or Ctrl+C) to turn ASTRO off", {75, 75, 85});
    s.out += "\x1b[0m\x1b[J";
    DWORD n;
    WriteConsoleA(out, s.out.data(), DWORD(s.out.size()), &n, nullptr);
}

bool g_preview = false;
BOOL WINAPI OnConsole(DWORD ev) {
    if (ev == CTRL_C_EVENT || ev == CTRL_BREAK_EVENT) { g_quit = true; return TRUE; }   // the loop restores the screen
    if (!g_preview) Unregister();   // close button, logoff, shutdown: the process ends right after
    return FALSE;
}

// the art needs the whole screen: Windows Terminal -> its fullscreen (F11, only when not already fullscreen);
// the classic console -> fullscreen display mode, else maximized
void GoFullscreen(HANDLE out) {
    HWND con = GetConsoleWindow();
    wchar_t cls[128] = {};
    if (con) GetClassNameW(con, cls, 128);
    if (!wcscmp(cls, L"ConsoleWindowClass")) {
        COORD size;
        if (!SetConsoleDisplayMode(out, CONSOLE_FULLSCREEN_MODE, &size)) ShowWindow(con, SW_MAXIMIZE);
        return;
    }
    // Windows Terminal: its window carries the title of the active tab, ours ("ASTRO CTR V2")
    HWND wt = nullptr;
    for (int tries = 0; !wt && tries < 20; ++tries, Sleep(50))
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            wchar_t c[128] = {}, t[256] = {};
            GetClassNameW(h, c, 128);
            GetWindowTextW(h, t, 256);
            if (!wcscmp(c, L"CASCADIA_HOSTING_WINDOW_CLASS") && wcsstr(t, L"ASTRO CTR V2")) { *reinterpret_cast<HWND*>(lp) = h; return FALSE; }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&wt));
    if (!wt) return;
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    RECT r{};
    GetWindowRect(wt, &r);
    if (GetMonitorInfoW(MonitorFromWindow(wt, MONITOR_DEFAULTTONEAREST), &mi) && r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top &&
        r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom && !(GetWindowLongW(wt, GWL_STYLE) & WS_CAPTION))
        return;   // already fullscreen: F11 would turn it off
    ShowWindow(wt, SW_MAXIMIZE);   // always works; fullscreen below needs the key
    SetForegroundWindow(wt);
    Sleep(100);
    if (GetForegroundWindow() != wt) return;   // the key would go to another window: stay maximized
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_F11;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void Message(const wchar_t* text) {
    std::fwprintf(stdout, L"\n  ASTRO CTR V2\n\n  %ls\n\n  Press Enter to close.", text);
    std::getwchar();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"ASTRO CTR V2");
    // --preview: the screen only (no registration, no single-instance check), for screenshots and testing
    const bool preview = argc > 1 && !wcscmp(argv[1], L"--preview");
    g_preview = preview;
    const fs::path dir = ExeDir();
    g_manifest = (dir / L"VkLayer_ASTRO_ctr_implicit.json").wstring();
    if (!fs::exists(dir / L"astro_ctr.dll") || !fs::exists(g_manifest)) {
        Message(L"astro_ctr.dll or VkLayer_ASTRO_ctr_implicit.json is missing next to ASTROCTRV2.exe. Keep the files together.");
        return 2;
    }
    HANDLE watcher = preview ? nullptr : CreateMutexW(nullptr, FALSE, L"Local\\ASTRO_CTR_Watcher");
    if (!preview && GetLastError() == ERROR_ALREADY_EXISTS) {
        Message(L"ASTRO is already open (ASTRO_CTR.exe or another ASTRO CTR V2). Close it, then open this again.");
        return 1;
    }
    SetConsoleCtrlHandler(&OnConsole, TRUE);
    if (!preview && !Register()) { Message(L"Could not register the ASTRO layer for this Windows user."); return 4; }

    // the art, line by line
    for (const char* p = kArt; *p;) {
        const char* e = std::strchr(p, '\n');
        g_art.emplace_back(p, e ? e - p : std::strlen(p));
        g_art_w = std::max(g_art_w, int(g_art.back().size()));
        p = e ? e + 1 : p + std::strlen(p);
    }
    FindStars();
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(out, &mode);
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
    SetConsoleOutputCP(CP_UTF8);
    GoFullscreen(out);
    Sleep(150);   // the terminal settles on its new size before the first frame
    // the classic console: make the window as large as the screen allows, so the art shows at full size
    CONSOLE_SCREEN_BUFFER_INFO bi{};
    if (GetConsoleScreenBufferInfo(out, &bi)) {
        const COORD big = GetLargestConsoleWindowSize(out);
        const SHORT w = SHORT(std::min<int>(big.X, g_art_w + 2)), h = SHORT(std::min<int>(big.Y, int(g_art.size()) + 14));
        if (w > bi.srWindow.Right - bi.srWindow.Left + 1 || h > bi.srWindow.Bottom - bi.srWindow.Top + 1) {
            SetConsoleScreenBufferSize(out, COORD{std::max(w, bi.dwSize.X), std::max(h, bi.dwSize.Y)});
            SMALL_RECT r{0, 0, SHORT(w - 1), SHORT(h - 1)};
            SetConsoleWindowInfo(out, TRUE, &r);
        }
    }
    const char* enter = "\x1b[?1049h\x1b[?25l\x1b[2J";   // own screen, no cursor
    DWORD n;
    WriteConsoleA(out, enter, DWORD(std::strlen(enter)), &n, nullptr);

    Log(preview ? "preview: nothing registered" : "ASTRO layer registered for this Windows user", 1);
    Watch w;
    w.Init(dir);
    const ULONGLONG t0 = GetTickCount64();
    while (!g_quit) {
        w.Poll();
        Draw(out, float(GetTickCount64() - t0) / 1000.f);
        Sleep(50);
    }
    const char* leave = "\x1b[0m\x1b[?25h\x1b[?1049l";
    WriteConsoleA(out, leave, DWORD(std::strlen(leave)), &n, nullptr);
    if (!preview) Unregister();
    std::fwprintf(stdout, L"ASTRO CTR V2 closed: ASTRO is off.\n");
    if (watcher) CloseHandle(watcher);
    return 0;
}
