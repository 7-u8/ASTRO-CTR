#include "gamepad_view.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"

#include "../config/config.h"
#include "../config/json.h"
#include "../core/gamepad_draw.h"
#include "../diagnostics/log.h"

namespace astro {
namespace {

struct Opt { bool on = false, sticks = true; float x = 0.02f, y = 0.62f, scale = 0.6f, opacity = 0.9f; };
Opt g_opt;
ULONGLONG g_read_at = 0;
ImFontAtlasRectId g_ids[pad::kCellCount];
bool g_ready = false;

std::filesystem::path DllDir() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&DllDir), &m);
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

void ReadOptions() {
    const ULONGLONG now = GetTickCount64();
    if (g_read_at && now - g_read_at < 1000) return;
    g_read_at = now;
    Opt o;
    if (auto root = json::Parse(ReadAll(DefaultConfigPath().parent_path() / "misc.json"))) {
        auto num = [&](const char* k, float& v) { if (const json::Value* x = root->Find(k); x && x->type == json::Value::Type::Number) v = float(x->n); };
        auto flag = [&](const char* k, bool& v) { if (const json::Value* x = root->Find(k); x && x->type == json::Value::Type::Bool) v = x->b; };
        flag("pad_on", o.on); flag("pad_sticks", o.sticks);
        num("pad_x", o.x); num("pad_y", o.y); num("pad_scale", o.scale); num("pad_opacity", o.opacity);
    }
    g_opt = o;
}

}  // namespace

void GamepadView_Setup() {
    if (g_ready) return;
    const std::string rgba = ReadAll(DllDir() / "gamepad_sheet.rgba");
    if (rgba.size() != size_t(pad::kSheetW) * pad::kSheetH * 4) {
        Logger::Instance().Warn("gamepad view: gamepad_sheet.rgba is missing next to the DLL (run run.bat)");
        return;
    }
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    for (int c = 0; c < pad::kCellCount; ++c) {
        const int x0 = pad::kCells[c][0], y0 = pad::kCells[c][1], w = pad::kCells[c][2] - x0, h = pad::kCells[c][3] - y0;
        ImFontAtlasRect r;
        g_ids[c] = atlas->AddCustomRect(w, h, &r);
        ImTextureData* tex = atlas->TexData;
        if (g_ids[c] == ImFontAtlasRectId_Invalid || !tex || !tex->Pixels || tex->Format != ImTextureFormat_RGBA32) {
            Logger::Instance().Warn("gamepad view: the font atlas did not take the button art");
            return;
        }
        for (int row = 0; row < h; ++row)
            std::memcpy(tex->GetPixelsAt(r.x, r.y + row), rgba.data() + (size_t(y0 + row) * pad::kSheetW + x0) * 4, size_t(w) * 4);
        ImFontAtlasTextureBlockQueueUpload(atlas, tex, r.x, r.y, r.w, r.h);
    }
    atlas->TexPixelsUseColors = true;
    g_ready = true;
    Logger::Instance().Info("gamepad view: the game's button art is in the overlay atlas");
}

bool GamepadView_Enabled() {
    ReadOptions();
    return g_opt.on && g_ready;
}

void GamepadView_Draw() {
    ImGuiIO& io = ImGui::GetIO();
    pad::Sheet sh;
    sh.tex = io.Fonts->TexRef;
    sh.ok = true;
    for (int c = 0; c < pad::kCellCount; ++c) {
        ImFontAtlasRect r;
        if (!io.Fonts->GetCustomRect(g_ids[c], &r)) { sh.ok = false; break; }
        sh.uv0[c] = r.uv0;
        sh.uv1[c] = r.uv1;
    }
    pad::State st;
    pad::Poll(st);
    const ImVec2 size = pad::Size(g_opt.scale);
    const ImVec2 o(g_opt.x * (io.DisplaySize.x - size.x), g_opt.y * (io.DisplaySize.y - size.y));
    pad::Draw(ImGui::GetForegroundDrawList(), o, g_opt.scale, g_opt.opacity, st, sh, g_opt.sticks);
}

}  // namespace astro
