#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace astro::json {
namespace {

struct Parser {
    const std::string& t;
    std::size_t p = 0;
    std::string err;
    int depth = 0;
    explicit Parser(const std::string& s) : t(s) {}

    void Ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\t' || t[p] == '\r' || t[p] == '\n')) ++p; }
    bool Fail(const char* m) { if (err.empty()) err = std::string(m) + " at offset " + std::to_string(p); return false; }

    bool ParseString(std::string& out) {
        if (p >= t.size() || t[p] != '"') return Fail("expected string");
        ++p;
        out.clear();
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= t.size()) return Fail("bad escape");
                char e = t[p++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        if (p + 4 > t.size()) return Fail("bad \\u escape");
                        unsigned cp = static_cast<unsigned>(std::strtoul(t.substr(p, 4).c_str(), nullptr, 16));
                        p += 4;
                        out += cp < 0x80 ? static_cast<char>(cp) : '?';  // configs are ASCII; non-ASCII degraded
                        break;
                    }
                    default: return Fail("bad escape");
                }
            } else {
                out += c;
            }
        }
        return Fail("unterminated string");
    }

    bool ParseValue(Value& v) {
        Ws();
        if (p >= t.size()) return Fail("unexpected end");
        if (++depth > 32) return Fail("nesting too deep");
        struct Guard { int& d; ~Guard() { --d; } } g{depth};
        char c = t[p];
        if (c == '{') {
            ++p;
            v = Value::MakeObject();
            Ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            for (;;) {
                Ws();
                std::string key;
                if (!ParseString(key)) return false;
                Ws();
                if (p >= t.size() || t[p++] != ':') return Fail("expected ':'");
                Value child;
                if (!ParseValue(child)) return false;
                (*v.o)[key] = std::move(child);
                Ws();
                if (p >= t.size()) return Fail("unterminated object");
                if (t[p] == ',') { ++p; continue; }
                if (t[p] == '}') { ++p; return true; }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            v.type = Value::Type::Array;
            v.a = std::make_shared<Array>();
            Ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            for (;;) {
                Value child;
                if (!ParseValue(child)) return false;
                v.a->push_back(std::move(child));
                Ws();
                if (p >= t.size()) return Fail("unterminated array");
                if (t[p] == ',') { ++p; continue; }
                if (t[p] == ']') { ++p; return true; }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') { v.type = Value::Type::String; return ParseString(v.s); }
        if (t.compare(p, 4, "true") == 0) { p += 4; v = Value::Bool(true); return true; }
        if (t.compare(p, 5, "false") == 0) { p += 5; v = Value::Bool(false); return true; }
        if (t.compare(p, 4, "null") == 0) { p += 4; v = Value(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            char* end = nullptr;
            double d = std::strtod(t.c_str() + p, &end);
            if (end == t.c_str() + p || !std::isfinite(d)) return Fail("bad number");
            p = static_cast<std::size_t>(end - t.c_str());
            v = Value::Number(d);
            return true;
        }
        return Fail("unexpected character");
    }
};

void Escape(const std::string& s, std::string& out) {
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out += c;
        }
    }
    out += '"';
}

void Write(const Value& v, int indent, int level, std::string& out) {
    auto nl = [&](int lv) { if (indent > 0) { out += '\n'; out.append(static_cast<std::size_t>(indent * lv), ' '); } };
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.b ? "true" : "false"; break;
        case Value::Type::Number: {
            char b[40];
            if (v.n == std::floor(v.n) && std::fabs(v.n) < 1e15) std::snprintf(b, sizeof b, "%.0f", v.n);
            else std::snprintf(b, sizeof b, "%.6g", v.n);
            out += b;
            break;
        }
        case Value::Type::String: Escape(v.s, out); break;
        case Value::Type::Array:
            out += '[';
            if (v.a) for (std::size_t i = 0; i < v.a->size(); ++i) { if (i) out += ','; nl(level + 1); Write((*v.a)[i], indent, level + 1, out); }
            if (v.a && !v.a->empty()) nl(level);
            out += ']';
            break;
        case Value::Type::Object: {
            out += '{';
            bool first = true;
            if (v.o) for (const auto& kv : *v.o) {
                if (!first) out += ',';
                first = false;
                nl(level + 1);
                Escape(kv.first, out);
                out += indent > 0 ? ": " : ":";
                Write(kv.second, indent, level + 1, out);
            }
            if (!first) nl(level);
            out += '}';
            break;
        }
    }
}

}  // namespace

std::optional<Value> Parse(const std::string& text, std::string* error) {
    Parser ps(text);
    Value v;
    if (!ps.ParseValue(v)) { if (error) *error = ps.err; return std::nullopt; }
    ps.Ws();
    if (ps.p != text.size()) { if (error) *error = "trailing data at offset " + std::to_string(ps.p); return std::nullopt; }
    return v;
}

std::string Serialize(const Value& v, int indent) {
    std::string out;
    Write(v, indent, 0, out);
    return out;
}

}  // namespace astro::json
