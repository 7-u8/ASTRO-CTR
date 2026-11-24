#pragma once
// Effects editor: how the ASTRO app and the layer inside the game talk (files in %APPDATA%\ASTRO-CTR + events).
//   app -> layer : fx_edits.json  {"edits": {"<item id>": [r, g, b, a], ...}}  then event kFxApplyEvent
//   app -> layer : event kFxScanEvent (list the effects now)
//   layer -> app : effects.json   {"items": [{"id", "names": [...], "orig": [r, g, b, a], "count"}]}  then kFxScannedEvent
// An item = every live colour operator with the same original colour in the same effect(s). r g b are 0..1 floats
// (may exceed 1 for bright effects), a is the effect's own 4th value (alpha or intensity).
#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "config.h"

namespace astro::fx {

inline constexpr wchar_t kFxScanEvent[] = L"Local\\ASTRO_CTR_FxScan";
inline constexpr wchar_t kFxApplyEvent[] = L"Local\\ASTRO_CTR_FxApply";
inline constexpr wchar_t kFxScannedEvent[] = L"Local\\ASTRO_CTR_FxScanned";

using Color4 = std::array<float, 4>;

struct Item {
    std::string id;
    std::vector<std::string> names;   // effect file names this colour belongs to (best guess first)
    Color4 orig{};
    int count = 0;                     // live copies
};

inline std::filesystem::path Dir() { return DefaultConfigPath().parent_path(); }
inline std::filesystem::path EffectsPath() { return Dir() / "effects.json"; }
inline std::filesystem::path EditsPath() { return Dir() / "fx_edits.json"; }

bool SaveItems(const std::vector<Item>& items);
std::vector<Item> LoadItems();
bool SaveEdits(const std::map<std::string, Color4>& edits);
std::map<std::string, Color4> LoadEdits();

}  // namespace astro::fx
