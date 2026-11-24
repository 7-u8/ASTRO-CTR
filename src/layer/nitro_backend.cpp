#include "nitro_backend.h"
#include "switches.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../config/config.h"
#include "../diagnostics/log.h"
#include "paint_palette.h"

namespace astro {

namespace {

constexpr int kLayers = 4;
constexpr size_t kObjSize = 400;                       // igVfxColorCurveOperator
constexpr size_t kKeysAt = 64, kKeysLen = 48;          // curve keys: unique per layer, never written
constexpr size_t kBlockAt[3] = {112, 176, 240};        // R, G, B blocks
// object offsets relative to layer 0, as in the file and in memory
constexpr size_t kLayerOffset[kLayers] = {0x0, 0x190, 0x670, 0x960};
// curve keys of each layer (ctr_kart_exhaust_fire_1.igz, objects 0x1e00 / 0x1f90 / 0x2470 / 0x2760)
const unsigned char kKeys[kLayers][kKeysLen] = {
    {0x00,0x00,0x00,0x80,0x72,0xbc,0x46,0xc0,0x2c,0x99,0xf9,0xc0,0xf7,0x68,0x10,0xc0,0x37,0xc3,0x09,0x40,0xdb,0xe4,0xd5,0x40,
     0xda,0x32,0x4b,0x41,0xf0,0x68,0x50,0x40,0xb7,0xdb,0xed,0x3e,0x75,0x3a,0x1d,0x3f,0xc6,0x62,0x31,0x3f,0x00,0x00,0x80,0x3f},
    {0x00,0x00,0x00,0x80,0x6a,0x78,0xf8,0xbe,0x60,0x9d,0x78,0xc0,0x02,0x00,0x00,0x3f,0x74,0x4d,0x76,0x40,0x01,0x0f,0xef,0x3f,
     0xab,0x4e,0x9c,0x40,0x02,0x00,0x00,0xbf,0x14,0x0a,0x85,0x3e,0x2e,0x97,0x4b,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0xbf},
    {0x00,0x00,0x00,0x80,0x76,0x54,0x25,0xc2,0x02,0x00,0x00,0x3f,0x02,0x00,0x00,0x3f,0xc3,0x18,0x83,0x3f,0x70,0x54,0x29,0x42,
     0x02,0x00,0x00,0xbf,0x02,0x00,0x00,0xbf,0xe8,0xf3,0x79,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0xbf,0x00,0x00,0x80,0xbf},
    {0x00,0x00,0x00,0x80,0x00,0xfe,0x79,0xc2,0x02,0x00,0x00,0x3f,0x02,0x00,0x00,0x3f,0x46,0x0c,0x82,0x3f,0xf8,0xfd,0x7d,0x42,
     0x02,0x00,0x00,0xbf,0x02,0x00,0x00,0xbf,0xf0,0xf7,0x7b,0x3f,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0xbf,0x00,0x00,0x80,0xbf},
};
// dark smoke curves of ctr_kart_exhaust_fire_1_blacksmoke.igz (objects 0x1cb0 / 0x1fa0), placed 0x960 / 0x670
// bytes before that file's flame layer 0 (0x2610)
constexpr size_t kSmokeBefore[2] = {0x960, 0x670};
const unsigned char kSmokeKeys[2][kKeysLen] = {
    {0x00,0x00,0x00,0x80,0x02,0x00,0x00,0x3f,0x02,0x00,0x00,0x3f,0x02,0x00,0x00,0x3f,0xf7,0xff,0x7f,0x3f,0x02,0x00,0x00,0xbf,
     0x02,0x00,0x00,0xbf,0x02,0x00,0x00,0xbf,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0xbf,0x00,0x00,0x80,0xbf,0x00,0x00,0x80,0xbf},
    {0x00,0x00,0x00,0x80,0x4e,0x55,0xa5,0xbe,0x02,0x00,0x00,0x3f,0x02,0x00,0x00,0x3f,0xb4,0x18,0x83,0x40,0x4e,0x55,0xa9,0x3f,
     0x02,0x00,0x00,0xbf,0x02,0x00,0x00,0xbf,0xe8,0xf3,0x79,0x3e,0x00,0x00,0x80,0x3f,0x00,0x00,0x80,0xbf,0x00,0x00,0x80,0xbf},
};

// What each colour slot drives: layers 1+4, layer 2, layer 3 (all used by both the normal and the blue boost),
// and the black-smoke variant's two smoke curves.
enum Slot { kMain, kSecond, kThird, kSmoke, kSlots };
constexpr Slot kLayerSlot[kLayers] = {kMain, kSecond, kThird, kMain};

bool ReadSelf(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), out, n, &got) && got == n;
}
bool WriteSelf(uintptr_t a, const void* in, size_t n) {
    JournalWrite("nitro", a, in, n);
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), in, n, &put) && put == n;
}

