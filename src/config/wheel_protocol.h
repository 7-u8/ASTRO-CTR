#pragma once
// Wheel Editor: the wheel catalogue (data/wheel_catalog.json, built by tools/wheel_catalog_build.py) and how the
// ASTRO app and the layer inside the game talk (files in %APPDATA%\ASTRO-CTR + events).
//   app -> layer : event kWheelScanEvent (find which wheel textures are loaded)
//   layer -> app : wheels_found.json {"textures": ["<texture id>", ...]}  then kWheelScannedEvent
//   app -> layer : wheel_edits.json {"<texture id>": {"color": "#RRGGBBAA", "strength", "brightness", "greys"}}
//                  then kWheelApplyEvent (a texture missing from the file goes back to the game's own colours)
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../core/tex_recolor.h"
#include "config.h"

namespace astro::wheel {

inline constexpr wchar_t kWheelScanEvent[] = L"Local\\ASTRO_CTR_WheelScan";
inline constexpr wchar_t kWheelApplyEvent[] = L"Local\\ASTRO_CTR_WheelApply";
inline constexpr wchar_t kWheelScannedEvent[] = L"Local\\ASTRO_CTR_WheelScanned";

struct Texture {
    std::string name;
    std::string id;            // name@fingerprint: variants of a wheel can share a texture name with different pixels
    bool glow = false;         // emissive mask (grey): its tint colours the glow
    bool wheel = true;         // false: a kart texture the wheel model also uses (editing it recolours the kart too)
    bool rgba = false;         // uncompressed RGBA8 (some skies: T152, T233, T241, T243) instead of BC1
    bool bc3 = false;          // BC3 (16-byte blocks: 8 alpha bytes + a BC1 colour block): most snow and ice textures
    std::string group;         // MAPS page: "snow" or "ice" (empty for wheels and skies)
    int w = 0, h = 0;
    size_t len = 0;            // BC1 payload bytes (all mips), as in game memory
    std::string head;          // first 16 bytes of the payload
    size_t probe_off = 0;
    std::string probe;         // 64 bytes at probe_off: a quick first check
    std::uint64_t fnv = 0;     // fingerprint of the whole payload (Fingerprint below): the final check
    std::vector<std::pair<size_t, std::string>> probes;   // (offset, 64 bytes): the texture is recognised when most match
};
// A colour in a wheel's customisation data that its effects use as tint (drift sparks, boost flare...): the game keeps
// copies of that data in memory, found by these 16 bytes (a vec4: r g b 1).
struct Tint {
    std::string bytes;         // 16 bytes
    std::string id;            // "tint:<hex of the bytes>": the key of its edit
    Rgba colour;
};
struct Wheel {
    std::string name;
    std::string display;       // the name the game shows (empty: a wheel this editor does not cover)
    std::vector<Texture> textures;
    std::vector<std::string> effects;   // effect files in the wheel's pack
    std::vector<Tint> tints;
};

std::vector<Wheel> LoadCatalog(const std::filesystem::path& file);

// FNV-1a over the payload's 64-bit words (same as tools/wheel_catalog_build.py)
inline std::uint64_t Fingerprint(const std::uint8_t* p, size_t len) {
    std::uint64_t h = 0xcbf29ce484222325ull, w;
    for (size_t i = 0; i + 8 <= len; i += 8) {
        __builtin_memcpy(&w, p + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    return h;
}

inline std::filesystem::path Dir() { return DefaultConfigPath().parent_path(); }
inline std::filesystem::path FoundPath() { return Dir() / "wheels_found.json"; }
inline std::filesystem::path EditsPath() { return Dir() / "wheel_edits.json"; }
// MAPS page: the sky textures (data/sky_catalog.json, built by tools/sky_catalog_build.py) go through the same texture
// engine; their edits have their own file so the Wheel Editor and the MAPS page never overwrite each other's.
inline std::filesystem::path SkyEditsPath() { return Dir() / "sky_edits.json"; }
// MAPS page: snow and ice textures (data/snow_catalog.json, tools/snow_catalog_build.py), same engine, own edits file
inline std::filesystem::path SnowEditsPath() { return Dir() / "snow_edits.json"; }
// MAPS page: speed pad textures (data/pad_catalog.json, tools/pad_catalog_build.py)
inline std::filesystem::path PadEditsPath() { return Dir() / "pad_edits.json"; }
// MAPS page: lava, fire and volcanic rock textures (data/lava_catalog.json, tools/lava_catalog_build.py)
inline std::filesystem::path LavaEditsPath() { return Dir() / "lava_edits.json"; }
// MAPS page: Roo's Tubes ground (sand/mud), water and red light textures (data/roo_catalog.json, tools/roo_catalog_build.py)
inline std::filesystem::path RooEditsPath() { return Dir() / "roo_edits.json"; }
// MAPS page: the colour textures of every driver (data/driver_catalog.json, tools/driver_catalog_build.py)
inline std::filesystem::path DriverEditsPath() { return Dir() / "driver_edits.json"; }
// ITEMS page: TNT / Nitro crate, Aku Aku and Uka Uka models (data/items_catalog.json, tools/items_catalog_build.py)
inline std::filesystem::path ItemsEditsPath() { return Dir() / "items_edits.json"; }

bool SaveFound(const std::vector<std::string>& textures);
std::vector<std::string> LoadFound();
bool SaveEdits(const std::map<std::string, RecolorParams>& edits, const std::filesystem::path& file = EditsPath());
std::map<std::string, RecolorParams> LoadEdits(const std::filesystem::path& file = EditsPath());

}  // namespace astro::wheel
