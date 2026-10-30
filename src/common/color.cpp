#include "color.h"

#include <cstdio>

namespace astro {

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<Rgba> ParseHexColor(const std::string& text) {
    std::size_t i = (!text.empty() && text[0] == '#') ? 1 : 0;
    const std::size_t n = text.size() - i;
    if (n != 6 && n != 8) return std::nullopt;
    std::uint8_t v[4] = {0, 0, 0, 255};
    for (std::size_t k = 0; k < n / 2; ++k) {
        int hi = HexVal(text[i + 2 * k]), lo = HexVal(text[i + 2 * k + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        v[k] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return Rgba{v[0], v[1], v[2], v[3]};
}

std::string FormatHexColor(const Rgba& c) {
    char buf[12];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
    return buf;
}

}  // namespace astro
