#pragma once
// Gamepad view (MISC page, and in the game): the pad drawn with the game's own button art (the image "button" of its
// font fonts/GeekSpeak.igz, extracted by tools/gamepad_sheet_build.py) and lit up by what you press.
// Header only: used by the ASTRO app (preview) and by the layer (on the game's screen). Needs imgui.h.
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"

namespace astro::pad {

enum Cell { kL1, kR1, kL2, kR2, kCross, kCircle, kTriangle, kSquare, kLStick, kRStick, kCellCount };

// where each button is on the 512x263 sheet (x0, y0, x1, y1); same as data/gamepad_cells.json
inline constexpr int kCells[kCellCount][4] = {
    {0, 0, 147, 65},      {147, 0, 294, 65},   {263, 170, 364, 263}, {364, 170, 465, 263}, {294, 0, 379, 85},
    {379, 0, 472, 85},    {0, 85, 93, 170},    {93, 85, 186, 170},   {272, 85, 360, 170},  {362, 85, 450, 170}};
inline constexpr int kSheetW = 512, kSheetH = 263;

// what is pressed (XInput's layout: A = Cross, B = Circle, X = Square, Y = Triangle, LB/RB = L1/R1, LT/RT = L2/R2)
struct State {
    bool connected = false;
    bool cross = false, circle = false, square = false, triangle = false;
    bool l1 = false, r1 = false, l3 = false, r3 = false;
    bool up = false, down = false, left = false, right = false;
    bool start = false, back = false;
    float l2 = 0, r2 = 0;            // 0..1
    float lx = 0, ly = 0, rx = 0, ry = 0;   // -1..1 (up = +)
};

inline bool Neutral(const State& s) {
    return !(s.cross || s.circle || s.square || s.triangle || s.l1 || s.r1 || s.l3 || s.r3 || s.up || s.down || s.left || s.right || s.start || s.back ||
             s.l2 > 0.05f || s.r2 > 0.05f || std::fabs(s.lx) > 0.2f || std::fabs(s.ly) > 0.2f || std::fabs(s.rx) > 0.2f || std::fabs(s.ry) > 0.2f);
}

// ---- DualSense / DualSense Edge / DualShock 4 read straight from their HID reports (seen 2026-11-02: the user's pad is
// a DualSense Edge on USB and XInput sees nothing). The game's own host reads the same device; every open handle has
// its own report queue, so this does not take anything from the game.
struct HidPad {
    State st;
    ULONGLONG active_at = 0;   // last time something was pressed / moved
    bool up = false;
};
inline std::mutex& HidMutex() { static std::mutex m; return m; }
inline HidPad g_hid[4];
inline std::atomic<bool> g_hid_started{false};
inline std::string g_hid_note;   // what the reader says (shown in the MISC page)

inline void ParseHat(State& s, int hat) {
    s.up = hat == 7 || hat == 0 || hat == 1;
    s.right = hat == 1 || hat == 2 || hat == 3;
    s.down = hat == 3 || hat == 4 || hat == 5;
    s.left = hat == 5 || hat == 6 || hat == 7;
}
// ds: DualSense (true) or DualShock 4; returns false for reports it does not know
inline bool ParseReport(const unsigned char* b, size_t n, bool ds, State& s) {
    s = State{};
    const unsigned char id = b[0];
    int base = 0;
    if (ds) base = id == 0x01 ? 1 : id == 0x31 ? 2 : 0;
    else base = id == 0x01 ? 1 : id == 0x11 ? 3 : 0;
    if (!base || n < size_t(base) + 10) return false;
    const unsigned char* d = b + base;
    auto axis = [](unsigned char v) { const float f = (float(v) - 128.f) / 127.f; return std::fabs(f) < 0.10f ? 0.f : std::clamp(f, -1.f, 1.f); };
    s.connected = true;
    s.lx = axis(d[0]); s.ly = -axis(d[1]); s.rx = axis(d[2]); s.ry = -axis(d[3]);
    unsigned char face, shoulders;
    if (ds) {   // d[4] L2, d[5] R2, d[6] counter, d[7] hat + face, d[8] shoulders
        s.l2 = d[4] / 255.f; s.r2 = d[5] / 255.f; face = d[7]; shoulders = d[8];
    } else {    // DualShock 4: d[4] hat + face, d[5] shoulders, d[6] ps/touch, d[7] L2, d[8] R2
        face = d[4]; shoulders = d[5]; s.l2 = d[7] / 255.f; s.r2 = d[8] / 255.f;
    }
    ParseHat(s, face & 0x0F);
    s.square = face & 0x10; s.cross = face & 0x20; s.circle = face & 0x40; s.triangle = face & 0x80;
    s.l1 = shoulders & 0x01; s.r1 = shoulders & 0x02;
    if (s.l2 < 0.05f && (shoulders & 0x04)) s.l2 = 1.f;
    if (s.r2 < 0.05f && (shoulders & 0x08)) s.r2 = 1.f;
    s.back = shoulders & 0x10; s.start = shoulders & 0x20; s.l3 = shoulders & 0x40; s.r3 = shoulders & 0x80;
    return true;
}

inline void HidReader(int slot, std::wstring path, bool ds) {
    for (;;) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            { std::lock_guard<std::mutex> l(HidMutex()); g_hid[slot].up = false; g_hid[slot].st = State{}; }
            Sleep(1500);
            continue;
        }
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
        std::vector<unsigned char> buf(std::max<size_t>(caps.InputReportByteLength, 80));
        for (;;) {
            DWORD got = 0;
            if (!ReadFile(h, buf.data(), DWORD(std::max<size_t>(caps.InputReportByteLength, 64)), &got, nullptr) || got == 0) break;
            State st;
            if (!ParseReport(buf.data(), got, ds, st)) continue;
            std::lock_guard<std::mutex> l(HidMutex());
            // "in use" = something CHANGED (a virtual pad or a drifting stick can hold a constant non-neutral state)
            const State& o = g_hid[slot].st;
            const bool changed = !g_hid[slot].up || o.cross != st.cross || o.circle != st.circle || o.square != st.square || o.triangle != st.triangle ||
                                 o.l1 != st.l1 || o.r1 != st.r1 || o.l3 != st.l3 || o.r3 != st.r3 || o.up != st.up || o.down != st.down ||
                                 o.left != st.left || o.right != st.right || std::fabs(o.l2 - st.l2) > 0.05f || std::fabs(o.r2 - st.r2) > 0.05f ||
                                 std::fabs(o.lx - st.lx) > 0.12f || std::fabs(o.ly - st.ly) > 0.12f || std::fabs(o.rx - st.rx) > 0.12f || std::fabs(o.ry - st.ry) > 0.12f;
            if (changed && g_hid[slot].up) g_hid[slot].active_at = GetTickCount64();
            g_hid[slot].st = st;
            g_hid[slot].up = true;
        }
        CloseHandle(h);
        { std::lock_guard<std::mutex> l(HidMutex()); g_hid[slot].up = false; g_hid[slot].st = State{}; }
        Sleep(500);
    }
}

