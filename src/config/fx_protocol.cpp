#include "fx_protocol.h"

#include <fstream>
#include <sstream>

#include "json.h"

namespace astro::fx {

namespace {

using json::Value;

Value Num4(const Color4& c) {
    Value a;
    a.type = Value::Type::Array;
    a.a = std::make_shared<json::Array>();
    for (float f : c) a.a->push_back(Value::Number(f));
    return a;
}
bool Get4(const Value* v, Color4& out) {
    if (!v || v->type != Value::Type::Array || !v->a || v->a->size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        if ((*v->a)[i].type != Value::Type::Number) return false;
        out[i] = float((*v->a)[i].n);
    }
    return true;
}
std::optional<Value> ReadJson(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return json::Parse(ss.str());
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
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

}  // namespace

bool SaveItems(const std::vector<Item>& items) {
    Value root = Value::MakeObject();
    Value arr;
    arr.type = Value::Type::Array;
    arr.a = std::make_shared<json::Array>();
    for (const Item& it : items) {
        Value o = Value::MakeObject();
        o["id"] = Value::String(it.id);
        Value names;
        names.type = Value::Type::Array;
        names.a = std::make_shared<json::Array>();
        for (auto& n : it.names) names.a->push_back(Value::String(n));
        o["names"] = names;
        o["orig"] = Num4(it.orig);
        o["count"] = Value::Number(it.count);
        arr.a->push_back(o);
    }
    root["items"] = arr;
    return WriteAtomic(EffectsPath(), json::Serialize(root));
}

std::vector<Item> LoadItems() {
    std::vector<Item> out;
    auto root = ReadJson(EffectsPath());
    const Value* items = root ? root->Find("items") : nullptr;
    if (!items || items->type != Value::Type::Array || !items->a) return out;
    for (const Value& v : *items->a) {
        Item it;
        const Value* id = v.Find("id");
        if (!id || id->type != Value::Type::String || !Get4(v.Find("orig"), it.orig)) continue;
        it.id = id->s;
        if (const Value* n = v.Find("names"); n && n->type == Value::Type::Array && n->a)
            for (const Value& s : *n->a) if (s.type == Value::Type::String) it.names.push_back(s.s);
        if (const Value* c = v.Find("count"); c && c->type == Value::Type::Number) it.count = int(c->n);
        out.push_back(std::move(it));
    }
    return out;
}

bool SaveEdits(const std::map<std::string, Color4>& edits) {
    Value root = Value::MakeObject();
    Value e = Value::MakeObject();
    for (auto& [id, c] : edits) e[id] = Num4(c);
    root["edits"] = e;
    return WriteAtomic(EditsPath(), json::Serialize(root, 1));
}

std::map<std::string, Color4> LoadEdits() {
    std::map<std::string, Color4> out;
    auto root = ReadJson(EditsPath());
    const Value* e = root ? root->Find("edits") : nullptr;
    if (!e || e->type != Value::Type::Object || !e->o) return out;
    for (auto& [id, v] : *e->o) {
        Color4 c;
        if (Get4(&v, c)) out[id] = c;
    }
    return out;
}

}  // namespace astro::fx
