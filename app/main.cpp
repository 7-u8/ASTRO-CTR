// ASTRO CTR app: the window the user keeps open while playing.
// While it is open the ASTRO Vulkan layer is registered for the current Windows user only
// (HKCU\Software\Khronos\Vulkan\ImplicitLayers), so the Vulkan loader loads it when the game starts. Nothing is
// injected. Closing the app removes the registration; the layer stays inert unless this app is running (it checks
// the "Local\ASTRO_CTR_Watcher" mutex).
// The app edits the same %APPDATA%\ASTRO-CTR\config.json as the in-game menu and tells the game to reload it
// (event kConfigPushedEvent), and manages the shared presets folder.
//
// The same file is compiled into the layer DLL with ASTRO_IN_GAME defined: there every page except the MODS XML list
// is drawn inside the game, in the F8 menu (AstroUi_Draw at the end). The app window itself keeps the layer
// registration, the DLL autoload and the MODS XML list.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#ifndef ASTRO_IN_GAME
#include <d3d11.h>
#include <dwmapi.h>
#endif
#include <shellapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <wincodec.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>
#include <sstream>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#ifndef ASTRO_IN_GAME
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#endif

#include "config/config.h"
#include "core/gamepad_draw.h"
#include "config/fx_protocol.h"
#include "config/json.h"
#include "config/mods_protocol.h"
#include "config/presets.h"
#include "config/wheel_protocol.h"
#include "core/tex_recolor.h"
#include "core/paint_patterns.h"
#ifdef ASTRO_IN_GAME
namespace astro { void PlayersEsp_Tab(); void Menu_DiagPage(); }   // pages of the layer itself
#else
#include "resource.h"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#endif

#pragma GCC diagnostic ignored "-Wunused-function"   // one file, two builds: each leaves some pages unused
namespace fs = std::filesystem;
using namespace astro;

namespace {

// ---------------- brand ----------------
// Two colours: near-black and ASTRO orange. Greys are the black lifted, text is white at reduced alpha.
const ImU32 kBg = IM_COL32(13, 13, 15, 255);
const ImU32 kPanel = IM_COL32(20, 20, 23, 255);
const ImU32 kPanel2 = IM_COL32(27, 27, 31, 255);
const ImU32 kLine = IM_COL32(38, 38, 43, 255);
const ImU32 kOrange = IM_COL32(255, 90, 31, 255);
const ImU32 kOrangeDim = IM_COL32(255, 90, 31, 40);
const ImU32 kText = IM_COL32(240, 240, 242, 255);
const ImU32 kMuted = IM_COL32(240, 240, 242, 120);
const ImVec4 kOrangeV(1.0f, 90 / 255.f, 31 / 255.f, 1);

HWND g_hwnd = nullptr;   // owner of file dialogs (the app window; none in the game)
struct Tex { ImTextureID id = ImTextureID_Invalid; float w = 0, h = 0; };
Tex g_mark, g_word;      // logos (resources of the app exe; not in the game)

#ifndef ASTRO_IN_GAME
// ---------------- D3D11 ----------------
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
IDXGISwapChain* g_swap = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
UINT g_resize_w = 0, g_resize_h = 0;

void CreateRtv() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) { g_dev->CreateRenderTargetView(back, nullptr, &g_rtv); back->Release(); }
}
void ReleaseRtv() { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } }

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)   // no usable GPU driver: software renderer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRtv();
    return true;
}
void DestroyDevice() {
    ReleaseRtv();
    if (g_swap) g_swap->Release();
    if (g_ctx) g_ctx->Release();
    if (g_dev) g_dev->Release();
}

// PNG from the exe's resources -> texture (WIC decodes it)
Tex LoadPngResource(int res) {
    Tex t;
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(res), MAKEINTRESOURCEW(10));   // RT_RCDATA
    if (!r) return t;
    HGLOBAL h = LoadResource(nullptr, r);
    const void* data = LockResource(h);
    const DWORD size = SizeofResource(nullptr, r);
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return t;
    IWICStream* st = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    UINT w = 0, hh = 0;
    std::vector<BYTE> px;
    if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)data, size)) &&
        SUCCEEDED(wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &hh))) {
        px.resize(size_t(w) * hh * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) px.clear();
    }
    for (IUnknown* u : {(IUnknown*)conv, (IUnknown*)fr, (IUnknown*)dec, (IUnknown*)st, (IUnknown*)wic}) if (u) u->Release();
    if (px.empty()) return t;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = hh; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sub{px.data(), w * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sub, &tex))) {
        g_dev->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
    }
    if (srv) { t.id = (ImTextureID)(intptr_t)srv; t.w = float(w); t.h = float(hh); }
    return t;
}
#endif


// PNG file -> RGBA pixels (WIC)
std::vector<BYTE> DecodePngFile(const fs::path& file, UINT& w, UINT& h) {
    std::vector<BYTE> px;
    w = h = 0;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return px;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(conv->GetSize(&w, &h))) {
        px.resize(size_t(w) * h * 4);
        if (FAILED(conv->CopyPixels(nullptr, w * 4, UINT(px.size()), px.data()))) px.clear();
    }
    for (IUnknown* u : {(IUnknown*)conv, (IUnknown*)fr, (IUnknown*)dec, (IUnknown*)wic}) if (u) u->Release();
    return px;
}

// a texture the app can redraw (live previews): an ImGui user texture, so the renderer (D3D11 in the app, Vulkan in
// the game) creates and uploads it
struct DynTex {
    ImTextureData* td = nullptr;
    bool ok() const { return td != nullptr; }
    ImTextureRef Id() const { return td ? td->GetTexRef() : ImTextureRef(); }
    bool Create(UINT width, UINT height) {
        td = IM_NEW(ImTextureData)();
        td->Create(ImTextureFormat_RGBA32, int(width), int(height));
        ImGui::RegisterUserTexture(td);
        return true;
    }
    void Update(const std::vector<BYTE>& rgba) {
        if (!td || rgba.size() < size_t(td->Width) * td->Height * 4) return;
        std::memcpy(td->GetPixels(), rgba.data(), size_t(td->Width) * td->Height * 4);
        if (td->Status == ImTextureStatus_OK) ImTextureDataQueueUpload(td, 0, 0, td->Width, td->Height);   // else: uploaded when created
    }
};

// the folder of ASTRO's files (catalogs, wheels): next to ASTRO_CTR.exe, the same folder as astro_ctr.dll
fs::path ExeDir() {
    wchar_t p[MAX_PATH] = {};
    HMODULE self = nullptr;   // in the game: astro_ctr.dll, not the game's exe
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ExeDir), &self);
    GetModuleFileNameW(self, p, MAX_PATH);
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

#ifdef ASTRO_IN_GAME
// inside the game the game is always there: the pages that wait for it just work
enum class Game { None, AlreadyRunning, Starting, NoLayer, Loading, Ready };
struct GameWatch {
    DWORD pid = GetCurrentProcessId();
    Game state = Game::Ready;
    bool Connected() const { return true; }
};
GameWatch g_game;
#else
// ---------------- layer registration ----------------
const wchar_t* kRegKey = L"Software\\Khronos\\Vulkan\\ImplicitLayers";
std::wstring g_manifest;

bool Register() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    // Only ONE ASTRO layer may be registered: the Vulkan loader takes one of several layers with the same name, so a
    // registration left by another copy (an older folder, a window that was killed) made the game load that copy's DLL
    // instead of this one (seen 2026-11-01: the game ran an old DLL from dist\ while run\ was open).
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
    const fs::path staged = dir / L"astro_ctr.new.dll";
    std::error_code ec;
    if (fs::exists(staged, ec)) MoveFileExW(staged.c_str(), (dir / L"astro_ctr.dll").c_str(), MOVEFILE_REPLACE_EXISTING);
}

// ---------------- game state ----------------
enum class Game { None, AlreadyRunning, Starting, NoLayer, Loading, Ready };
struct GameWatch {
    HANDLE loaded = nullptr, ready = nullptr;
    DWORD pid = 0;
    DWORD stale_pid = 0;   // game that was running before ASTRO: it has no layer
    ULONGLONG seen_at = 0, next_poll = 0;
    Game state = Game::None;

    void Init() {
        loaded = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ASTRO_CTR_LayerLoaded");
        ready = CreateEventW(nullptr, TRUE, FALSE, L"Local\\ASTRO_CTR_OverlayReady");
        stale_pid = FindProcess(L"sacredfire.exe");
        if (stale_pid) state = Game::AlreadyRunning;
    }
    // returns true when a game with ASTRO just appeared
    bool Poll(const fs::path& dir) {
        const ULONGLONG now = GetTickCount64();
        if (now < next_poll) return false;
        next_poll = now + 500;
        const Game before = state;
        const DWORD running = FindProcess(L"sacredfire.exe");
        if (!running) {
            if (pid || state != Game::None) { ResetEvent(loaded); ResetEvent(ready); }
            pid = stale_pid = 0;
            state = Game::None;
            InstallUpdate(dir);
            return false;
        }
        if (running == stale_pid) { state = Game::AlreadyRunning; return false; }
        if (pid != running) { pid = running; seen_at = now; }
        if (WaitForSingleObject(ready, 0) == WAIT_OBJECT_0) state = Game::Ready;
        else if (WaitForSingleObject(loaded, 0) == WAIT_OBJECT_0) state = Game::Loading;
        else state = now - seen_at > 30000 ? Game::NoLayer : Game::Starting;
        return (state == Game::Loading || state == Game::Ready) && before != Game::Loading && before != Game::Ready;
    }
    bool Connected() const { return state == Game::Loading || state == Game::Ready; }
};
GameWatch g_game;
#endif

// ---------------- config sync ----------------
struct Sync {
    Config cfg;
    bool dirty = false;
    double changed_at = 0, saved_at = 0;
    fs::file_time_type stamp{};
    HANDLE pushed = nullptr;
    std::string note;
    double note_at = -10;

    void Load() {
        cfg = LoadConfig(DefaultConfigPath()).config;
        std::error_code ec;
        stamp = fs::last_write_time(DefaultConfigPath(), ec);
    }
    void Touch() { dirty = true; changed_at = ImGui::GetTime(); }
    void Note(const std::string& s) { note = s; note_at = ImGui::GetTime(); }
    // write about 15 times a second while the user is changing (the kart follows the picker live), then tell the
    // game; pick up saves made in the game
    void Tick() {
        std::error_code ec;
        if (dirty && ImGui::GetTime() - saved_at > 0.066) {
            saved_at = ImGui::GetTime();
            std::string err;
            if (SaveConfig(DefaultConfigPath(), cfg, &err)) {
                dirty = false;
                stamp = fs::last_write_time(DefaultConfigPath(), ec);
                if (pushed) SetEvent(pushed);
            } else Note("could not save: " + err);
        } else if (!dirty) {
            const auto t = fs::last_write_time(DefaultConfigPath(), ec);
            if (!ec && t != stamp) { Load(); Note("settings updated from the game"); }
        }
    }
};
Sync g_sync;

// ---------------- fonts ----------------
ImFont* g_font = nullptr;   // Bahnschrift
ImFont* g_bold = nullptr;   // Segoe UI Bold
float g_dpi = 1.0f;

// ---------------- widgets ----------------
ImU32 U(const Rgba& c) { return IM_COL32(c.r, c.g, c.b, 255); }

void Text(const char* s, ImU32 col = kText, float size = 0, ImFont* f = nullptr) {
    ImGui::PushFont(f ? f : g_font, size);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(s);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
void Wrapped(const char* s, ImU32 col = kMuted) {
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", s);
    ImGui::PopStyleColor();
}

// pill switch; returns true when toggled
bool Switch(const char* id, bool* v) {
    const float h = 22, w = 42;
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool hit = ImGui::InvisibleButton(id, ImVec2(w, h));
    if (hit) *v = !*v;
    ImGuiStorage* st = ImGui::GetStateStorage();
    const ImGuiID key = ImGui::GetID(id);
    float t = st->GetFloat(key, *v ? 1.f : 0.f);
    t += ((*v ? 1.f : 0.f) - t) * std::min(1.f, ImGui::GetIO().DeltaTime * 14);
    st->SetFloat(key, t);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 track = ImGui::ColorConvertFloat4ToU32(ImLerp(ImGui::ColorConvertU32ToFloat4(kLine), kOrangeV, t));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), track, h);
    dl->AddCircleFilled(ImVec2(p.x + h / 2 + t * (w - h), p.y + h / 2), h / 2 - 3, *v ? kBg : kText);
    return hit;
}

// rounded button; primary = orange fill
bool Button(const char* label, bool primary = false, ImVec2 size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, primary ? kOrange : kPanel2);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, primary ? IM_COL32(255, 120, 70, 255) : kLine);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, primary ? IM_COL32(230, 75, 20, 255) : kLine);
    ImGui::PushStyleColor(ImGuiCol_Text, primary ? kBg : kText);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 9));
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

// card = rounded panel; Begin/End
void BeginCard(const char* id, float height = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::BeginChild(id, ImVec2(0, height), height > 0 ? ImGuiChildFlags_None : ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar);
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 6));
}

// card header: title on the left, switch on the right
bool CardHeader(const char* title, const char* sub, bool* on) {
    ImGui::BeginGroup();
    Text(title, kText, 20, g_bold);
    if (sub) Text(sub, kMuted, 14);
    ImGui::EndGroup();
    bool ch = false;
    if (on) {
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 4);
        ch = Switch((std::string("##sw") + title).c_str(), on);
    }
    ImGui::Dummy(ImVec2(0, 8));
    return ch;
}

// rainbow ids, the same strings the in-game menu uses
bool Rainbow(const std::string& id) {
    const std::string& r = g_sync.cfg.fx.rainbow;
    for (size_t p = 0; (p = r.find(id, p)) != std::string::npos; p += id.size())
        if ((p == 0 || r[p - 1] == ',') && (p + id.size() == r.size() || r[p + id.size()] == ',')) return true;
    return false;
}
void SetRainbow(const std::string& id, bool on) {
    std::string out;
    const std::string& r = g_sync.cfg.fx.rainbow;
    for (size_t a = 0; a <= r.size();) {
        size_t b = r.find(',', a);
        if (b == std::string::npos) b = r.size();
        std::string t = r.substr(a, b - a);
        if (!t.empty() && t != id) out += (out.empty() ? "" : ",") + t;
        a = b + 1;
    }
    if (on) out += (out.empty() ? "" : ",") + id;
    g_sync.cfg.fx.rainbow = out;
}

const Rgba kQuick[] = {{255, 90, 31, 255}, {255, 30, 30, 255}, {255, 210, 0, 255}, {40, 230, 90, 255}, {0, 220, 255, 255},
                       {40, 90, 255, 255}, {170, 60, 255, 255}, {255, 40, 170, 255}, {255, 255, 255, 255}, {20, 20, 20, 255}};

bool g_open_first_picker = false;

// Saturation/value square + hue bar + hex field, drawn here (the colour follows the mouse while dragging).
// The hue is kept while dragging so it does not jump when the colour reaches grey or black.
bool HsvPicker(Rgba& c) {
    const float W = 252, H = 168, bar = 16;
    static float hue = 0;
    static ImGuiID dragging = 0;
    Hsv hsv = ToHsv(c);
    const ImGuiID sq_id = ImGui::GetID("sv"), hue_id = ImGui::GetID("hue");
    if (dragging != sq_id && dragging != hue_id && hsv.s > 0.001f && hsv.v > 0.001f) hue = hsv.h;
    bool ch = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // square: white -> pure hue across, then transparent -> black down
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("sv", ImVec2(W, H));
    const ImU32 pure = U(FromHsv(hue, 1, 1));
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + W, p.y + H), IM_COL32_WHITE, pure, pure, IM_COL32_WHITE);
    dl->AddRectFilledMultiColor(p, ImVec2(p.x + W, p.y + H), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32_BLACK, IM_COL32_BLACK);
    if (ImGui::IsItemActive()) {
        dragging = sq_id;
        const ImVec2 m = ImGui::GetIO().MousePos;
        const Rgba n = FromHsv(hue, std::clamp((m.x - p.x) / W, 0.f, 1.f), 1 - std::clamp((m.y - p.y) / H, 0.f, 1.f));
        if (!(n == c)) { c = n; ch = true; }
    }
    const Hsv now = ToHsv(c);
    const ImVec2 dot(p.x + now.s * W, p.y + (1 - now.v) * H);
    dl->AddCircle(dot, 7, IM_COL32_BLACK, 0, 3);
    dl->AddCircle(dot, 7, IM_COL32_WHITE, 0, 2);
    // hue bar
    ImGui::Dummy(ImVec2(0, 4));
    ImVec2 b = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("hue", ImVec2(W, bar));
    for (int i = 0; i < 6; ++i)
        dl->AddRectFilledMultiColor(ImVec2(b.x + W * i / 6, b.y), ImVec2(b.x + W * (i + 1) / 6, b.y + bar),
                                    U(FromHsv(i / 6.f, 1, 1)), U(FromHsv((i + 1) / 6.f, 1, 1)), U(FromHsv((i + 1) / 6.f, 1, 1)), U(FromHsv(i / 6.f, 1, 1)));
    if (ImGui::IsItemActive()) {
        dragging = hue_id;
        hue = std::clamp((ImGui::GetIO().MousePos.x - b.x) / W, 0.f, 0.9999f);
        const Rgba n = FromHsv(hue, std::max(now.s, 0.05f), std::max(now.v, 0.05f));
        if (!(n == c)) { c = n; ch = true; }
    }
    const float hx = b.x + hue * W;
    dl->AddRect(ImVec2(hx - 3, b.y - 2), ImVec2(hx + 3, b.y + bar + 2), IM_COL32_WHITE, 2.0f, 0, 2.0f);
    if (!ImGui::IsMouseDown(0)) dragging = 0;
    // hex
    ImGui::Dummy(ImVec2(0, 4));
    char hex[8];
    std::snprintf(hex, sizeof hex, "%02X%02X%02X", c.r, c.g, c.b);
    ImGui::SetNextItemWidth(W);
    if (ImGui::InputText("##hex", hex, sizeof hex, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase) && std::strlen(hex) == 6)
        if (auto n = ParseHexColor(hex)) { c = *n; c.a = 255; ch = true; }
    ImGui::Dummy(ImVec2(0, 2));
    return ch;
}

// colour row: label, swatch that opens a picker, hex, and an RGB chip when id is given. Returns true on change.
bool ColorRow(const char* label, Rgba& c, const char* id = nullptr) {
    bool ch = false;
    ImGui::PushID(label);
    const float rowh = 34;
    ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPosY(start.y + 7);
    Text(label, kText, 16);
    const float right = ImGui::GetContentRegionMax().x;
    const bool rb = id && Rainbow(id);
    // RGB chip
    if (id) {
        ImGui::SetCursorPos(ImVec2(right - 250, start.y + 4));
        ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("rgb", ImVec2(54, 26))) { SetRainbow(id, !rb); ch = true; }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (rb) {
            const float t = float(ImGui::GetTime()) * 0.4f;
            for (int i = 0; i < 4; ++i)
                dl->AddRectFilled(ImVec2(p.x + i * 13.5f, p.y), ImVec2(p.x + (i + 1) * 13.5f, p.y + 26), U(FromHsv(t + i * 0.1f, 1, 1)),
                                  13, i == 0 ? ImDrawFlags_RoundCornersLeft : i == 3 ? ImDrawFlags_RoundCornersRight : ImDrawFlags_RoundCornersNone);
        } else dl->AddRect(p, ImVec2(p.x + 54, p.y + 26), ImGui::IsItemHovered() ? kOrange : kLine, 13, 0, 1.5f);
        ImGui::PushFont(g_bold, 13);
        const ImVec2 ts = ImGui::CalcTextSize("RGB");
        dl->AddText(ImVec2(p.x + (54 - ts.x) / 2, p.y + (26 - ts.y) / 2), rb ? kBg : kMuted, "RGB");
        ImGui::PopFont();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rainbow: cycle this colour through every colour");
    }
    // swatch
    ImGui::SetCursorPos(ImVec2(right - 186, start.y + 2));
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("sw", ImVec2(186, 30))) ImGui::OpenPopup("pick");
    if (g_open_first_picker) { g_open_first_picker = false; ImGui::OpenPopup("pick"); }   // --preview ... pick
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + 186, p.y + 30), kPanel2, 8);
    dl->AddRectFilled(ImVec2(p.x + 4, p.y + 4), ImVec2(p.x + 52, p.y + 26), U(c), 6);
    if (ImGui::IsItemHovered()) dl->AddRect(p, ImVec2(p.x + 186, p.y + 30), kOrange, 8, 0, 1.5f);
    char hex[16];
    std::snprintf(hex, sizeof hex, "#%02X%02X%02X", c.r, c.g, c.b);
    ImGui::PushFont(g_font, 15);
    dl->AddText(ImVec2(p.x + 64, p.y + 7), rb ? kMuted : kText, rb ? "RGB cycle" : hex);
    ImGui::PopFont();
    ImGui::SetNextWindowBgAlpha(1);
    if (ImGui::BeginPopup("pick")) {
        ch |= HsvPicker(c);
        // ready-made colours: hovering one shows it in the game at once, leaving puts the colour back, a click keeps it
        static Rgba before;
        static ImGuiID previewing = 0;
        const ImGuiID me = ImGui::GetID("quick");
        if (ch) previewing = 0;   // the picker was used: that colour stays
        bool hovered_any = false;
        for (int i = 0; i < IM_ARRAYSIZE(kQuick); ++i) {
            if (i) ImGui::SameLine(0, 6);
            ImGui::PushID(i);
            ImVec2 q = ImGui::GetCursorScreenPos();
            const bool click = ImGui::InvisibleButton("q", ImVec2(22, 22));
            const bool hov = ImGui::IsItemHovered();
            ImGui::GetWindowDrawList()->AddRectFilled(q, ImVec2(q.x + 22, q.y + 22), U(kQuick[i]), 5);
            if (hov) ImGui::GetWindowDrawList()->AddRect(ImVec2(q.x - 2, q.y - 2), ImVec2(q.x + 24, q.y + 24), kText, 6, 0, 1.5f);
            if (click) { c = kQuick[i]; ch = true; previewing = 0; }
            else if (hov) {
                hovered_any = true;
                if (previewing != me) { before = c; previewing = me; }
                if (!(c == kQuick[i])) { c = kQuick[i]; ch = true; }
            }
            ImGui::PopID();
        }
        if (!hovered_any && previewing == me) { c = before; ch = true; previewing = 0; }
        ImGui::EndPopup();
    }
    ImGui::SetCursorPos(ImVec2(start.x, start.y + rowh + 6));
    ImGui::Dummy(ImVec2(0, 0));   // the row's height counts for the parent
    ImGui::PopID();
    return ch;
}

bool Slider(const char* label, float* v, float lo, float hi, const char* fmt, ImGuiSliderFlags flags = ImGuiSliderFlags_Logarithmic) {
    ImGui::PushID(label);
    ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPosY(start.y + 5);
    Text(label, kText, 16);
    ImGui::SetCursorPos(ImVec2(ImGui::GetContentRegionMax().x - 250, start.y + 2));
    ImGui::SetNextItemWidth(250);
    bool r = ImGui::SliderFloat("##s", v, lo, hi, fmt, flags);
    ImGui::SetCursorPos(ImVec2(start.x, start.y + 40));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
    return r;
}

// ---------------- pages ----------------
enum class Page { Home, Game, Players, Nitro, Paint, Glow, Wheel, Mods, Maps, Effects, Items, Misc, Presets, Inject, Settings, Diag, About };
Page g_page = Page::Home;
bool g_main = false;                 // false = the "OPEN CRASHNF" screen
int g_pattern_all = 1, g_pattern[kPaintSlots] = {1, 1, 1};
Rgba g_paint_all{255, 90, 31, 255};

void PageTitle(const char* t, const char* sub) {
    Text(t, kText, 30, g_bold);
    Text(sub, kMuted, 16);
    ImGui::Dummy(ImVec2(0, 14));
}

void StatusCard() {
    BeginCard("status");
    const char* title;
    const char* sub;
    ImU32 dot;
    switch (g_game.state) {
#ifdef ASTRO_IN_GAME
    case Game::Ready: title = "Connected"; sub = "ASTRO is running inside the game. F8 opens and closes this menu."; dot = kOrange; break;
#else
    case Game::Ready: title = "Connected"; sub = "ASTRO is running inside the game. Press F8 in game for the overlay menu."; dot = kOrange; break;
#endif
    case Game::Loading: title = "Loading"; sub = "ASTRO is loaded; the menu appears with the first frame."; dot = kOrange; break;
    case Game::Starting: title = "Game starting"; sub = "Waiting for the game to load ASTRO..."; dot = kMuted; break;
    case Game::NoLayer: title = "Not loaded"; sub = "The game started but ASTRO did not load. Close the game and start it again."; dot = kMuted; break;
    case Game::AlreadyRunning: title = "Restart the game"; sub = "The game was already running before ASTRO. Close it and start it again."; dot = kMuted; break;
    default: title = "Waiting for the game"; sub = "Start Crash Team Racing Nitro-Fueled. Your colours apply automatically."; dot = kMuted; break;
    }
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float pulse = g_game.Connected() ? 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 3) : 0;
    if (g_game.Connected()) ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 8, p.y + 14), 8 + 6 * pulse, IM_COL32(255, 90, 31, int(60 * (1 - pulse))));
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 8, p.y + 14), 7, dot);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 28);
    ImGui::BeginGroup();
    Text(title, kText, 22, g_bold);
    Wrapped(sub);
    ImGui::EndGroup();
    EndCard();
}

void Swatches(const std::vector<Rgba>& cols, float s = 18) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    for (size_t i = 0; i < cols.size(); ++i)
        ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x + i * (s + 5), p.y), ImVec2(p.x + i * (s + 5) + s, p.y + s), U(cols[i]), 5);
    ImGui::Dummy(ImVec2(cols.size() * (s + 5), s));
}

void HomePage() {
    PageTitle("Home", "Everything ASTRO changes, at a glance.");
    StatusCard();
#ifndef ASTRO_IN_GAME
    BeginCard("f8");
    CardHeader("Everything is in the game now", nullptr, nullptr);
    Wrapped("Colours, paint, wheels, maps, items, effects, presets, the race tools, players and settings are in the in-game "
            "menu: press F8 in the game. This window keeps ASTRO running and holds the MODS (XML patches) list.");
    EndCard();
    return;
#endif
    Config& c = g_sync.cfg;
    struct Row { const char* name; const char* sub; bool* on; std::vector<Rgba> cols; Page go; };
    Row rows[] = {
        {"Nitro flame", "Boost flame colours and smoke", &c.normal.enabled, {c.normal.color, c.normal.color2, c.normal.color3, c.normal.smoke}, Page::Nitro},
        {"Kart paint", "Your kart's paint, 3 slots", &c.paint.enabled, {c.paint.layer[0][1], c.paint.layer[1][1], c.paint.layer[2][1]}, Page::Paint},
        {"Champion underglow", "Neon light under the Champion kart", &c.champion.enabled, {c.champion.lower_region}, Page::Glow},
    };
    for (auto& r : rows) {
        ImGui::PushID(r.name);
        BeginCard("row");
        ImGui::BeginGroup();
        Text(r.name, kText, 19, g_bold);
        Text(r.sub, kMuted, 14);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42 - 130 - r.cols.size() * 23);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10);
        Swatches(r.cols);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42 - 100);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 6);
        if (Button("Edit")) g_page = r.go;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
        if (Switch("on", r.on)) {
            g_sync.Touch();
        }
        EndCard();
        ImGui::PopID();
    }
}

