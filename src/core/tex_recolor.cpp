#include "tex_recolor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "paint_patterns.h"

namespace astro {

namespace {

constexpr float kColoured = 0.4f;   // saturation above which a colour counts as a coloured part (not grey)

void Unpack565(std::uint16_t c, std::uint8_t rgb[3]) {
    rgb[0] = std::uint8_t(((c >> 11) & 31) * 255 / 31);
    rgb[1] = std::uint8_t(((c >> 5) & 63) * 255 / 63);
    rgb[2] = std::uint8_t((c & 31) * 255 / 31);
}
std::uint16_t Pack565(const std::uint8_t rgb[3]) {
    return std::uint16_t(((rgb[0] * 31 + 127) / 255) << 11 | ((rgb[1] * 63 + 127) / 255) << 5 | ((rgb[2] * 31 + 127) / 255));
}

}  // namespace

void RecolorRgb(const RecolorParams& p, const std::uint8_t in[3], std::uint8_t out[3]) {
    const Rgba src{in[0], in[1], in[2], 255};
    const Hsv h = ToHsv(src), t = ToHsv(p.target);
    Rgba n = src;
    if (h.s > kColoured && h.v > 0.05f) {
        // coloured part: the new hue, saturation relative to the original, the same lightness
        if (p.colours) n = FromHsv(t.h, std::min(1.f, h.s * (0.35f + 0.65f * t.s)), h.v * (0.4f + 0.6f * t.v));
    } else if (p.greys) {
        // grey part: tinted by the grey colour, keeping its lightness
        const Hsv g = ToHsv(p.grey_target);
        auto m = [&](std::uint8_t a, std::uint8_t b) { return std::uint8_t(std::lround(a * (b / 255.f))); };
        n = Rgba{m(src.r, p.grey_target.r), m(src.g, p.grey_target.g), m(src.b, p.grey_target.b), 255};
        const float lift = h.v / std::max(1e-3f, ToHsv(n).v);   // a tint of white keeps white's brightness
        n = Scale(n, std::min(lift, 1.0f / std::max(1e-3f, g.v)));
    }
    if (p.blacks) {   // the darker the pixel, the more it becomes the chosen colour (full colour at black, none from 35% brightness up)
        const float dark = 1.f - std::min(1.f, h.v / 0.35f);
        if (dark > 0.f) n = Mix(n, p.black_target, dark);
    }
    n = Mix(src, n, std::clamp(p.strength, 0.f, 1.f));
    n = Scale(n, p.brightness);
    out[0] = n.r;
    out[1] = n.g;
    out[2] = n.b;
}

namespace {
// RGB565 has only 65536 values: the recoloured value of each is worked out once per set of parameters (a table) instead of
// two HSV conversions per block. A 2048x2048 snow texture has ~350 thousand blocks, a map has dozens of them: dragging a
// colour took seconds before this.
struct Lut {
    RecolorParams p;
    bool valid = false;
    std::uint16_t v[65536];
};
const std::uint16_t* ColourTable(const RecolorParams& p) {
    static thread_local Lut lut;
    auto same = [](const RecolorParams& a, const RecolorParams& b) {
        return a.target == b.target && a.grey_target == b.grey_target && a.strength == b.strength && a.brightness == b.brightness &&
               a.colours == b.colours && a.greys == b.greys && a.blacks == b.blacks && a.black_target == b.black_target;
    };
    if (!lut.valid || !same(lut.p, p)) {
        for (std::uint32_t c = 0; c < 65536; ++c) {
            std::uint8_t rgb[3];
            Unpack565(std::uint16_t(c), rgb);
            RecolorRgb(p, rgb, rgb);
            lut.v[c] = Pack565(rgb);
        }
        lut.p = p;
        lut.valid = true;
    }
    return lut.v;
}

// one 8-byte colour block (BC1, or the colour half of a BC3 block: BC3 always has 4 colours whatever the end colours' order)
void RecolorColourBlock(const std::uint16_t* lut, const std::uint8_t* in, std::uint8_t* out, bool bc3) {
    {
        std::uint16_t c0, c1;
        std::uint32_t bits;
        std::memcpy(&c0, in, 2);
        std::memcpy(&c1, in + 2, 2);
        std::memcpy(&bits, in + 4, 4);
        const bool four = bc3 || c0 > c1;   // BC1: 4-colour block when c0 > c1, else 3 colours + transparent
        std::uint16_t n0 = lut[c0], n1 = lut[c1];
        // keep the block's mode: the order of the end colours selects it
        if (four) {
            if (n0 < n1) { std::swap(n0, n1); bits ^= 0x55555555u; }   // indices 0<->1, 2<->3
            else if (n0 == n1) { if (n0 < 0xffff) ++n0; else --n1; }
        } else if (n0 > n1) {
            std::swap(n0, n1);
            for (int k = 0; k < 16; ++k)   // indices 0<->1; 2 (the middle) and 3 (transparent) stay
                if (((bits >> (2 * k)) & 3) < 2) bits ^= 1u << (2 * k);
        }
        std::memcpy(out, &n0, 2);
        std::memcpy(out + 2, &n1, 2);
        std::memcpy(out + 4, &bits, 4);
    }
}
}  // namespace

void RecolorBc1(const RecolorParams& p, const std::uint8_t* in, std::uint8_t* out, size_t len) {
    const std::uint16_t* lut = ColourTable(p);
    for (size_t i = 0; i + 8 <= len; i += 8) RecolorColourBlock(lut, in + i, out + i, false);
}

void RecolorBc3(const RecolorParams& p, const std::uint8_t* in, std::uint8_t* out, size_t len) {
    const std::uint16_t* lut = ColourTable(p);
    for (size_t i = 0; i + 16 <= len; i += 16) {
        std::memcpy(out + i, in + i, 8);   // the 8 alpha bytes stay as they are
        RecolorColourBlock(lut, in + i + 8, out + i + 8, true);
    }
}

void RecolorRgba8(const RecolorParams& p, const std::uint8_t* in, std::uint8_t* out, size_t len) {
    for (size_t i = 0; i + 4 <= len; i += 4) {
        RecolorRgb(p, in + i, out + i);
        out[i + 3] = in[i + 3];
    }
}

}  // namespace astro
