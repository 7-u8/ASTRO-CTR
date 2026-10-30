// ASTRO CTR (watcher). Run it before or after opening the CTRNF launcher, then start the game as usual.
// While this window is open the ASTRO Vulkan layer is registered for the current Windows user only
// (HKCU\Software\Khronos\Vulkan\ImplicitLayers), so the Vulkan loader loads it when the game starts.
// Nothing is injected. Closing this window removes the registration. The layer stays inert unless this
// watcher is running (it checks the "Local\ASTRO_CTR_Watcher" mutex), so a leftover registration does nothing.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

namespace {

const wchar_t* kRegKey = L"Software\\Khronos\\Vulkan\\ImplicitLayers";
std::wstring g_manifest;   // full path of the implicit-layer manifest, the registry value name

void Say(const std::wstring& s) { std::fwprintf(stdout, L"%ls\n", s.c_str()); std::fflush(stdout); }

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

// pid of the game the layer just loaded in: last "<pid> layer_loaded" line of %APPDATA%\ASTRO-CTR\status.txt
DWORD LayerPid() {
    const wchar_t* appdata = _wgetenv(L"APPDATA");
    if (!appdata) return 0;
    std::FILE* f = _wfopen((fs::path(appdata) / L"ASTRO-CTR" / L"status.txt").c_str(), L"r");
    if (!f) return 0;
    DWORD pid = 0;
    unsigned long p;
    char what[64];
    while (std::fscanf(f, "%lu %63s", &p, what) == 2)
        if (!strcmp(what, "layer_loaded")) pid = p;
    std::fclose(f);
    return pid;
}

bool Register() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    DWORD zero = 0;   // 0 = enabled
    bool ok = RegSetValueExW(k, g_manifest.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero), sizeof zero) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

void Unregister() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegDeleteValueW(k, g_manifest.c_str());
    RegCloseKey(k);
}

// A new build is dropped as astro_ctr.new.dll (the DLL in use cannot be replaced while the game runs);
// it replaces astro_ctr.dll as soon as no game holds it.
void InstallUpdate(const fs::path& dir) {
    static bool told = false;
    const fs::path staged = dir / L"astro_ctr.new.dll";
    if (!fs::exists(staged) || FindProcess(L"sacredfire.exe")) return;
    if (MoveFileExW(staged.c_str(), (dir / L"astro_ctr.dll").c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Say(L"ASTRO update installed (astro_ctr.dll replaced).");
        told = false;
    } else if (!told) {
        Say(L"an ASTRO update is waiting: astro_ctr.dll is still in use by another program (error " +
            std::to_wstring(GetLastError()) + L"); retrying.");
        told = true;
    }
}

BOOL WINAPI OnConsoleClose(DWORD) {   // close button, Ctrl+C, logoff, shutdown
    Unregister();
    return FALSE;
}

}  // namespace

int wmain() {
    Say(L"ASTRO CTR " ASTRO_VERSION_W L"  -  by ABU 3RB  (TikTok _tmo | Instagram 7.u8 | Discord 7_ug)");
    const fs::path dir = ExeDir();
    g_manifest = (dir / L"VkLayer_ASTRO_ctr_implicit.json").wstring();
    if (!fs::exists(dir / L"astro_ctr.dll") || !fs::exists(g_manifest)) {
        Say(L"ERROR: astro_ctr.dll or VkLayer_ASTRO_ctr_implicit.json is missing next to this program: " + dir.wstring());
        std::getwchar();
        return 2;
    }
    HANDLE watcher = CreateMutexW(nullptr, FALSE, L"Local\\ASTRO_CTR_Watcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Say(L"ASTRO CTR is already open in another window.");
        std::getwchar();
        return 1;
    }
    SetConsoleCtrlHandler(&OnConsoleClose, TRUE);
    if (!Register()) {
        Say(L"ERROR: could not register the layer for this user (HKCU).");
        std::getwchar();
        return 4;
    }
    Say(L"ready: start the game normally (CTRNF launcher -> Play). Keep this window open.");
    Say(L"closing this window turns ASTRO off.\n");

    HANDLE loaded = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ASTRO_CTR_LayerLoaded");
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ASTRO_CTR_OverlayReady");
    if (DWORD pid = FindProcess(L"sacredfire.exe"))
        Say(L"note: the game is already running (pid " + std::to_wstring(pid) +
            L") without ASTRO. ASTRO loads when the game starts: close the game and press Play again.");

    for (;;) {   // one pass per game session
        // signalled by the layer inside the starting game; meanwhile install a staged update when the game is closed
        while (WaitForSingleObject(loaded, 2000) == WAIT_TIMEOUT) InstallUpdate(dir);
        DWORD pid = LayerPid();
        if (!pid) pid = FindProcess(L"sacredfire.exe");
        Say(L"game detected (pid " + std::to_wstring(pid) + L"): ASTRO layer loaded");
        HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
        HANDLE waits[2] = {ready, game};
        DWORD w = game ? WaitForMultipleObjects(2, waits, FALSE, 180000) : WaitForSingleObject(ready, 180000);
        if (w == WAIT_OBJECT_0) Say(L"OK: menu ready. Press F8 in game.");
        else if (w == WAIT_OBJECT_0 + 1) Say(L"the game closed before the menu was ready");
        else Say(L"PROBLEM: the menu did not draw. Send %APPDATA%\\ASTRO-CTR\\astro_ctr.log");
        if (game) { WaitForSingleObject(game, INFINITE); CloseHandle(game); }
        ResetEvent(loaded);
        ResetEvent(ready);
        Say(L"game closed. Waiting for the next start...\n");
    }
    CloseHandle(watcher);
}