void RainbowSpeed() {
    if (g_sync.cfg.fx.rainbow.empty()) return;
    if (Slider("RGB speed", &g_sync.cfg.fx.rainbow_speed, 0.02f, 2.0f, "%.2f cycles/s")) g_sync.Touch();
}

void NitroPage() {
    PageTitle("Nitro flame", "The boost flame of your kart. The game uses the same flame for the normal and the blue boost.");
    NitroNormal& n = g_sync.cfg.normal;
    bool ch = false;
    BeginCard("flame");
    ch |= CardHeader("Flame colours", "Three layers of the flame, from the core outwards", &n.enabled);
    ImGui::BeginDisabled(!n.enabled);
    ch |= ColorRow("Colour 1", n.color, "n1");
    ch |= ColorRow("Colour 2", n.color2, "n2");
    ch |= ColorRow("Colour 3", n.color3, "n3");
    ch |= Slider("Brightness", &n.brightness, 0.25f, 8.0f, "x%.2f");
    ImGui::EndDisabled();
    EndCard();
    BeginCard("smoke");
    CardHeader("Smoke", "Tint of the dark smoke behind the flame", nullptr);
    ImGui::BeginDisabled(!n.enabled);
    ch |= ColorRow("Smoke colour", n.smoke, "smoke");
    ImGui::EndDisabled();
    EndCard();
    BeginCard("nfx");
    RainbowSpeed();
    if (Button("Reset to the game's flame")) {
        n = NitroNormal{};
        for (const char* id : {"n1", "n2", "n3", "smoke"}) SetRainbow(id, false);
        ch = true;
    }
    EndCard();
    if (ch) g_sync.Touch();
}

void PaintPage() {
    PageTitle("Kart paint", "Recolour the paint job on your kart. Works in the garage and in races.");
    KartPaint& kp = g_sync.cfg.paint;
    bool ch = false;
    BeginCard("whole");
    bool en = kp.enabled;
    if (CardHeader("Whole kart", "One colour, or a colour layout built from it", &en)) {
        kp.enabled = en;   // colours never set here: the game starts from the kart's own paint
        ch = true;
    }
    ImGui::BeginDisabled(!kp.enabled);
    if (ColorRow("Colour", g_paint_all, "kart")) {
        for (auto& sl : kp.layer) for (auto& c : sl) c = g_paint_all;
        ch = true;
    }
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
    Text("Layout", kText, 16);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 250);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5);
    ImGui::SetNextItemWidth(130);
    ImGui::Combo("##lay", &g_pattern_all, kPatterns, kPatternCount);
    ImGui::SameLine();
    if (Button("Apply", true, ImVec2(112, 0))) {
        for (int s = 0; s < kPaintSlots; ++s)
            MakePattern(g_pattern_all, Mix(g_paint_all, FromHsv(ToHsv(g_paint_all).h + s * 0.06f, 1, 1), 0.15f * s), kp.layer[s]);
        ch = true;
    }
    ImGui::EndDisabled();
    EndCard();

    static const char* kSlot[kPaintSlots] = {"Slot 1  -  main body", "Slot 2", "Slot 3"};
    for (int s = 0; s < kPaintSlots; ++s) {
        ImGui::PushID(s);
        BeginCard("slot");
        CardHeader(kSlot[s], nullptr, nullptr);
        ImGui::BeginDisabled(!kp.enabled);
        const std::string sid = "s" + std::to_string(s + 1);
        Rgba whole = kp.layer[s][1];
        if (ColorRow("Slot colour", whole, sid.c_str())) { for (auto& c : kp.layer[s]) c = whole; ch = true; }
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);
        Text("Layout", kText, 16);
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 250);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 5);
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("##lay", &g_pattern[s], kPatterns, kPatternCount);
        ImGui::SameLine();
        if (Button("Apply", false, ImVec2(112, 0))) { MakePattern(g_pattern[s], whole, kp.layer[s]); ch = true; }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushStyleColor(ImGuiCol_Header, kPanel2);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kLine);
        if (ImGui::CollapsingHeader("All 7 layers")) {
            for (int l = 0; l < kPaintLayers; ++l) {
                char label[32];
                std::snprintf(label, sizeof label, l ? "Layer %d" : "Tint (sheen)", l);
                const std::string lid = sid + "l" + std::to_string(l);
                ch |= ColorRow(label, kp.layer[s][l], lid.c_str());
            }
        }
        ImGui::PopStyleColor(2);
        ImGui::EndDisabled();
        EndCard();
        ImGui::PopID();
    }
    BeginCard("pfx");
    RainbowSpeed();
    Wrapped("Tip: special paint jobs (Atomic, Gem, Spectral...) come from the game's Paint Job menu; pick one there, then recolour it here.");
    EndCard();
    if (ch) g_sync.Touch();
}

void GlowPage() {
    PageTitle("Champion underglow", "The neon light under the Champion kart.");
    Champion& c = g_sync.cfg.champion;
    bool ch = false;
    BeginCard("glow");
    ch |= CardHeader("Underglow colour", "Show the Champion kart in the garage or in a race", &c.enabled);
    ImGui::BeginDisabled(!c.enabled);
    ch |= ColorRow("Colour", c.lower_region, "glow");
    RainbowSpeed();
    ImGui::EndDisabled();
    if (Button("Reset underglow")) { c = Champion{}; SetRainbow("glow", false); ch = true; }
    EndCard();
    if (ch) g_sync.Touch();
}

std::wstring Widen(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), int(w.size()));
    if (!w.empty()) w.pop_back();
    return w;
}

fs::path FileDialog(bool save, const std::wstring& name) {
    wchar_t buf[MAX_PATH] = {};
    wcsncpy(buf, name.c_str(), MAX_PATH - 1);
    OPENFILENAMEW o{};
    o.lStructSize = sizeof o;
    o.hwndOwner = g_hwnd;
    o.lpstrFilter = L"ASTRO preset (*.json)\0*.json\0";
    o.lpstrFile = buf;
    o.nMaxFile = MAX_PATH;
    o.lpstrDefExt = L"json";
    o.Flags = save ? OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST : OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return (save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o)) ? fs::path(buf) : fs::path();
}

// ---------------- effects editor ----------------
// The layer inside the game lists every live effect colour (effects.json) and applies fx_edits.json.
struct FxState {
    std::vector<fx::Item> items;
    std::map<std::string, fx::Color4> edits;
    bool loaded = false, scanning = false, dirty = false;
    double scan_at = 0, changed_at = 0;
    std::string flash_id;
    double flash_until = 0;
    fx::Color4 flash_prev{};
    bool flash_had = false;
    HANDLE scan = nullptr, apply = nullptr, scanned = nullptr;
};
FxState g_fx;

std::string Lower(std::string s);
std::string ReadText(const fs::path& p);
bool WriteText(const fs::path& p, const std::string& s);
void FxInit() {
    g_fx.loaded = true;
    g_fx.scan = CreateEventW(nullptr, FALSE, FALSE, fx::kFxScanEvent);
    g_fx.apply = CreateEventW(nullptr, FALSE, FALSE, fx::kFxApplyEvent);
    g_fx.scanned = CreateEventW(nullptr, FALSE, FALSE, fx::kFxScannedEvent);
    g_fx.items = fx::LoadItems();
    g_fx.edits = fx::LoadEdits();
    // leftovers of an earlier version: edits of the plain white colour that hundreds of effects share
    for (auto it = g_fx.edits.begin(); it != g_fx.edits.end();)
        if (it->first.rfind("9|10000|10000|10000|10000@", 0) == 0) { it = g_fx.edits.erase(it); g_fx.dirty = true; }
        else ++it;
    // ITEMS page of 2026-11-01 matched names shared by many effects (menus, crate smashes): those edits are dropped, once
    // (fx_items_fix.done). Its first version also dropped the wheel editor's kart_premium / kart_training / kart_wheel
    // edits: those come back from the copy it kept (fx_edits.json.before_items_fix).
    {
        const fs::path dir = DefaultConfigPath().parent_path();
        const fs::path done = dir / "fx_items_fix.done";
        std::error_code ec;
        if (!fs::exists(done, ec)) {
            static const char* bad[] = {"ctr_startscreen", "ctr_gui_", "ctr_t504_gate", "ctr_collectible", "crate_basic_destroy", "ctr_relic_crate",
                                        "ctr_crate_destroy", "ctr_pwrup_tracking_missile_explosion"};
            for (auto it = g_fx.edits.begin(); it != g_fx.edits.end();) {
                const size_t at = it->first.find('@');
                const std::string own = at == std::string::npos ? "" : Lower(it->first.substr(at + 1));
                bool hit = false;
                for (const char* w : bad) if (own.rfind(w, 0) == 0) hit = true;
                if (own.rfind("ctr_pwrup_warp_orb", 0) == 0)   // only the white colour that many effects share was edited by mistake
                    for (auto& item : g_fx.items) if (item.id == it->first && item.orig[0] > 0.9f && item.orig[1] > 0.9f && item.orig[2] > 0.9f) hit = true;
                if (hit) { it = g_fx.edits.erase(it); g_fx.dirty = true; } else ++it;
            }
            if (auto root = json::Parse(ReadText(dir / "fx_edits.json.before_items_fix")))
                if (const json::Value* e = root->Find("edits"); e && e->type == json::Value::Type::Object && e->o)
                    for (auto& [id, v] : *e->o) {
                        const size_t at = id.find('@');
                        const std::string own = at == std::string::npos ? "" : Lower(id.substr(at + 1));
                        if (own.rfind("ctr_kart_premium", 0) && own.rfind("ctr_kart_training", 0)) continue;
                        if (v.type != json::Value::Type::Array || !v.a || v.a->size() < 4 || g_fx.edits.count(id)) continue;
                        fx::Color4 c{};
                        for (int i = 0; i < 4; ++i) c[i] = (*v.a)[i].type == json::Value::Type::Number ? float((*v.a)[i].n) : 0.f;
                        g_fx.edits[id] = c;
                        g_fx.dirty = true;
                    }
            WriteText(done, "1\n");
        }
    }
}
void FxTouch() { g_fx.dirty = true; g_fx.changed_at = ImGui::GetTime(); }
void FxTick() {
    if (!g_fx.loaded) return;
    if (g_fx.scanning && WaitForSingleObject(g_fx.scanned, 0) == WAIT_OBJECT_0) {
        g_fx.items = fx::LoadItems();
        g_fx.scanning = false;
        g_sync.Note(std::to_string(g_fx.items.size()) + " effect colours found");
    }
    if (g_fx.scanning && ImGui::GetTime() - g_fx.scan_at > 30) { g_fx.scanning = false; g_sync.Note("no answer from the game"); }
    if (!g_fx.flash_id.empty() && ImGui::GetTime() > g_fx.flash_until) {   // flash over: back to what it was
        if (g_fx.flash_had) g_fx.edits[g_fx.flash_id] = g_fx.flash_prev;
        else g_fx.edits.erase(g_fx.flash_id);
        g_fx.flash_id.clear();
        FxTouch();
    }
    if (g_fx.dirty && ImGui::GetTime() - g_fx.changed_at > 0.15) {
        g_fx.dirty = false;
        if (fx::SaveEdits(g_fx.edits) && g_fx.apply) SetEvent(g_fx.apply);
    }
}

// colour editor for one effect colour: hue picker on the colour divided by its peak, plus a brightness factor
bool FxColor(fx::Color4& c) {
    const float peak = std::max({c[0], c[1], c[2], 1e-4f});
    const float k = peak > 1 ? peak : 1;
    auto q = [k](float v) { return std::uint8_t(std::lround(std::clamp(v / k, 0.f, 1.f) * 255)); };
    Rgba shown{q(c[0]), q(c[1]), q(c[2]), 255};
    bool ch = false;
    if (ColorRow("Colour", shown)) {
        c[0] = shown.r / 255.f * k;
        c[1] = shown.g / 255.f * k;
        c[2] = shown.b / 255.f * k;
        ch = true;
    }
    float bright = k;
    if (Slider("Brightness", &bright, 0.1f, 25.f, "x%.2f")) {
        for (int i = 0; i < 3; ++i) c[i] = c[i] / k * bright;
        ch = true;
    }
    return ch;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
    return s;
}

// ITEMS: the models of TNT / Nitro crates, Aku Aku and Uka Uka (textures, items_catalog.json) and the effects of the power-ups
struct ItemModel { bool on = false; Rgba color{0, 255, 120, 255}; };
std::map<std::string, ItemModel> g_item_models;   // tnt, nitro, aku, uka
int g_item_strength = 100;
std::vector<wheel::Wheel> g_item_cat;
bool g_items_loaded = false;
// TNT / Nitro crates: their vertex colours (layer: mesh_colors.cpp, mesh_catalog.json); key -> "rrggbbaa"
bool WriteText(const fs::path& p, const std::string& s);
std::map<std::string, std::string> g_mesh_cols;
void MeshSave() {
    json::Value root = json::Value::MakeObject();
    for (auto& [k, v] : g_mesh_cols) root[k] = json::Value::String(v);
    WriteText(DefaultConfigPath().parent_path() / "mesh_edits.json", json::Serialize(root, 1) + "\n");
}
void ItemsSave() {
    std::map<std::string, RecolorParams> edits;
    for (auto& g : g_item_cat)
        for (auto& t : g.textures) {
            auto it = g_item_models.find(t.group);
            if (it == g_item_models.end() || !it->second.on) continue;
            RecolorParams p;
            p.target = p.grey_target = it->second.color;
            p.strength = g_item_strength / 100.f;
            p.colours = true;
            p.greys = true;
            edits[t.id] = p;
        }
    wheel::SaveEdits(edits, wheel::ItemsEditsPath());
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, wheel::kWheelApplyEvent)) { SetEvent(e); CloseHandle(e); }
}

json::Value RgbJson(const Rgba& c);
Rgba RgbFromJson(const json::Value* a, Rgba def);
bool g_item_rows_reload = false;   // Restore everything: the ITEMS rows are read again (all off)
void EffectsPage(bool items = false) {
    if (!g_fx.loaded) FxInit();
    if (items) {
        if (!g_items_loaded) {
            g_items_loaded = true;
            g_item_cat = wheel::LoadCatalog(ExeDir() / "items_catalog.json");
            for (const char* k : {"tnt", "nitro", "aku", "uka", "shield", "potion"}) g_item_models[k];
            g_item_models["tnt"].color = {60, 140, 255, 255};
            g_item_models["nitro"].color = {255, 60, 200, 255};
            g_item_models["aku"].color = {255, 60, 60, 255};
            g_item_models["uka"].color = {60, 255, 120, 255};
        }
        PageTitle("ITEMS", "The colours of the power-ups as they look in the race: crates, masks, potions, shields, electricity...");
        // the short list: one row per item, one colour for everything of it (its effects in the race and its model)
        struct Row { const char* key; const char* label; std::vector<const char*> names; int hue; const char* model; const char* mesh = nullptr; };   // hue: 0 any, 1 green, 2 red, 3 blue
        static const std::vector<Row> rows = {
            {"potion_green", "Green potion", {"beaker", "potion", "poison"}, 1, nullptr, "potion_green"},
            {"potion_red", "Red potion", {"beaker", "potion", "poison"}, 2, nullptr, "potion_red"},
            {"tnt", "TNT crate", {"explosive_crate"}, 0, "tnt", "tnt_body"},
            {"tnt_band", "TNT yellow stripes", {}, 0, nullptr, "tnt_band"},
            {"nitro", "Nitro crate", {"nitro_crate", "nitrocrate"}, 0, "nitro", "nitro_body"},
            {"nitro_band", "Nitro yellow stripes", {}, 0, nullptr, "nitro_band"},
            {"shield_green", "Green shield", {"pwrup_shield"}, 1, nullptr, "shield_green"},
            {"shield_blue", "Blue shield", {"pwrup_shield"}, 3, nullptr, "shield_blue"},
            {"electric", "Electricity", {"electrified", "electricity", "warp_orb"}, 0, nullptr},
            {"aku", "Aku Aku", {"aku_aku", "pwrup_aku"}, 0, "aku"},
            {"uka", "Uka Uka", {"uka_uka", "pwrup_uka"}, 0, "uka"},
            {"velo", "Velo", {"pwrup_velo"}, 0, nullptr},
        };
        auto hue_of = [](const fx::Color4& c) { return c[1] > c[0] * 1.2f && c[1] > c[2] * 1.2f ? 1 : c[0] > c[1] * 1.3f && c[0] > c[2] * 1.3f ? 2 : c[2] > c[0] * 1.2f && c[2] >= c[1] * 0.9f ? 3 : 0; };
        auto members = [&](const Row& r) {
            std::vector<fx::Item*> out;
            for (auto& it : g_fx.items) {
                bool name_ok = false;
                const size_t at = it.id.find('@');   // its own effect (the colour of the id), not the other effects that share it
                const std::string l = at == std::string::npos ? std::string() : Lower(it.id.substr(at + 1));
                for (const char* w : r.names) if (l.find(w) != std::string::npos) name_ok = true;
                const bool white = it.orig[0] > 0.9f && it.orig[1] > 0.9f && it.orig[2] > 0.9f;   // plain white is shared by many effects: left alone
                if (name_ok && !white && (r.hue == 0 || hue_of(it.orig) == r.hue)) out.push_back(&it);
            }
            return out;
        };
        // the switches and colours of the rows, kept in items.json (the edits they made stay in the game files
        // after a restart: without this the page showed them off and could not switch them off)
        static std::map<std::string, ItemModel> st_rows;
        static bool rows_loaded = false;
        const fs::path rows_file = DefaultConfigPath().parent_path() / "items.json";
        if (!rows_loaded || g_item_rows_reload) {
            rows_loaded = true;
            g_item_rows_reload = false;
            st_rows.clear();
            if (auto root = json::Parse(ReadText(rows_file)); root && root->type == json::Value::Type::Object && root->o)
                for (auto& [k, v] : *root->o) {
                    ItemModel& m = st_rows[k];
                    if (const json::Value* x = v.Find("on"); x && x->type == json::Value::Type::Bool) m.on = x->b;
                    m.color = RgbFromJson(v.Find("color"), m.color);
                }
        }
        static bool show_all = false;
        BeginCard("itemtop");
        ImGui::BeginDisabled(!g_game.Connected() || g_fx.scanning);
        if (Button(g_fx.scanning ? "Scanning..." : "Find the items", true, ImVec2(170, 0))) { SetEvent(g_fx.scan); g_fx.scanning = true; g_fx.scan_at = ImGui::GetTime(); }
        ImGui::EndDisabled();
        ImGui::SameLine();
        Wrapped(g_game.Connected() ? "Use each item once in a race (throw the potion, take a shield...), then press Find the items. Switch an item on and pick its colour."
                                   : "Start the game first: the items are read from the running game.");
        EndCard();
        bool ch = false;
        BeginCard("itemrows");
        Text("Items", kOrange, 22, g_bold);
        float st = float(g_item_strength);
        for (const Row& r : rows) {
            ItemModel& m = st_rows[r.key];
            if (m.color.r == 0 && m.color.g == 255 && m.color.b == 120 && !m.on) m.color = {255, 60, 200, 255};
            const auto mem = members(r);
            ImGui::PushID(r.key);
            ImGui::BeginGroup();
            Text(r.label, kText, 16, g_bold);
            const std::string sub = std::to_string(mem.size()) + (mem.size() == 1 ? " effect found" : " effects found") + (r.model ? " + its model" : "");
            Text(sub.c_str(), kMuted, 13);
            ImGui::EndGroup();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
            bool rc = false;
            if (Switch("##on", &m.on)) rc = true;
            if (m.on && ColorRow("Colour", m.color)) rc = true;
            if (rc) {
                ch = true;
                for (fx::Item* it : mem) {
                    if (!m.on) { g_fx.edits.erase(it->id); continue; }
                    const float pk = std::max({it->orig[0], it->orig[1], it->orig[2], 1.f});
                    g_fx.edits[it->id] = fx::Color4{m.color.r / 255.f * pk, m.color.g / 255.f * pk, m.color.b / 255.f * pk, it->orig[3]};
                }
                if (!mem.empty()) FxTouch();
                if (r.model) { ItemModel& im = g_item_models[r.model]; im.on = m.on; im.color = m.color; }
                if (r.mesh) {
                    char hex[16];
                    std::snprintf(hex, sizeof hex, "%02x%02x%02xff", m.color.r, m.color.g, m.color.b);
                    if (m.on) g_mesh_cols[r.mesh] = hex; else g_mesh_cols.erase(r.mesh);
                    MeshSave();
                }
            }
            // effects found after the switch was turned on (an item used later in the race) get the colour too
            if (m.on && !rc) {
                bool any = false;
                for (fx::Item* it : mem) {
                    if (g_fx.edits.count(it->id)) continue;
                    const float pk = std::max({it->orig[0], it->orig[1], it->orig[2], 1.f});
                    g_fx.edits[it->id] = fx::Color4{m.color.r / 255.f * pk, m.color.g / 255.f * pk, m.color.b / 255.f * pk, it->orig[3]};
                    any = true;
                }
                if (any) FxTouch();
            }
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 4));
        }
        if (Slider("Strength (models)", &st, 0.f, 100.f, "%.0f%%", 0)) { g_item_strength = int(st + 0.5f); ch = true; }
        if (g_item_cat.empty()) Text("items_catalog.json is missing next to ASTRO_CTR.exe (run run.bat).", kMuted, 14);
        EndCard();
        if (ch) {
            ItemsSave();
            json::Value root = json::Value::MakeObject();
            for (auto& [k, m] : st_rows) {
                json::Value o = json::Value::MakeObject();
                o["on"] = json::Value::Bool(m.on);
                o["color"] = RgbJson(m.color);
                root[k] = o;
            }
            WriteText(rows_file, json::Serialize(root, 1) + "\n");
        }
        BeginCard("itemall");
        if (Button(show_all ? "Hide the full effects list" : "Show every effect of the items (advanced)", false)) show_all = !show_all;
        EndCard();
        if (!show_all) return;
    } else
    PageTitle("Effects", "Every colour of the effects loaded in the game right now: drifts, boosts, power-ups, crates, menus...");
    static char filter_all[64] = "", filter_items[64] = "pwrup";
    char* filter = items ? filter_items : filter_all;
    static std::string open_id;
    BeginCard("fxtop");
    ImGui::BeginDisabled(!g_game.Connected() || g_fx.scanning);
    if (Button(g_fx.scanning ? "Scanning..." : "Scan effects", true, ImVec2(150, 0))) {
        SetEvent(g_fx.scan);
        g_fx.scanning = true;
        g_fx.scan_at = ImGui::GetTime();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 132);
    ImGui::InputTextWithHint("##flt", "Search an effect (drift, boost, wumpa, aku...)", filter, 64);
    ImGui::SameLine();
    ImGui::BeginDisabled(g_fx.edits.empty());
    if (Button("Reset all", false, ImVec2(120, 0))) { g_fx.edits.clear(); FxTouch(); }
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 4));
    // quick filters (the effect file names use these words)
    const std::pair<const char*, const char*> chips_all[] = {{"All", ""}, {"Wheels", "wheel"}, {"Drift", "drift"}, {"Boost", "boost"},
                                                             {"Power-ups", "pwrup"}, {"Crates", "crate"}, {"Kart", "ctr_kart"}};
    const std::pair<const char*, const char*> chips_items[] = {{"All items", "pwrup"}, {"Potions", "beaker"}, {"Shields", "shield"}, {"Aku", "aku"}, {"Uka", "uka"},
                                                               {"Velo", "velo"}, {"TNT / crates", "crate"}, {"Missile", "missile"}, {"Clock", "clock"},
                                                               {"Warp", "warp"}, {"Electric", "electri"}, {"Invisible", "invisib"}};
    const auto* chips = items ? chips_items : chips_all;
    const size_t nchips = items ? IM_ARRAYSIZE(chips_items) : IM_ARRAYSIZE(chips_all);
    for (size_t i = 0; i < nchips; ++i) {
        if (i) ImGui::SameLine(0, 6);
        const bool on = std::strcmp(filter, chips[i].second) == 0;
        if (Button(chips[i].first, on)) std::snprintf(filter, 64, "%s", chips[i].second);
    }
    ImGui::Dummy(ImVec2(0, 4));
    Wrapped(g_game.Connected() ? "Scan, find an effect, change its colour: it changes in the game at once. Flash shows where an "
                                 "effect is (it turns magenta for 2 seconds). Your changes stay for next time."
                               : "Start the game first: the effects are read from the running game.");
    EndCard();

    const std::string f = Lower(filter);
    int shown = 0;
    for (auto& it : g_fx.items) {
        if (!f.empty() && std::none_of(it.names.begin(), it.names.end(), [&](const std::string& n) { return Lower(n).find(f) != std::string::npos; }))
            continue;
        if (++shown > 200) break;   // keep the page fast: narrow the search for more
        const std::string title = it.names.empty() ? "Unnamed effect" : it.names.front();
        ImGui::PushID(it.id.c_str());
        auto e = g_fx.edits.find(it.id);
        const bool edited = e != g_fx.edits.end() && g_fx.flash_id != it.id;
        const fx::Color4 cur = e != g_fx.edits.end() ? e->second : it.orig;
        BeginCard("fx");
        const float pk = std::max({cur[0], cur[1], cur[2], 1.f});
        auto ch = [pk](float v) { return int(std::clamp(v / pk, 0.f, 1.f) * 255); };
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + 34, p.y + 34), IM_COL32(ch(cur[0]), ch(cur[1]), ch(cur[2]), 255), 8);
        ImGui::Dummy(ImVec2(34, 34));
        ImGui::SameLine(0, 14);
        ImGui::BeginGroup();
        Text(title.c_str(), edited ? kOrange : kText, 17, g_bold);
        char sub[96];
        std::snprintf(sub, sizeof sub, "%d %s%s", it.count, it.count == 1 ? "copy" : "copies",
                      it.names.size() > 1 ? "  -  shared with other effects (hover)" : "");
        Text(sub, kMuted, 13);
        if (it.names.size() > 1 && ImGui::IsItemHovered()) {
            std::string all;
            for (auto& n : it.names) all += n + "\n";
            ImGui::SetTooltip("%s", all.c_str());
        }
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 356);
        ImGui::BeginDisabled(!g_game.Connected() || !g_fx.flash_id.empty());
        if (Button("Flash", false, ImVec2(110, 0))) {
            g_fx.flash_id = it.id;
            g_fx.flash_had = e != g_fx.edits.end();
            if (g_fx.flash_had) g_fx.flash_prev = e->second;
            g_fx.edits[it.id] = fx::Color4{4, 0, 4, it.orig[3] > 0 ? it.orig[3] : 1};
            g_fx.flash_until = ImGui::GetTime() + 2;
            FxTouch();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button(open_id == it.id ? "Close" : "Edit", true, ImVec2(110, 0))) open_id = open_id == it.id ? "" : it.id;
        ImGui::SameLine();
        ImGui::BeginDisabled(!edited);
        if (Button("Reset", false, ImVec2(110, 0))) { g_fx.edits.erase(it.id); FxTouch(); }
        ImGui::EndDisabled();
        if (open_id == it.id) {
            ImGui::Dummy(ImVec2(0, 6));
            fx::Color4 c = cur;
            if (FxColor(c)) { g_fx.edits[it.id] = c; FxTouch(); }
        }
        EndCard();
        ImGui::PopID();
    }
    if (g_fx.items.empty()) { BeginCard("none"); Wrapped("No effects listed yet: start the game and press Scan effects."); EndCard(); }
    else if (shown > 200) { BeginCard("more"); Wrapped("Showing the first 200: type in the search box to find the rest."); EndCard(); }
}

