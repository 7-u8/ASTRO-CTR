#include "paint_patterns.h"

#include <algorithm>
#include <cmath>

namespace astro {

Hsv ToHsv(const Rgba& c) {
    const float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    float h = 0;
    if (d > 0) {
        if (mx == r) h = std::fmod((g - b) / d, 6.f);
        else if (mx == g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h /= 6;
        if (h < 0) h += 1;
    }
    return {h, mx > 0 ? d / mx : 0, mx};
}

Rgba FromHsv(float h, float s, float v) {
    h -= std::floor(h);
    s = std::clamp(s, 0.f, 1.f);
    v = std::clamp(v, 0.f, 1.f);
    const float c = v * s, x = c * (1 - std::fabs(std::fmod(h * 6, 2.f) - 1)), m = v - c;
    float r, g, b;
    switch (int(h * 6) % 6) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    auto q = [m](float f) { return std::uint8_t(std::lround(std::clamp(f + m, 0.f, 1.f) * 255)); };
    return Rgba{q(r), q(g), q(b), 255};
}

Rgba Mix(const Rgba& a, const Rgba& b, float t) {
    auto m = [t](std::uint8_t x, std::uint8_t y) { return std::uint8_t(std::lround(x + (y - x) * t)); };
    return Rgba{m(a.r, b.r), m(a.g, b.g), m(a.b, b.b), 255};
}

Rgba Scale(const Rgba& a, float k) {
    auto m = [k](std::uint8_t x) { return std::uint8_t(std::clamp(std::lround(x * k), 0L, 255L)); };
    return Rgba{m(a.r), m(a.g), m(a.b), 255};
}

const char* const kPatterns[kPatternCount] = {"Solid", "Glow", "Icy", "Metal", "Duo-tone", "Pastel", "Dark", "Rainbow layers"};

void MakePattern(int p, const Rgba& c, Rgba out[kPaintLayers]) {
    const Hsv h = ToHsv(c);
    const Rgba white{255, 255, 255, 255}, black{0, 0, 0, 255};
    switch (p) {
    default:   // Solid
        for (int i = 0; i < kPaintLayers; ++i) out[i] = c;
        break;
    case 1: {  // Glow: deep base, bright highlights, a shifted glow
        const Rgba elec = FromHsv(h.h + 0.08f, 0.85f, 1.0f);
        Rgba r[] = {elec, Scale(c, 0.35f), c, white, elec, Scale(c, 0.6f), FromHsv(h.h - 0.05f, 1.0f, 1.0f)};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 2: {  // Icy: pale, glassy
        Rgba r[] = {white, Mix(c, white, 0.55f), Mix(c, white, 0.8f), Mix(c, white, 0.35f), white, Mix(c, white, 0.65f),
                    Mix(FromHsv(h.h + 0.1f, 0.4f, 1.0f), white, 0.4f)};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 3: {  // Metal: greys with a hint of the colour
        auto g = [&](float v) { return Mix(Rgba{std::uint8_t(v * 255), std::uint8_t(v * 255), std::uint8_t(v * 255), 255}, c, 0.18f); };
        Rgba r[] = {white, g(0.25f), g(0.95f), g(0.5f), g(1.0f), g(0.3f), g(0.75f)};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 4: {  // Duo-tone: saturated colour with its complement
        const Rgba sat = FromHsv(h.h, 1.0f, 1.0f), comp = FromHsv(h.h + 0.5f, 1.0f, 1.0f);
        Rgba r[] = {comp, sat, sat, comp, sat, comp, sat};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 5: {  // Pastel: white with soft shifts
        Rgba r[] = {white, white, Mix(c, white, 0.7f), Mix(FromHsv(h.h + 0.08f, 0.5f, 1), white, 0.6f), white,
                    Mix(FromHsv(h.h - 0.08f, 0.5f, 1), white, 0.6f), Mix(c, white, 0.85f)};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 6: {  // Dark: dark base with bright colour and magenta
        Rgba r[] = {FromHsv(h.h + 0.15f, 1, 1), Mix(black, c, 0.25f), c, FromHsv(0.83f, 0.9f, 0.9f), black, Scale(c, 0.6f), white};
        std::copy(r, r + kPaintLayers, out);
        break;
    }
    case 7:    // Rainbow layers: the colour wheel across the layers, starting at the chosen colour
        for (int i = 0; i < kPaintLayers; ++i) out[i] = FromHsv(h.h + i / float(kPaintLayers), std::max(h.s, 0.7f), std::max(h.v, 0.8f));
        break;
    }
}

}  // namespace astro
