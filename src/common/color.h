#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace astro {

struct Rgba {
    std::uint8_t r = 255, g = 255, b = 255, a = 255;
    bool operator==(const Rgba& o) const { return r == o.r && g == o.g && b == o.b && a == o.a; }
};

// Accepts "#RRGGBB" or "#RRGGBBAA" (leading '#' optional). Returns nullopt on anything else.
std::optional<Rgba> ParseHexColor(const std::string& text);
// Always "#RRGGBBAA", upper-case.
std::string FormatHexColor(const Rgba& c);

}  // namespace astro