// ---------------- wheel editor ----------------
// The layer finds the loaded wheel textures (wheels_found.json) and writes the recoloured ones (wheel_edits.json).
struct WheelUi {
    bool loaded = false, scanning = false, dirty = false, auto_scanned = false;
    double scan_at = 0, saved_at = 0;
    std::vector<wheel::Wheel> catalog;
    std::vector<std::string> found;
    std::map<std::string, RecolorParams> edits;
    std::string selected;
    HANDLE scan = nullptr, apply = nullptr, scanned = nullptr;
    struct Preview { std::vector<BYTE> orig, now; DynTex tex; std::string drawn; };
    std::map<std::string, Preview> previews;   // wheel name -> preview of its first colour texture
};
WheelUi g_wh;

void WheelInit() {
    g_wh.loaded = true;
    g_wh.catalog = wheel::LoadCatalog(ExeDir() / L"wheel_catalog.json");
    g_wh.scan = CreateEventW(nullptr, FALSE, FALSE, wheel::kWheelScanEvent);
    g_wh.apply = CreateEventW(nullptr, FALSE, FALSE, wheel::kWheelApplyEvent);
    g_wh.scanned = CreateEventW(nullptr, FALSE, FALSE, wheel::kWheelScannedEvent);
    g_wh.found = wheel::LoadFound();
    g_wh.edits = wheel::LoadEdits();
    // edits are keyed by texture id (name@fingerprint): drop leftovers of earlier versions of the page
    for (auto it = g_wh.edits.begin(); it != g_wh.edits.end();)
        if (it->first.find('@') == std::string::npos) { it = g_wh.edits.erase(it); g_wh.dirty = true; }
        else ++it;
}
void WheelScan() {
    SetEvent(g_wh.scan);
    g_wh.scanning = true;
    g_wh.scan_at = ImGui::GetTime();
    if (!g_fx.loaded) FxInit();   // the wheel's effects come from the effects list
    // effects: a full pass of their own, so only when none are listed yet or once a minute
    if (!g_fx.scanning && (g_fx.items.empty() || ImGui::GetTime() - g_fx.scan_at > 60)) {
        SetEvent(g_fx.scan);
        g_fx.scanning = true;
        g_fx.scan_at = ImGui::GetTime();
    }
}
void WheelTick() {
    if (!g_wh.loaded) return;
    if (g_wh.scanning && WaitForSingleObject(g_wh.scanned, 0) == WAIT_OBJECT_0) { g_wh.found = wheel::LoadFound(); g_wh.scanning = false; }
    if (g_wh.scanning && ImGui::GetTime() - g_wh.scan_at > 40) { g_wh.scanning = false; g_sync.Note("no answer from the game"); }
    // about 15 writes a second while a colour is being dragged: the wheel follows the picker
    if (g_wh.dirty && ImGui::GetTime() - g_wh.saved_at > 0.066) {
        g_wh.dirty = false;
        g_wh.saved_at = ImGui::GetTime();
        if (wheel::SaveEdits(g_wh.edits) && g_wh.apply) SetEvent(g_wh.apply);
    }
}

bool Found(const std::string& id) { return std::find(g_wh.found.begin(), g_wh.found.end(), id) != g_wh.found.end(); }

// the wheel's preview, recoloured like its first colour texture
ImTextureRef WheelPreview(const wheel::Wheel& w) {
    auto& pv = g_wh.previews[w.name];
    if (pv.orig.empty() && !pv.tex.ok()) {
        UINT pw, ph;
        pv.orig = DecodePngFile(ExeDir() / L"wheels" / (Widen(w.name) + L".png"), pw, ph);
        if (pv.orig.empty() || !pv.tex.Create(pw, ph)) return ImTextureRef();
    }
    const wheel::Texture* first = nullptr;
    for (auto& t : w.textures) if (!t.glow) { first = &t; break; }
    auto e = first ? g_wh.edits.find(first->id) : g_wh.edits.end();
    std::string key = "orig";
    if (e != g_wh.edits.end())
        key = FormatHexColor(e->second.target) + FormatHexColor(e->second.grey_target) + std::to_string(e->second.strength) + std::to_string(e->second.brightness) + (e->second.greys ? "g" : "") + (e->second.colours ? "c" : "");
    if (key != pv.drawn) {
        pv.now = pv.orig;
        if (e != g_wh.edits.end())
            for (size_t i = 0; i + 4 <= pv.now.size(); i += 4) RecolorRgb(e->second, &pv.orig[i], &pv.now[i]);
        pv.tex.Update(pv.now);
        pv.drawn = key;
    }
    return pv.tex.Id();
}

// ---------------- wheel parts ----------------
// A wheel is edited by named parts, like the kart paint: each part is one of its textures (recoloured live) or one
// of its effects. Wheels with a hand-made list (kMods) use their own names; the others get names from their files.
struct Part {
    std::string label, sub;
    bool effect = false;   // false: texture recolour, true: effect colour
    std::string target;    // texture name or effect file name
    bool greys = false;    // texture: tint grey/white parts too (glow masks)
};
std::vector<fx::Item*> EffectItems(const std::string& effect) {
    std::vector<fx::Item*> out;
    for (auto& it : g_fx.items) {
        const size_t at = it.id.find('@');
        if (at != std::string::npos && it.id.compare(at + 1, std::string::npos, effect) == 0) out.push_back(&it);   // its own name, not a colour shared with other effects
    }
    return out;
}
bool PartOn(const Part& p) {
    if (!p.effect) return g_wh.edits.count(p.target) > 0;
    for (auto* it : EffectItems(p.target)) if (g_fx.edits.count(it->id)) return true;
    return false;
}
Rgba PartColour(const Part& p) {
    if (!p.effect) { auto e = g_wh.edits.find(p.target); return e != g_wh.edits.end() ? e->second.target : Rgba{0, 220, 255, 255}; }
    for (auto* it : EffectItems(p.target)) {
        auto e = g_fx.edits.find(it->id);
        const fx::Color4 c = e != g_fx.edits.end() ? e->second : it->orig;
        const float pk = std::max({c[0], c[1], c[2], 1e-4f});
        auto q = [pk](float v) { return std::uint8_t(std::lround(std::clamp(v / pk, 0.f, 1.f) * 255)); };
        return Rgba{q(c[0]), q(c[1]), q(c[2]), 255};
    }
    return Rgba{0, 220, 255, 255};
}
// sets a part to a colour (on) or back to the game's own (off)
void SetPart(const Part& p, bool on, const Rgba& c) {
    if (!p.effect) {
        if (on) {
            RecolorParams& r = g_wh.edits[p.target];
            r.target = c;
            r.greys = p.greys;
        } else g_wh.edits.erase(p.target);
        g_wh.dirty = true;
        return;
    }
    for (auto* it : EffectItems(p.target)) {
        if (!on) { g_fx.edits.erase(it->id); continue; }
        // keep the effect's own brightness (values above 1 make it glow) and its 4th value
        const float pk = std::max({it->orig[0], it->orig[1], it->orig[2], 1.f});
        g_fx.edits[it->id] = fx::Color4{c.r / 255.f * pk, c.g / 255.f * pk, c.b / 255.f * pk, it->orig[3]};
    }
    FxTouch();
}

void PartCard(const Part& p) {
    ImGui::PushID(p.target.c_str());
    BeginCard("part");
    bool on = PartOn(p);
    std::string sub = p.sub;
    if (p.effect && EffectItems(p.target).empty())
        sub += g_fx.scanning ? "   -   looking..." : "   -   shows once it appears in the game (drive or drift)";
    if (CardHeader(p.label.c_str(), sub.c_str(), &on)) SetPart(p, on, PartColour(p));
    if (on) {
        Rgba c = PartColour(p);
        if (ColorRow("Colour", c)) SetPart(p, true, c);
        if (!p.effect) {
            RecolorParams& r = g_wh.edits[p.target];
            if (Slider("Brightness", &r.brightness, 0.2f, 3.f, "x%.2f", 0)) g_wh.dirty = true;
        }
    }
    EndCard();
    ImGui::PopID();
}

// ---------------- wheel editor page ----------------
// The wheels the editor covers (the ones with a display name in the catalogue). The page finds the wheel that is on
// the kart by itself and edits it like the kart paint: a colour, and the wheel follows while you drag. For each wheel:
//   Main colour  = the coloured pixels of its colour textures (the neon lines, the lava, the stripes...)
//   Silver parts = the grey pixels of the same textures (rims, metal)
//   Glow         = its glow textures (grey masks), tinted
// plus the colours of the effects of its pack. (seen in game 2026-10-31: swapping red and blue in
// KartLimited4_Wheels_C turned the cyan lines yellow)

std::string PrettyEffect(const std::string& f) {
    const std::string l = Lower(f);
    if (l.find("trail") != std::string::npos) return "Light trail";
    if (l.find("smoke") != std::string::npos) return "Drift smoke";
    if (l.find("training") != std::string::npos) return "Wheel lights";
    if (l.find("spark") != std::string::npos) return "Sparks";
    if (l.find("water") != std::string::npos) return "Water splash";
    if (l.find("mud") != std::string::npos || l.find("dirt") != std::string::npos) return "Dirt";
    return "Effect";
}

// wheels whose wheel textures are the same share one editor (their textures are the same pixels)
struct WheelGroup {
    std::string key;
    std::vector<const wheel::Wheel*> members;
    std::vector<const wheel::Texture*> colour, glow;   // the wheel's own textures
};
std::vector<WheelGroup> g_groups;
size_t g_groups_for = 0;

void BuildGroups() {
    if (g_groups_for == g_wh.catalog.size() && !g_groups.empty()) return;
    g_groups_for = g_wh.catalog.size();
    g_groups.clear();
    for (auto& w : g_wh.catalog) {
        if (w.display.empty()) continue;
        std::vector<const wheel::Texture*> col, glo;
        for (auto& t : w.textures) if (t.wheel) (t.glow ? glo : col).push_back(&t);
        if (col.empty()) continue;
        std::string key;
        for (auto* t : col) key += t->id + ";";
        for (auto* t : glo) key += t->id + ";";
        auto it = std::find_if(g_groups.begin(), g_groups.end(), [&](const WheelGroup& g) { return g.key == key; });
        if (it == g_groups.end()) { g_groups.push_back({key, {}, col, glo}); it = g_groups.end() - 1; }
        it->members.push_back(&w);
    }
}
// a wheel is on the kart when its colour textures are loaded (effect sheets such as Fx_Wheels_04 are not needed)
bool GroupLoaded(const WheelGroup& g) {
    if (!g_game.Connected()) return false;
    auto is_fx = [](const wheel::Texture* t) { return Lower(t->name).find("fx") != std::string::npos; };
    bool any = false;
    for (auto* t : g.colour) if (!is_fx(t)) { any = true; if (!Found(t->id)) return false; }
    if (any) return true;
    return std::all_of(g.colour.begin(), g.colour.end(), [](const wheel::Texture* t) { return Found(t->id); });
}
std::string GroupTitle(const WheelGroup& g) { return g.members[0]->display; }

struct WheelState {
    bool inited = false, main = false, silver = false, glow = false, tint = false;
    Rgba tint_c{255, 60, 60, 255};
    Rgba main_c{170, 60, 255, 255}, silver_c{255, 190, 30, 255}, glow_c{170, 60, 255, 255};
    float glow_strength = 1.0f;
};
std::map<std::string, WheelState> g_wstate;

void WheelStateLoad(const WheelGroup& g, WheelState& s) {
    s.inited = true;
    auto c = g_wh.edits.find(g.colour[0]->id);
    if (c != g_wh.edits.end()) {
        s.main = c->second.colours;
        s.silver = c->second.greys;
        s.main_c = c->second.target;
        s.silver_c = c->second.grey_target;
    }
    for (auto* m : g.members)
        for (auto& tn : m->tints) {
            auto e = g_wh.edits.find(tn.id);
            if (e != g_wh.edits.end()) { s.tint = true; s.tint_c = e->second.target; }
        }
    if (!g.glow.empty()) {
        auto e = g_wh.edits.find(g.glow[0]->id);
        if (e != g_wh.edits.end()) {
            s.glow = true;
            s.glow_c = e->second.target;
            s.glow_strength = e->second.brightness;
        }
    }
}

void WheelStateApply(const WheelGroup& g, const WheelState& s) {
    for (auto* t : g.colour) {
        if (s.main || s.silver) {
            RecolorParams p;
            p.colours = s.main;
            p.greys = s.silver;
            p.target = s.main_c;
            p.grey_target = s.silver_c;
            g_wh.edits[t->id] = p;
        } else g_wh.edits.erase(t->id);
    }
    for (auto* t : g.glow) {
        if (s.glow) {   // a glow mask is mostly grey: tint everything in it
            RecolorParams p;
            p.colours = p.greys = true;
            p.target = p.grey_target = s.glow_c;
            p.brightness = s.glow_strength;
            g_wh.edits[t->id] = p;
        } else g_wh.edits.erase(t->id);
    }
    // the wheel's effect tint (what its drift and boost effects are tinted with)
    for (auto* m : g.members)
        for (auto& tn : m->tints) {
            if (s.tint) {
                RecolorParams p;
                p.target = p.grey_target = s.tint_c;
                g_wh.edits[tn.id] = p;
            } else g_wh.edits.erase(tn.id);
        }
    g_wh.dirty = true;
}

void GroupEffects(const WheelGroup& g) {
    // the effects of the pack that are loaded in the game now
    std::vector<Part> parts;
    std::map<std::string, int> seen;
    for (auto& e : g.members[0]->effects) {
        if (EffectItems(e).empty()) continue;
        std::string label = PrettyEffect(e);
        if (++seen[label] > 1) label += " " + std::to_string(seen[label]);
        parts.push_back({label, "Comes out of the wheel", true, e, false});
    }
    ImGui::Dummy(ImVec2(0, 2));
    Text("What comes out of the wheel", kText, 20, g_bold);
    ImGui::Dummy(ImVec2(0, 4));
    if (parts.empty()) {
        BeginCard("nofx");
        Wrapped(g_fx.scanning ? "Looking for its effects..." : "Its effects (trails, sparks, lights) appear here once they show in the game: drive or drift.");
        EndCard();
    }
    for (auto& p : parts) PartCard(p);
}

void WheelEditorFor(const WheelGroup& g) {
    WheelState& s = g_wstate[g.key];
    if (!s.inited) WheelStateLoad(g, s);
    const wheel::Wheel& w = *g.members[0];
    bool ch = false;

    BeginCard("whead");
    if (ImTextureRef id = WheelPreview(w); id._TexData) { ImGui::Image(id, ImVec2(96, 96)); ImGui::SameLine(0, 18); }
    ImGui::BeginGroup();
    Text(GroupTitle(g).c_str(), kText, 28, g_bold);
    if (g.members.size() > 1) {
        std::string others;
        for (size_t i = 0; i < g.members.size() && i < 8; ++i) others += (i ? ", " : "") + g.members[i]->display;
        Text(("Same wheel textures as: " + others).c_str(), kMuted, 13);
    }
    ImGui::Dummy(ImVec2(0, 6));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushFont(g_font, 15);
    const char* st = "On your kart";
    const ImVec2 ts = ImGui::CalcTextSize(st);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + ts.x + 34, p.y + ts.y + 12), kOrangeDim, 20);
    dl->AddCircleFilled(ImVec2(p.x + 13, p.y + (ts.y + 12) / 2), 4, kOrange);
    dl->AddText(ImVec2(p.x + 24, p.y + 6), kOrange, st);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(ts.x + 34, ts.y + 12));
    ImGui::EndGroup();
    EndCard();

    BeginCard("main");
    if (CardHeader("Main colour", "The coloured parts of the wheel", &s.main)) {
        if (s.main && !g.glow.empty()) { s.glow = true; s.glow_c = s.main_c; }   // the glow follows at first
        ch = true;
    }
    if (s.main && ColorRow("Colour", s.main_c)) { if (s.glow) s.glow_c = s.main_c; ch = true; }
    EndCard();

    BeginCard("silver");
    ch |= CardHeader("Silver parts", "The grey and silver parts (rim, metal)", &s.silver);
    if (s.silver) ch |= ColorRow("Colour", s.silver_c);
    EndCard();

    if (!g.glow.empty()) {
        BeginCard("glow");
        ch |= CardHeader("Glow", "How the wheel shines", &s.glow);
        if (s.glow) {
            ch |= ColorRow("Colour", s.glow_c);
            ch |= Slider("Glow strength", &s.glow_strength, 0.2f, 3.f, "x%.2f", 0);
        }
        EndCard();
    }
    bool has_tint = false;
    for (auto* m : g.members) if (!m->tints.empty()) has_tint = true;
    if (has_tint) {
        BeginCard("tint");
        if (CardHeader("Effect colour", "The colour of what comes out of the wheel (drift, boost, speed)", &s.tint)) {
            if (s.tint) s.tint_c = g.members[0]->tints[0].colour;   // starts as the wheel's own colour
            ch = true;
        }
        if (s.tint) ch |= ColorRow("Colour", s.tint_c);
        EndCard();
    }
    if (ch) WheelStateApply(g, s);

    GroupEffects(g);

    BeginCard("wreset");
    if (Button("Back to the game's colours")) {
        s.main = s.silver = s.glow = false;
        WheelStateApply(g, s);
        for (auto& e : w.effects) for (auto* it : EffectItems(e)) g_fx.edits.erase(it->id);
        FxTouch();
    }
    EndCard();
}

void WheelPage() {
    if (!g_wh.loaded) WheelInit();
    if (!g_fx.loaded) FxInit();
    BuildGroups();
    const double now = ImGui::GetTime();
    // opening the page, and every 15 seconds after: look at the game (a quick look)
    static double last_seen = -10;
    const bool entered = now - last_seen > 0.5;
    last_seen = now;
    if (g_game.Connected() && !g_wh.scanning && (entered || now - g_wh.scan_at > 15)) WheelScan();

    PageTitle("Wheel Editor", "Your wheel shows up here by itself. Change its colours and what comes out of it, live.");
    if (g_wh.catalog.empty()) {
        BeginCard("nocat");
        Wrapped("wheel_catalog.json is missing next to ASTRO_CTR.exe.");
        EndCard();
        return;
    }
    static std::string chosen;
    // RE-CHECK: after changing the wheel in the game, press it and the editor looks at the kart again
    BeginCard("recheck");
    ImGui::BeginDisabled(!g_game.Connected() || g_wh.scanning);
    if (Button(g_wh.scanning ? "CHECKING..." : "RE-CHECK", true, ImVec2(180, 0))) {
        g_wh.found.clear();   // forget what was seen: only what the game shows now counts
        chosen.clear();
        WheelScan();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    Wrapped(g_game.Connected() ? "Changed your wheel in the game? Press RE-CHECK and the editor finds the new one."
                               : "Start the game first.");
    EndCard();
    std::vector<const WheelGroup*> loaded;
    for (auto& g : g_groups) if (GroupLoaded(g)) loaded.push_back(&g);

    if (loaded.empty()) {
        BeginCard("none");
        CardHeader(!g_game.Connected() ? "Game not running" : g_wh.scanning ? "Checking your kart..." : "No supported wheel on your kart", nullptr, nullptr);
        Wrapped(!g_game.Connected() ? "Start the game, then open this page."
                                    : "Put one of the wheels below on your kart (Karts > Wheels), stay on that screen, then press RE-CHECK.");
        EndCard();
        BeginCard("supported");
        CardHeader("Wheels this editor covers", nullptr, nullptr);
        for (auto& g : g_groups) {
            std::string line = g.members[0]->display;
            for (size_t i = 1; i < g.members.size(); ++i) line += ", " + g.members[i]->display;
            Wrapped(line.c_str(), kMuted);
        }
        EndCard();
        return;
    }
    if (std::none_of(loaded.begin(), loaded.end(), [](const WheelGroup* g) { return g->key == chosen; })) chosen = loaded[0]->key;
    if (loaded.size() > 1) {
        BeginCard("pick");
        CardHeader("Wheels on the karts in the game", nullptr, nullptr);
        for (size_t i = 0; i < loaded.size(); ++i) {
            if (i) ImGui::SameLine(0, 6);
            if (Button(GroupTitle(*loaded[i]).c_str(), loaded[i]->key == chosen)) chosen = loaded[i]->key;
        }
        EndCard();
    }
    for (auto* g : loaded) if (g->key == chosen) WheelEditorFor(*g);
}

// ---------------- MODS page ----------------
// The launcher's own patches (60 FPS, motion blur, resolution, online, unlock all) stay with the launcher; the other
// XML mods in its patches folder are switched here: live in the running game (layer: mod_engine) and in the
// launcher's patches.json so it applies them from the start next time. FULL OPTIMIZE edits the launcher's
// bbport.ini (its graphics settings), which it reads when the game starts.

const char* const kLauncherOwn[] = {"CrashTeamRacing.xml", "CrashTeamRacingEU.xml", "CrashTeamRacingOnline.xml", "CrashTeamRacingOnlineEU.xml",
                                    "CrashTeamRacingUnlockAll.xml", "CrashTeamRacingUnlockAllEU.xml", "OCTR-Mod-Library.xml"};

// the game folder (the one holding bin\sacredfire.exe), remembered for when the game is not running
fs::path GameDir() {
    static fs::path dir;
    static double checked = -100;
    if (ImGui::GetTime() - checked < 5 && !dir.empty()) return dir;
    checked = ImGui::GetTime();
    const fs::path memo = DefaultConfigPath().parent_path() / "game_dir.txt";
    if (DWORD pid = FindProcess(L"sacredfire.exe")) {
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t p[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, p, &n)) {
                dir = fs::path(p).parent_path().parent_path();
                if (FILE* f = _wfopen(memo.c_str(), L"wb")) { std::fputs(dir.u8string().c_str(), f); std::fclose(f); }
            }
            CloseHandle(h);
        }
    }
    if (dir.empty())
        if (FILE* f = _wfopen(memo.c_str(), L"rb")) {
            char b[1024] = {};
            std::fread(b, 1, sizeof b - 1, f);
            std::fclose(f);
            dir = fs::u8path(b);
        }
    return dir;
}

// The launcher writes each run to last_run_<name>.log and overwrites it at the next start, so a crash report is lost
// once the game is started again. A run that ended with an error is copied to crash_logs\ (named by the run's start).
void CrashLogsTick() {
    static double checked = -100;
    if (ImGui::GetTime() - checked < 5) return;
    checked = ImGui::GetTime();
    const fs::path game = GameDir();
    std::error_code ec;
    if (game.empty() || !fs::is_directory(game, ec)) return;
    const fs::path out_dir = DefaultConfigPath().parent_path() / "crash_logs";
    for (auto& e : fs::directory_iterator(game, ec)) {
        const std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"last_run", 0) != 0 || e.path().extension() != L".log") continue;
        std::FILE* f = _wfopen(e.path().c_str(), L"rb");
        if (!f) continue;
        std::string s;
        char b[4096];
        for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
        std::fclose(f);
        const size_t end = s.rfind("--- the game ended with code ");
        if (end == std::string::npos || std::atoi(s.c_str() + end + 29) == 0) continue;
        // "--- SacredFire launch 2026-10-31 22:56:45" -> 2026-10-31_22-56-45
        std::string stamp = "unknown";
        if (s.rfind("--- SacredFire launch ", 0) == 0 && s.size() > 41) {
            stamp = s.substr(22, 19);
            for (char& c : stamp) if (c == ' ') c = '_'; else if (c == ':') c = '-';
        }
        const fs::path dst = out_dir / (stamp + "_" + e.path().filename().string());
        if (fs::exists(dst, ec)) continue;
        fs::create_directories(out_dir, ec);
        fs::copy_file(e.path(), dst, ec);
    }
}

std::string ReadText(const fs::path& p) {
    std::string s;
    if (FILE* f = _wfopen(p.c_str(), L"rb")) {
        char b[4096];
        for (size_t n; (n = std::fread(b, 1, sizeof b, f)) > 0;) s.append(b, n);
        std::fclose(f);
    }
    return s;
}
bool WriteText(const fs::path& p, const std::string& s) {
    fs::path tmp = p;
    tmp += ".astro_tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    const bool ok = std::fwrite(s.data(), 1, s.size(), f) == s.size();
    std::fclose(f);
    if (!ok) return false;
    for (int i = 0; i < 40; ++i) {   // a reader (the layer in the game) may hold the file for a moment: try again, never drop the change
        if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(10);
    }
    return false;
}
// a copy of the file as it was before ASTRO first changed it (made once, never overwritten)
void BackupOnce(const fs::path& p) {
    fs::path b = p;
    b += ".astro_backup";
    std::error_code ec;
    if (!fs::exists(b, ec) && fs::exists(p, ec)) fs::copy_file(p, b, ec);
}

struct ModsUi {
    bool loaded = false;
    std::vector<mods::Mod> list;
    std::map<std::string, bool> on;
    std::map<std::string, mods::Status> status;
    std::string confirm;   // a gameplay mod waiting for the second click
    HANDLE apply = nullptr, status_ev = nullptr;
    std::string error;
};
ModsUi g_mods;