bool Readable(DWORD p) {
    if (p & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
}

bool KeysAt(uintptr_t curve, const unsigned char* keys) {
    unsigned char k[kKeysLen];
    return ReadSelf(curve + kKeysAt, k, kKeysLen) && !std::memcmp(k, keys, kKeysLen);
}

// Champion underglow: igVfxColorOperator of vfx/CTR/share/ctr_kart_shadow_kartchampion.igz (object 0x12d0), colour
// 0.1 / 0.2 / 1.0 x intensity 1.4 over a white neon texture. The decal copies this value when it is created, so the
// operator and every live copy are rewritten (verified in game 2026-10-30: zero -> light gone, red -> red light).
const unsigned char kGlowOrig[16] = {0xcd,0xcc,0xcc,0x3d, 0xcd,0xcc,0x4c,0x3e, 0x00,0x00,0x80,0x3f, 0x33,0x33,0xb3,0x3f};
constexpr float kGlowIntensity = 1.4f;

float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// One loaded colour curve. The game keeps several copies of the flame effect (9 seen in a race: the plain
// and the black-smoke variant, per use); every copy is recoloured.
struct Curve {
    uintptr_t addr;
    Slot slot;
    const unsigned char* keys;
    unsigned char orig[kObjSize];
    float last[3][16];      // what this mod wrote last in each channel block (equal to orig until written)
};

class GuestBackend final : public IVisualBackend {
public:
    GuestBackend() { worker_ = std::thread([this] { Search(); }); }
    ~GuestBackend() override {
        stop_ = true;
        if (worker_.joinable()) worker_.join();
    }

    BackendInfo Probe() override {
        BackendInfo i;
        std::lock_guard<std::mutex> l(mu_);
        if (!curves_.empty()) {
            i.status = BackendStatus::Ready;
            i.detail = "nitro flame found in game memory (" + std::to_string(sets_) + " copies)";
        } else if (passes_) {
            i.detail = "nitro flame not in memory yet: searching again every 5 s (start a race)";
        } else {
            i.detail = "searching game memory for the nitro flame...";
        }
        return i;
    }

    bool SetNormalNitro(const Rgba* c) override {
        if (!c) return SetNormalNitroLayers(nullptr, 1.0f);
        const Rgba all[4] = {*c, *c, *c, Rgba{255, 102, 102, 255}};
        return SetNormalNitroLayers(all, 1.0f);
    }
    // colors {curves 1+4, curve 2, curve 3, smoke}; brightness multiplies the flame curves (not the smoke)
    bool SetNormalNitroLayers(const Rgba* c, float brightness) override {
        std::lock_guard<std::mutex> l(mu_);
        for (int s = 0; s < kSlots; ++s) SetSlot(Slot(s), c ? &c[s] : nullptr);
        brightness_ = brightness;
        if (curves_.empty()) return !c;      // applied as soon as the search finds the flame
        return ApplyLocked();
    }
    // the blue boost uses the same curves as the normal boost: no separate colour found yet
    bool SetBlueNitroPrimary(const Rgba* c) override { return c == nullptr; }
    bool SetBlueNitroSecondary(const Rgba* c) override { return c == nullptr; }
    bool SetChampionLowerRegion(const Rgba* c) override {
        std::lock_guard<std::mutex> l(mu_);
        glow_on_ = c != nullptr;
        if (c) glow_ = *c;
        if (glows_.empty()) return !c;       // applied as soon as the search finds the underglow
        ApplyGlowLocked();
        return true;
    }
    // Kart paint test. Paint colours sit in the scheme materials' command streams after a fixed 48-byte header
    // (commands 0x24, 0x1e, 0x01). The kart draws straight from that stream (FUN_00e7a5b0 shares the "GBuffer"
    // pass), but writes from another process did not show: these stores are plain in-process writes, which the
    // emulator's memory tracking can see.
    void PaintTest(int mode) override {
        if (paint_busy_.exchange(true)) return;   // one test at a time
        std::thread([this, mode] {
            auto status = [this](const std::string& t) { std::lock_guard<std::mutex> l(paint_mu_); paint_status_ = t; };
            if (mode == 0) {
                size_t n = 0;
                for (auto& [a, orig] : paint_orig_) {
                    float cur[4];
                    if (ReadSelf(a, cur, 16) && cur[0] == 1.0f && cur[1] == 0.0f && cur[2] == 1.0f) { std::memcpy(reinterpret_cast<void*>(a), orig.data(), 16); ++n; }
                }
                paint_orig_.clear();
                status("restored " + std::to_string(n) + " colours");
                paint_busy_ = false;
                return;
            }
            static const unsigned char hdr[48] = {0x24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0x1e,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                                  0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
            status("searching (about 10 s)...");
            const auto searcher = std::boyer_moore_horspool_searcher(hdr, hdr + 48);
            std::vector<unsigned char> buf;
            std::vector<uintptr_t> found;
            MEMORY_BASIC_INFORMATION m{};
            for (uintptr_t a = 0x1000000000ull; a < 0x8000000000ull; a = uintptr_t(m.BaseAddress) + m.RegionSize) {
                if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
                if (m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE) continue;
                const uintptr_t end = uintptr_t(m.BaseAddress) + m.RegionSize;
                for (uintptr_t p = uintptr_t(m.BaseAddress); p < end; p += (16u << 20)) {
                    size_t n = std::min<size_t>((16u << 20) + 64, end - p);
                    buf.resize(n);
                    if (!ReadSelf(p, buf.data(), n)) continue;
                    for (auto it = buf.begin(); (it = std::search(it, buf.end(), searcher)) != buf.end(); ++it) {
                        uintptr_t c = p + size_t(it - buf.begin()) + 48;
                        float v[4];
                        if (c % 4 == 0 && ReadSelf(c, v, 16) && v[3] == 1.0f && (found.empty() || found.back() != c)) found.push_back(c);
                    }
                }
            }
            const float mag[4] = {1, 0, 1, 1};
            for (uintptr_t c : found) {
                std::array<unsigned char, 16> o;
                ReadSelf(c, o.data(), 16);
                paint_orig_.push_back({c, o});
                std::memcpy(reinterpret_cast<void*>(c), mag, 16);   // plain store from inside the game process
            }
            status("painted " + std::to_string(found.size()) + " colours magenta (in-process)");
            Logger::Instance().Info("paint test: painted " + std::to_string(found.size()) + " colours");
            paint_busy_ = false;
        }).detach();
    }
    std::string PaintTestStatus() override { std::lock_guard<std::mutex> l(paint_mu_); return paint_status_; }

    void RestoreAll() override {
        std::lock_guard<std::mutex> l(mu_);
        for (bool& on : want_on_) on = false;
        glow_on_ = false;
        kart_on_ = false;
        ApplyLocked();
        ApplyGlowLocked();
        if (fast_ok_) FastPaintTickLocked();
        else if (kart_have_orig_) ApplyKartLocked();
    }

private:
    void SetSlot(Slot s, const Rgba* c) {
        want_on_[s] = c != nullptr;
        if (c) want_[s] = *c;
    }

    bool Write(Curve& cv) {
        float orig[3];
        for (int ch = 0; ch < 3; ++ch) std::memcpy(&orig[ch], cv.orig + kBlockAt[ch] + 8, 4);
        float target[3] = {orig[0], orig[1], orig[2]};
        if (want_on_[cv.slot]) {
            // match perceived brightness (x the brightness setting for the flame): pure blue needs far more than 0.28
            // to look as bright as the original orange, so the colour is scaled to a luminance (HDR values allowed)
            const Rgba& c = want_[cv.slot];
            const float w[3] = {c.r / 255.f, c.g / 255.f, c.b / 255.f};
            const float lw = Luma(w), lo = Luma(orig) * (cv.slot == kSmoke ? 1.0f : brightness_);
            for (int ch = 0; ch < 3; ++ch) target[ch] = lw > 1e-4f ? std::min(w[ch] * lo / lw, 8.0f) : 0.0f;
        }
        // A curve the game freed is soon reused for other objects (pointers, vtables): writing there crashed the game
        // (2026-11-01, write journal: a level exit freed a curve during the 8 s search, the scaled "floats" turned
        // the pointer 0x101e601930 into 0x0e1e465a3f, the exact address the game then faulted on). So a block is only
        // written while the curve's keys are still in place and the block holds exactly the game's bytes or ours.
        if (!KeysAt(cv.addr, cv.keys)) return false;
        bool ok = true;
        for (int ch = 0; ch < 3; ++ch) {
            if (orig[ch] == 0) continue;   // channel without a curve: cannot be scaled
            float blk[16], cur[16];
            std::memcpy(blk, cv.orig + kBlockAt[ch], sizeof blk);
            for (float& f : blk) f *= target[ch] / orig[ch];
            if (!ReadSelf(cv.addr + kBlockAt[ch], cur, sizeof cur)) { ok = false; continue; }
            if (!std::memcmp(cur, blk, sizeof cur)) { std::memcpy(cv.last[ch], blk, sizeof blk); continue; }   // already right
            if (std::memcmp(cur, cv.orig + kBlockAt[ch], sizeof cur) && std::memcmp(cur, cv.last[ch], sizeof cur)) { ok = false; continue; }
            if (WriteSelf(cv.addr + kBlockAt[ch], blk, sizeof blk)) std::memcpy(cv.last[ch], blk, sizeof blk);
            else ok = false;
        }
        return ok;
    }

    bool ApplyLocked() {
        bool ok = true;
        for (Curve& cv : curves_)
            if (KeysAt(cv.addr, cv.keys)) ok &= Write(cv);   // never write a copy the game has freed
        if (!ok) Logger::Instance().Error("nitro colour write failed");
        return ok;
    }

    // Underglow: every place that holds the colour (the operator and the live decal copies) is rewritten. A place is
    // only touched while it still holds the game's blue or the colour this mod wrote last, so memory the game has
    // reused for something else is never written.
    void ApplyGlowLocked() {
        unsigned char want[16];
        if (glow_on_) {
            const float v[4] = {glow_.r / 255.f, glow_.g / 255.f, glow_.b / 255.f, kGlowIntensity};
            std::memcpy(want, v, 16);
        } else {
            std::memcpy(want, kGlowOrig, 16);
        }
        std::vector<uintptr_t> keep;
        for (uintptr_t a : glows_) {
            unsigned char cur[16];
            if (!ReadSelf(a, cur, 16) || (std::memcmp(cur, kGlowOrig, 16) && std::memcmp(cur, glow_written_, 16))) continue;
            if (std::memcmp(cur, want, 16)) WriteSelf(a, want, 16);
            keep.push_back(a);
        }
        glows_.swap(keep);
        std::memcpy(glow_written_, want, 16);
    }

    // ---------------- player kart paint ----------------
    // The paint-job colours live in the KartColorScheme materials' command streams (after kHdr). Writing there does
    // not show: the kart is drawn from copies the game makes when it builds the kart. Those copies hold the same RGB
    // floats; a kart's copy group = colours of one scheme within kNear bytes of each other (verified in game
    // 2026-10-30: the 3 copies of scheme 00's blue -> magenta turned the kart body magenta, nothing else changed).
    using Rgb = std::array<float, 3>;
    static constexpr uintptr_t kNear = 0x3000;
    static inline const unsigned char kHdr[48] = {0x24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0, 0x1e,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
                                                  0x01,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
    struct PaintKey { int off; int idx; };   // channel offset of the key value inside the colour, index into key_rgbs

    static Rgb ToRgb(const Rgba& c) { return {c.r / 255.f, c.g / 255.f, c.b / 255.f}; }
    static Rgba ToRgba(const Rgb& c) {
        auto b = [](float v) { return std::uint8_t(v <= 0 ? 0 : v >= 1 ? 255 : int(v * 255.f + 0.5f)); };
        return Rgba{b(c[0]), b(c[1]), b(c[2]), 255};
    }

    bool SetKartPaint(const Rgba* c) override {
        std::lock_guard<std::mutex> l(mu_);
        kart_on_ = c != nullptr;
        if (c)
            for (int i = 0; i < 3; ++i) {
                kart_want_[i] = ToRgb(c[i * kPaintLayers]);
                for (int k = 0; k < kRamp; ++k) ramp_want_[i][k] = ToRgb(c[i * kPaintLayers + 1 + k]);
            }
        FastPaintTickLocked();
        if (fast_ok_) return true;
        if (kart_copies_.empty()) return !c;   // applied as soon as the kart's copies are found
        ApplyKartLocked();
        return true;
    }
    std::vector<std::vector<Rgba>> KartPaintCandidates() override {
        std::lock_guard<std::mutex> l(mu_);
        std::vector<std::vector<Rgba>> out;
        for (auto& c : candidates_) out.push_back({ToRgba(c[0]), ToRgba(c[1]), ToRgba(c[2])});
        return out;
    }
    void ChooseKartPaint(const std::vector<Rgba>& c) override {
        std::lock_guard<std::mutex> l(mu_);
        for (auto& cand : candidates_)
            if (ToRgba(cand[0]) == c[0] && ToRgba(cand[1]) == c[1] && ToRgba(cand[2]) == c[2]) {
                for (int s = 0; s < 3; ++s) kart_orig_[s] = kart_written_[s] = cand[s];
                kart_have_orig_ = true;
                kart_copies_.clear();
                Logger::Instance().Info("kart paint: user chose " + FormatHexColor(c[0]) + " " + FormatHexColor(c[1]) + " " + FormatHexColor(c[2]));
            }
    }
    bool KartPaintOriginal(Rgba* out) override {
        std::lock_guard<std::mutex> l(mu_);
        if (!kart_have_orig_) return false;
        for (int i = 0; i < 3; ++i) {
            out[i * kPaintLayers] = ToRgba(kart_orig_[i]);
            for (int k = 0; k < kRamp; ++k) out[i * kPaintLayers + 1 + k] = fast_r_[i] ? ToRgba(ramp_orig_[i][k]) : out[i * kPaintLayers];
        }
        return true;
    }

    void ApplyKartLocked() {
        Rgb target[3];
        for (int s = 0; s < 3; ++s) target[s] = kart_on_ ? kart_want_[s] : kart_orig_[s];
        std::vector<std::pair<uintptr_t, int>> keep;
        for (auto [a, s] : kart_copies_) {
            Rgb cur;
            if (!ReadSelf(a, cur.data(), 12) || (cur != kart_orig_[s] && cur != kart_written_[s])) continue;   // reused memory
            if (cur != target[s]) WriteSelf(a, target[s].data(), 12);
            keep.push_back({a, s});
        }
        kart_copies_.swap(keep);
        for (int s = 0; s < 3; ++s) kart_written_[s] = target[s];
    }

    // One chunk of the memory pass: stream colours (after kHdr) and candidate copies (key channel, then all 3).
    void PaintScanChunk(uintptr_t p, const std::vector<unsigned char>& buf, const std::vector<Rgb>& key_rgbs,
                        const std::unordered_multimap<uint32_t, PaintKey>& keys, const std::vector<uint8_t>& filter,
                        std::vector<std::pair<uintptr_t, Rgb>>& streams, std::vector<std::pair<uintptr_t, int>>& hits) {
        static const auto hdr_searcher = std::boyer_moore_horspool_searcher(kHdr, kHdr + 48);
        for (auto it = buf.begin(); (it = std::search(it, buf.end(), hdr_searcher)) != buf.end(); ++it) {
            size_t i = size_t(it - buf.begin()) + 48;
            if (i + 12 > buf.size()) break;
            Rgb c;
            std::memcpy(c.data(), &buf[i], 12);
            streams.push_back({p + i, c});
        }
        if (keys.empty()) return;
        const size_t n = buf.size() & ~size_t(3);
        for (size_t i = 0; i + 4 <= n; i += 4) {
            uint32_t v;
            std::memcpy(&v, &buf[i], 4);
            if (!filter[v & 0xffff]) continue;
            auto range = keys.equal_range(v);
            for (auto k = range.first; k != range.second; ++k) {
                if (i < size_t(k->second.off) || i - k->second.off + 12 > buf.size()) continue;
                const size_t at = i - k->second.off;
                if (!std::memcmp(&buf[at], key_rgbs[k->second.idx].data(), 12)) hits.push_back({p + at, k->second.idx});
            }
        }
    }

    // After a memory pass: learn the schemes, find the kart's copy groups, track and write them.
    void ProcessPaintLocked(std::vector<std::pair<uintptr_t, Rgb>>& streams, const std::vector<std::pair<uintptr_t, int>>& hits,
                            const std::vector<Rgb>& key_rgbs) {
        // every paint colour currently loaded in the scheme materials (keys for the next pass)
        std::unordered_set<uintptr_t> stream_addrs;
        std::vector<Rgb> cols;
        for (auto& [a, c] : streams) {
            stream_addrs.insert(a);
            if (std::find(cols.begin(), cols.end(), c) == cols.end()) cols.push_back(c);
        }
        schemes_ = {cols};

        // copies outside the streams, as (address, colour), in address order
        std::vector<std::pair<uintptr_t, Rgb>> copies;
        for (auto& [a, idx] : hits)
            if (!stream_addrs.count(a)) copies.push_back({a, key_rgbs[idx]});
        std::sort(copies.begin(), copies.end(), [](auto& x, auto& y) { return x.first < y.first; });

        // clusters of copies of the given colours (>= 2 different ones close together) = one kart's copy
        auto clusters_of = [&](const std::vector<Rgb>& sc) {
            std::vector<std::pair<uintptr_t, uintptr_t>> out;   // [first, last] address of each valid cluster
            uintptr_t first = 0, last = 0;
            unsigned slots = 0;
            auto flush = [&] { if (__builtin_popcount(slots) >= 2) out.push_back({first, last}); slots = 0; };
            for (auto& [a, c] : copies) {
                int s = int(std::find(sc.begin(), sc.end(), c) - sc.begin());
                if (s >= int(sc.size())) continue;
                if (slots && a - last > kNear) flush();
                if (!slots) first = a;
                last = a;
                slots |= 1u << s;
            }
            if (slots) flush();
            return out;
        };

        if (kart_on_) {
            // candidates: every cluster of copies with 3 different paint colours, colours in address order
            // (slot 1 = main body first, as in the materials); the user picks the kart's own when there are several
            std::vector<std::array<Rgb, 3>> found;
            std::vector<int> count;   // copy groups per candidate: the shown kart has several (LODs / passes)
            for (size_t i = 0; i < copies.size();) {
                size_t j = i;
                std::vector<Rgb> distinct;
                while (j < copies.size() && (j == i || copies[j].first - copies[j - 1].first <= kNear)) {
                    if (std::find(distinct.begin(), distinct.end(), copies[j].second) == distinct.end()) distinct.push_back(copies[j].second);
                    ++j;
                }
                if (distinct.size() == 3) {
                    std::array<Rgb, 3> c{distinct[0], distinct[1], distinct[2]};
                    auto f = std::find(found.begin(), found.end(), c);
                    if (f == found.end()) { found.push_back(c); count.push_back(1); }
                    else ++count[f - found.begin()];
                }
                i = j;
            }
            // the kart on screen = the colour set with the most copy groups; equal counts: the more colourful one
            auto sat = [](const std::array<Rgb, 3>& c) {
                float t = 0;
                for (auto& x : c) t += *std::max_element(x.begin(), x.end()) - *std::min_element(x.begin(), x.end());
                return t;
            };
            std::vector<size_t> order(found.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](size_t x, size_t y) {
                return count[x] != count[y] ? count[x] > count[y] : sat(found[x]) > sat(found[y]);
            });
            candidates_.clear();
            for (size_t i : order) candidates_.push_back(found[i]);
            if (!kart_have_orig_) {
                std::string list;
                for (size_t i : order) list += " [" + FormatHexColor(ToRgba(found[i][0])) + " " + FormatHexColor(ToRgba(found[i][1])) + " " +
                                               FormatHexColor(ToRgba(found[i][2])) + " x" + std::to_string(count[i]) + "]";
                Logger::Instance().Info("kart paint: " + std::to_string(copies.size()) + " copies, candidates:" + list);
            }
            const bool tracked_alive = !kart_copies_.empty();
            if (!candidates_.empty() && (!kart_have_orig_ || (!tracked_alive && candidates_[0] != std::array<Rgb, 3>{kart_orig_[0], kart_orig_[1], kart_orig_[2]}))) {
                for (int s = 0; s < 3; ++s) kart_orig_[s] = kart_written_[s] = candidates_[0][s];
                kart_have_orig_ = true;
                kart_copies_.clear();
                Logger::Instance().Info("kart paint: kart uses " + FormatHexColor(ToRgba(kart_orig_[0])) + " " +
                                        FormatHexColor(ToRgba(kart_orig_[1])) + " " + FormatHexColor(ToRgba(kart_orig_[2])));
            }
        }
        if (!kart_have_orig_) return;

        // every copy of the kart's colours inside a valid group (also slots whose colour has no key channel)
        size_t added = 0;
        for (auto [lo, hi] : clusters_of(std::vector<Rgb>(kart_orig_, kart_orig_ + 3))) {
            const uintptr_t from = lo - kNear, to = hi + kNear;
            std::vector<unsigned char> win(to - from);
            if (!ReadSelf(from, win.data(), win.size())) continue;
            for (size_t i = 0; i + 12 <= win.size(); i += 4)
                for (int s = 0; s < 3; ++s)
                    if (!std::memcmp(&win[i], kart_orig_[s].data(), 12) && !stream_addrs.count(from + i) &&
                        std::none_of(kart_copies_.begin(), kart_copies_.end(), [&](auto& c) { return c.first == from + i; })) {
                        kart_copies_.push_back({from + i, s});
                        ++added;
                    }
        }
        ApplyKartLocked();
        if (added) {
            std::string where;
            char buf[48];
            for (auto& [addr, slot] : kart_copies_) { std::snprintf(buf, sizeof buf, " %d@0x%llx", slot + 1, (unsigned long long)addr); where += buf; }
            Logger::Instance().Info("kart paint: " + std::to_string(added) + " new copies, " + std::to_string(kart_copies_.size()) + " tracked:" + where);
        }
    }

    // ---- direct path (no search) ----
    // The shown kart's paint is its own resolved command stream: 0x24 | ptr | 0x1e | 0 | P | R G B A | 0x1f ...
    // Body colour = <kart material object> + 0x2b0 (pointer scans + checks in game 2026-10-30: writing there turned
    // the kart body magenta). Colours 2 and 3 follow in the same block with the same structure and the same P.
    // Paths to the material object, tried in order (EU 1.21 eboot data; the USA 1.21 eboot has them at the same addresses). The first was found both on the
    // customization screen and in a race (pointer scans intersected, 2026-10-30); the second only on the
    // customization screen, the third only inside the Paint Job menu.
    struct KartPath { uintptr_t root; int n; uint32_t off[4]; };
    static constexpr uintptr_t kKnownBody = 0x108672e420ull;   // garage and races, every run (2026-10-30)
    static constexpr KartPath kKartPaths[] = {
        {0x802a20068ull, 3, {0x108, 0x30, 0x70}},
        {0x802a20068ull, 3, {0x28, 0xa8, 0x70}},
        {0x802ab9200ull, 2, {0x88, 0x0}},
    };
    static constexpr int kRamp = kPaintLayers - 1;          // ramp colours per slot

    // The colour ramp of a slot (the base colour; the tint alone only gives a sheen): after the tint, a vector with
    // w = 0 followed by 5 colours with w = 1 and a closing vector, all in 0..1 (found in game 2026-10-30). Returns
    // the address of the first of the 6 ramp vectors, 0 if not found.
    // ramp shape starting at a (the w = 0 vector): returns a + 16 (the first colour) or 0
    static uintptr_t FindRampAt(uintptr_t a, bool need_colour = true) {
        float v[7][4];
        if (!ReadSelf(a, v, sizeof v)) return 0;
        // real 0..1 floats only: command words read as floats are tiny denormals and must not pass
        auto unit = [](const float* x) {
            for (int i = 0; i < 4; ++i) if (!(x[i] == 0.0f || (x[i] >= 1e-4f && x[i] <= 1.0001f))) return false;
            return true;
        };
        bool ok = unit(v[0]) && v[0][3] == 0.0f && unit(v[6]);
        float sum = 0;
        for (int k = 1; k < 6 && ok; ++k) { ok = unit(v[k]) && v[k][3] == 1.0f; sum += v[k][0] + v[k][1] + v[k][2]; }
        return ok && (!need_colour || sum > 0.05f) ? a + 16 : 0;   // (a black ramp written by the mod is valid)
    }
    // The colour ramp of a slot (the base colour; the tint alone only gives a sheen): after the tint, a vector with
    // w = 0 followed by 5 colours with w = 1 and a closing vector (found in game 2026-10-30). Returns the address of
    // the first of the 6 ramp vectors (the w = 0 one), 0 if not found.
    static uintptr_t FindRamp(uintptr_t tint) {
        for (uintptr_t a = tint + 0x20; a + 0x70 <= tint + 0x900; a += 16)
            if (FindRampAt(a)) return a;
        return 0;
    }

    static bool Q(uintptr_t a, uint64_t& v) { return ReadSelf(a, &v, 8); }
    static bool IsHeap(uint64_t v) { return v >= 0x1000000000ull && v < 0x8000000000ull; }

    // the colour address if a resolved paint command sits there; P = the pointer before the colour
    static bool PaintCmdAt(uintptr_t c, uint64_t& p) {
        uint64_t a, b;
        uint32_t tail;
        return Q(c - 0x30, a) && a == 0x24 && Q(c - 0x20, b) && b == 0x1e && Q(c - 0x10, p) && IsHeap(p) &&
               ReadSelf(c + 0x10, &tail, 4) && tail == 0x1f;
    }

    // Called every 100 ms: follows the path, (re)finds the 3 colours when the kart was rebuilt, writes the wanted ones.
    // Every target must still be what was found: same command shape and pointer, ramp shape, and a value that is
    // the game's original or what this mod wrote. When the game rebuilds the kart (new paint job) the stream can be
    // rebuilt at the same address with another layout; writing at the old offsets corrupted draw commands
    // (crash in igCommandStreamQueue, 2026-10-30).
    bool FastTargetsValidLocked(uint64_t sig) {
        for (int s = 0; s < 3; ++s) {
            uint64_t p;
            Rgb cur;
            if (!PaintCmdAt(fast_c_[s], p) || p != sig || !ReadSelf(fast_c_[s], cur.data(), 12)) return false;
            if (cur != kart_orig_[s] && cur != kart_written_[s]) return false;
            if (!fast_r_[s]) continue;
            if (!FindRampAt(fast_r_[s], false)) return false;
            for (int k = 0; k < kRamp; ++k) {
                if (!ReadSelf(fast_r_[s] + 16 * k, cur.data(), 12)) return false;
                if (cur != ramp_orig_[s][k] && cur != ramp_written_[s][k]) return false;
            }
        }
        return true;
    }

    // Called every 100 ms: follows the path, (re)finds the 3 colours when the kart was rebuilt, writes the wanted ones.
    void FastPaintTickLocked() {
        if (Off("paint")) return;   // test run: no kart paint writes (see switches.h)
        uint64_t p3 = 0, sig = 0;
        // 1. the kart already found, while every target still passes the checks. In races the pointer paths can lead
        //    elsewhere (they differ per track) but the shown kart's paint stays where it was (two races, 2026-10-30);
        //    keeping it first also stops a path from switching to another (computer) kart.
        if (fast_ok_ && FastTargetsValidLocked(fast_sig_)) { p3 = fast_c_[0] - 0x2b0; sig = fast_sig_; }
        // 2. the pointer paths
        for (size_t i = 0; !p3 && i < std::size(kKartPaths); ++i) {
            const KartPath& kp = kKartPaths[i];
            uint64_t p;
            bool ok = Q(kp.root, p) && IsHeap(p);
            for (int k = 0; k < kp.n && ok; ++k) ok = Q(p + kp.off[k], p) && IsHeap(p);
            if (ok && PaintCmdAt(p + 0x2b0, sig)) p3 = p;
        }
        // 3. the address the player's kart paint had in every run so far (only if the command structure is there)
        if (!p3 && PaintCmdAt(kKnownBody, sig)) p3 = kKnownBody - 0x2b0;
        if (!p3) {
            fast_ok_ = false;
            return;
        }
        const uintptr_t body = p3 + 0x2b0;
        if (fast_ok_ && (body != fast_c_[0] || sig != fast_sig_ || !FastTargetsValidLocked(sig))) {
            fast_ok_ = false;   // kart rebuilt: nothing is written until it has been found again
            Logger::Instance().Info("kart paint (direct): kart rebuilt, finding its colours again");
        }
        if (!fast_ok_) {
            // new kart (or first time): find colours 2 and 3 after the body, read the kart's own colours
            uintptr_t c[3] = {body, 0, 0};
            int n = 1;
            for (uintptr_t a = body + 0x20; a < body + 0x4000 && n < 3; a += 4) {
                uint64_t p;
                if (PaintCmdAt(a, p) && p == sig) c[n++] = a;
            }
            if (n < 3) return;
            for (int s = 0; s < 3; ++s) {
                fast_c_[s] = c[s];
                ReadSelf(c[s], kart_orig_[s].data(), 12);
                kart_written_[s] = kart_orig_[s];
                fast_r_[s] = FindRamp(c[s]);
                for (int k = 0; k < kRamp; ++k) {
                    if (fast_r_[s]) ReadSelf(fast_r_[s] + 16 * k, ramp_orig_[s][k].data(), 12);
                    ramp_written_[s][k] = ramp_orig_[s][k];
                }
            }
            fast_sig_ = sig;
            kart_have_orig_ = true;
            kart_copies_.clear();
            fast_ok_ = true;
            char msg[220];
            std::snprintf(msg, sizeof msg, "kart paint (direct): colours at 0x%llx 0x%llx 0x%llx = %s %s %s, ramps %d %d %d",
                          (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)c[2],
                          FormatHexColor(ToRgba(kart_orig_[0])).c_str(), FormatHexColor(ToRgba(kart_orig_[1])).c_str(),
                          FormatHexColor(ToRgba(kart_orig_[2])).c_str(), fast_r_[0] != 0, fast_r_[1] != 0, fast_r_[2] != 0);
            Logger::Instance().Info(msg);
        }
        // compare with what is in memory now, not with what was written last: entering a race rebuilds the kart at
        // the same addresses with the game's own colours, which must be overwritten again (2026-10-30)
        for (int s = 0; s < 3; ++s) {
            const Rgb& want = kart_on_ ? kart_want_[s] : kart_orig_[s];
            Rgb cur;
            if (ReadSelf(fast_c_[s], cur.data(), 12) && cur != want && WriteSelf(fast_c_[s], want.data(), 12)) kart_written_[s] = want;
            if (!fast_r_[s]) continue;
            for (int k = 0; k < kRamp; ++k) {   // only r, g, b: the 4th value of each ramp entry stays the game's
                const Rgb& rw = kart_on_ ? ramp_want_[s][k] : ramp_orig_[s][k];
                if (ReadSelf(fast_r_[s] + 16 * k, cur.data(), 12) && cur != rw && WriteSelf(fast_r_[s] + 16 * k, rw.data(), 12))
                    ramp_written_[s][k] = rw;
            }
        }
    }

    // keys for the next pass: one channel per colour that is neither 0 nor 1 (those are everywhere)
    void BuildPaintKeysLocked(std::vector<Rgb>& key_rgbs, std::unordered_multimap<uint32_t, PaintKey>& keys, std::vector<uint8_t>& filter) {
        // every paint colour of the game (embedded table), not just the ones loaded right now
        for (auto& bits : kPaintPalette) {
            Rgb c;
            std::memcpy(c.data(), bits, 12);
            key_rgbs.push_back(c);
        }
        filter.assign(65536, 0);
        for (size_t i = 0; i < key_rgbs.size(); ++i)
            for (int ch = 0; ch < 3; ++ch)
                if (key_rgbs[i][ch] != 0.0f && key_rgbs[i][ch] != 1.0f) {
                    uint32_t v;
                    std::memcpy(&v, &key_rgbs[i][ch], 4);
                    keys.insert({v, PaintKey{ch * 4, int(i)}});
                    filter[v & 0xffff] = 1;
                    break;
                }
    }

    // Research hook: %APPDATA%\ASTRO-CTR\poke.txt with lines "<hex addr> r g b a" writes those 4 floats from inside
    // the game (writes from another process do not show), only where a resolved colour command
    // (0x24 | ptr | 0x1e | 0 | P | RGBA | 0x1f) sits; "* <hex addr> r g b a" writes any 16-byte-aligned address.
    // The file is renamed to poke.done with one result line per request.
    void PokeFile() {
        const char* appdata = std::getenv("APPDATA");
        if (!appdata) return;
        const std::string dir = std::string(appdata) + "\\ASTRO-CTR\\";
        FILE* f = std::fopen((dir + "poke.txt").c_str(), "r");
        if (!f) return;
        std::string report;
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            const bool raw = line[0] == '*';
            unsigned long long a = 0;
            float v[4];
            if (std::sscanf(line + (raw ? 1 : 0), "%llx %f %f %f %f", &a, &v[0], &v[1], &v[2], &v[3]) != 5) continue;
            uint64_t p;
            char buf[96];
            const bool ok = raw ? (a % 16 == 0 && IsHeap(a)) : PaintCmdAt(uintptr_t(a), p);
            float old[4] = {};
            if (ok && ReadSelf(uintptr_t(a), old, 16)) std::memcpy(reinterpret_cast<void*>(uintptr_t(a)), v, 16);
            std::snprintf(buf, sizeof buf, "%llx %s was %g %g %g %g\n", a, ok ? "written" : "refused", old[0], old[1], old[2], old[3]);
            report += buf;
        }
        std::fclose(f);
        std::remove((dir + "poke.done").c_str());
        std::rename((dir + "poke.txt").c_str(), (dir + "poke.done").c_str());
        if (FILE* o = std::fopen((dir + "poke.done").c_str(), "a")) { std::fputs(("--- result\n" + report).c_str(), o); std::fclose(o); }
        Logger::Instance().Info("poke: " + report);
    }

    // Copies appear when a race loads and disappear after it: search every 5 s until found, then every 20 s
    // (every 5 s while a custom underglow is on: the game re-creates it in blue whenever the kart is reloaded).
    void Search() {
        NameThread(L"astro:nitro");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);   // the game's threads always come first
        while (!stop_) {
            SearchOnce();
            int wait;
            { std::lock_guard<std::mutex> l(mu_); wait = curves_.empty() || glow_on_ || kart_on_ ? 50 : 200; }
            for (int i = 0; i < wait && !stop_; ++i) {
                Sleep(100);
                std::lock_guard<std::mutex> l(mu_);
                FastPaintTickLocked();   // direct path: instant, no search
                if (i % 10 == 0) PokeFile();
            }
        }
    }

    void SearchOnce() {
        if (Off("nitro")) return;   // test run: no memory search and no nitro / underglow writes (see switches.h)
        const auto t0 = GetTickCount64();
        std::vector<uintptr_t> bases, glows;
        // Both patterns are looked for in ONE pass, at 4-byte steps (floats are 4-aligned), by comparing their first 8
        // bytes: the old std::search per pattern over 16 MB chunks took ~8 s of a core per 4 GB heap pass and made the
        // game 15 fps slower (measured 2026-11-01: 176 fps without ASTRO, 160 with it, 176 again with this part off).
        std::uint64_t key0 = 0, glow0 = 0;
        std::memcpy(&key0, kKeys[0], 8);
        std::memcpy(&glow0, kGlowOrig, 8);
        // kart paint: keys from the schemes learnt in the previous pass (only while a custom paint is on)
        bool paint;
        std::vector<Rgb> key_rgbs;
        std::unordered_multimap<uint32_t, PaintKey> keys;
        std::vector<uint8_t> filter;
        {
            std::lock_guard<std::mutex> l(mu_);
            // The memory-wide paint search is off: it held the lock for long stretches (3 FPS in races) and the
            // direct path covers the shown kart. Kept in the code for diagnostics only.
            paint = false;
            if (paint) BuildPaintKeysLocked(key_rgbs, keys, filter);
        }
        std::vector<std::pair<uintptr_t, Rgb>> streams;
        std::vector<std::pair<uintptr_t, int>> hits;
        std::vector<unsigned char> buf;
        constexpr size_t kChunk = 1u << 20;      // small chunks stay in the CPU's cache: the game's own data is not pushed out
        MEMORY_BASIC_INFORMATION m{};
        for (uintptr_t a = 0x1000000000ull; !stop_ && a < 0x8000000000ull; a = uintptr_t(m.BaseAddress) + m.RegionSize) {
            if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m)) break;
            if (m.State != MEM_COMMIT || !Readable(m.Protect)) continue;
            const uintptr_t end = uintptr_t(m.BaseAddress) + m.RegionSize;
            for (uintptr_t p = uintptr_t(m.BaseAddress); p < end && !stop_; p += kChunk) {
                size_t n = std::min<size_t>(kChunk + kKeysLen, end - p);
                buf.resize(n);
                if (!ReadSelf(p, buf.data(), n)) continue;
                const size_t own = std::min<size_t>(kChunk, end - p);   // start positions of this chunk only (the extra bytes are for matches that cross its end)
                for (size_t i = 0; i < own && i + 8 <= n; i += 4) {
                    std::uint64_t v;
                    std::memcpy(&v, &buf[i], 8);
                    if (v == key0) {
                        if (i + kKeysLen > n || std::memcmp(&buf[i], kKeys[0], kKeysLen) || p + i < kKeysAt) continue;
                        uintptr_t obj = p + i - kKeysAt;
                        bool all = true;
                        for (int L = 1; L < kLayers && all; ++L) all = KeysAt(obj + kLayerOffset[L], kKeys[L]);
                        if (all && (bases.empty() || bases.back() != obj)) bases.push_back(obj);
                    } else if (v == glow0) {
                        if (i + 16 <= n && !std::memcmp(&buf[i], kGlowOrig, 16)) glows.push_back(p + i);
                    }
                }
                if (paint) PaintScanChunk(p, buf, key_rgbs, keys, filter, streams, hits);
            }
        }
        if (stop_) return;

        std::lock_guard<std::mutex> l(mu_);
        ++passes_;
        // drop copies the game freed, then add the new ones (originals read before any write)
        curves_.erase(std::remove_if(curves_.begin(), curves_.end(), [](const Curve& c) { return !KeysAt(c.addr, c.keys); }),
                      curves_.end());
        size_t added = 0, smoke = 0;
        for (uintptr_t b : bases) {
            if (std::any_of(curves_.begin(), curves_.end(), [b](const Curve& c) { return c.addr == b; })) continue;
            auto add = [&](uintptr_t addr, Slot s, const unsigned char* keys) {
                Curve c{addr, s, keys, {}, {}};
                // the search took seconds: the curve must still be there now, before its bytes count as the game's
                if (!KeysAt(addr, keys) || !ReadSelf(addr, c.orig, kObjSize) || !KeysAt(addr, keys)) return;
                for (int ch = 0; ch < 3; ++ch) std::memcpy(c.last[ch], c.orig + kBlockAt[ch], sizeof c.last[ch]);
                curves_.push_back(c);
                Write(curves_.back());
                ++added;
            };
            for (int L = 0; L < kLayers; ++L) add(b + kLayerOffset[L], kLayerSlot[L], kKeys[L]);
            for (int i = 0; i < 2; ++i)
                if (b > kSmokeBefore[i] && KeysAt(b - kSmokeBefore[i], kSmokeKeys[i])) { add(b - kSmokeBefore[i], kSmoke, kSmokeKeys[i]); ++smoke; }
        }
        sets_ = bases.size();
        size_t new_glows = 0;
        for (uintptr_t a : glows)
            if (std::find(glows_.begin(), glows_.end(), a) == glows_.end()) { glows_.push_back(a); ++new_glows; }
        ApplyGlowLocked();
        if (paint) ProcessPaintLocked(streams, hits, key_rgbs);
        if (new_glows) Logger::Instance().Info("underglow: " + std::to_string(new_glows) + " new copies, " + std::to_string(glows_.size()) + " tracked");
        if (added || passes_ == 1)
            Logger::Instance().Info("nitro search: " + std::to_string(bases.size()) + " flame copies, " + std::to_string(added) +
                                    " curves added (" + std::to_string(smoke) + " smoke), " + std::to_string(curves_.size()) +
                                    " tracked, " + std::to_string(GetTickCount64() - t0) + " ms");
    }

    std::mutex mu_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::vector<Curve> curves_;
    size_t sets_ = 0;
    int passes_ = 0;
    bool want_on_[kSlots] = {};
    float brightness_ = 1.0f;
    Rgba want_[kSlots];
    std::vector<uintptr_t> glows_;
    bool glow_on_ = false;
    Rgba glow_;
    unsigned char glow_written_[16] = {};
    bool kart_on_ = false, kart_have_orig_ = false;
    Rgb kart_want_[3] = {}, kart_orig_[3] = {}, kart_written_[3] = {};
    bool fast_ok_ = false;
    uintptr_t fast_c_[3] = {};                             // direct path: the 3 colour addresses of the shown kart
    uintptr_t fast_r_[3] = {};                             // and the start of each slot's colour ramp (0 = none)
    uint64_t fast_sig_ = 0;                                // the pointer before the colours of the shown kart
    Rgb ramp_written_[3][kPaintLayers - 1] = {};
    Rgb ramp_want_[3][kPaintLayers - 1] = {}, ramp_orig_[3][kPaintLayers - 1] = {};
    std::vector<std::pair<uintptr_t, int>> kart_copies_;   // (address, slot) of the kart's live paint copies
    std::vector<std::vector<Rgb>> schemes_;                // scheme colours seen in the last pass
    std::vector<std::array<Rgb, 3>> candidates_;           // paint jobs with a live copy group (last pass)
    std::mutex paint_mu_;
    std::vector<std::pair<uintptr_t, std::array<unsigned char, 16>>> paint_orig_;
    std::string paint_status_ = "not run";
    std::atomic<bool> paint_busy_{false};
};

}  // namespace

IVisualBackend* CreateGuestBackend() { return new GuestBackend; }

}  // namespace astro
