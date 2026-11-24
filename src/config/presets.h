#pragma once
// Presets: complete colour setups saved as JSON files in %APPDATA%\ASTRO-CTR\presets (same format as config.json).
// The ASTRO app and the in-game menu share this folder, so a preset exported in one shows up in the other.
#include <filesystem>
#include <string>
#include <vector>

#include "config.h"

namespace astro {

// Named event the app sets after it wrote config.json: the in-game menu reloads the file and applies it.
inline constexpr wchar_t kConfigPushedEvent[] = L"Local\\ASTRO_CTR_ConfigPushed";

struct PresetInfo {
    std::string name;              // file name without .json
    std::filesystem::path path;
};

std::filesystem::path PresetsDir();
std::vector<PresetInfo> ListPresets();   // sorted by name

// Saves under a free file name (never overwrites another preset): "<name>.json", "<name> (2).json", ...
// Returns the path written, empty on failure (*error filled).
std::filesystem::path SavePreset(const std::string& name, const Config& c, std::string* error = nullptr);

// Copies an outside JSON file into the presets folder after checking it is a valid ASTRO config.
std::filesystem::path ImportPreset(const std::filesystem::path& from, std::string* error = nullptr);

}  // namespace astro