// patches.json: {"enabled": ["file/Name", ...], "disabled": [...]}
void LoadPatchesJson(std::set<std::string>& enabled) {
    auto root = json::Parse(ReadText(GameDir() / L"patches.json"));
    const json::Value* e = root ? root->Find("enabled") : nullptr;
    if (e && e->type == json::Value::Type::Array && e->a)
        for (auto& v : *e->a) if (v.type == json::Value::Type::String) enabled.insert(v.s);
}
bool SavePatchesJson() {
    const fs::path p = GameDir() / L"patches.json";
    auto root = json::Parse(ReadText(p));
    if (!root || root->type != json::Value::Type::Object) root = json::Value::MakeObject();
    std::vector<std::string> en, dis;
    // keep entries ASTRO does not manage (the launcher's own patches) where they are
    std::set<std::string> mine;
    for (auto& m : g_mods.list) mine.insert(m.id);
    for (const char* key : {"enabled", "disabled"})
        if (const json::Value* a = root->Find(key); a && a->type == json::Value::Type::Array && a->a)
            for (auto& v : *a->a)
                if (v.type == json::Value::Type::String && !mine.count(v.s)) (std::string(key) == "enabled" ? en : dis).push_back(v.s);
    for (auto& m : g_mods.list) (g_mods.on[m.id] ? en : dis).push_back(m.id);
    auto arr = [](const std::vector<std::string>& v) {
        json::Value a;
        a.type = json::Value::Type::Array;
        a.a = std::make_shared<json::Array>();
        for (auto& s : v) a.a->push_back(json::Value::String(s));
        return a;
    };
    (*root)["enabled"] = arr(en);
    (*root)["disabled"] = arr(dis);
    BackupOnce(p);
    return WriteText(p, json::Serialize(*root, 2) + "\n");
}

void ModsLoad() {
    g_mods.loaded = true;
    g_mods.list.clear();
    g_mods.error.clear();
    if (!g_mods.apply) {
        g_mods.apply = CreateEventW(nullptr, FALSE, FALSE, mods::kModsApplyEvent);
        g_mods.status_ev = CreateEventW(nullptr, FALSE, FALSE, mods::kModsStatusEvent);
    }
    const fs::path dir = GameDir() / L"patches";
    std::error_code ec;
    if (GameDir().empty() || !fs::exists(dir, ec)) { g_mods.error = "Start the game once so ASTRO knows where it is."; return; }
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".xml") continue;
        const std::string f = it->path().filename().string();
        if (std::any_of(std::begin(kLauncherOwn), std::end(kLauncherOwn), [&](const char* x) { return f == x; })) continue;
        for (auto& m : mods::ParseXml(it->path())) g_mods.list.push_back(std::move(m));
    }
    std::sort(g_mods.list.begin(), g_mods.list.end(), [](const mods::Mod& a, const mods::Mod& b) { return Lower(a.name) < Lower(b.name); });
    std::set<std::string> enabled;
    LoadPatchesJson(enabled);
    g_mods.on.clear();
    for (auto& m : g_mods.list) g_mods.on[m.id] = enabled.count(m.id) > 0;
    g_mods.status = mods::LoadStatus();
    // The game-side engine reads mods_live.json at every game start; the launcher's patches.json is the truth the
    // user also edits (in the launcher). Both must say the same, or the mods the user switched off come back.
    mods::SaveLive(g_mods.list, g_mods.on);
}

void ModsCommit() {
    if (!SavePatchesJson()) g_sync.Note("could not write patches.json");
    mods::SaveLive(g_mods.list, g_mods.on);
    if (g_mods.apply) SetEvent(g_mods.apply);
}
void ModsTick() {
    if (!g_mods.loaded && !GameDir().empty()) ModsLoad();   // once at start, so the game-side file follows patches.json
    if (g_mods.status_ev && WaitForSingleObject(g_mods.status_ev, 0) == WAIT_OBJECT_0) g_mods.status = mods::LoadStatus();
}

// ---------------- FULL OPTIMIZE ----------------
// bbport.ini lines are "key=value"; only the keys below are touched, everything else stays as it is.
struct OptKey { const char* key; const char* label; const char* sub; };
const OptKey kOptToggles[] = {
    {"effect_dynamic_shadows", "Dynamic shadows", "Every moving shadow (karts, characters, objects)"},
    {"effect_ssao", "Ambient occlusion (SSAO)", "The soft shade in corners and under objects"},
    {"effect_dof", "Depth of field", "The blur of far and near things"},
    {"effect_motion_blur", "Motion blur", "The blur when moving fast"},
    {"effect_chromatic_aberration", "Chromatic aberration", "The colour fringes at the screen edges"},
    {"effect_game_aa", "Anti-aliasing", "Smooths the edges (off = sharper and faster, more jagged)"},
};

std::map<std::string, std::string> ReadIni(const fs::path& p) {
    std::map<std::string, std::string> kv;
    std::istringstream in(ReadText(p));
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}
bool WriteIni(const fs::path& p, const std::map<std::string, std::string>& set) {
    std::string out, text = ReadText(p);
    std::set<std::string> done;
    std::istringstream in(text);
    for (std::string line; std::getline(in, line);) {
        const bool cr = !line.empty() && line.back() == '\r';
        if (cr) line.pop_back();
        const size_t eq = line.find('=');
        if (!line.empty() && line[0] != '#' && eq != std::string::npos) {
            auto it = set.find(line.substr(0, eq));
            if (it != set.end()) { line = it->first + "=" + it->second; done.insert(it->first); }
        }
        out += line + (cr ? "\r\n" : "\n");
    }
    for (auto& [k, v] : set) if (!done.count(k)) out += k + "=" + v + "\n";
    BackupOnce(p);
    return WriteText(p, out);
}

struct Perf { double fps = 0, avg = 0, worst = 0; double read_at = -10; size_t ram_mb = 0; };
Perf g_perf;
void PerfTick() {
    if (ImGui::GetTime() - g_perf.read_at < 1.0) return;
    g_perf.read_at = ImGui::GetTime();
    auto root = json::Parse(ReadText(DefaultConfigPath().parent_path() / "perf.json"));
    auto num = [&](const char* k) { const json::Value* v = root ? root->Find(k) : nullptr; return v && v->type == json::Value::Type::Number ? v->n : 0.0; };
    g_perf.fps = num("fps");
    g_perf.avg = num("avg_ms");
    g_perf.worst = num("worst_ms");
    g_perf.ram_mb = 0;
    if (DWORD pid = g_game.Connected() ? FindProcess(L"sacredfire.exe") : 0)
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            PROCESS_MEMORY_COUNTERS pmc{};
            if (K32GetProcessMemoryInfo(h, &pmc, sizeof pmc)) g_perf.ram_mb = pmc.WorkingSetSize >> 20;
            CloseHandle(h);
        }
}

void PerfCard() {
    BeginCard("perf");
    CardHeader("Performance now", g_game.Connected() ? "Measured inside the game, every second" : "Start the game to measure", nullptr);
    char b[160];
    std::snprintf(b, sizeof b, "%.0f FPS      %.1f ms per frame      worst %.1f ms      game memory %zu MB",
                  g_perf.fps, g_perf.avg, g_perf.worst, g_perf.ram_mb);
    Text(g_game.Connected() ? b : "-", g_game.Connected() ? kText : kMuted, 18, g_bold);
    EndCard();
}

void OptimizeCard() {
    const fs::path ini = GameDir() / L"bbport.ini";
    std::error_code ec;
    BeginCard("opt");
    Text("FULL OPTIMIZE", kOrange, 24, g_bold);
    Text("Lighter graphics for weaker PCs. These are the launcher's own settings: they apply when the game starts next.", kMuted, 14);
    ImGui::Dummy(ImVec2(0, 8));
    if (GameDir().empty() || !fs::exists(ini, ec)) { Wrapped("bbport.ini not found: start the game once."); EndCard(); return; }
    auto kv = ReadIni(ini);
    std::map<std::string, std::string> set;
    // the launcher writes what it really used to its newest last_run*.log; on the European 1.21 build it turns its
    // graphics settings off ("Graphics tab: unexpected game code, off") and always renders at 1080p (seen 2026-10-31)
    static double log_checked = -100;
    static bool ignored = false;
    static std::string internal_res;
    if (ImGui::GetTime() - log_checked > 10) {
        log_checked = ImGui::GetTime();
        fs::path newest;
        fs::file_time_type t{};
        for (fs::directory_iterator it(GameDir(), ec), end; !ec && it != end; it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (n.rfind("last_run", 0) == 0 && it->path().extension() == ".log" && (newest.empty() || it->last_write_time(ec) > t)) {
                newest = it->path();
                t = it->last_write_time(ec);
            }
        }
        const std::string head = ReadText(newest).substr(0, 20000);
        ignored = head.find("Graphics tab: unexpected game code, off") != std::string::npos;
        const size_t r = head.find("Internal resolution: ");
        internal_res = r == std::string::npos ? "" : head.substr(r + 21, head.find(char(10), r) - r - 21);
    }
    if (ignored) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kOrangeDim);
        ImGui::BeginChild("ignored", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        Text("The launcher ignores these settings on your game version", kOrange, 16, g_bold);
        Wrapped(("Its log says \"Graphics tab: unexpected game code, off\": on the European 1.21 build it does not apply the effect "
                 "switches, and the game renders at " + (internal_res.empty() ? std::string("1080p") : internal_res) +
                 " whatever the resolution says. Nothing below changes the game. ASTRO's own optimizations (inside the game) "
                 "are being built next.").c_str(), kText);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8));
    }

    Text("Presets", kText, 16, g_bold);
    ImGui::Dummy(ImVec2(0, 2));
    if (Button("Very weak PC", true)) {
        set = {{"render_res", "720"}, {"fps_limit", "60"}};
        for (auto& t : kOptToggles) set[t.key] = "0";
    }
    ImGui::SameLine(0, 6);
    if (Button("Balanced")) {
        set = {{"render_res", "900"}, {"effect_dynamic_shadows", "1"}, {"effect_ssao", "0"}, {"effect_dof", "0"},
               {"effect_motion_blur", "0"}, {"effect_chromatic_aberration", "0"}, {"effect_game_aa", "1"}};
    }
    ImGui::SameLine(0, 6);
    if (Button("Back to my settings")) {
        fs::path b = ini;
        b += ".astro_backup";
        if (fs::exists(b, ec)) {
            for (auto& [k, v] : ReadIni(b))
                if (k == "render_res" || k == "fps_limit" || std::any_of(std::begin(kOptToggles), std::end(kOptToggles), [&](const OptKey& t) { return k == t.key; }))
                    set[k] = v;
        } else g_sync.Note("no backup yet: nothing was changed");
    }
    ImGui::Dummy(ImVec2(0, 10));

    // render resolution: the biggest cost (the game draws at this height, then the image is scaled to the window)
    Text("Render resolution", kText, 16);
    const int res[] = {540, 720, 900, 1080, 1440};
    const int cur = std::atoi(kv["render_res"].c_str());
    for (size_t i = 0; i < IM_ARRAYSIZE(res); ++i) {
        if (i) ImGui::SameLine(0, 6);
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "%dp", res[i]);
        if (Button(lbl, cur == res[i])) set["render_res"] = std::to_string(res[i]);
    }
    Text(("Your screen: " + kv["output_res"] + ". Drawing above it costs a lot; 720p or 900p are much lighter.").c_str(), kMuted, 13);
    ImGui::Dummy(ImVec2(0, 8));

    Text("FPS limit", kText, 16);
    const int fps[] = {30, 45, 60};
    const int fcur = std::atoi(kv["fps_limit"].c_str());
    for (size_t i = 0; i < IM_ARRAYSIZE(fps); ++i) {
        if (i) ImGui::SameLine(0, 6);
        char lbl[16];
        std::snprintf(lbl, sizeof lbl, "%d", fps[i]);
        if (Button(lbl, fcur == fps[i])) set["fps_limit"] = std::to_string(fps[i]);
    }
    ImGui::Dummy(ImVec2(0, 8));

    for (auto& t : kOptToggles) {
        ImGui::PushID(t.key);
        bool v = kv[t.key] != "0";
        ImGui::BeginGroup();
        Text(t.label, kText, 16);
        Text(t.sub, kMuted, 13);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch("##t", &v)) set[t.key] = v ? "1" : "0";
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::PopID();
    }
    if (!set.empty()) {
        if (WriteIni(ini, set)) g_sync.Note(FindProcess(L"sacredfire.exe") ? "saved: restart the game to apply" : "saved: applies when the game starts");
        else g_sync.Note("could not write bbport.ini");
    }
    ImGui::Dummy(ImVec2(0, 6));
    Wrapped("Coming next, applied live inside the game: shadow maps off at the engine, lighter water, lower texture detail and fewer effect particles.");
    EndCard();
}

// ASTRO's own optimizations, inside the game (layer: render_hooks). They work on every game version and apply live:
// any render pass kind can be hidden (its draws are dropped). The shadows are the tall depth pass of the cascade
// shadow atlas (seen in a race 2026-10-31: "2048x8192 c1 depth", 4 cascades of 2048x2048, 26 draws).
struct PassRow { std::string kind; double per_frame = 0, draws = 0; };
struct InGameOpt {
    bool loaded = false, aniso_off = false;
    int texture_level = 0;   // mip levels skipped (0 = full detail)
    // engine render settings: -1 = the game's own, 0 = off, 1 = on
    int vfx = -1, bloom = -1, motion_blur = -1, depth_blur = -1;
    std::set<std::string> skip;
    std::set<std::string> hide_shaders, reduce_shaders;   // shader fingerprints (hex)
    int effects_keep = 100;                               // percent of instances kept by reduced shaders
    struct ShaderRow { std::string id, pass; double draws = 0, instances = 0, hidden = 0; };
    std::vector<ShaderRow> shaders;
    double stats_read = -10;
    std::vector<PassRow> passes;
};
InGameOpt g_igo;

// "WxH cN depth" -> its numbers
bool ParseKind(const std::string& k, unsigned& w, unsigned& h, unsigned& c, bool& depth) {
    depth = k.find(" depth") != std::string::npos;
    return std::sscanf(k.c_str(), "%ux%u c%u", &w, &h, &c) == 3;
}
// the cascade shadow atlas: a pass with depth, 4 square cascades stacked (height = 4 x width), at least 512 wide
bool ShadowKind(const std::string& k) {
    unsigned w, h, c;
    bool d;
    return ParseKind(k, w, h, c, d) && d && h == 4 * w && w >= 512 && c <= 1;
}

// shaders named after testing in the game (fingerprints of their shader code, the same in every session)
struct NamedShaders { const char* name; const char* sub; std::vector<const char*> ids; };
const std::vector<NamedShaders> kNamedShaders = {
    {"Flat grass", "Removes the grass blades (2 shaders, the heaviest of the game: about 280 draws a frame)",
     {"de61546a1ee27f1d", "0a4276d8df2335dd"}},
    {"No sky light", "Removes the light rays from the sky", {"a1d08377779438bd"}},
};

void InGameOptSave() {
    json::Value root = json::Value::MakeObject();
    json::Value a;
    a.type = json::Value::Type::Array;
    a.a = std::make_shared<json::Array>();
    for (auto& k : g_igo.skip) a.a->push_back(json::Value::String(k));
    root["skip"] = a;
    root["texture_level"] = json::Value::Number(g_igo.texture_level);
    root["aniso_off"] = json::Value::Bool(g_igo.aniso_off);
    // engine switches: written only when ASTRO changes them (absent = the game decides)
    if (g_igo.vfx >= 0) root["vfx"] = json::Value::Bool(g_igo.vfx == 1);
    if (g_igo.bloom >= 0) root["bloom"] = json::Value::Bool(g_igo.bloom == 1);
    if (g_igo.motion_blur >= 0) root["motion_blur"] = json::Value::Bool(g_igo.motion_blur == 1);
    if (g_igo.depth_blur >= 0) root["depth_blur"] = json::Value::Bool(g_igo.depth_blur == 1);
    json::Value hs;
    hs.type = json::Value::Type::Array;
    hs.a = std::make_shared<json::Array>();
    for (auto& k : g_igo.hide_shaders) hs.a->push_back(json::Value::String(k));
    root["hide_shaders"] = hs;
    json::Value rs;
    rs.type = json::Value::Type::Array;
    rs.a = std::make_shared<json::Array>();
    for (auto& k : g_igo.reduce_shaders) rs.a->push_back(json::Value::String(k));
    root["reduce_shaders"] = rs;   // for the app: which shaders the strength slider applies to
    root["effects_keep"] = json::Value::Number(g_igo.effects_keep);
    json::Value keep = json::Value::MakeObject();
    if (g_igo.effects_keep < 100)
        for (auto& k : g_igo.reduce_shaders) keep[k] = json::Value::Number(g_igo.effects_keep);
    root["keep_instances"] = keep;
    WriteText(DefaultConfigPath().parent_path() / "optimize.json", json::Serialize(root, 1) + "\n");
}

void InGameOptCard() {
    if (!g_igo.loaded) {
        g_igo.loaded = true;
        auto root = json::Parse(ReadText(DefaultConfigPath().parent_path() / "optimize.json"));
        if (const json::Value* a = root ? root->Find("skip") : nullptr; a && a->type == json::Value::Type::Array && a->a)
            for (auto& v : *a->a) if (v.type == json::Value::Type::String) g_igo.skip.insert(v.s);
        if (const json::Value* v = root ? root->Find("texture_level") : nullptr; v && v->type == json::Value::Type::Number) g_igo.texture_level = int(v->n);
        if (const json::Value* v = root ? root->Find("aniso_off") : nullptr; v && v->type == json::Value::Type::Bool) g_igo.aniso_off = v->b;
        for (const char* key : {"hide_shaders", "reduce_shaders"})
            if (const json::Value* a2 = root ? root->Find(key) : nullptr; a2 && a2->type == json::Value::Type::Array && a2->a)
                for (auto& v : *a2->a) if (v.type == json::Value::Type::String) (std::string(key) == "hide_shaders" ? g_igo.hide_shaders : g_igo.reduce_shaders).insert(v.s);
        auto tri = [&](const char* k) { const json::Value* v = root ? root->Find(k) : nullptr; return v && v->type == json::Value::Type::Bool ? int(v->b) : -1; };
        g_igo.vfx = tri("vfx");
        g_igo.bloom = tri("bloom");
        g_igo.motion_blur = tri("motion_blur");
        g_igo.depth_blur = tri("depth_blur");
        if (const json::Value* v = root ? root->Find("effects_keep") : nullptr; v && v->type == json::Value::Type::Number) g_igo.effects_keep = int(v->n);
    }
    if (ImGui::GetTime() - g_igo.stats_read > 2) {   // what the game draws (written by the layer every 5 s)
        g_igo.stats_read = ImGui::GetTime();
        g_igo.passes.clear();
        auto root = json::Parse(ReadText(DefaultConfigPath().parent_path() / "render_stats.json"));
        const json::Value* a = root ? root->Find("passes") : nullptr;
        if (a && a->type == json::Value::Type::Array && a->a)
            for (auto& p : *a->a) {
                auto num = [&](const char* k) { const json::Value* v = p.Find(k); return v && v->type == json::Value::Type::Number ? v->n : 0.0; };
                const json::Value* k = p.Find("kind");
                if (!k || k->type != json::Value::Type::String || k->s.find("(render pass)") != std::string::npos) continue;
                PassRow r;
                r.kind = k->s;
                const size_t sk = r.kind.find(" [skipped]");
                if (sk != std::string::npos) r.kind.erase(sk);
                r.per_frame = num("per_frame");
                r.draws = num("draws_per_frame") + num("skipped_per_frame");
                auto same = std::find_if(g_igo.passes.begin(), g_igo.passes.end(), [&](const PassRow& x) { return x.kind == r.kind; });
                if (same == g_igo.passes.end()) g_igo.passes.push_back(r);
                else { same->per_frame += r.per_frame; same->draws += r.draws; }
            }
        g_igo.shaders.clear();
        if (const json::Value* sh = root ? root->Find("shaders") : nullptr; sh && sh->type == json::Value::Type::Array && sh->a)
            for (auto& v : *sh->a) {
                auto num = [&](const char* k) { const json::Value* x = v.Find(k); return x && x->type == json::Value::Type::Number ? x->n : 0.0; };
                InGameOpt::ShaderRow r;
                if (const json::Value* x = v.Find("id"); x && x->type == json::Value::Type::String) r.id = x->s;
                if (const json::Value* x = v.Find("pass"); x && x->type == json::Value::Type::String) r.pass = x->s;
                r.draws = num("draws");
                r.instances = num("instances");
                r.hidden = num("hidden");
                if (!r.id.empty() && r.id != "0000000000000000") g_igo.shaders.push_back(r);
            }
        // the busiest passes first
        std::sort(g_igo.passes.begin(), g_igo.passes.end(), [](const PassRow& a, const PassRow& b) { return a.draws > b.draws; });
    }
    bool changed = false;
    BeginCard("ingame");
    Text("ASTRO in-game optimize", kOrange, 22, g_bold);
    Text("Done by ASTRO inside the game: works on your version and changes the game at once.", kMuted, 14);
    ImGui::Dummy(ImVec2(0, 8));

    // shadows: every shadow-atlas kind seen in the game (and the one measured in a race, before the list fills in)
    std::set<std::string> shadow_kinds = {"2048x8192 c1 depth", "2048x8192 c0 depth"};
    for (auto& p : g_igo.passes) if (ShadowKind(p.kind)) shadow_kinds.insert(p.kind);
    bool shadows = std::all_of(shadow_kinds.begin(), shadow_kinds.end(), [](const std::string& k) { return g_igo.skip.count(k) > 0; });
    ImGui::BeginGroup();
    Text("Remove shadows", kText, 16, g_bold);
    Text("The cascade shadow maps (the 2048x8192 pass) are not drawn: no shadows, less GPU work", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##shadows", &shadows)) {
        for (auto& k : shadow_kinds) { if (shadows) g_igo.skip.insert(k); else g_igo.skip.erase(k); }
        changed = true;
    }
    ImGui::Dummy(ImVec2(0, 10));

    // texture detail (the GPU reads textures at 1/2, 1/4, 1/8 size)
    Text("Texture detail", kText, 16, g_bold);
    Text("Lower = the GPU reads smaller versions of every texture. The emulator makes its texture settings once: restart the game after changing this.", kMuted, 13);
    ImGui::Dummy(ImVec2(0, 2));
    const char* levels[] = {"Full", "Half", "Quarter", "Eighth"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, 6);
        if (Button(levels[i], g_igo.texture_level == i)) { g_igo.texture_level = i; changed = true; }
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::BeginGroup();
    Text("No anisotropic filtering", kText, 15);
    Text("Textures seen at an angle (the road far ahead) get blurrier, the GPU works less", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##aniso", &g_igo.aniso_off)) changed = true;
    ImGui::Dummy(ImVec2(0, 10));

    // the engine's own render switches (the launcher ignores its graphics tab on this version; ASTRO sets them inside)
    struct EngineSw { const char* name; const char* sub; int* v; };
    const EngineSw sws[] = {
        {"No effects", "Every effect off: boosts, smoke, sparks, power-ups (yours too). The game neither computes nor draws them", &g_igo.vfx},
        {"No bloom", "The glow around bright light", &g_igo.bloom},
        {"No motion blur", "The blur when moving fast", &g_igo.motion_blur},
        {"No depth blur", "The blur of far and near things (depth of field)", &g_igo.depth_blur},
    };
    for (auto& sw : sws) {
        ImGui::PushID(sw.name);
        bool off = *sw.v == 0;
        ImGui::BeginGroup();
        Text(sw.name, kText, 16, g_bold);
        Text(sw.sub, kMuted, 13);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch("##sw", &off)) { *sw.v = off ? 0 : 1; changed = true; }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PopID();
    }

    // named heavy parts (found by switching shaders off in a race)
    for (auto& ns : kNamedShaders) {
        ImGui::PushID(ns.name);
        bool off = std::all_of(ns.ids.begin(), ns.ids.end(), [](const char* id) { return g_igo.hide_shaders.count(id) > 0; });
        ImGui::BeginGroup();
        Text(ns.name, kText, 16, g_bold);
        Text(ns.sub, kMuted, 13);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch("##ns", &off)) {
            for (const char* id : ns.ids) { if (off) g_igo.hide_shaders.insert(id); else g_igo.hide_shaders.erase(id); }
            changed = true;
        }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0, 6));

    // effects strength: reduced shaders keep this share of their draws (the game draws each effect with its own draw)
    Text("Effects strength", kText, 16, g_bold);
    Text("Applies to the shaders marked \"Reduce\" below: only this share of their effects is drawn. 100% = all of them.", kMuted, 13);
    float keep = float(g_igo.effects_keep);
    if (Slider("Keep", &keep, 0.f, 100.f, "%.0f%%", 0)) { g_igo.effects_keep = int(keep + 0.5f); changed = true; }
    ImGui::Dummy(ImVec2(0, 10));

    // shaders: grass, water and the effects each have their own; find them by switching them off
    Text("Shaders the game draws with (busiest first)", kText, 16, g_bold);
    Text("Switch one off and look at the game: it shows what disappears (grass, water, effects...). Tell me which is which and I name them.", kMuted, 13);
    ImGui::Dummy(ImVec2(0, 4));
    if (g_igo.shaders.empty()) Text("Start the game with the new ASTRO and drive: the list fills in after 5 seconds.", kMuted, 13);
    int shown_rows = 0;
    for (auto& r : g_igo.shaders) {
        if (r.draws + r.hidden < 1 && !g_igo.hide_shaders.count(r.id)) continue;
        if (++shown_rows > 40) break;
        ImGui::PushID(r.id.c_str());
        bool on = !g_igo.hide_shaders.count(r.id);
        char b[160];
        std::snprintf(b, sizeof b, "%.0f draws, %.0f instances per frame   -   in %s", r.draws + r.hidden, r.instances, r.pass.c_str());
        ImGui::BeginGroup();
        Text(("Shader " + r.id.substr(0, 6)).c_str(), on ? kText : kOrange, 15);
        Text(b, kMuted, 12);
        ImGui::EndGroup();
        // any shader can be reduced (a share of its draws) instead of hidden
        {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 42 - 130);
            const bool red = g_igo.reduce_shaders.count(r.id) > 0;
            if (Button(red ? "Reduced" : "Reduce", red, ImVec2(110, 0))) {
                if (red) g_igo.reduce_shaders.erase(r.id); else g_igo.reduce_shaders.insert(r.id);
                changed = true;
            }
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch("##sh", &on)) {
            if (on) g_igo.hide_shaders.erase(r.id); else g_igo.hide_shaders.insert(r.id);
            changed = true;
        }
        ImGui::PopID();
    }
    if (!g_igo.hide_shaders.empty() && Button("Show every shader again")) { g_igo.hide_shaders.clear(); changed = true; }
    ImGui::Dummy(ImVec2(0, 10));

    Text("Every pass the game draws (busiest first)", kText, 16, g_bold);
    Text("Switch one off to hide what it draws, live. If something important disappears, switch it back on.", kMuted, 13);
    ImGui::Dummy(ImVec2(0, 4));
    if (!g_game.Connected() || g_igo.passes.empty()) Text("Start the game and drive: the list fills in after 5 seconds.", kMuted, 13);
    for (auto& p : g_igo.passes) {
        if (p.draws < 2 && !g_igo.skip.count(p.kind)) continue;   // a clear or a single copy: nothing to gain
        ImGui::PushID(p.kind.c_str());
        bool shown = !g_igo.skip.count(p.kind);
        char b[128];
        std::snprintf(b, sizeof b, "%.0f passes, %.0f draws per frame%s", p.per_frame, p.draws, ShadowKind(p.kind) ? "   -   shadows" : "");
        ImGui::BeginGroup();
        Text(p.kind.c_str(), shown ? kText : kOrange, 15);
        Text(b, kMuted, 12);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch("##p", &shown)) {
            if (shown) g_igo.skip.erase(p.kind); else g_igo.skip.insert(p.kind);
            changed = true;
        }
        ImGui::PopID();
    }
    if (!g_igo.skip.empty()) {
        ImGui::Dummy(ImVec2(0, 6));
        if (Button("Show everything again")) { g_igo.skip.clear(); changed = true; }
    }
    EndCard();
    if (changed) InGameOptSave();
}

