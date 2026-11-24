#include "wheel_protocol.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include "json.h"

namespace astro::wheel {

namespace {

using json::Value;

std::optional<Value> ReadJson(const std::filesystem::path& p) {
    // opened so that the writer (the other program) can still replace the file while it is being read: with the default
    // sharing the replace failed and the change was lost (a colour that stayed on its old value, 2026-11-01)
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    std::string s;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got) s.append(buf, got);
    CloseHandle(h);
    return json::Parse(s);
}
bool WriteAtomic(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!(out << text)) return false;
    }
    // a reader may hold the file for a moment: try again instead of dropping the change
    for (int i = 0; i < 40; ++i) {
        if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(10);
    }
    return false;
}
std::string Str(const Value& o, const char* k) {
    const Value* v = o.Find(k);
    return v && v->type == Value::Type::String ? v->s : std::string();
}
double Num(const Value& o, const char* k, double d = 0) {
    const Value* v = o.Find(k);
    return v && v->type == Value::Type::Number ? v->n : d;
}
bool Bool(const Value& o, const char* k, bool d = false) {
    const Value* v = o.Find(k);
    return v && v->type == Value::Type::Bool ? v->b : d;
}
std::string Unhex(const std::string& h) {
    std::string out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out += char(std::stoi(h.substr(i, 2), nullptr, 16));
    return out;
}
Value Array() { Value a; a.type = Value::Type::Array; a.a = std::make_shared<json::Array>(); return a; }

}  // namespace

std::vector<Wheel> LoadCatalog(const std::filesystem::path& file) {
    std::vector<Wheel> out;
    auto root = ReadJson(file);
    const Value* ws = root ? root->Find("wheels") : nullptr;
    if (!ws || ws->type != Value::Type::Array || !ws->a) return out;
    for (const Value& w : *ws->a) {
        Wheel wh;
        wh.name = Str(w, "name");
        wh.display = Str(w, "display");
        if (const Value* t = w.Find("textures"); t && t->type == Value::Type::Array && t->a)
            for (const Value& x : *t->a) {
                Texture tx;
                tx.name = Str(x, "name");
                tx.glow = Str(x, "kind") == "glow";
                tx.wheel = Bool(x, "wheel", true);
                tx.rgba = Str(x, "fmt") == "rgba8";
                tx.bc3 = Str(x, "fmt") == "bc3";
                tx.group = Str(x, "group");
                tx.w = int(Num(x, "w"));
                tx.h = int(Num(x, "h"));
                tx.len = size_t(Num(x, "len"));
                tx.head = Unhex(Str(x, "head"));
                tx.probe_off = size_t(Num(x, "probe_off"));
                tx.probe = Unhex(Str(x, "probe"));
                if (const Value* pr = x.Find("probes"); pr && pr->type == Value::Type::Array && pr->a)
                    for (const Value& e : *pr->a) tx.probes.push_back({size_t(Num(e, "o")), Unhex(Str(e, "h"))});
                tx.fnv = std::stoull(Str(x, "fnv").empty() ? "0" : Str(x, "fnv"), nullptr, 16);
                { char b[17]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)tx.fnv); tx.id = tx.name + "@" + b; }
                if (tx.len && tx.fnv && tx.head.size() == 16 && tx.probe.size() == 64 && tx.probes.size() >= 8) wh.textures.push_back(std::move(tx));
            }
        if (const Value* t = w.Find("tints"); t && t->type == Value::Type::Array && t->a)
            for (const Value& x : *t->a) {
                Tint tn;
                tn.bytes = Unhex(Str(x, "c"));
                if (tn.bytes.size() != 16) continue;
                float f[4];
                std::memcpy(f, tn.bytes.data(), 16);
                auto q = [](float v) { return std::uint8_t(v <= 0 ? 0 : v >= 1 ? 255 : int(v * 255.f + 0.5f)); };
                tn.colour = Rgba{q(f[0]), q(f[1]), q(f[2]), 255};
                tn.id = "tint:" + Str(x, "c");
                wh.tints.push_back(std::move(tn));
            }
        if (const Value* e = w.Find("effects"); e && e->type == Value::Type::Array && e->a)
            for (const Value& x : *e->a) if (x.type == Value::Type::String) wh.effects.push_back(x.s);
        if (!wh.name.empty() && !wh.textures.empty()) out.push_back(std::move(wh));
    }
    return out;
}

bool SaveFound(const std::vector<std::string>& textures) {
    Value root = Value::MakeObject();
    Value a = Array();
    for (auto& t : textures) a.a->push_back(Value::String(t));
    root["textures"] = a;
    return WriteAtomic(FoundPath(), json::Serialize(root));
}

std::vector<std::string> LoadFound() {
    std::vector<std::string> out;
    auto root = ReadJson(FoundPath());
    const Value* a = root ? root->Find("textures") : nullptr;
    if (a && a->type == Value::Type::Array && a->a)
        for (const Value& v : *a->a) if (v.type == Value::Type::String) out.push_back(v.s);
    return out;
}

bool SaveEdits(const std::map<std::string, RecolorParams>& edits, const std::filesystem::path& file) {
    Value root = Value::MakeObject();
    for (auto& [name, p] : edits) {
        Value o = Value::MakeObject();
        o["color"] = Value::String(FormatHexColor(p.target));
        o["grey_color"] = Value::String(FormatHexColor(p.grey_target));
        o["colours"] = Value::Bool(p.colours);
        o["strength"] = Value::Number(p.strength);
        o["brightness"] = Value::Number(p.brightness);
        o["greys"] = Value::Bool(p.greys);
        o["blacks"] = Value::Bool(p.blacks);
        o["black_color"] = Value::String(FormatHexColor(p.black_target));
        root[name] = o;
    }
    return WriteAtomic(file, json::Serialize(root, 1));
}

std::map<std::string, RecolorParams> LoadEdits(const std::filesystem::path& file) {
    std::map<std::string, RecolorParams> out;
    auto root = ReadJson(file);
    if (!root || root->type != Value::Type::Object || !root->o) return out;
    for (auto& [name, o] : *root->o) {
        if (o.type != Value::Type::Object) continue;
        RecolorParams p;
        if (auto c = ParseHexColor(Str(o, "color"))) p.target = *c;
        p.grey_target = p.target;
        if (auto c = ParseHexColor(Str(o, "grey_color"))) p.grey_target = *c;
        p.colours = Bool(o, "colours", true);
        p.strength = float(Num(o, "strength", 1));
        p.brightness = float(Num(o, "brightness", 1));
        p.greys = Bool(o, "greys");
        p.blacks = Bool(o, "blacks");
        p.black_target = p.target;
        if (auto c = ParseHexColor(Str(o, "black_color"))) p.black_target = *c;
        out[name] = p;
    }
    return out;
}

}  // namespace astro::wheel