// starts one reader thread per Sony pad found (once)
inline void StartHid() {
    if (g_hid_started.exchange(true)) return;
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) { g_hid_note = "the HID list could not be read"; return; }
    int slot = 0, opened = 0, found = 0;
    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA di{};
        di.cbSize = sizeof di;
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, i, &di)) break;
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &di, nullptr, 0, &need, nullptr);
        std::vector<unsigned char> raw(need);
        auto* det = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(raw.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &di, det, need, nullptr, nullptr)) continue;
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);   // attributes only
        if (h == INVALID_HANDLE_VALUE) { const std::wstring p = det->DevicePath; if (p.find(L"vid_054c") != std::wstring::npos) ++found; continue; }
        HIDD_ATTRIBUTES at{};
        at.Size = sizeof at;
        const bool ok = HidD_GetAttributes(h, &at);
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (ok && HidD_GetPreparsedData(h, &pp)) { HidP_GetCaps(pp, &caps); HidD_FreePreparsedData(pp); }
        CloseHandle(h);
        if (!ok || at.VendorID != 0x054C || caps.UsagePage != 1 || caps.Usage != 5) continue;
        const bool ds = at.ProductID == 0x0CE6 || at.ProductID == 0x0DF2;
        const bool ds4 = at.ProductID == 0x05C4 || at.ProductID == 0x09CC || at.ProductID == 0x0BA0;
        if (!ds && !ds4) continue;
        ++found;
        if (slot >= 4) break;
        std::thread(HidReader, slot++, std::wstring(det->DevicePath), ds).detach();
        ++opened;
    }
    SetupDiDestroyDeviceInfoList(info);
    g_hid_note = opened ? "reading " + std::to_string(opened) + " Sony pad(s) directly" : found ? "a Sony pad is there but cannot be opened (HidHide?)" : "no Sony pad found";
}