void ModCard(mods::Mod& m) {
    ImGui::PushID(m.id.c_str());
    BeginCard("mod");
    bool on = g_mods.on[m.id];
    ImGui::BeginGroup();
    Text(m.name.c_str(), kText, 18, g_bold);
    std::string sub = (m.author.empty() ? "" : "by " + m.author) + (m.version.empty() ? "" : "  -  v" + m.version);
    Text(sub.c_str(), kMuted, 13);
    if (!m.note.empty()) Wrapped(m.note.c_str(), kMuted);
    // state reported by the game
    auto st = g_mods.status.find(m.id);
    std::string state;
    ImU32 col = kMuted;
    if (!g_game.Connected()) state = on ? "On (from the next game start)" : "Off";
    else if (st == g_mods.status.end()) state = on ? "On" : "Off";
    else if (st->second.state == "on") { state = "On in the game now"; col = kOrange; }
    else if (st->second.state == "off") state = "Off";
    else if (st->second.state == "pending") state = "Turns off when the game restarts";
    else state = "Not applied: " + st->second.detail;
    Text(state.c_str(), col, 13);
    if (m.gameplay) Text("Changes how the game plays: use it offline only", kOrange, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##on", &on)) {
        if (on && m.gameplay && g_mods.confirm != m.id) {
            g_mods.confirm = m.id;   // ask once more
        } else {
            g_mods.on[m.id] = on;
            g_mods.confirm.clear();
            ModsCommit();
        }
    }
    if (g_mods.confirm == m.id) {
        ImGui::Dummy(ImVec2(0, 4));
        Wrapped("This mod changes the gameplay. In online races it gives you an unfair edge over real players and can get you "
                "banned from the server. Turn it on for offline play only?", kText);
        if (Button("Yes, offline only", true)) { g_mods.on[m.id] = true; g_mods.confirm.clear(); ModsCommit(); }
        ImGui::SameLine();
        if (Button("Cancel")) g_mods.confirm.clear();
    }
    EndCard();
    ImGui::PopID();
}

void ModsPage() {
#ifdef ASTRO_IN_GAME
    // the XML mods list stays in the ASTRO window; the game shows the rest of the page
    PageTitle("MODS", "Performance and optimize. The XML mods list is in the ASTRO window.");
    PerfCard();
    InGameOptCard();
    OptimizeCard();
    return;
#endif
    if (!g_mods.loaded) ModsLoad();
    PageTitle("MODS", "Your game mods in one place. Switching a mod applies it in the running game and keeps it for the next start.");
    ImGui::Dummy(ImVec2(0, 4));
    Text("Mods", kText, 20, g_bold);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 112);
    if (Button("Reload", false, ImVec2(112, 0))) ModsLoad();
    ImGui::Dummy(ImVec2(0, 4));
    if (!g_mods.error.empty()) { BeginCard("merr"); Wrapped(g_mods.error.c_str()); EndCard(); return; }
    for (auto& m : g_mods.list) ModCard(m);
    BeginCard("mfoot");
    Wrapped("The launcher keeps its own patches (60 FPS, motion blur, resolution, online, unlock all). These mods are the XML "
            "files in its patches folder.");
    if (Button("Open patches folder")) ShellExecuteW(nullptr, L"open", (GameDir() / L"patches").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    EndCard();
}

// ---- MAPS: the colours of the map being raced (the layer applies maps.json, see src/layer/map_colors.cpp) ----
struct MapsState {
    bool loaded = false, color_on = false, opaque = false;
    Rgba water{56, 10, 82, 255};   // dark purple
    double status_read = -10;
    int materials = -1, copies = 0;
    // sky: the map's skybox textures recoloured by the layer's texture engine (sky_catalog.json, sky_edits.json)
    bool sky_on = false, sky_clouds = true;   // skies are mostly pale (Crash Cove sunset): tinting only the strong colours left them unchanged
    Rgba sky{120, 40, 200, 255};
    int sky_strength = 80;                      // percent
    std::vector<wheel::Wheel> sky_cat;          // per map: its sky textures
    int sky_found = 0;                          // sky textures of the current map found in game memory
    std::string sky_map;                        // the map they belong to
    // snow and ice: their textures recoloured by the layer's texture engine (snow_catalog.json, snow_edits.json)
    bool snow_on = false, ice_on = false;
    Rgba snow{255, 150, 210, 255}, ice{0, 200, 255, 255};
    int snow_strength = 90;                     // percent
    std::vector<wheel::Wheel> snow_cat;         // "Snow" and "Ice": their textures
    int snow_found = 0, ice_found = 0;          // textures of each group found in game memory
    // Roo's Tubes (roo_catalog.json, roo_edits.json): the sand/mud ground on the sides, the water, the red lights
    bool wl_on = false;                       // the zone's whole light in one colour (layer: zone_light.cpp)
    Rgba wl{255, 0, 0, 255};
    bool roo_ground_on = false, roo_water_on = false, roo_light_on = false;
    Rgba roo_ground{60, 120, 255, 255}, roo_water{160, 40, 220, 255}, roo_light{40, 255, 120, 255};
    int roo_strength = 100;
    std::vector<wheel::Wheel> roo_cat;
    int roo_ground_found = 0, roo_water_found = 0, roo_light_found = 0;
    // lava, fire and volcanic rock (lava_catalog.json, lava_edits.json): each group its own colour
    bool lava_on = false, fire_on = false, rock_on = false;
    Rgba lava{0, 150, 255, 255}, fire{60, 255, 80, 255}, rock{90, 40, 160, 255};
    int lava_strength = 100;
    std::vector<wheel::Wheel> lava_cat;
    int lava_found = 0, fire_found = 0, rock_found = 0;
    // speed pads: their textures recoloured by the layer's texture engine (pad_catalog.json, pad_edits.json)
    bool pad_on = false;                        // the light (the glowing arrows)
    bool pad_body_on = false;                   // the body: the black field, the border and the printed arrows
    Rgba pad{0, 255, 120, 255}, pad_body{255, 60, 0, 255};
    int pad_strength = 100;
    std::vector<wheel::Wheel> pad_cat;
    int pad_found = 0;
    // lighting (the layer's zone_light.cpp writes them into the map's lighting object)
    bool fog_on = false, sun_on = false, amb_on = false, grade_on = false, tint_on = false;
    Rgba fog{150, 80, 220, 255}, sun{255, 170, 90, 255}, amb{255, 255, 255, 255}, tint{255, 255, 255, 255};
    float fog_density = 100, sun_power = 100;                                         // percent of the game's
    float exposure = 0, hue = 0, sat = 0, bright = 0, contrast = 0, vignette = 0;     // added to the game's
};
MapsState g_maps;

json::Value RgbJson(const Rgba& c) {
    json::Value a;
    a.type = json::Value::Type::Array;
    a.a = std::make_shared<json::Array>();
    for (int v : {c.r, c.g, c.b}) a.a->push_back(json::Value::Number(v / 255.0));
    return a;
}
Rgba RgbFromJson(const json::Value* a, Rgba def) {
    if (!a || a->type != json::Value::Type::Array || !a->a || a->a->size() < 3) return def;
    auto ch = [&](int i) { const json::Value& x = (*a->a)[i]; return std::uint8_t(std::clamp(x.type == json::Value::Type::Number ? x.n : 0.0, 0.0, 1.0) * 255 + 0.5); };
    return {ch(0), ch(1), ch(2), 255};
}

// writes the sky edit for every sky texture of every map (the one being raced is the one loaded), or none when off;
// the layer then recolours what is loaded (or puts the game's own colours back)
void SkySave() {
    std::map<std::string, RecolorParams> edits;
    if (g_maps.sky_on)
        for (auto& m : g_maps.sky_cat)
            for (auto& t : m.textures) {
                RecolorParams p;
                p.target = g_maps.sky;
                p.grey_target = g_maps.sky;
                p.strength = g_maps.sky_strength / 100.f;
                p.colours = true;
                p.greys = g_maps.sky_clouds;   // white clouds stay white unless asked
                edits[t.id] = p;
            }
    wheel::SaveEdits(edits, wheel::SkyEditsPath());
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, wheel::kWheelApplyEvent)) { SetEvent(e); CloseHandle(e); }
}

// snow and ice: every snow (or ice) texture of the catalogue gets the chosen colour; white parts are tinted too, since snow is white
void SnowSave() {
    std::map<std::string, RecolorParams> edits;
    for (auto& g : g_maps.snow_cat) {
        const bool snow = g.name == "Snow";
        if (!(snow ? g_maps.snow_on : g_maps.ice_on)) continue;
        for (auto& t : g.textures) {
            RecolorParams p;
            p.target = p.grey_target = snow ? g_maps.snow : g_maps.ice;
            p.strength = g_maps.snow_strength / 100.f;
            p.colours = true;
            p.greys = true;
            edits[t.id] = p;
        }
    }
    wheel::SaveEdits(edits, wheel::SnowEditsPath());
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, wheel::kWheelApplyEvent)) { SetEvent(e); CloseHandle(e); }
}

// speed pads: every pad texture gets the chosen colour (glow and arrows too)
void PadSave() {
    std::map<std::string, RecolorParams> edits;
    for (auto& g : g_maps.pad_cat)
        for (auto& t : g.textures) {
            const bool light = t.group == "light";
            if (!(light ? g_maps.pad_on : g_maps.pad_body_on)) continue;
            RecolorParams p;
            p.target = p.grey_target = light ? g_maps.pad : g_maps.pad_body;
            p.strength = g_maps.pad_strength / 100.f;
            p.colours = true;
            p.greys = true;
            p.blacks = !light;                   // the body is black: its dark parts take the colour too
            p.black_target = g_maps.pad_body;
            edits[t.id] = p;
        }
    wheel::SaveEdits(edits, wheel::PadEditsPath());
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, wheel::kWheelApplyEvent)) { SetEvent(e); CloseHandle(e); }
}

void RooSave() {
    std::map<std::string, RecolorParams> edits;
    for (auto& g : g_maps.roo_cat)
        for (auto& t : g.textures) {
            const bool gr = t.group == "ground", wa = t.group == "water";
            if (!(gr ? g_maps.roo_ground_on : wa ? g_maps.roo_water_on : g_maps.roo_light_on)) continue;
            RecolorParams p;
            p.target = p.grey_target = gr ? g_maps.roo_ground : wa ? g_maps.roo_water : g_maps.roo_light;
            p.strength = g_maps.roo_strength / 100.f;
            p.colours = true;
            p.greys = true;
            edits[t.id] = p;
        }
    wheel::SaveEdits(edits, wheel::RooEditsPath());
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, wheel::kWheelApplyEvent)) { SetEvent(e); CloseHandle(e); }
}

void LavaSave() {
    std::map<std::string, RecolorParams> edits;
    for (auto& g : g_maps.lava_cat)
        for (auto& t : g.textures) {
            const bool on = t.group == "lava" ? g_maps.lava_on : t.group == "fire" ? g_maps.fire_on : g_maps.rock_on;
            if (!on) continue;
            RecolorParams p;
            p.target = p.grey_target = t.group == "lava" ? g_maps.lava : t.group == "fire" ? g_maps.fire : g_maps.rock;
            p.strength = g_maps.lava_strength / 100.f;
            p.colours = true;
            p.greys = true;
            edits[t.id] = p;
        }
    wheel::SaveEdits(edits, wheel::LavaEditsPath());
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, wheel::kWheelApplyEvent)) { SetEvent(e); CloseHandle(e); }
}

void MapsSave() {
    json::Value root = json::Value::MakeObject();
    root["water_color_on"] = json::Value::Bool(g_maps.color_on);
    root["water_opaque"] = json::Value::Bool(g_maps.opaque);
    json::Value c;
    c.type = json::Value::Type::Array;
    c.a = std::make_shared<json::Array>();
    for (int v : {g_maps.water.r, g_maps.water.g, g_maps.water.b}) c.a->push_back(json::Value::Number(v / 255.0));
    root["water_color"] = c;
    root["sky_color_on"] = json::Value::Bool(g_maps.sky_on);
    root["sky_clouds"] = json::Value::Bool(g_maps.sky_clouds);
    root["sky_color"] = json::Value::String(FormatHexColor(g_maps.sky));
    root["sky_strength"] = json::Value::Number(g_maps.sky_strength);
    root["roo_ground_on"] = json::Value::Bool(g_maps.roo_ground_on);
    root["roo_water_on"] = json::Value::Bool(g_maps.roo_water_on);
    root["roo_light_on"] = json::Value::Bool(g_maps.roo_light_on);
    root["roo_ground_color"] = json::Value::String(FormatHexColor(g_maps.roo_ground));
    root["roo_water_color"] = json::Value::String(FormatHexColor(g_maps.roo_water));
    root["roo_light_color"] = json::Value::String(FormatHexColor(g_maps.roo_light));
    root["roo_strength"] = json::Value::Number(g_maps.roo_strength);
    root["lava_on"] = json::Value::Bool(g_maps.lava_on);
    root["fire_on"] = json::Value::Bool(g_maps.fire_on);
    root["rock_on"] = json::Value::Bool(g_maps.rock_on);
    root["lava_color"] = json::Value::String(FormatHexColor(g_maps.lava));
    root["fire_color"] = json::Value::String(FormatHexColor(g_maps.fire));
    root["rock_color"] = json::Value::String(FormatHexColor(g_maps.rock));
    root["lava_strength"] = json::Value::Number(g_maps.lava_strength);
    root["pad_on"] = json::Value::Bool(g_maps.pad_on);
    root["pad_body_on"] = json::Value::Bool(g_maps.pad_body_on);
    root["pad_body_color"] = json::Value::String(FormatHexColor(g_maps.pad_body));
    root["pad_color"] = json::Value::String(FormatHexColor(g_maps.pad));
    root["pad_strength"] = json::Value::Number(g_maps.pad_strength);
    root["snow_on"] = json::Value::Bool(g_maps.snow_on);
    root["ice_on"] = json::Value::Bool(g_maps.ice_on);
    root["snow_color"] = json::Value::String(FormatHexColor(g_maps.snow));
    root["ice_color"] = json::Value::String(FormatHexColor(g_maps.ice));
    root["snow_strength"] = json::Value::Number(g_maps.snow_strength);
    root["wl_on"] = json::Value::Bool(g_maps.wl_on);
    root["wl_color"] = RgbJson(g_maps.wl);
    root["fog_on"] = json::Value::Bool(g_maps.fog_on);
    root["fog_color"] = RgbJson(g_maps.fog);
    root["fog_density"] = json::Value::Number(g_maps.fog_density);
    root["sun_on"] = json::Value::Bool(g_maps.sun_on);
    root["sun_color"] = RgbJson(g_maps.sun);
    root["sun_power"] = json::Value::Number(g_maps.sun_power);
    root["amb_on"] = json::Value::Bool(g_maps.amb_on);
    root["amb_color"] = RgbJson(g_maps.amb);
    root["grade_on"] = json::Value::Bool(g_maps.grade_on);
    root["exposure"] = json::Value::Number(g_maps.exposure);
    root["hue"] = json::Value::Number(g_maps.hue);
    root["saturation"] = json::Value::Number(g_maps.sat);
    root["brightness"] = json::Value::Number(g_maps.bright);
    root["contrast"] = json::Value::Number(g_maps.contrast);
    root["vignette"] = json::Value::Number(g_maps.vignette);
    root["tint_on"] = json::Value::Bool(g_maps.tint_on);
    root["tint_color"] = RgbJson(g_maps.tint);
    WriteText(DefaultConfigPath().parent_path() / "maps.json", json::Serialize(root, 1) + "\n");
}

void ResetAllButton();   // Settings page (below)

// my character: the player's driver (read from the launcher's log) and a colour for each of its textures
struct CharPart { bool on = false; Rgba color{255, 60, 60, 255}; };
struct CharState {
    bool loaded = false;
    std::map<std::string, CharPart> parts;        // texture name -> choice (remembered for every character)
    int strength = 100, bright = 100;             // percent
    std::vector<wheel::Wheel> cat;
    std::string hero;                             // "pentapenguin" ...
    double hero_checked = -100;
    int found = 0;
} g_char;

void CharSave() {
    json::Value root = json::Value::MakeObject(), parts = json::Value::MakeObject();
    for (auto& [n, p] : g_char.parts) {
        json::Value o = json::Value::MakeObject();
        o["on"] = json::Value::Bool(p.on);
        o["color"] = RgbJson(p.color);
        parts[n] = o;
    }
    root["parts"] = parts;
    root["strength"] = json::Value::Number(g_char.strength);
    root["bright"] = json::Value::Number(g_char.bright);
    WriteText(DefaultConfigPath().parent_path() / "character.json", json::Serialize(root, 1) + "\n");
    std::map<std::string, RecolorParams> edits;
    for (auto& e : g_char.cat) {
        if (g_char.hero.empty() || e.name.rfind(g_char.hero, 0) != 0) continue;
        for (auto& t : e.textures) {
            auto it = g_char.parts.find(t.name);
            if (it == g_char.parts.end() || !it->second.on) continue;
            RecolorParams p;
            p.target = p.grey_target = it->second.color;
            p.strength = g_char.strength / 100.f;
            p.brightness = g_char.bright / 100.f;
            p.colours = true;
            p.greys = true;
            edits[t.id] = p;
        }
    }
    wheel::SaveEdits(edits, wheel::DriverEditsPath());
    if (HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, wheel::kWheelApplyEvent)) { SetEvent(e); CloseHandle(e); }
}

// the driver being played: the last "changeHeroAndOutfit [0] [DriverXxx]" of the newest launcher log
std::string CurrentHero() {
    std::error_code ec;
    fs::path newest;
    fs::file_time_type t{};
    for (fs::directory_iterator it(GameDir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("last_run", 0) == 0 && it->path().extension() == ".log" && (newest.empty() || it->last_write_time(ec) > t)) {
            newest = it->path();
            t = it->last_write_time(ec);
        }
    }
    if (newest.empty()) return {};
    std::string s = ReadText(newest);
    const std::string key = "changeHeroAndOutfit [0] [";
    const size_t at = s.rfind(key);
    if (at == std::string::npos) return {};
    std::string h = s.substr(at + key.size(), s.find(']', at + key.size()) - at - key.size());
    if (h.rfind("Driver", 0) == 0) h = h.substr(6);
    for (char& c : h) c = char(std::tolower((unsigned char)c));
    return h;
}

