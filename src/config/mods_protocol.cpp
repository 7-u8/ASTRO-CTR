#include "mods_protocol.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include "json.h"

namespace astro::mods {

namespace {

using json::Value;

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
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

// value of attribute `name` inside one XML tag
std::string Attr(const std::string& tag, const char* name) {
    const std::string key = std::string(name) + "=\"";
    size_t a = 0;
    while ((a = tag.find(key, a)) != std::string::npos) {
        if (a == 0 || std::isspace(static_cast<unsigned char>(tag[a - 1]))) {
            a += key.size();
            const size_t b = tag.find('"', a);
            return b == std::string::npos ? std::string() : tag.substr(a, b - a);
        }
        a += key.size();
    }
    return {};
}
std::string Unescape(std::string s) {
    const std::pair<const char*, const char*> e[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    for (auto& [from, to] : e)
        for (size_t p; (p = s.find(from)) != std::string::npos;) s.replace(p, std::string(from).size(), to);
    return s;
}
std::vector<std::uint8_t> Hex(const std::string& h) {
    std::vector<std::uint8_t> out;
    std::string d;
    for (char c : h) if (std::isxdigit(static_cast<unsigned char>(c))) d += c;
    for (size_t i = 0; i + 1 < d.size(); i += 2) out.push_back(std::uint8_t(std::stoi(d.substr(i, 2), nullptr, 16)));
    return out;
}
std::vector<int> Sig(const std::string& s) {
    std::vector<int> out;
    std::istringstream in(s);
    std::string tok;
    while (in >> tok) out.push_back(tok == "??" || tok == "?" ? -1 : std::stoi(tok, nullptr, 16));
    return out;
}
std::uint64_t Num(const std::string& s) { return s.empty() ? 0 : std::stoull(s, nullptr, 0); }
std::string HexOf(const std::vector<std::uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (auto b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}
std::string SigOf(const std::vector<int>& v) {
    std::string s;
    static const char* d = "0123456789abcdef";
    for (int b : v) {
        if (!s.empty()) s += ' ';
        if (b < 0) s += "??";
        else { s += d[b >> 4]; s += d[b & 15]; }
    }
    return s;
}
// names of mods that change how the game plays (not looks or content)
bool Gameplay(const std::string& name) {
    std::string l;
    for (char c : name) l += char(std::tolower(static_cast<unsigned char>(c)));
    for (const char* k : {"win", "fly", "gravity", "super engine", "speed", "boost", "invincib"})
        if (l.find(k) != std::string::npos) return true;
    return false;
}

}  // namespace

std::vector<Mod> ParseXml(const std::filesystem::path& file) {
    std::vector<Mod> out;
    const std::string t = ReadFile(file);
    const std::string fname = file.filename().string();
    for (size_t a = 0; (a = t.find("<Metadata", a)) != std::string::npos;) {
        const size_t head_end = t.find('>', a);
        const size_t end = t.find("</Metadata>", a);
        if (head_end == std::string::npos || end == std::string::npos) break;
        const std::string head = t.substr(a, head_end - a);
        Mod m;
        m.file = fname;
        m.name = Unescape(Attr(head, "Name"));
        m.author = Unescape(Attr(head, "Author"));
        m.version = Attr(head, "PatchVer");
        m.note = Unescape(Attr(head, "Note"));
        m.id = fname + "/" + m.name;
        m.gameplay = Gameplay(m.name);
        for (size_t l = head_end; (l = t.find("<Line", l)) != std::string::npos && l < end;) {
            const size_t le = t.find('>', l);
            const std::string tag = t.substr(l, le - l);
            l = le;
            if (Attr(tag, "Type") != "bytes") continue;
            Line ln;
            ln.addr = Num(Attr(tag, "Address"));
            ln.value = Hex(Attr(tag, "Value"));
            ln.sig = Sig(Attr(tag, "Signature"));
            ln.sig_off = Num(Attr(tag, "SignatureOffset"));
            if (ln.addr && !ln.value.empty()) m.lines.push_back(std::move(ln));
        }
        if (!m.name.empty() && !m.lines.empty()) out.push_back(std::move(m));
        a = end;
    }
    return out;
}

bool SaveLive(const std::vector<Mod>& mods, const std::map<std::string, bool>& on) {
    Value root = Value::MakeObject();
    Value arr;
    arr.type = Value::Type::Array;
    arr.a = std::make_shared<json::Array>();
    for (const Mod& m : mods) {
        Value o = Value::MakeObject();
        o["id"] = Value::String(m.id);
        auto it = on.find(m.id);
        o["on"] = Value::Bool(it != on.end() && it->second);
        Value lines;
        lines.type = Value::Type::Array;
        lines.a = std::make_shared<json::Array>();
        for (const Line& l : m.lines) {
            Value lo = Value::MakeObject();
            lo["addr"] = Value::Number(double(l.addr));
            lo["value"] = Value::String(HexOf(l.value));
            lo["sig"] = Value::String(SigOf(l.sig));
            lo["sig_off"] = Value::Number(double(l.sig_off));
            lines.a->push_back(lo);
        }
        o["lines"] = lines;
        arr.a->push_back(o);
    }
    root["mods"] = arr;
    return WriteAtomic(LivePath(), json::Serialize(root, 1));
}

bool LoadLive(std::vector<Mod>& mods, std::map<std::string, bool>& on) {
    auto root = json::Parse(ReadFile(LivePath()));
    const Value* arr = root ? root->Find("mods") : nullptr;
    if (!arr || arr->type != Value::Type::Array || !arr->a) return false;
    mods.clear();
    on.clear();
    for (const Value& o : *arr->a) {
        Mod m;
        if (const Value* v = o.Find("id"); v && v->type == Value::Type::String) m.id = v->s;
        const Value* onv = o.Find("on");
        on[m.id] = onv && onv->type == Value::Type::Bool && onv->b;
        if (const Value* ls = o.Find("lines"); ls && ls->type == Value::Type::Array && ls->a)
            for (const Value& lo : *ls->a) {
                Line l;
                if (const Value* v = lo.Find("addr"); v && v->type == Value::Type::Number) l.addr = std::uint64_t(v->n);
                if (const Value* v = lo.Find("value"); v && v->type == Value::Type::String) l.value = Hex(v->s);
                if (const Value* v = lo.Find("sig"); v && v->type == Value::Type::String) l.sig = Sig(v->s);
                if (const Value* v = lo.Find("sig_off"); v && v->type == Value::Type::Number) l.sig_off = std::uint64_t(v->n);
                m.lines.push_back(std::move(l));
            }
        mods.push_back(std::move(m));
    }
    return true;
}

bool SaveStatus(const std::map<std::string, Status>& s) {
    Value root = Value::MakeObject();
    for (auto& [id, st] : s) {
        Value o = Value::MakeObject();
        o["state"] = Value::String(st.state);
        o["detail"] = Value::String(st.detail);
        root[id] = o;
    }
    return WriteAtomic(StatusPath(), json::Serialize(root, 1));
}

std::map<std::string, Status> LoadStatus() {
    std::map<std::string, Status> out;
    auto root = json::Parse(ReadFile(StatusPath()));
    if (!root || root->type != Value::Type::Object || !root->o) return out;
    for (auto& [id, o] : *root->o) {
        Status s;
        if (const Value* v = o.Find("state"); v && v->type == Value::Type::String) s.state = v->s;
        if (const Value* v = o.Find("detail"); v && v->type == Value::Type::String) s.detail = v->s;
        out[id] = s;
    }
    return out;
}

}  // namespace astro::mods