// the pad in use: XInput first (Xbox pads, DS4Windows, Steam Input), else a DualSense / DualShock 4 read directly. Of
// several pads, the one that was pressed last.
inline bool Poll(State& s) {
    struct Gp { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    struct St { DWORD packet; Gp gp; };
    using Fn = DWORD(WINAPI*)(DWORD, St*);
    static Fn fn = nullptr;
    static bool tried = false;
    static ULONGLONG next_probe[4] = {};
    static int last_pad = 0;
    if (!tried) {
        tried = true;
        for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"})
            if (HMODULE m = LoadLibraryW(dll)) { fn = reinterpret_cast<Fn>(GetProcAddress(m, "XInputGetState")); if (fn) break; }
        StartHid();
    }
    s = State{};
    const ULONGLONG now = GetTickCount64();
    if (fn)
        for (int k = 0; k < 4; ++k) {
            const int i = (last_pad + k) % 4;
            if (i != last_pad && now < next_probe[i]) continue;   // pads that were not there are looked for twice a second
            St st{};
            if (fn(DWORD(i), &st) != 0) { next_probe[i] = now + 500; continue; }
            last_pad = i;
            const WORD b = st.gp.buttons;
            s.connected = true;
            s.cross = b & 0x1000; s.circle = b & 0x2000; s.square = b & 0x4000; s.triangle = b & 0x8000;
            s.l1 = b & 0x0100; s.r1 = b & 0x0200; s.l3 = b & 0x0040; s.r3 = b & 0x0080;
            s.up = b & 0x0001; s.down = b & 0x0002; s.left = b & 0x0004; s.right = b & 0x0008;
            s.start = b & 0x0010; s.back = b & 0x0020;
            s.l2 = st.gp.lt / 255.f; s.r2 = st.gp.rt / 255.f;
            auto stick = [](SHORT v) { const float f = v / 32767.f; return std::fabs(f) < 0.12f ? 0.f : std::clamp(f, -1.f, 1.f); };
            s.lx = stick(st.gp.lx); s.ly = stick(st.gp.ly); s.rx = stick(st.gp.rx); s.ry = stick(st.gp.ry);
            if (!Neutral(s)) return true;
            break;   // idle: a DualSense that is being used may be the one that matters
        }
    std::lock_guard<std::mutex> l(HidMutex());
    int best = -1;
    for (int i = 0; i < 4; ++i)
        if (g_hid[i].up && (best < 0 || g_hid[i].active_at > g_hid[best].active_at)) best = i;
    if (best >= 0) { s = g_hid[best].st; return true; }
    return s.connected;
}

// the sheet as the caller has it: its texture and the uv rectangle of every cell
struct Sheet {
    ImTextureRef tex;
    ImVec2 uv0[kCellCount], uv1[kCellCount];
    bool ok = false;
};

inline ImU32 Mul(ImU32 c, float k, float alpha) {
    const int r = int(((c >> IM_COL32_R_SHIFT) & 255) * k), g = int(((c >> IM_COL32_G_SHIFT) & 255) * k), b = int(((c >> IM_COL32_B_SHIFT) & 255) * k);
    return IM_COL32(std::min(r, 255), std::min(g, 255), std::min(b, 255), int(std::clamp(alpha, 0.f, 1.f) * 255));
}

// The pad, 430 x 285 units at scale 1, top-left at 'o'. Not pressed = dim, pressed = lit, a little bigger and with a
// glow in the button's colour (Cross blue, Circle red, Square pink, Triangle green: the colours of the game's prompts).
inline void Draw(ImDrawList* dl, ImVec2 o, float scale, float opacity, const State& st, const Sheet& sh, bool sticks = true) {
    auto P = [&](float x, float y) { return ImVec2(o.x + x * scale, o.y + y * scale); };
    auto image = [&](Cell c, float cx, float cy, float w, float lit, ImU32 colour, float grow) {
        if (!sh.ok) return;
        const float cw = float(kCells[c][2] - kCells[c][0]), ch = float(kCells[c][3] - kCells[c][1]);
        const float ww = w * scale * (1.f + grow * lit), hh = ww * ch / cw;
        const ImVec2 ctr = P(cx, cy);
        if (lit > 0.01f && colour != IM_COL32_WHITE)   // the glow
            dl->AddCircleFilled(ctr, ww * 0.62f, Mul(colour, 1.f, 0.30f * lit * opacity), 32);
        const float k = 0.38f + 0.62f * lit;
        const ImU32 tint = colour == IM_COL32_WHITE ? Mul(IM_COL32_WHITE, k, opacity * (0.55f + 0.45f * lit))
                                                    : Mul(colour, 0.55f + 0.45f * lit, opacity * (0.6f + 0.4f * lit));
        dl->AddImage(sh.tex, ImVec2(ctr.x - ww / 2, ctr.y - hh / 2), ImVec2(ctr.x + ww / 2, ctr.y + hh / 2), sh.uv0[c], sh.uv1[c], tint);
    };
    const ImU32 kBlue = IM_COL32(110, 170, 255, 255), kRed = IM_COL32(255, 95, 95, 255), kPink = IM_COL32(255, 130, 225, 255), kGreen = IM_COL32(110, 235, 140, 255);

    image(kL2, 140, 62, 70, st.l2, IM_COL32_WHITE, 0.0f);
    image(kR2, 290, 62, 70, st.r2, IM_COL32_WHITE, 0.0f);
    image(kL1, 140, 122, 96, st.l1 ? 1.f : 0.f, IM_COL32_WHITE, 0.06f);
    image(kR1, 290, 122, 96, st.r1 ? 1.f : 0.f, IM_COL32_WHITE, 0.06f);

    // D-pad: four separate arrows (tip out) around a small square, with a gap between all of them
    auto arrow = [&](float cx, float cy, float ux, float uy, bool on) {
        const float vx = -uy, vy = ux;   // perpendicular
        auto pt = [&](float u, float v) { return P(cx + ux * u + vx * v, cy + uy * u + vy * v); };
        const ImVec2 pts[5] = {pt(46, 0), pt(33, 12), pt(19, 12), pt(19, -12), pt(33, -12)};
        if (on) dl->AddCircleFilled(pt(30, 0), 24 * scale, IM_COL32(255, 190, 90, int(70 * opacity)), 24);
        dl->AddConvexPolyFilled(pts, 5, on ? IM_COL32(255, 205, 110, int(255 * opacity)) : IM_COL32(24, 22, 26, int(225 * opacity)));
        dl->AddPolyline(pts, 5, on ? IM_COL32(255, 245, 210, int(255 * opacity)) : IM_COL32(225, 225, 232, int(150 * opacity)), ImDrawFlags_Closed, 1.5f * scale);
    };
    const float dcx = 78, dcy = 236;
    dl->AddRectFilled(P(dcx - 11, dcy - 11), P(dcx + 11, dcy + 11), IM_COL32(24, 22, 26, int(200 * opacity)), 3 * scale);
    dl->AddRect(P(dcx - 11, dcy - 11), P(dcx + 11, dcy + 11), IM_COL32(225, 225, 232, int(110 * opacity)), 3 * scale, 0, 1.2f * scale);
    arrow(dcx, dcy, 0, -1, st.up);
    arrow(dcx, dcy, 0, 1, st.down);
    arrow(dcx, dcy, -1, 0, st.left);
    arrow(dcx, dcy, 1, 0, st.right);

    image(kTriangle, 345, 188, 50, st.triangle ? 1.f : 0.f, kGreen, 0.10f);
    image(kSquare, 305, 226, 50, st.square ? 1.f : 0.f, kPink, 0.10f);
    image(kCircle, 387, 226, 50, st.circle ? 1.f : 0.f, kRed, 0.10f);
    image(kCross, 345, 264, 50, st.cross ? 1.f : 0.f, kBlue, 0.10f);

    if (sticks) {   // sticks: the disc moves with the stick, lights when pressed in (L3 / R3)
        image(kLStick, 175 + st.lx * 9, 262 - st.ly * 9, 42, st.l3 ? 1.f : 0.25f + 0.2f * std::min(1.f, std::fabs(st.lx) + std::fabs(st.ly)), IM_COL32_WHITE, 0.08f);
        image(kRStick, 245 + st.rx * 9, 262 - st.ry * 9, 42, st.r3 ? 1.f : 0.25f + 0.2f * std::min(1.f, std::fabs(st.rx) + std::fabs(st.ry)), IM_COL32_WHITE, 0.08f);
    }
}

inline ImVec2 Size(float scale) { return ImVec2(430 * scale, 290 * scale); }

}  // namespace astro::pad