void MapsPage() {
    if (!g_char.loaded) {
        g_char.loaded = true;
        g_char.cat = wheel::LoadCatalog(ExeDir() / "driver_catalog.json");
        auto root = json::Parse(ReadText(DefaultConfigPath().parent_path() / "character.json"));
        if (root) {
            if (const json::Value* v = root->Find("strength"); v && v->type == json::Value::Type::Number) g_char.strength = int(v->n);
            if (const json::Value* v = root->Find("bright"); v && v->type == json::Value::Type::Number) g_char.bright = int(v->n);
            if (const json::Value* p = root->Find("parts"); p && p->type == json::Value::Type::Object && p->o)
                for (auto& [n, o] : *p->o) {
                    CharPart cp;
                    if (const json::Value* x = o.Find("on"); x && x->type == json::Value::Type::Bool) cp.on = x->b;
                    cp.color = RgbFromJson(o.Find("color"), cp.color);
                    g_char.parts[n] = cp;
                }
        }
    }
    if (!g_maps.loaded) {
        g_maps.loaded = true;
        auto root = json::Parse(ReadText(DefaultConfigPath().parent_path() / "maps.json"));
        if (const json::Value* v = root ? root->Find("water_color_on") : nullptr; v && v->type == json::Value::Type::Bool) g_maps.color_on = v->b;
        if (const json::Value* v = root ? root->Find("water_opaque") : nullptr; v && v->type == json::Value::Type::Bool) g_maps.opaque = v->b;
        if (const json::Value* a = root ? root->Find("water_color") : nullptr; a && a->type == json::Value::Type::Array && a->a && a->a->size() >= 3) {
            auto ch = [&](int i) { const json::Value& x = (*a->a)[i]; return std::uint8_t(std::clamp(x.type == json::Value::Type::Number ? x.n : 0.0, 0.0, 1.0) * 255 + 0.5); };
            g_maps.water = {ch(0), ch(1), ch(2), 255};
        }
        if (const json::Value* v = root ? root->Find("sky_color_on") : nullptr; v && v->type == json::Value::Type::Bool) g_maps.sky_on = v->b;
        if (const json::Value* v = root ? root->Find("sky_clouds") : nullptr; v && v->type == json::Value::Type::Bool) g_maps.sky_clouds = v->b;
        if (const json::Value* v = root ? root->Find("sky_color") : nullptr; v && v->type == json::Value::Type::String)
            if (auto c = ParseHexColor(v->s)) g_maps.sky = *c;
        if (const json::Value* v = root ? root->Find("sky_strength") : nullptr; v && v->type == json::Value::Type::Number) g_maps.sky_strength = int(v->n);
        g_maps.sky_cat = wheel::LoadCatalog(ExeDir() / "sky_catalog.json");
        g_maps.snow_cat = wheel::LoadCatalog(ExeDir() / "snow_catalog.json");
        g_maps.pad_cat = wheel::LoadCatalog(ExeDir() / "pad_catalog.json");
        g_maps.lava_cat = wheel::LoadCatalog(ExeDir() / "lava_catalog.json");
        g_maps.roo_cat = wheel::LoadCatalog(ExeDir() / "roo_catalog.json");
        for (auto [k, v] : {std::pair<const char*, bool*>{"roo_ground_on", &g_maps.roo_ground_on}, {"roo_water_on", &g_maps.roo_water_on}, {"roo_light_on", &g_maps.roo_light_on}})
            if (const json::Value* x = root ? root->Find(k) : nullptr; x && x->type == json::Value::Type::Bool) *v = x->b;
        for (auto [k, v] : {std::pair<const char*, Rgba*>{"roo_ground_color", &g_maps.roo_ground}, {"roo_water_color", &g_maps.roo_water}, {"roo_light_color", &g_maps.roo_light}})
            if (const json::Value* x = root ? root->Find(k) : nullptr; x && x->type == json::Value::Type::String)
                if (auto c = ParseHexColor(x->s)) *v = *c;
        if (const json::Value* v = root ? root->Find("roo_strength") : nullptr; v && v->type == json::Value::Type::Number) g_maps.roo_strength = int(v->n);
        for (auto [k, v] : {std::pair<const char*, bool*>{"lava_on", &g_maps.lava_on}, {"fire_on", &g_maps.fire_on}, {"rock_on", &g_maps.rock_on}})
            if (const json::Value* x = root ? root->Find(k) : nullptr; x && x->type == json::Value::Type::Bool) *v = x->b;
        for (auto [k, v] : {std::pair<const char*, Rgba*>{"lava_color", &g_maps.lava}, {"fire_color", &g_maps.fire}, {"rock_color", &g_maps.rock}})
            if (const json::Value* x = root ? root->Find(k) : nullptr; x && x->type == json::Value::Type::String)
                if (auto c = ParseHexColor(x->s)) *v = *c;
        if (const json::Value* v = root ? root->Find("lava_strength") : nullptr; v && v->type == json::Value::Type::Number) g_maps.lava_strength = int(v->n);
        if (const json::Value* v = root ? root->Find("pad_on") : nullptr; v && v->type == json::Value::Type::Bool) g_maps.pad_on = v->b;
        if (const json::Value* v = root ? root->Find("pad_body_on") : nullptr; v && v->type == json::Value::Type::Bool) g_maps.pad_body_on = v->b;
        if (const json::Value* v = root ? root->Find("pad_body_color") : nullptr; v && v->type == json::Value::Type::String)
            if (auto c = ParseHexColor(v->s)) g_maps.pad_body = *c;
        if (const json::Value* v = root ? root->Find("pad_color") : nullptr; v && v->type == json::Value::Type::String)
            if (auto c = ParseHexColor(v->s)) g_maps.pad = *c;
        if (const json::Value* v = root ? root->Find("pad_strength") : nullptr; v && v->type == json::Value::Type::Number) g_maps.pad_strength = int(v->n);
        if (const json::Value* v = root ? root->Find("snow_color") : nullptr; v && v->type == json::Value::Type::String)
            if (auto c = ParseHexColor(v->s)) g_maps.snow = *c;
        if (const json::Value* v = root ? root->Find("ice_color") : nullptr; v && v->type == json::Value::Type::String)
            if (auto c = ParseHexColor(v->s)) g_maps.ice = *c;
        if (const json::Value* v = root ? root->Find("snow_strength") : nullptr; v && v->type == json::Value::Type::Number) g_maps.snow_strength = int(v->n);
        auto flag = [&](const char* k, bool& v) { if (const json::Value* x = root ? root->Find(k) : nullptr; x && x->type == json::Value::Type::Bool) v = x->b; };
        auto num = [&](const char* k, float& v) { if (const json::Value* x = root ? root->Find(k) : nullptr; x && x->type == json::Value::Type::Number) v = float(x->n); };
        flag("snow_on", g_maps.snow_on); flag("ice_on", g_maps.ice_on); flag("fog_on", g_maps.fog_on); flag("sun_on", g_maps.sun_on); flag("amb_on", g_maps.amb_on); flag("grade_on", g_maps.grade_on); flag("tint_on", g_maps.tint_on);
        g_maps.fog = RgbFromJson(root ? root->Find("fog_color") : nullptr, g_maps.fog);
        g_maps.wl = RgbFromJson(root ? root->Find("wl_color") : nullptr, g_maps.wl);
        if (const json::Value* x = root ? root->Find("wl_on") : nullptr; x && x->type == json::Value::Type::Bool) g_maps.wl_on = x->b;
        g_maps.sun = RgbFromJson(root ? root->Find("sun_color") : nullptr, g_maps.sun);
        g_maps.amb = RgbFromJson(root ? root->Find("amb_color") : nullptr, g_maps.amb);
        g_maps.tint = RgbFromJson(root ? root->Find("tint_color") : nullptr, g_maps.tint);
        num("fog_density", g_maps.fog_density); num("sun_power", g_maps.sun_power);
        num("exposure", g_maps.exposure); num("hue", g_maps.hue); num("saturation", g_maps.sat); num("brightness", g_maps.bright);
        num("contrast", g_maps.contrast); num("vignette", g_maps.vignette);
    }
    if (ImGui::GetTime() - g_maps.status_read > 2) {
        g_maps.status_read = ImGui::GetTime();
        auto root = json::Parse(ReadText(DefaultConfigPath().parent_path() / "maps_status.json"));
        const json::Value* m = root ? root->Find("water_materials") : nullptr;
        const json::Value* c = root ? root->Find("water_copies") : nullptr;
        g_maps.materials = m && m->type == json::Value::Type::Number ? int(m->n) : -1;
        g_maps.copies = c && c->type == json::Value::Type::Number ? int(c->n) : 0;
        // which map's sky the layer found (wheels_found.json lists every texture the texture engine tracks)
        std::set<std::string> found;
        for (auto& id : wheel::LoadFound()) found.insert(id);
        g_maps.roo_ground_found = g_maps.roo_water_found = g_maps.roo_light_found = 0;
        for (auto& g : g_maps.roo_cat) {
            std::set<std::string> a, b, c;
            for (auto& t : g.textures) if (found.count(t.id)) (t.group == "ground" ? a : t.group == "water" ? b : c).insert(t.name);
            g_maps.roo_ground_found = int(a.size()); g_maps.roo_water_found = int(b.size()); g_maps.roo_light_found = int(c.size());
        }
        g_maps.lava_found = g_maps.fire_found = g_maps.rock_found = 0;
        for (auto& g : g_maps.lava_cat) {
            std::set<std::string> lv, fi, ro;
            for (auto& t : g.textures) if (found.count(t.id)) (t.group == "lava" ? lv : t.group == "fire" ? fi : ro).insert(t.name);
            g_maps.lava_found = int(lv.size()); g_maps.fire_found = int(fi.size()); g_maps.rock_found = int(ro.size());
        }
        g_maps.pad_found = 0;
        for (auto& g : g_maps.pad_cat) {
            std::set<std::string> names;
            for (auto& t : g.textures) if (found.count(t.id)) names.insert(t.name);
            g_maps.pad_found = std::max(g_maps.pad_found, int(names.size()));
        }
        g_maps.snow_found = g_maps.ice_found = 0;
        for (auto& g : g_maps.snow_cat) {
            std::set<std::string> names;
            for (auto& t : g.textures) if (found.count(t.id)) names.insert(t.name);
            (g.name == "Snow" ? g_maps.snow_found : g_maps.ice_found) = int(names.size());
        }
        g_maps.sky_found = 0;
        g_maps.sky_map.clear();
        for (auto& m : g_maps.sky_cat) {
            std::set<std::string> names;
            for (auto& t : m.textures) if (found.count(t.id)) names.insert(t.name);
            if (int(names.size()) > g_maps.sky_found) { g_maps.sky_found = int(names.size()); g_maps.sky_map = m.display.empty() ? m.name : m.display; }
        }
    }
    PageTitle("MAPS", "The colours of the map you are racing, changed live in the game.");
    ResetAllButton();
    ImGui::Dummy(ImVec2(0, 6));
    bool changed = false;
    // while a colour is dragged the files are written at most every 0.15 s (not every frame), and once more when it stops
    static std::map<std::string, double> last_save;
    static std::set<std::string> pending;
    auto flush = [&](const char* key, bool ch, void (*save)()) {
        if (ch) pending.insert(key);
        if (pending.count(key) && ImGui::GetTime() - last_save[key] > 0.15) { save(); last_save[key] = ImGui::GetTime(); pending.erase(key); }
    };
    // my character
    {
        if (ImGui::GetTime() - g_char.hero_checked > 3) {
            g_char.hero_checked = ImGui::GetTime();
            const std::string h = CurrentHero();
            if (h != g_char.hero) { g_char.hero = h; CharSave(); }
        }
        std::vector<const wheel::Texture*> list;
        std::set<std::string> seen;
        std::set<std::string> found;
        for (auto& id : wheel::LoadFound()) found.insert(id);
        g_char.found = 0;
        for (auto& e : g_char.cat)
            if (!g_char.hero.empty() && e.name.rfind(g_char.hero, 0) == 0)
                for (auto& t : e.textures) {
                    if (found.count(t.id)) ++g_char.found;
                    if (seen.insert(t.name).second) list.push_back(&t);
                }
        bool ch = false;
        BeginCard("character");
        Text("My character", kOrange, 22, g_bold);
        if (g_char.cat.empty()) Text("driver_catalog.json is missing next to ASTRO_CTR.exe (run run.bat).", kMuted, 14);
        else if (g_char.hero.empty()) Text("Start the game and pick a character: ASTRO reads it from the launcher log.", kMuted, 14);
        else {
            const std::string t = "Character: " + g_char.hero + "  -  " + std::to_string(list.size()) + " parts, " + std::to_string(g_char.found) +
                                  " found in the game now";
            Text(t.c_str(), kMuted, 14);
        }
        ImGui::Dummy(ImVec2(0, 8));
        int i = 0;
        for (const wheel::Texture* t : list) {
            CharPart& p = g_char.parts[t->name];
            ImGui::PushID(i++);
            ImGui::BeginGroup();
            Text(t->name.c_str(), kText, 15, g_bold);
            ImGui::EndGroup();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
            if (Switch("##on", &p.on)) ch = true;
            if (p.on && ColorRow("Colour", p.color)) ch = true;
            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 4));
        }
        if (!list.empty()) {
            float st = float(g_char.strength), br = float(g_char.bright);
            if (Slider("Strength", &st, 0.f, 100.f, "%.0f%%", 0)) { g_char.strength = int(st + 0.5f); ch = true; }
            if (Slider("Brightness", &br, 20.f, 200.f, "%.0f%%", 0)) { g_char.bright = int(br + 0.5f); ch = true; }
        }
        EndCard();
        if (ch) CharSave();
    }

    BeginCard("water");
    Text("Water", kOrange, 22, g_bold);
    if (!g_game.Connected()) Text("Start the game: the water is found when you are in a race.", kMuted, 14);
    else if (!g_maps.color_on && !g_maps.opaque) Text("Switch an option on and ASTRO finds the water of this map.", kMuted, 14);
    else if (g_maps.materials < 0) Text("Looking for the water of this map...", kMuted, 14);
    else if (g_maps.materials == 0) Text("No flowing water found in this map (or still loading).", kMuted, 14);
    else Text(("Water found in this map (" + std::to_string(g_maps.copies) + " places changed)").c_str(), kMuted, 14);
    ImGui::Dummy(ImVec2(0, 8));

    ImGui::BeginGroup();
    Text("Own water colour", kText, 16, g_bold);
    Text("The colour of the deep water", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##wcol", &g_maps.color_on)) changed = true;
    if (g_maps.color_on && ColorRow("Water colour", g_maps.water)) changed = true;
    ImGui::Dummy(ImVec2(0, 6));

    ImGui::BeginGroup();
    Text("Opaque water", kText, 16, g_bold);
    Text("The water is not see-through: rocks, plants and fish under it are hidden", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##wopaque", &g_maps.opaque)) changed = true;
    EndCard();

    bool sky_changed = false;
    BeginCard("sky");
    Text("Sky", kOrange, 22, g_bold);
    if (g_maps.sky_cat.empty()) Text("sky_catalog.json is missing next to ASTRO_CTR.exe (run run.bat).", kMuted, 14);
    else if (!g_game.Connected()) Text("Start the game: the sky is found when you are in a race.", kMuted, 14);
    else if (!g_maps.sky_on) Text("Switch it on and ASTRO finds the sky of this map.", kMuted, 14);
    else if (g_maps.sky_found == 0) Text("Looking for the sky of this map...", kMuted, 14);
    else Text(("Sky of " + g_maps.sky_map + " found (" + std::to_string(g_maps.sky_found) + " sides)").c_str(), kMuted, 14);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::BeginGroup();
    Text("Own sky colour", kText, 16, g_bold);
    Text("Recolours the sky pictures of the map you are racing", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##skyon", &g_maps.sky_on)) sky_changed = true;
    if (g_maps.sky_on) {
        if (ColorRow("Sky colour", g_maps.sky)) sky_changed = true;
        float st = float(g_maps.sky_strength);
        if (Slider("Strength", &st, 0.f, 100.f, "%.0f%%", 0)) { g_maps.sky_strength = int(st + 0.5f); sky_changed = true; }
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::BeginGroup();
        Text("Colour the clouds too", kText, 15);
        Text("Off: white clouds and light stay white", kMuted, 13);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch("##skyclouds", &g_maps.sky_clouds)) sky_changed = true;
    }
    EndCard();
    if (sky_changed) changed = true;
    flush("sky", sky_changed, SkySave);

    // Roo's Tubes: the sand/mud ground on the sides, the water, the red lights
    bool roo_changed = false;
    BeginCard("roo");
    Text("Roo's Tubes", kOrange, 22, g_bold);
    if (g_maps.roo_cat.empty()) Text("roo_catalog.json is missing next to ASTRO_CTR.exe (run run.bat).", kMuted, 14);
    else if (!g_game.Connected()) Text("Start the game: they are found when you are in a race on Roo's Tubes.", kMuted, 14);
    else if (!g_maps.roo_ground_on && !g_maps.roo_water_on && !g_maps.roo_light_on) Text("Switch an option on and ASTRO finds them in this map.", kMuted, 14);
    else {
        const std::string t = "Found in this map: " + std::to_string(g_maps.roo_ground_found) + " ground, " + std::to_string(g_maps.roo_water_found) + " water, " +
                              std::to_string(g_maps.roo_light_found) + " light textures";
        Text(t.c_str(), kMuted, 14);
    }
    ImGui::Dummy(ImVec2(0, 8));
    auto roo_row = [&](const char* title, const char* sub, const char* sw, bool* on, const char* clabel, Rgba* col) {
        ImGui::BeginGroup();
        Text(title, kText, 16, g_bold);
        Text(sub, kMuted, 13);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch(sw, on)) roo_changed = true;
        if (*on && ColorRow(clabel, *col)) roo_changed = true;
        ImGui::Dummy(ImVec2(0, 6));
    };
    {   // the blue water and the red tunnel light are the zone's light, not textures
        ImGui::BeginGroup();
        Text("Own light colour of the zone", kText, 16, g_bold);
        Text("The light of the water and the tunnels: sun, fog, ambient, sky, sun shafts", kMuted, 13);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        bool c = false;
        if (Switch("##woolwl", &g_maps.wl_on)) c = true;
        if (g_maps.wl_on && ColorRow("Zone light colour", g_maps.wl)) c = true;
        ImGui::Dummy(ImVec2(0, 6));
        if (c) { roo_changed = true; MapsSave(); }
    }
    roo_row("Own sand colour", "The sand and mud slopes on the sides that slow you down", "##rooground", &g_maps.roo_ground_on, "Sand colour", &g_maps.roo_ground);
    roo_row("Own water colour", "The water outside the track (caustics and water textures)", "##roowater", &g_maps.roo_water_on, "Water colour", &g_maps.roo_water);
    roo_row("Own red light colour", "The red lights and the glow in the tubes", "##roolight", &g_maps.roo_light_on, "Light colour", &g_maps.roo_light);
    if (g_maps.roo_ground_on || g_maps.roo_water_on || g_maps.roo_light_on) {
        float st = float(g_maps.roo_strength);
        if (Slider("Strength", &st, 0.f, 100.f, "%.0f%%", 0)) { g_maps.roo_strength = int(st + 0.5f); roo_changed = true; }
    }
    EndCard();
    if (roo_changed) changed = true;
    flush("roo", roo_changed, RooSave);

    // lava, fire and volcanic rock (Inferno Island, Dragon Mines, Barin Ruins, Fire Zone ...)
    bool lava_changed = false;
    BeginCard("lava");
    Text("Lava, fire and volcanic rock", kOrange, 22, g_bold);
    if (g_maps.lava_cat.empty()) Text("lava_catalog.json is missing next to ASTRO_CTR.exe (run run.bat).", kMuted, 14);
    else if (!g_game.Connected()) Text("Start the game: they are found when you are in a race on a map that has them.", kMuted, 14);
    else if (!g_maps.lava_on && !g_maps.fire_on && !g_maps.rock_on) Text("Switch an option on and ASTRO finds them in this map.", kMuted, 14);
    else {
        const std::string t = "Found in this map: " + std::to_string(g_maps.lava_found) + " lava, " + std::to_string(g_maps.fire_found) + " fire, " +
                              std::to_string(g_maps.rock_found) + " rock textures";
        Text(t.c_str(), kMuted, 14);
    }
    ImGui::Dummy(ImVec2(0, 8));
    auto lava_row = [&](const char* title, const char* sub, const char* sw, bool* on, const char* clabel, Rgba* col) {
        ImGui::BeginGroup();
        Text(title, kText, 16, g_bold);
        Text(sub, kMuted, 13);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch(sw, on)) lava_changed = true;
        if (*on && ColorRow(clabel, *col)) lava_changed = true;
        ImGui::Dummy(ImVec2(0, 6));
    };
    lava_row("Own lava colour", "The lava, its flow and its glow", "##lavaon", &g_maps.lava_on, "Lava colour", &g_maps.lava);
    lava_row("Own fire colour", "The flames and the fire of torches", "##fireon", &g_maps.fire_on, "Fire colour", &g_maps.fire);
    lava_row("Own volcanic rock colour", "The dark volcanic rocks and Inferno Island stone", "##rockon", &g_maps.rock_on, "Rock colour", &g_maps.rock);
    if (g_maps.lava_on || g_maps.fire_on || g_maps.rock_on) {
        float st = float(g_maps.lava_strength);
        if (Slider("Strength", &st, 0.f, 100.f, "%.0f%%", 0)) { g_maps.lava_strength = int(st + 0.5f); lava_changed = true; }
    }
    EndCard();
    if (lava_changed) changed = true;
    flush("lava", lava_changed, LavaSave);

    // speed pads (every map has them: the pad, its glow and the boost arrows)
    bool pad_changed = false;
    BeginCard("pads");
    Text("Speed pads", kOrange, 22, g_bold);
    if (g_maps.pad_cat.empty()) Text("pad_catalog.json is missing next to ASTRO_CTR.exe (run run.bat).", kMuted, 14);
    else if (!g_game.Connected()) Text("Start the game: the pads are found when you are in a race.", kMuted, 14);
    else if (!g_maps.pad_on && !g_maps.pad_body_on) Text("Switch an option on and ASTRO finds the speed pads of this map.", kMuted, 14);
    else Text(g_maps.pad_found ? ("Speed pads found in this map (" + std::to_string(g_maps.pad_found) + " textures)").c_str() : "Looking for the speed pads of this map...", kMuted, 14);
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::BeginGroup();
    Text("Pad light", kText, 16, g_bold);
    Text("The glowing arrows of the pad and their light", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##padon", &g_maps.pad_on)) pad_changed = true;
    if (g_maps.pad_on && ColorRow("Light colour", g_maps.pad)) pad_changed = true;
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::BeginGroup();
    Text("Pad body and arrows", kText, 16, g_bold);
    Text("The black part of the pad, its border and the printed arrows", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##padbody", &g_maps.pad_body_on)) pad_changed = true;
    if (g_maps.pad_body_on && ColorRow("Body colour", g_maps.pad_body)) pad_changed = true;
    if (g_maps.pad_on || g_maps.pad_body_on) {
        float st = float(g_maps.pad_strength);
        if (Slider("Strength", &st, 0.f, 100.f, "%.0f%%", 0)) { g_maps.pad_strength = int(st + 0.5f); pad_changed = true; }
    }
    EndCard();
    if (pad_changed) changed = true;
    flush("pad", pad_changed, PadSave);

    // snow and ice (the maps that have them: Blizzard Bluff, Polar Pass, Frozen Frenzy, Glacier Park ...)
    bool snow_changed = false;
    BeginCard("snow");
    Text("Snow and ice", kOrange, 22, g_bold);
    if (g_maps.snow_cat.empty()) Text("snow_catalog.json is missing next to ASTRO_CTR.exe (run run.bat).", kMuted, 14);
    else if (!g_game.Connected()) Text("Start the game: the snow is found when you are in a race on a snowy map.", kMuted, 14);
    else if (!g_maps.snow_on && !g_maps.ice_on) Text("Switch an option on and ASTRO finds the snow and ice of this map.", kMuted, 14);
    else {
        const std::string t = "Found in this map: " + std::to_string(g_maps.snow_found) + " snow and " + std::to_string(g_maps.ice_found) + " ice textures" +
                              (g_maps.snow_found + g_maps.ice_found == 0 ? " (this map may have none, or it is still loading)" : "");
        Text(t.c_str(), kMuted, 14);
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::BeginGroup();
    Text("Own snow colour", kText, 16, g_bold);
    Text("The snow on the ground, the roofs and the trees", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##snowon", &g_maps.snow_on)) snow_changed = true;
    if (g_maps.snow_on && ColorRow("Snow colour", g_maps.snow)) snow_changed = true;
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::BeginGroup();
    Text("Own ice colour", kText, 16, g_bold);
    Text("The ice walls, the frozen track and the frost", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##iceon", &g_maps.ice_on)) snow_changed = true;
    if (g_maps.ice_on && ColorRow("Ice colour", g_maps.ice)) snow_changed = true;
    if (g_maps.snow_on || g_maps.ice_on) {
        float st = float(g_maps.snow_strength);
        if (Slider("Strength", &st, 0.f, 100.f, "%.0f%%", 0)) { g_maps.snow_strength = int(st + 0.5f); snow_changed = true; }
    }
    EndCard();
    if (snow_changed) changed = true;
    flush("snow", snow_changed, SnowSave);

    // a switch row: title, sub line, switch on the right
    auto sw_row = [&](const char* title, const char* sub, const char* id, bool* v) {
        ImGui::BeginGroup();
        Text(title, kText, 16, g_bold);
        Text(sub, kMuted, 13);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
        if (Switch(id, v)) changed = true;
    };
    auto slider = [&](const char* label, float* v, float lo, float hi, const char* fmt) { if (Slider(label, v, lo, hi, fmt, 0)) changed = true; };

    // ready-made looks: they set the lighting switches below (the sky and water keep what you chose)
    BeginCard("looks");
    Text("Looks", kOrange, 22, g_bold);
    Text("One click sets the lighting of the map. Change anything below afterwards.", kMuted, 14);
    ImGui::Dummy(ImVec2(0, 6));
    struct Look { const char* name; void (*set)(MapsState&); };
    const Look looks[] = {
        {"Night", [](MapsState& m) { m.grade_on = true; m.exposure = -60; m.sat = -30; m.bright = -20; m.contrast = 10; m.vignette = 0.5f;
                                     m.tint_on = true; m.tint = {150, 170, 255, 255}; m.sun_on = true; m.sun = {120, 140, 255, 255}; m.sun_power = 30;
                                     m.fog_on = true; m.fog = {20, 25, 60, 255}; m.fog_density = 150; m.amb_on = true; m.amb = {90, 100, 160, 255}; }},
        {"Sunset", [](MapsState& m) { m.grade_on = true; m.exposure = 0; m.sat = 20; m.bright = 0; m.contrast = 10; m.vignette = 0.2f;
                                      m.tint_on = true; m.tint = {255, 200, 160, 255}; m.sun_on = true; m.sun = {255, 140, 60, 255}; m.sun_power = 120;
                                      m.fog_on = true; m.fog = {255, 150, 110, 255}; m.fog_density = 120; m.amb_on = false; }},
        {"Black & white", [](MapsState& m) { m.grade_on = true; m.sat = -100; m.exposure = 0; m.bright = 0; m.contrast = 20; m.vignette = 0.3f; m.tint_on = false; }},
        {"Neon", [](MapsState& m) { m.grade_on = true; m.sat = 60; m.contrast = 30; m.exposure = 0; m.bright = 0; m.vignette = 0.4f;
                                    m.tint_on = true; m.tint = {255, 150, 255, 255}; m.fog_on = true; m.fog = {200, 0, 255, 255}; m.fog_density = 130; }},
        {"Game's own", [](MapsState& m) { m.fog_on = m.sun_on = m.amb_on = m.grade_on = m.tint_on = false; }},
    };
    for (int i = 0; i < IM_ARRAYSIZE(looks); ++i) {
        if (i) ImGui::SameLine(0, 6);
        if (Button(looks[i].name, false)) { looks[i].set(g_maps); changed = true; }
    }
    EndCard();

    BeginCard("fog");
    Text("Fog", kOrange, 22, g_bold);
    sw_row("Own fog", "The haze that covers what is far away", "##fogon", &g_maps.fog_on);
    if (g_maps.fog_on) {
        if (ColorRow("Fog colour", g_maps.fog)) changed = true;
        slider("Thickness", &g_maps.fog_density, 0.f, 400.f, "%.0f%%");
    }
    EndCard();

    BeginCard("sun");
    Text("Sun", kOrange, 22, g_bold);
    sw_row("Own sunlight", "The colour and strength of the light on the whole map", "##sunon", &g_maps.sun_on);
    if (g_maps.sun_on) {
        if (ColorRow("Sun colour", g_maps.sun)) changed = true;
        slider("Strength", &g_maps.sun_power, 0.f, 300.f, "%.0f%%");
    }
    ImGui::Dummy(ImVec2(0, 6));
    sw_row("Own shadow light", "The light in the shade (ambient): white = the game's, darker = darker shadows", "##ambon", &g_maps.amb_on);
    if (g_maps.amb_on && ColorRow("Shadow light", g_maps.amb)) changed = true;
    EndCard();

    BeginCard("grade");
    Text("Colours of the whole map", kOrange, 22, g_bold);
    sw_row("Colour grading", "Changes every colour on screen, like a camera filter (0 = the game's own)", "##gradeon", &g_maps.grade_on);
    if (g_maps.grade_on) {
        slider("Hue", &g_maps.hue, -180.f, 180.f, "%.0f");
        slider("Saturation", &g_maps.sat, -100.f, 100.f, "%.0f");
        slider("Brightness", &g_maps.bright, -100.f, 100.f, "%.0f");
        slider("Contrast", &g_maps.contrast, -100.f, 100.f, "%.0f");
        slider("Exposure", &g_maps.exposure, -100.f, 100.f, "%.0f");
        slider("Dark edges", &g_maps.vignette, 0.f, 1.f, "%.2f");
        if (Button("Reset grading")) { g_maps.hue = g_maps.sat = g_maps.bright = g_maps.contrast = g_maps.exposure = g_maps.vignette = 0; changed = true; }
    }
    ImGui::Dummy(ImVec2(0, 6));
    sw_row("Colour filter", "Tints the whole picture with one colour", "##tinton", &g_maps.tint_on);
    if (g_maps.tint_on && ColorRow("Filter colour", g_maps.tint)) changed = true;
    EndCard();
    flush("maps", changed, MapsSave);
}

void PresetsPage() {
    PageTitle("Presets", "Save your colours, share them as a JSON file, and load them here or in the game menu (F8).");
    static char name[48] = "My colours";
    BeginCard("save");
    CardHeader("Save current colours", nullptr, nullptr);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 250);
    ImGui::InputText("##name", name, sizeof name);
    ImGui::SameLine();
    if (Button("Save preset", true, ImVec2(120, 0))) {
        std::string err;
        const auto p = SavePreset(name, g_sync.cfg, &err);
        g_sync.Note(p.empty() ? "save failed: " + err : "saved " + p.stem().string());
    }
    ImGui::SameLine();
    if (Button("Import", false, ImVec2(112, 0))) {
        const fs::path from = FileDialog(false, L"");
        if (!from.empty()) {
            std::string err;
            const auto p = ImportPreset(from, &err);
            g_sync.Note(p.empty() ? "import failed: " + err : "imported " + p.stem().string());
        }
    }
    EndCard();

    const auto list = ListPresets();
    if (list.empty()) {
        BeginCard("empty");
        Wrapped("No presets yet. Save your current colours above, or import a JSON file someone shared with you.");
        EndCard();
    }
    for (const auto& pr : list) {
        ImGui::PushID(pr.name.c_str());
        LoadResult lr = LoadConfig(pr.path);
        const Config& c = lr.config;
        BeginCard("pr");
        ImGui::BeginGroup();
        Text(pr.name.c_str(), kText, 19, g_bold);
        if (lr.recovered_from_error) Text("this file could not be read", kMuted, 14);
        else Swatches({c.normal.color, c.normal.color2, c.normal.color3, c.paint.layer[0][1], c.paint.layer[1][1], c.paint.layer[2][1], c.champion.lower_region}, 16);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 240);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6);
        ImGui::BeginDisabled(lr.recovered_from_error);
        if (Button("Apply", true, ImVec2(112, 0))) {
            const UiSettings ui = g_sync.cfg.ui;
            g_sync.cfg = c;
            g_sync.cfg.ui = ui;
            g_sync.Touch();
            g_sync.Note("applied " + pr.name + (g_game.Connected() ? " in game" : ""));
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("Export", false, ImVec2(112, 0))) {
            const fs::path to = FileDialog(true, Widen(pr.name) + L".json");
            std::error_code ec;
            if (!to.empty()) {
                fs::copy_file(pr.path, to, fs::copy_options::overwrite_existing, ec);
                g_sync.Note(ec ? "export failed: " + ec.message() : "exported to " + to.filename().string());
            }
        }
        EndCard();
        ImGui::PopID();
    }
    BeginCard("folder");
    if (Button("Open presets folder")) {
        std::error_code ec;
        fs::create_directories(PresetsDir(), ec);
        ShellExecuteW(nullptr, L"open", PresetsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    EndCard();
}


// ---------------- DLL injector ----------------
// Loads a DLL the user picked into the running game (sacredfire.exe) with LoadLibraryW on a remote thread.
// The list of DLLs is kept in %APPDATA%\ASTRO-CTR\injector.txt (one path per line).
struct InjectEntry { fs::path path; std::string result; };
std::vector<InjectEntry> g_dlls;
bool g_dlls_loaded = false;

fs::path InjectListPath() { return DefaultConfigPath().parent_path() / "injector.txt"; }
void SaveDllList() {
    std::error_code ec;
    fs::create_directories(InjectListPath().parent_path(), ec);
    if (FILE* f = _wfopen(InjectListPath().c_str(), L"w")) {
        for (auto& e : g_dlls) std::fprintf(f, "%s\n", e.path.u8string().c_str());
        std::fclose(f);
    }
}
void LoadDllList() {
    g_dlls_loaded = true;
    if (FILE* f = _wfopen(InjectListPath().c_str(), L"r")) {
        char line[1024];
        while (std::fgets(line, sizeof line, f)) {
            std::string l(line);
            while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
            if (!l.empty()) g_dlls.push_back({fs::u8path(l), ""});
        }
        std::fclose(f);
    }
}

// machine type of a PE file: 0x8664 = x64 (the game is 64-bit), 0 = not a PE file
WORD PeMachine(const fs::path& p) {
    WORD m = 0;
    if (FILE* f = _wfopen(p.c_str(), L"rb")) {
        IMAGE_DOS_HEADER dos{};
        DWORD sig = 0;
        IMAGE_FILE_HEADER fh{};
        if (std::fread(&dos, sizeof dos, 1, f) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE && std::fseek(f, dos.e_lfanew, SEEK_SET) == 0 &&
            std::fread(&sig, 4, 1, f) == 1 && sig == IMAGE_NT_SIGNATURE && std::fread(&fh, sizeof fh, 1, f) == 1)
            m = fh.Machine;
        std::fclose(f);
    }
    return m;
}

std::string Inject(DWORD pid, const fs::path& dll) {
    std::error_code ec;
    if (!fs::exists(dll, ec)) return "file not found";
    const WORD m = PeMachine(dll);
    if (m == 0) return "not a DLL file";
    if (m != IMAGE_FILE_MACHINE_AMD64) return "this DLL is 32-bit; the game needs a 64-bit DLL";
    const std::wstring path = fs::absolute(dll, ec).wstring();
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) return "cannot open the game (error " + std::to_string(GetLastError()) + ")";
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    std::string out;
    if (!mem || !WriteProcessMemory(proc, mem, path.c_str(), bytes, nullptr)) out = "cannot write into the game";
    else {
        auto load = reinterpret_cast<LPTHREAD_START_ROUTINE>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW")));
        HANDLE th = CreateRemoteThread(proc, nullptr, 0, load, mem, 0, nullptr);
        if (!th) out = "cannot start the loader thread (error " + std::to_string(GetLastError()) + ")";
        else {
            DWORD code = 0;
            if (WaitForSingleObject(th, 10000) == WAIT_TIMEOUT) out = "the DLL is still starting (no answer after 10 s)";
            else { GetExitCodeThread(th, &code); out = code ? "loaded" : "the game refused the DLL (LoadLibrary failed)"; }
            CloseHandle(th);
        }
    }
    if (mem) VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return out;
}

// every DLL in autoload\ next to ASTRO_CTR.exe goes into each game ASTRO connects to, once per game process
void AutoInject(const fs::path& dir) {
    static DWORD done_pid = 0;
    if (!g_game.Connected() || g_game.pid == done_pid) return;
    done_pid = g_game.pid;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir / "autoload", ec))
        if (e.path().extension() == ".dll") Inject(g_game.pid, e.path());
}

