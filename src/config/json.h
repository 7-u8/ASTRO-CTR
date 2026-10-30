#pragma once
// Minimal JSON (objects, arrays, strings, numbers, booleans, null). No third-party dependency.
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace astro::json {

struct Value;
using Object = std::map<std::string, Value>;
using Array = std::vector<Value>;

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::shared_ptr<Array> a;
    std::shared_ptr<Object> o;

    static Value Bool(bool v) { Value x; x.type = Type::Bool; x.b = v; return x; }
    static Value Number(double v) { Value x; x.type = Type::Number; x.n = v; return x; }
    static Value String(std::string v) { Value x; x.type = Type::String; x.s = std::move(v); return x; }
    static Value MakeObject() { Value x; x.type = Type::Object; x.o = std::make_shared<Object>(); return x; }

    const Value* Find(const std::string& key) const {
        if (type != Type::Object || !o) return nullptr;
        auto it = o->find(key);
        return it == o->end() ? nullptr : &it->second;
    }
    Value& operator[](const std::string& key) { return (*o)[key]; }
};

// Returns nullopt (and fills *error) on malformed input. Nesting depth is capped.
std::optional<Value> Parse(const std::string& text, std::string* error = nullptr);
std::string Serialize(const Value& v, int indent = 0);

}  // namespace astro::json
