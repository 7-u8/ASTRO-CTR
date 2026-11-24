#include "presets.h"

#include <algorithm>
#include <cctype>

namespace astro {

namespace fs = std::filesystem;

fs::path PresetsDir() { return DefaultConfigPath().parent_path() / "presets"; }

std::vector<PresetInfo> ListPresets() {
    std::vector<PresetInfo> out;
    std::error_code ec;
    for (fs::directory_iterator it(PresetsDir(), ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".json")
            out.push_back({it->path().stem().string(), it->path()});
    std::sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) { return a.name < b.name; });
    return out;
}

namespace {

std::string SafeName(const std::string& name) {
    std::string s;
    for (char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')') s += c;
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s.empty() ? "Preset" : s.substr(0, 48);
}

fs::path FreePath(const std::string& name) {
    const std::string base = SafeName(name);
    fs::path p = PresetsDir() / (base + ".json");
    std::error_code ec;
    for (int i = 2; fs::exists(p, ec); ++i) p = PresetsDir() / (base + " (" + std::to_string(i) + ").json");
    return p;
}

}  // namespace

fs::path SavePreset(const std::string& name, const Config& c, std::string* error) {
    const fs::path p = FreePath(name);
    return SaveConfig(p, c, error) ? p : fs::path();
}

fs::path ImportPreset(const fs::path& from, std::string* error) {
    LoadResult r = LoadConfig(from);
    if (!r.file_found || r.recovered_from_error) {
        if (error) *error = "not a valid ASTRO preset (JSON could not be read)";
        return {};
    }
    return SavePreset(from.stem().string(), r.config, error);
}

}  // namespace astro
