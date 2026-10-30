#include "config.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

#include "json.h"

namespace astro {
namespace {

using json::Value;

void ReadBool(const Value* parent, const char* key, bool& out, const std::string& path, LoadResult& r) {
    if (!parent) return;
    const Value* v = parent->Find(key);
    if (!v) return;
    if (v->type != Value::Type::Bool) { r.warnings.push_back(path + "." + key + ": expected boolean, using default"); return; }
    out = v->b;
}

void ReadColor(const Value* parent, const char* key, Rgba& out, const std::string& path, LoadResult& r) {
    if (!parent) return;
    const Value* v = parent->Find(key);
    if (!v) return;
    if (v->type != Value::Type::String) { r.warnings.push_back(path + "." + key + ": expected color string, using default"); return; }
    auto c = ParseHexColor(v->s);
    if (!c) { r.warnings.push_back(path + "." + key + ": invalid color '" + v->s + "', using default"); return; }
    out = *c;
}

const Value* Section(const Value* parent, const char* key, const std::string& path, LoadResult& r) {
    if (!parent) return nullptr;
    const Value* v = parent->Find(key);
    if (!v) return nullptr;
    if (v->type != Value::Type::Object) { r.warnings.push_back(path + ": expected object, using defaults"); return nullptr; }
    return v;
}

Value ColorValue(const Rgba& c) { return Value::String(FormatHexColor(c)); }

}  // namespace

std::filesystem::path DefaultConfigPath() {
    const char* appdata = std::getenv("APPDATA");
    std::filesystem::path base = appdata && *appdata ? std::filesystem::path(appdata) : std::filesystem::temp_directory_path();
    return base / "ASTRO-CTR" / "config.json";
}

LoadResult ParseConfig(const std::string& text) {
    LoadResult r;
    std::string err;
    auto root = json::Parse(text, &err);
    if (!root || root->type != Value::Type::Object) {
        r.recovered_from_error = true;
        r.warnings.push_back("config is not valid JSON (" + (err.empty() ? std::string("not an object") : err) + "); defaults used");
        return r;
    }
    if (const Value* ver = root->Find("version")) {
        if (ver->type == Value::Type::Number) {
            r.config.version = static_cast<int>(ver->n);
            if (r.config.version > kConfigVersion)
                r.warnings.push_back("config version " + std::to_string(r.config.version) + " is newer than supported " +
                                     std::to_string(kConfigVersion) + "; unknown fields are ignored");
        }
    }
    r.config.version = kConfigVersion;  // re-saved in the current schema

    const Value* nitro = Section(&*root, "nitro", "nitro", r);
    const Value* normal = Section(nitro, "normal", "nitro.normal", r);
    ReadBool(normal, "enabled", r.config.normal.enabled, "nitro.normal", r);
    ReadColor(normal, "color", r.config.normal.color, "nitro.normal", r);
    ReadColor(normal, "color2", r.config.normal.color2, "nitro.normal", r);
    ReadColor(normal, "color3", r.config.normal.color3, "nitro.normal", r);
    ReadColor(normal, "smoke", r.config.normal.smoke, "nitro.normal", r);
    if (normal) if (const Value* b = normal->Find("brightness")) {
        if (b->type == Value::Type::Number && b->n >= 0.25 && b->n <= 8.0) r.config.normal.brightness = static_cast<float>(b->n);
        else r.warnings.push_back("nitro.normal.brightness: must be a number in [0.25, 8], using default");
    }
    const Value* blue = Section(nitro, "blue", "nitro.blue", r);
    ReadBool(blue, "enabled", r.config.blue.enabled, "nitro.blue", r);
    ReadColor(blue, "primary", r.config.blue.primary, "nitro.blue", r);
    ReadColor(blue, "secondary", r.config.blue.secondary, "nitro.blue", r);
    ReadBool(blue, "primary_custom", r.config.blue.primary_custom, "nitro.blue", r);
    ReadBool(blue, "secondary_custom", r.config.blue.secondary_custom, "nitro.blue", r);

    const Value* champ = Section(&*root, "champion", "champion", r);
    ReadBool(champ, "enabled", r.config.champion.enabled, "champion", r);
    ReadColor(champ, "lower_region_color", r.config.champion.lower_region, "champion", r);

    const Value* paint = Section(&*root, "kart_paint", "kart_paint", r);
    ReadBool(paint, "enabled", r.config.paint.enabled, "kart_paint", r);
    for (int s = 0; s < kPaintSlots; ++s)
        for (int l = 0; l < kPaintLayers; ++l)
            ReadColor(paint, ("slot" + std::to_string(s + 1) + "_layer" + std::to_string(l)).c_str(), r.config.paint.layer[s][l], "kart_paint", r);

    const Value* fx = Section(&*root, "effects", "effects", r);
    if (fx) {
        if (const Value* v = fx->Find("rainbow"); v && v->type == Value::Type::String) r.config.fx.rainbow = v->s;
        if (const Value* v = fx->Find("rainbow_speed"); v && v->type == Value::Type::Number && v->n > 0.0 && v->n <= 5.0)
            r.config.fx.rainbow_speed = static_cast<float>(v->n);
    }

    const Value* ui = Section(&*root, "ui", "ui", r);
    ReadBool(ui, "debug_logging", r.config.ui.debug_logging, "ui", r);
    if (ui) if (const Value* s = ui->Find("scale")) {
        if (s->type == Value::Type::Number && s->n >= 0.5 && s->n <= 3.0) r.config.ui.scale = static_cast<float>(s->n);
        else r.warnings.push_back("ui.scale: must be a number in [0.5, 3.0], using default");
    }
    return r;
}

std::string SerializeConfig(const Config& c) {
    Value root = Value::MakeObject();
    root["version"] = Value::Number(kConfigVersion);
    Value nitro = Value::MakeObject();
    Value normal = Value::MakeObject();
    normal["enabled"] = Value::Bool(c.normal.enabled);
    normal["color"] = ColorValue(c.normal.color);
    normal["color2"] = ColorValue(c.normal.color2);
    normal["color3"] = ColorValue(c.normal.color3);
    normal["smoke"] = ColorValue(c.normal.smoke);
    normal["brightness"] = Value::Number(c.normal.brightness);
    Value blue = Value::MakeObject();
    blue["enabled"] = Value::Bool(c.blue.enabled);
    blue["primary"] = ColorValue(c.blue.primary);
    blue["secondary"] = ColorValue(c.blue.secondary);
    blue["primary_custom"] = Value::Bool(c.blue.primary_custom);
    blue["secondary_custom"] = Value::Bool(c.blue.secondary_custom);
    nitro["normal"] = normal;
    nitro["blue"] = blue;
    root["nitro"] = nitro;
    Value champ = Value::MakeObject();
    champ["enabled"] = Value::Bool(c.champion.enabled);
    champ["lower_region_color"] = ColorValue(c.champion.lower_region);
    root["champion"] = champ;
    Value paint = Value::MakeObject();
    paint["enabled"] = Value::Bool(c.paint.enabled);
    for (int s = 0; s < kPaintSlots; ++s)
        for (int l = 0; l < kPaintLayers; ++l)
            paint["slot" + std::to_string(s + 1) + "_layer" + std::to_string(l)] = ColorValue(c.paint.layer[s][l]);
    root["kart_paint"] = paint;
    Value fx = Value::MakeObject();
    fx["rainbow"] = Value::String(c.fx.rainbow);
    fx["rainbow_speed"] = Value::Number(c.fx.rainbow_speed);
    root["effects"] = fx;
    Value ui = Value::MakeObject();
    ui["scale"] = Value::Number(c.ui.scale);
    ui["debug_logging"] = Value::Bool(c.ui.debug_logging);
    root["ui"] = ui;
    return json::Serialize(root, 2) + "\n";
}

LoadResult LoadConfig(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return LoadResult{};  // first run: defaults, not an error
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LoadResult r;
        r.file_found = true;
        r.recovered_from_error = true;
        r.warnings.push_back("config file exists but cannot be opened; defaults used");
        return r;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    LoadResult r = ParseConfig(ss.str());
    r.file_found = true;
    return r;
}

bool SaveConfig(const std::filesystem::path& path, const Config& c, std::string* error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { if (error) *error = "cannot write " + tmp.string(); return false; }
        out << SerializeConfig(c);
        out.flush();
        if (!out) { if (error) *error = "write failed for " + tmp.string(); fs::remove(tmp, ec); return false; }
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
        if (ec) { if (error) *error = "rename failed: " + ec.message(); fs::remove(tmp, ec); return false; }
    }
    return true;
}

}  // namespace astro