void InjectPage() {
    if (!g_dlls_loaded) LoadDllList();
    PageTitle("DLL injector", "Load your own DLL mods into the running game.");
    const DWORD pid = g_game.state == Game::None ? 0 : FindProcess(L"sacredfire.exe");
    BeginCard("target");
    CardHeader("Game", nullptr, nullptr);
    char buf[96];
    if (pid) std::snprintf(buf, sizeof buf, "sacredfire.exe is running  (pid %lu)", (unsigned long)pid);
    Text(pid ? buf : "The game is not running. Start it first.", pid ? kText : kMuted, 16);
    ImGui::Dummy(ImVec2(0, 4));
    Wrapped("Only load DLLs you trust: a DLL runs with full access inside the game. 64-bit DLLs only. "
            "ASTRO itself does not need this: it loads on its own when the game starts.");
    EndCard();

    BeginCard("list");
    CardHeader("DLLs", nullptr, nullptr);
    if (Button("Add DLL...", true)) {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW o{};
        o.lStructSize = sizeof o;
        o.hwndOwner = g_hwnd;
        o.lpstrFilter = L"DLL (*.dll)\0*.dll\0";
        o.lpstrFile = file;
        o.nMaxFile = MAX_PATH;
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&o) &&
            std::none_of(g_dlls.begin(), g_dlls.end(), [&](const InjectEntry& e) { return e.path == fs::path(file); })) {
            g_dlls.push_back({fs::path(file), ""});
            SaveDllList();
        }
    }
    ImGui::Dummy(ImVec2(0, 6));
    if (g_dlls.empty()) Text("No DLLs added yet.", kMuted, 15);
    int remove = -1;
    for (int i = 0; i < int(g_dlls.size()); ++i) {
        auto& e = g_dlls[i];
        ImGui::PushID(i);
        ImGui::BeginGroup();
        Text(e.path.filename().u8string().c_str(), kText, 17, g_bold);
        Text(e.path.parent_path().u8string().c_str(), kMuted, 13);
        if (!e.result.empty()) Text(e.result.c_str(), e.result == "loaded" ? kOrange : kMuted, 14);
        ImGui::EndGroup();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - 236);
        ImGui::BeginDisabled(!pid);
        if (Button("Inject", true, ImVec2(112, 0))) e.result = Inject(pid, e.path);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (Button("Remove", false, ImVec2(112, 0))) remove = i;   // removes it from this list only, never the file
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PopID();
    }
    if (remove >= 0) { g_dlls.erase(g_dlls.begin() + remove); SaveDllList(); }
    EndCard();
}

// ---------------- GAME page (live race controller) ----------------
struct GameRaceTabState {
    char lap_bufs[16][32] = {};
    int last_laps = 0;
    int manual_laps_override = 0;
    std::string last_track_path;
    std::string status_msg;
    double status_msg_time = 0;
    bool status_is_error = false;
};
static GameRaceTabState s_game_tab;

static std::string CleanTrackName(const std::string& path) {
    if (path.empty()) return "No Track Loaded";
    std::string lower = path;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.find("spyro") != std::string::npos) return "Spyro Circuit";
    if (lower.find("cove") != std::string::npos) return "Crash Cove";
    if (lower.find("sewer") != std::string::npos) return "Sewer Speedway";
    if (lower.find("tiger") != std::string::npos) return "Tiger Temple";
    if (lower.find("pyramid") != std::string::npos) return "Papu's Pyramid";
    if (lower.find("roo") != std::string::npos) return "Roo's Tubes";
    if (lower.find("coco") != std::string::npos) return "Coco Park";
    if (lower.find("blizzard") != std::string::npos) return "Blizzard Bluff";
    if (lower.find("polar") != std::string::npos) return "Polar Pass";
    if (lower.find("tiny") != std::string::npos) return "Tiny Arena";
    if (lower.find("dragon") != std::string::npos) return "Dragon Mines";
    if (lower.find("cortex") != std::string::npos) return "Cortex Castle";
    if (lower.find("gin") != std::string::npos) return "N. Gin Labs";
    if (lower.find("hot_air") != std::string::npos) return "Hot Air Skyway";
    if (lower.find("oxide") != std::string::npos) return "Oxide Station";
    if (lower.find("slide") != std::string::npos) return "Slide Coliseum";
    if (lower.find("turbo") != std::string::npos) return "Turbo Track";
    if (lower.find("clockwork") != std::string::npos) return "Clockwork Wumpa";
    if (lower.find("electron") != std::string::npos) return "Electron Avenue";
    if (lower.find("deep_sea") != std::string::npos) return "Deep Sea Driving";
    if (lower.find("thunder") != std::string::npos) return "Thunder Struck";
    if (lower.find("prehistoric") != std::string::npos) return "Prehistoric Playground";
    if (lower.find("gingerbread") != std::string::npos) return "Gingerbread Joyride";
    if (lower.find("megamix") != std::string::npos) return "Megamix Mania";
    if (lower.find("drive_thru") != std::string::npos) return "Drive-Thru Danger";
    if (lower.find("twilight") != std::string::npos) return "Twilight Tour";
    if (lower.find("koala") != std::string::npos) return "Koala Carnival";
    if (lower.find("nina") != std::string::npos) return "Nina's Nightmare";
    if (lower.find("assembly") != std::string::npos) return "Assembly Lane";
    if (lower.find("android") != std::string::npos) return "Android Alley";
    if (lower.find("hyper") != std::string::npos) return "Hyper Spaceway";
    if (lower.find("meteor") != std::string::npos) return "Meteor Gorge";
    if (lower.find("barin") != std::string::npos) return "Barin Ruins";
    if (lower.find("out_of_time") != std::string::npos) return "Out of Time";
    size_t last_slash = path.find_last_of("/\\");
    std::string s = (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;
    for (char& c : s) if (c == '_') c = ' ';
    return s;
}

// lap times are typed as M:SS.CC (1:05.40), M:SS:CC (1:05:40) or plain seconds (65.4)
static double ParseLapTimeString(const char* str) {
    if (!str || !*str) return 0.0;
    std::string s(str);
    for (char& c : s) if (c == ':' || c == ';') c = ' ';
    std::stringstream ss(s);
    std::vector<std::string> parts;
    std::string token;
    while (ss >> token) parts.push_back(token);
    if (parts.size() == 1) return std::max(0.0, std::atof(parts[0].c_str()));
    if (parts.size() == 2) return std::max(0.0, std::atof(parts[0].c_str()) * 60.0 + std::atof(parts[1].c_str()));
    if (parts.size() >= 3) return std::max(0.0, std::atof(parts[0].c_str()) * 60.0 + std::atof(parts[1].c_str()) + std::atof(parts[2].c_str()) / 100.0);
    return 0.0;
}

// An online race: the newest launcher log shows packets from other players (sceNetRecvfrom on the game socket) after the
// last level load. Offline races have none (checked in last_run logs 2026-11-02: hundreds online, none offline).
static bool OnlineRaceNow() {
    static double checked = -100;
    static bool online = false;
    if (ImGui::GetTime() - checked < 3) return online;
    checked = ImGui::GetTime();
    std::error_code ec;
    fs::path newest;
    fs::file_time_type t{};
    for (fs::directory_iterator it(GameDir(), ec), end; !ec && it != end; it.increment(ec)) {
        const std::string n = it->path().filename().string();
        if (n.rfind("last_run", 0) == 0 && it->path().extension() == ".log" && (newest.empty() || it->last_write_time(ec) > t)) {
            newest = it->path();
            t = it->last_write_time(ec);
        }
    }
    const std::string log = ReadText(newest);
    size_t from = log.rfind("LOADING LEVEL");
    if (from == std::string::npos) from = 0;
    int packets = 0;
    for (size_t at = log.find("sceNetRecvfrom(fd=1", from); at != std::string::npos && packets < 20; at = log.find("sceNetRecvfrom(fd=1", at + 1)) ++packets;
    online = packets >= 20;
    return online;
}

static std::string FormatLapSeconds(double total_sec) {
    if (total_sec < 0.0) total_sec = 0.0;
    int m = static_cast<int>(total_sec / 60.0);
    double rem = total_sec - (m * 60.0);
    int s = static_cast<int>(rem);
    int cs = static_cast<int>(std::round((rem - s) * 100.0));
    if (cs >= 100) { cs = 0; s++; }
    if (s >= 60) { s = 0; m++; }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%02d:%02d", m, s, cs);
    return buf;
}

void GamePage() {
    PageTitle("GAME", "Offline races only: see the race you are in, set your lap times and finish it.");

    const DWORD pid = FindProcess(L"sacredfire.exe");

    if (!pid) {
        BeginCard("game_idle");
        CardHeader("Game Not Running", nullptr, nullptr);
        Text("sacredfire.exe is not currently running.", kMuted, 16);
        ImGui::Dummy(ImVec2(0, 4));
        Wrapped("Start Crash Team Racing Nitro-Fueled from the launcher to activate the live race controller.");
        EndCard();
        return;
    }

    // the game process, opened once (this page is drawn every frame)
    static HANDLE proc = nullptr;
    static DWORD proc_pid = 0;
    if (proc && proc_pid != pid) { CloseHandle(proc); proc = nullptr; }
    if (!proc) { proc = OpenProcess(PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION, FALSE, pid); proc_pid = pid; }
    if (!proc) {
        BeginCard("game_error");
        CardHeader("Access Denied", nullptr, nullptr);
        Text("Cannot open the game process. Make sure ASTRO CTR has administrator permissions.", kOrange, 16);
        EndCard();
        return;
    }

    auto read_mem = [&](uintptr_t addr, void* out, SIZE_T sz) -> bool {
        SIZE_T got = 0;
        return ReadProcessMemory(proc, reinterpret_cast<void*>(addr), out, sz, &got) && got == sz;
    };
    auto write_mem = [&](uintptr_t addr, const void* in, SIZE_T sz) -> bool {
        SIZE_T put = 0;
        return WriteProcessMemory(proc, reinterpret_cast<void*>(addr), in, sz, &put) && put == sz;
    };
    auto read_u64 = [&](uintptr_t addr) -> uint64_t {
        uint64_t v = 0; read_mem(addr, &v, 8); return v;
    };
    auto read_i32 = [&](uintptr_t addr) -> int32_t {
        int32_t v = 0; read_mem(addr, &v, 4); return v;
    };

    const uintptr_t kGuestBase = 0x800000000ull;
    const uintptr_t kRacerMgrPtr = kGuestBase + 0x02ad4b70ull;

    uint64_t mgr_ptr = read_u64(kRacerMgrPtr);
    uint64_t racers_list = mgr_ptr ? read_u64(mgr_ptr + 0x10) : 0;
    int32_t racers_count = racers_list ? read_i32(racers_list + 0x0c) : 0;
    uint64_t racers_arr = racers_list ? read_u64(racers_list + 0x20) : 0;
    uint64_t player = racers_arr ? read_u64(racers_arr + 0) : 0;

    bool in_race = false;
    std::string track_path;
    int total_laps = 3;
    uint64_t lap_times_obj = 0;
    uint64_t times_data_ptr = 0;
    int times_capacity = 0;   // how many lap times the game's array can hold: never written past

    if (mgr_ptr && racers_count > 0 && player) {
        in_race = true;

        // Read track path dynamically by scanning manager pointers
        for (uintptr_t off = 0x100; off < 0x300; off += 8) {
            uint64_t track_str_ptr = read_u64(mgr_ptr + off);
            if (track_str_ptr > 0x100000000ull && track_str_ptr < 0x2000000000ull) {
                char tpath[128] = {};
                read_mem(track_str_ptr, tpath, sizeof(tpath) - 1);
                if (std::strstr(tpath, "octane/")) {
                    track_path = tpath;
                    break;
                }
            }
        }

        // Read total laps from CRacerManager
        int32_t laps_mgr = read_i32(mgr_ptr + 0x19C);
        if (s_game_tab.manual_laps_override > 0) {
            total_laps = s_game_tab.manual_laps_override;
        } else if (laps_mgr >= 1 && laps_mgr <= 20) {
            total_laps = laps_mgr;
        } else {
            total_laps = 3;
        }

        lap_times_obj = read_u64(player + 0x90);
        if (lap_times_obj) {
            times_data_ptr = read_u64(lap_times_obj + 0x20);
            times_capacity = read_i32(lap_times_obj + 0x08);
            if (times_capacity < 0 || times_capacity > 64) times_capacity = 0;
        }
    } else {
        s_game_tab.manual_laps_override = 0;
    }

    // Card 1: Match Status
    BeginCard("match_info");
    CardHeader("Live Match Status", nullptr, nullptr);
    if (!in_race) {
        ImVec2 cur = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cur.x + 8, cur.y + 11), 5, kMuted);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
        Text("Waiting for race to start...", kMuted, 16);
        ImGui::Dummy(ImVec2(0, 4));
        Wrapped("Enter an offline Single Race, Cup or Adventure race against the computer. Online races and Time Trial "
                "(its times go to the online leaderboards) are locked.");
    } else {
        ImVec2 cur = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cur.x + 8, cur.y + 11), 5, kOrange);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 20);
        Text("Race In Progress", kOrange, 16, g_bold);
        ImGui::Dummy(ImVec2(0, 8));

        std::string track_title = CleanTrackName(track_path);
        Text("MAP / TRACK", kMuted, 13);
        Text(track_title.c_str(), kText, 22, g_bold);
        if (!track_path.empty()) Text(track_path.c_str(), kMuted, 13);

        ImGui::Dummy(ImVec2(0, 8));
        Text("TOTAL LAPS", kMuted, 13);
        char laps_str[32];
        std::snprintf(laps_str, sizeof laps_str, "%d LAPS", total_laps);
        Text(laps_str, kOrange, 20, g_bold);

        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 2);
        if (ImGui::Button("-##lapdec", ImVec2(28, 28)) && total_laps > 1) {
            s_game_tab.manual_laps_override = total_laps - 1;
            s_game_tab.last_laps = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("+##lapinc", ImVec2(28, 28)) && total_laps < std::min(16, times_capacity)) {
            s_game_tab.manual_laps_override = total_laps + 1;
            s_game_tab.last_laps = 0;
        }
    }
    EndCard();

    // locked: online races (real players) and Time Trial (you alone: its times go to the online leaderboards)
    const bool online = in_race && OnlineRaceNow();
    const bool time_trial = in_race && racers_count < 2;
    if (in_race && (online || time_trial)) {
        BeginCard("game_locked");
        CardHeader("Locked", nullptr, nullptr);
        Text(online ? "This is an online race: GAME works in offline races only." : "Time Trial: its times go to the online leaderboards, so GAME is locked here.",
             kOrange, 16, g_bold);
        EndCard();
        return;
    }

    if (in_race) {
        if (s_game_tab.last_laps != total_laps || s_game_tab.last_track_path != track_path) {
            s_game_tab.last_laps = total_laps;
            s_game_tab.last_track_path = track_path;
            std::vector<uint32_t> live_ticks(total_laps, 0);
            if (times_data_ptr) read_mem(times_data_ptr, live_ticks.data(), std::min(total_laps, times_capacity) * 4);
            uint32_t prev_t = 0;
            for (int i = 0; i < total_laps; i++) {
                uint32_t cur_t = live_ticks[i];
                double lap_sec = (cur_t > prev_t) ? (double)(cur_t - prev_t) / 8192.0 : 40.0;
                std::string s = FormatLapSeconds(lap_sec);
                std::strncpy(s_game_tab.lap_bufs[i], s.c_str(), sizeof(s_game_tab.lap_bufs[i]) - 1);
                prev_t = cur_t;
            }
        }

        BeginCard("lap_inputs");
        CardHeader("Lap Times Configuration", "Type the time of each lap: M:SS.CC (0:44.27) or seconds (44.27)", nullptr);

        double total_calc_sec = 0.0;
        for (int i = 0; i < total_laps; i++) {
            ImGui::PushID(i);
            char label[32];
            std::snprintf(label, sizeof label, "LAP %d", i + 1);
            Text(label, kText, 16, g_bold);
            ImGui::SameLine(100);

            ImGui::PushStyleColor(ImGuiCol_FrameBg, kPanel2);
            ImGui::PushStyleColor(ImGuiCol_Border, kLine);
            ImGui::PushStyleColor(ImGuiCol_Text, kText);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(14, 8));
            ImGui::SetNextItemWidth(160);
            ImGui::InputText("##lap_input", s_game_tab.lap_bufs[i], sizeof(s_game_tab.lap_bufs[i]));
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);

            double parsed = ParseLapTimeString(s_game_tab.lap_bufs[i]);
            total_calc_sec += parsed;

            ImGui::SameLine();
            std::string fmt = FormatLapSeconds(parsed);
            Text(fmt.c_str(), kMuted, 15);

            ImGui::PopID();
            ImGui::Dummy(ImVec2(0, 4));
        }

        ImGui::Dummy(ImVec2(0, 8));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 8));

        Text("TOTAL RACE TIME (SUM)", kMuted, 13);
        std::string total_fmt = FormatLapSeconds(total_calc_sec);
        Text(total_fmt.c_str(), kOrange, 26, g_bold);

        ImGui::Dummy(ImVec2(0, 14));

        if (Button("END", true, ImVec2(-1, 50))) {
            // only what the game's lap-time array can hold is written (its size and capacity stay the game's); no
            // memory is added to the game, no rank is forced: the finish order comes from the times
            std::string err;
            if (!times_data_ptr || !lap_times_obj) err = "This race has no lap-time list yet: drive past the start line first.";
            else if (times_capacity < total_laps) err = "This race holds " + std::to_string(times_capacity) + " lap times: set at most that many laps.";
            bool ok = err.empty();
            if (ok) {
                std::vector<uint32_t> cum_ticks(total_laps, 0);
                double sum_sec = 0.0;
                for (int i = 0; i < total_laps; i++) {
                    sum_sec += ParseLapTimeString(s_game_tab.lap_bufs[i]);
                    cum_ticks[i] = static_cast<uint32_t>(std::round(sum_sec * 8192.0));
                }
                const uint32_t laps_val = total_laps;
                ok = write_mem(times_data_ptr, cum_ticks.data(), total_laps * sizeof(uint32_t)) &&
                     write_mem(lap_times_obj + 0x0c, &laps_val, 4) &&   // list size (within its capacity)
                     write_mem(player + 0x4c, &laps_val, 4) &&          // _currentLap
                     write_mem(player + 0x50, &laps_val, 4) &&          // _positionLap
                     write_mem(player + 0x68, &laps_val, 4);            // _completedRaceLaps
                const uint32_t final_tick = cum_ticks.back();
                const uint8_t zero = 0, finished = 1;
                ok = ok && write_mem(player + 0x6c, &final_tick, 4) &&   // _endOfRaceTime
                     write_mem(player + 0x70, &zero, 1) &&               // lap lock
                     write_mem(player + 0x8c, &finished, 1);              // _isFinished
                if (!ok) err = "Could not write into the game's memory.";
            }
            s_game_tab.status_msg = ok ? "Lap times written and the race marked finished." : err;
            s_game_tab.status_is_error = !ok;
            s_game_tab.status_msg_time = ImGui::GetTime();
        }

        if (!s_game_tab.status_msg.empty()) {
            ImGui::Dummy(ImVec2(0, 8));
            Text(s_game_tab.status_msg.c_str(), s_game_tab.status_is_error ? IM_COL32(255, 60, 60, 255) : kOrange, 16, g_bold);
        }

        EndCard();
    }
}

// ---------------- MISC page: gamepad view ----------------
// The pad drawn with the game's own button art (data/gamepad_sheet.png, taken from the game's font by
// tools/gamepad_sheet_build.py), lit by what you press. The same drawing runs inside the game (layer, gamepad_view.cpp)
// when "Show on the game's screen" is on: its options are written to misc.json.
struct MiscState {
    bool loaded = false;
    bool pad_on = false, sticks = true;
    float x = 0.02f, y = 0.62f, scale = 0.6f, opacity = 0.9f;
    DynTex sheet;
    bool sheet_tried = false;
};
MiscState g_misc;

void MiscSave() {
    json::Value root = json::Value::MakeObject();
    root["pad_on"] = json::Value::Bool(g_misc.pad_on);
    root["pad_sticks"] = json::Value::Bool(g_misc.sticks);
    root["pad_x"] = json::Value::Number(g_misc.x);
    root["pad_y"] = json::Value::Number(g_misc.y);
    root["pad_scale"] = json::Value::Number(g_misc.scale);
    root["pad_opacity"] = json::Value::Number(g_misc.opacity);
    WriteText(DefaultConfigPath().parent_path() / "misc.json", json::Serialize(root, 1) + "\n");
}

void MiscPage() {
    if (!g_misc.loaded) {
        g_misc.loaded = true;
        if (auto root = json::Parse(ReadText(DefaultConfigPath().parent_path() / "misc.json"))) {
            auto num = [&](const char* k, float& v) { if (const json::Value* x = root->Find(k); x && x->type == json::Value::Type::Number) v = float(x->n); };
            auto flag = [&](const char* k, bool& v) { if (const json::Value* x = root->Find(k); x && x->type == json::Value::Type::Bool) v = x->b; };
            flag("pad_on", g_misc.pad_on); flag("pad_sticks", g_misc.sticks);
            num("pad_x", g_misc.x); num("pad_y", g_misc.y); num("pad_scale", g_misc.scale); num("pad_opacity", g_misc.opacity);
        }
    }
    if (!g_misc.sheet_tried) {   // the game's button sheet, as a texture of the app
        g_misc.sheet_tried = true;
        UINT w = 0, h = 0;
        auto px = DecodePngFile(ExeDir() / L"gamepad_sheet.png", w, h);
        if (!px.empty() && g_misc.sheet.Create(w, h)) g_misc.sheet.Update(px);
    }
    PageTitle("MISC", "Extras: the gamepad view.");
    bool ch = false;
    BeginCard("padview");
    Text("Gamepad view", kOrange, 22, g_bold);
    Wrapped("The pad drawn with the game's own button art. Every button you press lights up. It reads the pad through XInput "
            "(Xbox pads, DS4Windows, Steam Input) or straight from a DualSense / DualShock 4.");
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::BeginGroup();
    Text("Show on the game's screen", kText, 16, g_bold);
    Text("The pad stays over the game while you play (also with F8 closed)", kMuted, 13);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##padon", &g_misc.pad_on)) ch = true;
    ImGui::Dummy(ImVec2(0, 6));
    ImGui::BeginGroup();
    Text("Show the sticks", kText, 16, g_bold);
    ImGui::EndGroup();
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    if (Switch("##padsticks", &g_misc.sticks)) ch = true;
    ImGui::Dummy(ImVec2(0, 6));
    ch |= Slider("Position across", &g_misc.x, 0.f, 1.f, "%.2f", 0);
    ch |= Slider("Position down", &g_misc.y, 0.f, 1.f, "%.2f", 0);
    ch |= Slider("Size", &g_misc.scale, 0.3f, 1.5f, "x%.2f", 0);
    ch |= Slider("Opacity", &g_misc.opacity, 0.2f, 1.f, "%.2f", 0);
    EndCard();
    if (ch) MiscSave();

    BeginCard("padprev");
    Text("Preview", kOrange, 22, g_bold);
    static pad::State st;
    const bool got = pad::Poll(st);
    Text(got ? "Pad detected: press its buttons." : "No pad detected. Plug it in (Xbox, DualSense, DualShock 4).", got ? kText : kMuted, 14);
    if (!pad::g_hid_note.empty()) Text(("Direct reading: " + pad::g_hid_note).c_str(), kMuted, 13);
    ImGui::Dummy(ImVec2(0, 4));
    const float scale = 0.9f;
    const ImVec2 size = pad::Size(scale);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + size.x, o.y + size.y), IM_COL32(40, 28, 10, 255), 12);   // the game's brown (reference image)
    pad::Sheet sh;
    sh.tex = g_misc.sheet.Id();
    sh.ok = g_misc.sheet.ok();
    for (int c = 0; c < pad::kCellCount; ++c) {
        sh.uv0[c] = ImVec2(float(pad::kCells[c][0]) / pad::kSheetW, float(pad::kCells[c][1]) / pad::kSheetH);
        sh.uv1[c] = ImVec2(float(pad::kCells[c][2]) / pad::kSheetW, float(pad::kCells[c][3]) / pad::kSheetH);
    }
    pad::Draw(dl, o, scale, 1.f, st, sh, true);
    ImGui::Dummy(size);
    if (!sh.ok) Text("gamepad_sheet.png is missing next to ASTRO_CTR.exe (run run.bat).", kMuted, 14);
    EndCard();
}

