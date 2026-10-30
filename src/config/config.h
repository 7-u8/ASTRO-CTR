#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "../common/color.h"

namespace astro {

constexpr int kConfigVersion = 1;

// Nitro flame (ctr_kart_exhaust_fire_1 and its black-smoke variant). The game uses the same curves for the normal
// and the blue boost (seen in game 2026-10-30), so these colours apply to both. color = curves 1+4, color2 = curve 2,
// color3 = curve 3, smoke = the variant's two smoke curves. Defaults are the game's own hues.
struct NitroNormal {
    bool enabled = false;
    Rgba color{255, 91, 36, 255};    // orange
    Rgba color2{255, 12, 12, 255};   // red
    Rgba color3{255, 6, 197, 255};   // pink
    Rgba smoke{255, 102, 102, 255};  // black-smoke variant: dark red smoke
    float brightness = 2.5f;          // x the original flame brightness (1 = as dim as the game's own curves)
};
// primary_custom / secondary_custom: false after a per-colour reset -> that slot keeps the game's original
// color even while the blue-nitro override is enabled (the other slot is unaffected).
struct NitroBlue {
    bool enabled = false;
    Rgba primary{255, 255, 255, 255};
    Rgba secondary{255, 255, 255, 255};
    bool primary_custom = true;
    bool secondary_custom = true;
};
struct Champion { bool enabled = false; Rgba lower_region{255, 255, 255, 255}; };
// The player's kart paint: 3 slots of the paint job on screen (slot order of the KartColorScheme materials:
// 1 = main body, 2 and 3 = trim), each with 7 layers: 0 = tint (the sheen), 1..6 = the colour ramp that makes the
// base colour (verified in game 2026-10-30). Defaults are used only until the mod has read the kart's own colours.
constexpr int kPaintSlots = 3, kPaintLayers = 7;
struct KartPaint {
    bool enabled = false;
    Rgba layer[kPaintSlots][kPaintLayers] = {};
};
// Animated colours: ids of the colours that cycle through the spectrum (comma separated, e.g. "n1,glow,kart").
struct Effects {
    std::string rainbow;
    float rainbow_speed = 0.2f;   // cycles per second
};
struct UiSettings { float scale = 1.0f; bool debug_logging = false; };

struct Config {
    int version = kConfigVersion;
    NitroNormal normal;
    NitroBlue blue;
    Champion champion;
    KartPaint paint;
    Effects fx;
    UiSettings ui;
};

struct LoadResult {
    Config config;
    bool file_found = false;
    bool recovered_from_error = false;   // file existed but was unreadable/malformed -> defaults used
    std::vector<std::string> warnings;   // per-field problems (field fell back to its default)
};

// %APPDATA%\ASTRO-CTR\config.json (kept outside the game tree). Falls back to the temp dir.
std::filesystem::path DefaultConfigPath();

// Parse config text. Never throws; malformed input yields defaults + warnings.
LoadResult ParseConfig(const std::string& text);
std::string SerializeConfig(const Config& c);

LoadResult LoadConfig(const std::filesystem::path& path);
// Atomic: writes <path>.tmp then renames over <path>. Returns false and fills *error on failure.
bool SaveConfig(const std::filesystem::path& path, const Config& c, std::string* error = nullptr);

}  // namespace astro