// ---------------- restore everything ----------------
// Puts every change ASTRO makes in the game back to the game's own: colours (nitro, paint, underglow, rainbow),
// wheels, effects, water, sky, lighting and the in-game optimize. The MODS page patches are left as they are (they
// change the gameplay, not the look, and the launcher applies them too). Every settings file is copied first to
// %APPDATA%\ASTRO-CTR\before_reset_<date time>\, so the old settings can be copied back.
std::string g_reset_note;
double g_reset_armed = -10;   // first click arms the button for 4 seconds, the second one resets

void ResetAll() {
    const fs::path dir = DefaultConfigPath().parent_path();
    char stamp[32];
    std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", std::localtime(&now));
    const fs::path keep = dir / ("before_reset_" + std::string(stamp));
    std::error_code ec;
    fs::create_directories(keep, ec);
    for (const char* f : {"config.json", "maps.json", "sky_edits.json", "wheel_edits.json", "fx_edits.json", "optimize.json"})
        if (fs::exists(dir / f, ec)) fs::copy_file(dir / f, keep / f, fs::copy_options::overwrite_existing, ec);
    // colours: every effect off (the layer restores the game's own colours of what it changed)
    g_sync.cfg.normal.enabled = false;
    g_sync.cfg.blue.enabled = false;
    g_sync.cfg.champion.enabled = false;
    g_sync.cfg.paint.enabled = false;
    g_sync.cfg.fx.rainbow.clear();
    g_sync.Touch();
    // wheels and effects: no edits (the layer writes the game's own pixels / colours back)
    g_wh.edits.clear();
    if (wheel::SaveEdits(g_wh.edits) && g_wh.apply) SetEvent(g_wh.apply);
    g_fx.edits.clear();
    if (fx::SaveEdits(g_fx.edits) && g_fx.apply) SetEvent(g_fx.apply);
    // water, sky, lighting
    g_maps.color_on = g_maps.opaque = g_maps.sky_on = false;
    g_maps.fog_on = g_maps.sun_on = g_maps.amb_on = g_maps.grade_on = g_maps.tint_on = false;
    g_maps.snow_on = g_maps.ice_on = g_maps.pad_on = g_maps.pad_body_on = g_maps.lava_on = g_maps.fire_on = g_maps.rock_on = false;
    g_maps.roo_ground_on = g_maps.roo_water_on = g_maps.roo_light_on = g_maps.wl_on = false;
    for (auto& [n, p] : g_char.parts) p.on = false;
    CharSave();
    for (auto& [k, m] : g_item_models) m.on = false;
    ItemsSave();
    g_mesh_cols.clear();
    MeshSave();
    WriteText(DefaultConfigPath().parent_path() / "items.json", "{}\n");
    g_item_rows_reload = true;
    MapsSave();
    PadSave();
    SkySave();
    SnowSave();
    LavaSave();
    RooSave();
    // in-game optimize: everything drawn again, the engine switches back on, full texture detail
    g_igo.skip.clear();
    g_igo.hide_shaders.clear();
    g_igo.reduce_shaders.clear();
    g_igo.effects_keep = 100;
    g_igo.texture_level = 0;
    g_igo.aniso_off = false;
    for (int* v : {&g_igo.vfx, &g_igo.bloom, &g_igo.motion_blur, &g_igo.depth_blur}) if (*v == 0) *v = 1;
    InGameOptSave();
    g_reset_note = "Everything is back to the game's own. Old settings kept in " + keep.filename().string() +
                   ". Texture detail returns at the next game start.";
}

// the button (Settings page and MAPS page)
void ResetAllButton() {
    const bool armed = ImGui::GetTime() - g_reset_armed < 4;
    if (Button(armed ? "Click again to restore everything" : "Restore everything", armed)) {
        if (armed) { ResetAll(); g_reset_armed = -10; }
        else g_reset_armed = ImGui::GetTime();
    }
    if (!g_reset_note.empty()) Text(g_reset_note.c_str(), kMuted, 13);
}

void SettingsPage() {
    PageTitle("Settings", "In-game menu and files.");
    UiSettings& ui = g_sync.cfg.ui;
    bool ch = false;
    BeginCard("menu");
    CardHeader("In-game menu (F8)", nullptr, nullptr);
    ch |= Slider("Menu size", &ui.scale, 0.75f, 2.0f, "%.2f");
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 2);
    Text("Debug log", kText, 16);
    ImGui::SameLine(ImGui::GetContentRegionMax().x - 42);
    ch |= Switch("##dbg", &ui.debug_logging);
    EndCard();
    BeginCard("reset");
    CardHeader("Restore everything", nullptr, nullptr);
    Wrapped("Puts every change ASTRO makes back to the game's own: colours, nitro, kart paint, underglow, wheels, effects, "
            "water, sky, lighting and the in-game optimize. MODS patches stay as they are. Your settings are kept in a backup folder.");
    ImGui::Dummy(ImVec2(0, 4));
    ResetAllButton();
    EndCard();
    BeginCard("files");
    CardHeader("Files", nullptr, nullptr);
    const fs::path dir = DefaultConfigPath().parent_path();
    if (Button("Open ASTRO folder")) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine();
    if (Button("Open log")) ShellExecuteW(nullptr, L"open", (dir / L"astro_ctr.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::Dummy(ImVec2(0, 6));
    Text(DefaultConfigPath().string().c_str(), kMuted, 14);
    EndCard();
    if (ch) g_sync.Touch();
}

void AboutPage() {
    PageTitle("About", "ASTRO CTR " ASTRO_VERSION);
    BeginCard("about");
    if (g_word.id) ImGui::Image(g_word.id, ImVec2(316, 316 * g_word.h / g_word.w));
    ImGui::Dummy(ImVec2(0, 12));
    // ImGui does not shape Arabic: the name is given as joined presentation forms in visual order
    Text("Created by  ABU 3RB  |  \xef\xba\x8f\xef\xba\xae\xef\xbb\x8b\x20\xef\xbb\xae\xef\xba\x91\xef\xba\x8d", kText, 18, g_bold);
    ImGui::Dummy(ImVec2(0, 6));
    Text("TikTok   _tmo", kMuted, 16);
    Text("Instagram   7.u8", kMuted, 16);
    Text("Discord   7_ug", kMuted, 16);
    ImGui::Dummy(ImVec2(0, 10));
    Wrapped("Visual only: ASTRO changes colours and nothing else. No game files are modified, and nothing is injected: "
            "the game loads ASTRO as a Vulkan layer while this app is open. Unofficial fan project, not affiliated with "
            "Activision, Beenox or Toys for Bob.");
    EndCard();
}

// ---------------- frame ----------------
#ifndef ASTRO_IN_GAME
// custom title bar: drag area, minimise, close
void TitleBar(float w) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = 40;
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::InvisibleButton("##drag", ImVec2(w - 96, h));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 2)) {
        ReleaseCapture();
        SendMessageW(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        ImGui::GetIO().AddMouseButtonEvent(0, false);
    }
    auto icon_btn = [&](const char* id, float x, bool close) {
        ImGui::SetCursorPos(ImVec2(x, 0));
        bool r = ImGui::InvisibleButton(id, ImVec2(48, h));
        ImVec2 p = ImGui::GetItemRectMin();
        if (ImGui::IsItemHovered()) dl->AddRectFilled(p, ImGui::GetItemRectMax(), close ? kOrange : kPanel2);
        const ImU32 col = ImGui::IsItemHovered() && close ? kBg : kMuted;
        const ImVec2 c(p.x + 24, p.y + h / 2);
        if (close) {
            dl->AddLine(ImVec2(c.x - 5, c.y - 5), ImVec2(c.x + 5, c.y + 5), col, 1.5f);
            dl->AddLine(ImVec2(c.x - 5, c.y + 5), ImVec2(c.x + 5, c.y - 5), col, 1.5f);
        } else dl->AddLine(ImVec2(c.x - 5, c.y), ImVec2(c.x + 5, c.y), col, 1.5f);
        return r;
    };
    if (icon_btn("##min", w - 96, false)) ShowWindow(g_hwnd, SW_MINIMIZE);
    if (icon_btn("##close", w - 48, true)) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}
#endif

void Toast() {
    const double age = ImGui::GetTime() - g_sync.note_at;
    if (age > 3 || g_sync.note.empty()) return;
    const float a = float(std::min(1.0, (3 - age) * 3));
    ImGui::PushFont(g_font, 15);
    const ImVec2 ts = ImGui::CalcTextSize(g_sync.note.c_str());
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const ImVec2 p((ds.x + 230 - ts.x) / 2 - 18, ds.y - 64);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + ts.x + 36, p.y + ts.y + 20), IM_COL32(255, 90, 31, int(255 * a)), 10);
    dl->AddText(ImVec2(p.x + 18, p.y + 10), IM_COL32(13, 13, 15, int(255 * a)), g_sync.note.c_str());
    ImGui::PopFont();
}

#ifndef ASTRO_IN_GAME
// the "OPEN CRASHNF" screen shown until the game starts
void WaitScreen(ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float t = float(ImGui::GetTime());
    const ImVec2 c(size.x / 2, size.y / 2 - 40);
    // pulsing rings behind the mark
    for (int i = 0; i < 3; ++i) {
        const float k = std::fmod(t * 0.45f + i / 3.f, 1.f);
        dl->AddCircle(c, 70 + k * 120, IM_COL32(255, 90, 31, int(70 * (1 - k))), 96, 1.5f);
    }
    if (g_mark.id) {
        const float mw = 150, mh = mw * g_mark.h / g_mark.w;
        dl->AddImage(g_mark.id, ImVec2(c.x - mw / 2, c.y - mh / 2), ImVec2(c.x + mw / 2, c.y + mh / 2));
    }
    const char* hero = "OPEN CRASHNF";
    ImGui::PushFont(g_bold, 46);
    ImVec2 hs = ImGui::CalcTextSize(hero);
    const float spacing = 6;
    hs.x += spacing * (std::strlen(hero) - 1);
    float x = (size.x - hs.x) / 2;
    const float y = c.y + 150;
    for (const char* ch = hero; *ch; ++ch) {
        char s[2] = {*ch, 0};
        dl->AddText(ImVec2(x, y), kOrange, s);
        x += ImGui::CalcTextSize(s).x + spacing;
    }
    ImGui::PopFont();
    const char* sub;
    switch (g_game.state) {
    case Game::AlreadyRunning: sub = "The game is already running without ASTRO. Close it and open it again."; break;
    case Game::Starting: sub = "Game found. Loading ASTRO..."; break;
    case Game::NoLayer: sub = "The game started without ASTRO. Close it and open it again."; break;
    default: sub = "Start Crash Team Racing Nitro-Fueled. ASTRO opens here automatically."; break;
    }
    ImGui::PushFont(g_font, 17);
    ImVec2 ss = ImGui::CalcTextSize(sub);
    dl->AddText(ImVec2((size.x - ss.x) / 2, y + 66), kMuted, sub);
    // waiting dots
    for (int i = 0; i < 3; ++i) {
        const float k = 0.5f + 0.5f * std::sin(t * 4 - i * 0.8f);
        dl->AddCircleFilled(ImVec2(size.x / 2 - 18 + i * 18, y + 108), 4, IM_COL32(255, 90, 31, int(60 + 195 * k)));
    }
    // skip link
    const char* skip = "Edit colours without the game  >";
    ImVec2 ks = ImGui::CalcTextSize(skip);
    ImGui::SetCursorPos(ImVec2((size.x - ks.x) / 2, size.y - 86));
    if (ImGui::InvisibleButton("##skip", ks)) g_main = true;
    dl->AddText(ImGui::GetItemRectMin(), ImGui::IsItemHovered() ? kOrange : kMuted, skip);
    ImGui::PopFont();
    ImGui::PushFont(g_font, 14);
    const char* foot = "ASTRO CTR " ASTRO_VERSION "   -   by ABU 3RB";
    ImVec2 fs = ImGui::CalcTextSize(foot);
    dl->AddText(ImVec2((size.x - fs.x) / 2, size.y - 40), IM_COL32(240, 240, 242, 70), foot);
    ImGui::PopFont();
}
#endif

void NavItem(const char* label, Page p) {
    const bool on = g_page == p;
    ImGui::PushID(label);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::InvisibleButton("nav", ImVec2(w, 38))) g_page = p;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (on) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + 38), kOrangeDim, 10);
        dl->AddRectFilled(ImVec2(pos.x, pos.y + 9), ImVec2(pos.x + 3, pos.y + 29), kOrange, 2);
    } else if (ImGui::IsItemHovered()) dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + 38), kPanel2, 10);
    ImGui::PushFont(g_font, 17);
    dl->AddText(ImVec2(pos.x + 18, pos.y + 9), on ? kOrange : kText, label);
    ImGui::PopFont();
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0, 2));
}

void MainScreen(ImVec2 size) {
    const float side = 236;
    const ImVec2 o = ImGui::GetWindowPos();   // (0, 0) in the app; the F8 window's corner in the game
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + side, o.y + size.y), kPanel);
    dl->AddLine(ImVec2(o.x + side, o.y), ImVec2(o.x + side, o.y + size.y), kLine);
    // sidebar
    ImGui::SetCursorPos(ImVec2(22, 54));
    if (g_word.id) ImGui::Image(g_word.id, ImVec2(side - 52, (side - 52) * g_word.h / g_word.w));
    else Text("ASTRO CTR", kOrange, 30, g_bold);   // the game has no logo images
    ImGui::SetCursorPos(ImVec2(14, 128));
    ImGui::BeginChild("nav", ImVec2(side - 28, size.y - 230), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    NavItem("Home", Page::Home);
#ifdef ASTRO_IN_GAME
    NavItem("GAME", Page::Game);
    NavItem("PLAYERS", Page::Players);
    NavItem("Nitro flame", Page::Nitro);
    NavItem("Kart paint", Page::Paint);
    NavItem("Underglow", Page::Glow);
    NavItem("Wheel Editor", Page::Wheel);
    NavItem("MODS", Page::Mods);
    NavItem("MAPS", Page::Maps);
    NavItem("ITEMS", Page::Items);
    NavItem("MISC", Page::Misc);
    NavItem("Effects", Page::Effects);
    NavItem("Presets", Page::Presets);
    NavItem("DLL injector", Page::Inject);
    NavItem("Settings", Page::Settings);
    NavItem("Diagnostics", Page::Diag);
#else
    NavItem("MODS", Page::Mods);
#endif
    NavItem("About", Page::About);
    ImGui::EndChild();
    // connection pill
    const bool con = g_game.Connected();
    const char* st = con ? "Game connected" : "Waiting for game";
    ImVec2 p(o.x + 18, o.y + size.y - 66);
    dl->AddRectFilled(p, ImVec2(side - 18, p.y + 40), con ? kOrangeDim : kPanel2, 20);
    dl->AddCircleFilled(ImVec2(p.x + 20, p.y + 20), 5, con ? kOrange : kMuted);
    ImGui::PushFont(g_font, 15);
    dl->AddText(ImVec2(p.x + 36, p.y + 11), con ? kOrange : kMuted, st);
    ImGui::PopFont();
    // content
    ImGui::SetCursorPos(ImVec2(side + 1, 40));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36, 14));
    ImGui::BeginChild("content", ImVec2(size.x - side - 1, size.y - 40), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PushFont(g_font, 16);
    switch (g_page) {
    case Page::Home: HomePage(); break;
    case Page::Game: GamePage(); break;
    case Page::Nitro: NitroPage(); break;
    case Page::Paint: PaintPage(); break;
    case Page::Glow: GlowPage(); break;
    case Page::Presets: PresetsPage(); break;
    case Page::Settings: SettingsPage(); break;
    case Page::Wheel: WheelPage(); break;
    case Page::Mods: ModsPage(); break;
    case Page::Maps: MapsPage(); break;
    case Page::Effects: EffectsPage(); break;
    case Page::Items: EffectsPage(true); break;
    case Page::Misc: MiscPage(); break;
    case Page::Inject: InjectPage(); break;
    case Page::About: AboutPage(); break;
#ifdef ASTRO_IN_GAME
    case Page::Players: PageTitle("PLAYERS", "Every kart marked on the screen with its live data."); PlayersEsp_Tab(); break;
    case Page::Diag: PageTitle("Diagnostics", "The layer inside the game."); Menu_DiagPage(); break;
#else
    default: break;
#endif
    }
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 20));
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void Style() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameRounding = 8;
    s.PopupRounding = 10;
    s.GrabRounding = 8;
    s.ScrollbarRounding = 8;
    s.ScrollbarSize = 10;
    s.GrabMinSize = 14;
    s.FramePadding = ImVec2(12, 7);
    s.ItemSpacing = ImVec2(10, 8);
    s.PopupBorderSize = 1;
    ImVec4* c = s.Colors;
    auto v = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    c[ImGuiCol_WindowBg] = v(kBg);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = v(kPanel2);
    c[ImGuiCol_Border] = v(kLine);
    c[ImGuiCol_Text] = v(kText);
    c[ImGuiCol_TextDisabled] = v(kMuted);
    c[ImGuiCol_Button] = v(kLine);
    c[ImGuiCol_ButtonHovered] = kOrangeV;
    c[ImGuiCol_ButtonActive] = kOrangeV;
    c[ImGuiCol_FrameBg] = v(kPanel2);
    c[ImGuiCol_FrameBgHovered] = v(kLine);
    c[ImGuiCol_FrameBgActive] = v(kLine);
    c[ImGuiCol_SliderGrab] = kOrangeV;
    c[ImGuiCol_SliderGrabActive] = kOrangeV;
    c[ImGuiCol_CheckMark] = kOrangeV;
    c[ImGuiCol_Header] = v(kOrangeDim);
    c[ImGuiCol_HeaderHovered] = v(kLine);
    c[ImGuiCol_HeaderActive] = v(kOrangeDim);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = v(kLine);
    c[ImGuiCol_ScrollbarGrabHovered] = kOrangeV;
    c[ImGuiCol_ScrollbarGrabActive] = kOrangeV;
    c[ImGuiCol_TextSelectedBg] = v(kOrangeDim);
    c[ImGuiCol_NavCursor] = kOrangeV;
    c[ImGuiCol_TitleBg] = v(kPanel);          // the F8 window in the game (the app has no title bar)
    c[ImGuiCol_TitleBgActive] = v(kPanel2);
    c[ImGuiCol_TitleBgCollapsed] = v(kPanel);
    c[ImGuiCol_ResizeGrip] = v(kLine);
    c[ImGuiCol_ResizeGripHovered] = kOrangeV;
    c[ImGuiCol_ResizeGripActive] = kOrangeV;
}

#ifndef ASTRO_IN_GAME
LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) { g_resize_w = LOWORD(lp); g_resize_h = HIWORD(lp); }
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        mm->ptMinTrackSize = {int(900 * g_dpi), int(600 * g_dpi)};
        return 0;
    }
    case WM_NCHITTEST: {   // borderless window: keep the edges resizable
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const int b = 6;
        const bool l = pt.x < b, r = pt.x >= rc.right - b, t = pt.y < b, bo = pt.y >= rc.bottom - b;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (bo && l) return HTBOTTOMLEFT;
        if (bo && r) return HTBOTTOMRIGHT;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
        if (t) return HTTOP;
        if (bo) return HTBOTTOM;
        return HTCLIENT;
    }
    case WM_NCCALCSIZE:
        if (wp) return 0;   // no system frame
        break;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int Fail(const wchar_t* text) {
    MessageBoxW(nullptr, text, L"ASTRO CTR", MB_ICONERROR);
    return 2;
}
#endif

}  // namespace

#ifdef ASTRO_IN_GAME
// ---------------- in the game: the F8 menu ----------------
namespace astro {

// once, right after the layer creates its ImGui context: the app's look and fonts
void AstroUi_Setup() {
    Style();
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    const fs::path fonts = fs::path(windir) / L"Fonts";
    ImGuiIO& io = ImGui::GetIO();
    g_font = io.Fonts->AddFontFromFileTTF((fonts / L"bahnschrift.ttf").string().c_str());
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    g_bold = io.Fonts->AddFontFromFileTTF((fonts / L"segoeuib.ttf").string().c_str());
    if (!g_bold) g_bold = g_font;
    io.FontDefault = g_font;
}

// every frame while the F8 menu is open (inside NewFrame/Render)
void AstroUi_Draw(bool* open) {
    static bool started = false;
    if (!started) {
        started = true;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // WIC (PNG previews); an earlier mode of this thread is fine too
        g_sync.Load();
        g_sync.pushed = CreateEventW(nullptr, FALSE, FALSE, kConfigPushedEvent);   // the layer's config thread waits on it
        g_main = true;
    }
    g_sync.Tick();
    FxTick();
    WheelTick();
    PerfTick();
    ImGui::GetStyle().FontScaleMain = std::clamp(g_sync.cfg.ui.scale, 0.75f, 2.0f);   // Settings > Menu size
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(1180.f, ds.x - 40), std::min(820.f, ds.y - 40)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(900, 560), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    const bool shown = ImGui::Begin("ASTRO CTR " ASTRO_VERSION "   (F8)###astro", open,
                                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (shown) MainScreen(ImGui::GetWindowSize());
    ImGui::End();
    Toast();
}

// the menu was closed: write what is still waiting (the ticks only run while it is open)
void AstroUi_Flush() {
    if (g_sync.dirty) {
        std::string err;
        if (SaveConfig(DefaultConfigPath(), g_sync.cfg, &err)) { g_sync.dirty = false; if (g_sync.pushed) SetEvent(g_sync.pushed); }
    }
    if (g_fx.dirty) { g_fx.dirty = false; if (fx::SaveEdits(g_fx.edits) && g_fx.apply) SetEvent(g_fx.apply); }
    if (g_wh.dirty) { g_wh.dirty = false; if (wheel::SaveEdits(g_wh.edits) && g_wh.apply) SetEvent(g_wh.apply); }
}

}  // namespace astro
#else
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    const fs::path dir = ExeDir();
    // --preview: the interface only (no layer registration, no single-instance check), for screenshots and testing
    const bool preview = cmd && wcsstr(cmd, L"--preview");
    g_manifest = (dir / L"VkLayer_ASTRO_ctr_implicit.json").wstring();
    if (!fs::exists(dir / L"astro_ctr.dll") || !fs::exists(g_manifest))
        return Fail(L"astro_ctr.dll or VkLayer_ASTRO_ctr_implicit.json is missing next to ASTRO_CTR.exe.\nKeep the three files together.");
    HANDLE watcher = preview ? nullptr : CreateMutexW(nullptr, FALSE, L"Local\\ASTRO_CTR_Watcher");
    if (!preview && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"ASTRO_CTR_App", nullptr)) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); return 1; }
        // another ASTRO (e.g. the older console ASTRO_CTR_Launcher.exe) holds the watcher: say so instead of vanishing
        MessageBoxW(nullptr, L"Another ASTRO window is already open (for example the older ASTRO_CTR_Launcher.exe).\n"
                             L"Close it, then open ASTRO_CTR.exe again.", L"ASTRO CTR", MB_ICONINFORMATION);
        return 1;
    }
    if (!preview && !Register()) return Fail(L"Could not register the ASTRO layer for this Windows user.");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    g_dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc{sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)), LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
                   nullptr, nullptr, L"ASTRO_CTR_App", LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP))};
    RegisterClassExW(&wc);
    const int w = int(1100 * g_dpi), h = int(720 * g_dpi);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"ASTRO CTR", WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU,
                             (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h,
                             nullptr, nullptr, inst, nullptr);
    const DWORD dark = 1, round = 2;   // DWMWA_USE_IMMERSIVE_DARK_MODE, DWMWCP_ROUND
    DwmSetWindowAttribute(g_hwnd, 20, &dark, sizeof dark);
    DwmSetWindowAttribute(g_hwnd, 33, &round, sizeof round);
    const MARGINS shadow{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(g_hwnd, &shadow);
    if (!CreateDevice(g_hwnd)) { Unregister(); return Fail(L"Could not start Direct3D 11."); }
    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    Style();
    ImGui::GetStyle().ScaleAllSizes(g_dpi);
    ImGui::GetStyle().FontScaleDpi = g_dpi;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    const fs::path fonts = fs::path(windir) / L"Fonts";
    g_font = io.Fonts->AddFontFromFileTTF((fonts / L"bahnschrift.ttf").string().c_str());
    if (!g_font) g_font = io.Fonts->AddFontDefault();
    // Arabic glyphs for the credits line come from Segoe UI Bold
    g_bold = io.Fonts->AddFontFromFileTTF((fonts / L"segoeuib.ttf").string().c_str());
    if (!g_bold) g_bold = g_font;
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    g_mark = LoadPngResource(IDR_MARK);
    g_word = LoadPngResource(IDR_WORDMARK);

    g_game.Init();
    g_sync.Load();
    if (preview && cmd) {   // --preview <page>: open that page directly (screenshots)
        const std::pair<const wchar_t*, Page> pages[] = {{L"home", Page::Home}, {L"game", Page::Game}, {L"nitro", Page::Nitro}, {L"paint", Page::Paint},
                                                         {L"glow", Page::Glow}, {L"presets", Page::Presets}, {L"wheel", Page::Wheel}, {L"mods", Page::Mods}, {L"maps", Page::Maps}, {L"effects", Page::Effects}, {L"inject", Page::Inject}, {L"settings", Page::Settings}, {L"about", Page::About}};
        for (auto& [n, pg] : pages) if (wcsstr(cmd, n)) { g_main = true; g_page = pg; }
        g_open_first_picker = wcsstr(cmd, L"pick") != nullptr;
    }
    g_sync.pushed = CreateEventW(nullptr, FALSE, FALSE, kConfigPushedEvent);

    for (bool done = false; !done;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(g_hwnd)) { Sleep(50); g_game.Poll(dir); AutoInject(dir); continue; }
        if (g_resize_w) {
            ReleaseRtv();
            g_swap->ResizeBuffers(0, g_resize_w, g_resize_h, DXGI_FORMAT_UNKNOWN, 0);
            g_resize_w = g_resize_h = 0;
            CreateRtv();
        }
        // the game is connected (also when the window was minimized while it connected, which skipped the switch)
        if (g_game.Poll(dir) || (!g_main && g_game.Connected())) {   // open the full app
            g_main = true;
            g_page = Page::Home;
            FLASHWINFO fi{sizeof fi, g_hwnd, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
            FlashWindowEx(&fi);
        }
        AutoInject(dir);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        g_sync.Tick();
        FxTick();
        WheelTick();
        ModsTick();
        CrashLogsTick();
        PerfTick();
        const ImVec2 size = io.DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                            ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (g_main) MainScreen(size);
        else WaitScreen(size);
        TitleBar(size.x);
        ImGui::End();
        Toast();
        ImGui::Render();

        const float clear[4] = {13 / 255.f, 13 / 255.f, 15 / 255.f, 1};
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    // save pending edits before leaving
    if (g_sync.dirty) SaveConfig(DefaultConfigPath(), g_sync.cfg);
    if (!preview) Unregister();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    DestroyDevice();
    DestroyWindow(g_hwnd);
    if (watcher) CloseHandle(watcher);
    CoUninitialize();
    return 0;
}
#endif
