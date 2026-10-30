#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "../src/common/color.h"
#include "../src/config/config.h"
#include "../src/config/json.h"
#include "../src/core/controller.h"

using namespace astro;

static int g_fail = 0, g_run = 0;
#define CHECK(c) do { ++g_run; if (!(c)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Call { std::string what; bool apply; Rgba c; };
struct FakeBackend : IVisualBackend {
    std::vector<Call>* log;
    explicit FakeBackend(std::vector<Call>* l) : log(l) {}
    BackendInfo Probe() override { BackendInfo i; i.status = BackendStatus::Ready; return i; }
    bool Rec(const char* w, const Rgba* c) { log->push_back({w, c != nullptr, c ? *c : Rgba{}}); return true; }
    bool SetNormalNitro(const Rgba* c) override { return Rec("normal", c); }
    bool SetBlueNitroPrimary(const Rgba* c) override { return Rec("primary", c); }
    bool SetBlueNitroSecondary(const Rgba* c) override { return Rec("secondary", c); }
    bool SetChampionLowerRegion(const Rgba* c) override { return Rec("champion", c); }
    void RestoreAll() override { log->push_back({"restoreall", false, {}}); }
};

static int Count(const std::vector<Call>& l, const char* w) { int n = 0; for (auto& c : l) if (c.what == w) ++n; return n; }

static void TestColor() {
    auto c = ParseHexColor("#FF8000");
    CHECK(c && c->r == 255 && c->g == 128 && c->b == 0 && c->a == 255);
    auto d = ParseHexColor("0a0B0c80");
    CHECK(d && d->r == 10 && d->g == 11 && d->b == 12 && d->a == 128);
    CHECK(!ParseHexColor(""));
    CHECK(!ParseHexColor("#12345"));
    CHECK(!ParseHexColor("#GG0000"));
    CHECK(!ParseHexColor("#1234567"));
    CHECK(FormatHexColor(Rgba{1, 2, 3, 4}) == "#01020304");
    for (int i = 0; i < 256; i += 51) {
        Rgba x{static_cast<uint8_t>(i), static_cast<uint8_t>(255 - i), static_cast<uint8_t>(i / 2), static_cast<uint8_t>(i)};
        auto y = ParseHexColor(FormatHexColor(x));
        CHECK(y && *y == x);
    }
}

static void TestJson() {
    std::string e;
    CHECK(!json::Parse("", &e));
    CHECK(!json::Parse("{", &e));
    CHECK(!json::Parse("{\"a\":}", &e));
    CHECK(!json::Parse("{} trailing", &e));
    auto v = json::Parse("{\"a\":[1,2.5,true,null],\"b\":\"x\\ny\"}");
    CHECK(v && v->Find("a") && v->Find("a")->a->size() == 4);
    CHECK(v->Find("b")->s == "x\ny");
    std::string deep(100, '[');
    CHECK(!json::Parse(deep, &e));  // depth cap, no stack blowup
    auto round = json::Parse(json::Serialize(*v, 2));
    CHECK(round && round->Find("b")->s == "x\ny");
}

static void TestConfig() {
    // Defaults + roundtrip
    Config c;
    c.normal = {true, Rgba{255, 0, 0, 255}};
    c.blue.enabled = true;
    c.blue.primary = Rgba{0, 255, 0, 200};
    c.blue.secondary = Rgba{0, 0, 255, 100};
    c.blue.secondary_custom = false;
    c.champion = {true, Rgba{128, 0, 255, 255}};
    c.ui.scale = 1.5f;
    c.ui.debug_logging = true;
    auto r = ParseConfig(SerializeConfig(c));
    CHECK(r.warnings.empty());
    CHECK(r.config.normal.enabled && r.config.normal.color == Rgba({255, 0, 0, 255}));
    CHECK(r.config.blue.primary == Rgba({0, 255, 0, 200}) && r.config.blue.secondary == Rgba({0, 0, 255, 100}));
    CHECK(!r.config.blue.secondary_custom && r.config.blue.primary_custom);
    CHECK(r.config.champion.enabled && r.config.champion.lower_region == Rgba({128, 0, 255, 255}));
    CHECK(r.config.ui.scale == 1.5f && r.config.ui.debug_logging);

    // Malformed file -> defaults, flagged, no crash
    auto bad = ParseConfig("{ this is not json");
    CHECK(bad.recovered_from_error && !bad.config.normal.enabled && !bad.warnings.empty());
    auto notobj = ParseConfig("[1,2,3]");
    CHECK(notobj.recovered_from_error);

    // Invalid color value -> only that field falls back; its sibling is kept
    auto inv = ParseConfig("{\"nitro\":{\"blue\":{\"enabled\":true,\"primary\":\"#ZZZ\",\"secondary\":\"#112233\"}}}");
    CHECK(!inv.recovered_from_error && inv.warnings.size() == 1);
    CHECK(inv.config.blue.enabled && inv.config.blue.secondary == Rgba({0x11, 0x22, 0x33, 255}));
    CHECK(inv.config.blue.primary == Rgba({255, 255, 255, 255}));

    // Wrong types, out-of-range scale
    auto wt = ParseConfig("{\"nitro\":{\"normal\":{\"enabled\":\"yes\",\"color\":5}},\"ui\":{\"scale\":99}}");
    CHECK(wt.warnings.size() == 3 && !wt.config.normal.enabled && wt.config.ui.scale == 1.0f);

    // Newer version is accepted with a warning
    auto nv = ParseConfig("{\"version\":99,\"future\":{\"x\":1}}");
    CHECK(!nv.recovered_from_error && nv.warnings.size() == 1);

    // File IO: missing file, save, load, atomic overwrite
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "astro_ctr_test";
    fs::remove_all(dir);
    auto missing = LoadConfig(dir / "nope.json");
    CHECK(!missing.file_found && !missing.recovered_from_error);
    std::string err;
    CHECK(SaveConfig(dir / "sub" / "c.json", c, &err));
    CHECK(!fs::exists(dir / "sub" / "c.json.tmp"));
    Config c2 = c;
    c2.normal.color = Rgba{1, 2, 3, 255};
    c2.normal.color2 = Rgba{4, 5, 6, 255};
    c2.normal.color3 = Rgba{7, 8, 9, 255};
    CHECK(SaveConfig(dir / "sub" / "c.json", c2, &err));
    auto back = LoadConfig(dir / "sub" / "c.json");
    CHECK(back.file_found && back.config.normal.color == Rgba({1, 2, 3, 255}));
    CHECK(back.config.normal.color2 == Rgba({4, 5, 6, 255}) && back.config.normal.color3 == Rgba({7, 8, 9, 255}));
    // kart paint: three colours and the switch survive a save/load
    Config c3 = c;
    c3.paint.enabled = true;
    c3.paint.layer[0][0] = Rgba{10, 20, 30, 255};
    c3.paint.layer[2][6] = Rgba{40, 50, 60, 255};
    CHECK(SaveConfig(dir / "sub" / "p.json", c3, &err));
    auto bp = LoadConfig(dir / "sub" / "p.json");
    CHECK(bp.config.paint.enabled && bp.config.paint.layer[0][0] == Rgba({10, 20, 30, 255}) && bp.config.paint.layer[2][6] == Rgba({40, 50, 60, 255}));
    // rainbow ids and speed survive a save/load
    Config c4 = c;
    c4.fx.rainbow = "n1,glow,s2l3";
    c4.fx.rainbow_speed = 0.5f;
    CHECK(SaveConfig(dir / "sub" / "fx.json", c4, &err));
    auto bf = LoadConfig(dir / "sub" / "fx.json");
    CHECK(bf.config.fx.rainbow == "n1,glow,s2l3" && bf.config.fx.rainbow_speed == 0.5f);
    // an older config without color2/color3 keeps the game's own colours for them, with no warning
    auto old = ParseConfig("{\"nitro\":{\"normal\":{\"enabled\":true,\"color\":\"#00FF00\"}}}");
    CHECK(old.warnings.empty() && old.config.normal.color2 == NitroNormal{}.color2 && old.config.normal.color3 == NitroNormal{}.color3);
    { std::ofstream(dir / "garbage.json") << "\x01\x02 not json"; }
    CHECK(LoadConfig(dir / "garbage.json").recovered_from_error);
    fs::remove_all(dir);
}

static void TestController() {
    std::vector<Call> log;
    VisualController vc(std::make_unique<FakeBackend>(&log));

    // Nothing enabled -> backend never touched
    vc.Apply();
    CHECK(log.empty());

    // Normal nitro only: blue / champion untouched
    Config c;
    c.normal = {true, Rgba{255, 0, 0, 255}};
    vc.SetConfig(c);
    CHECK(vc.Apply().AllOk());
    CHECK(Count(log, "normal") == 1 && Count(log, "primary") == 0 && Count(log, "secondary") == 0 && Count(log, "champion") == 0);
    log.clear();

    // Idempotent: same config again sends nothing
    vc.Apply();
    CHECK(log.empty());

    // Blue primary & secondary independent: change primary only -> only primary is re-sent
    c.blue.enabled = true;
    c.blue.primary = Rgba{0, 255, 0, 255};
    c.blue.secondary = Rgba{0, 0, 255, 255};
    vc.SetConfig(c);
    vc.Apply();
    CHECK(Count(log, "primary") == 1 && Count(log, "secondary") == 1);
    log.clear();
    c.blue.primary = Rgba{9, 9, 9, 255};
    vc.SetConfig(c);
    vc.Apply();
    CHECK(Count(log, "primary") == 1 && Count(log, "secondary") == 0);
    log.clear();

    // Per-slot reset restores only that slot, other keeps its color
    vc.ResetBluePrimary();
    CHECK(Count(log, "primary") == 1 && !log.back().apply);
    CHECK(Count(log, "secondary") == 0);
    CHECK(vc.GetConfig().blue.secondary == Rgba({0, 0, 255, 255}) && vc.GetConfig().blue.enabled);
    log.clear();

    // Disable blue toggle -> restores the still-custom secondary
    Config d = vc.GetConfig();
    d.blue.enabled = false;
    vc.SetConfig(d);
    vc.Apply();
    CHECK(Count(log, "secondary") == 1 && !log.back().apply);
    log.clear();

    // Reset all blue, champion round trip, shutdown restores
    vc.ResetBlueAll();
    d = vc.GetConfig();
    d.champion = {true, Rgba{128, 0, 255, 255}};
    vc.SetConfig(d);
    vc.Apply();
    CHECK(Count(log, "champion") == 1 && log.back().apply);
    vc.ResetChampion();
    CHECK(!vc.GetConfig().champion.enabled && !log.back().apply);
    vc.Shutdown();
    CHECK(log.back().what == "restoreall");

    // Undiscovered backend: apply fails honestly, restore succeeds
    VisualController uv(std::make_unique<UndiscoveredBackend>());
    CHECK(uv.Backend().Probe().status == BackendStatus::NotDiscovered);
    Config e;
    e.normal = {true, Rgba{255, 0, 0, 255}};
    uv.SetConfig(e);
    CHECK(!uv.Apply().normal_ok);
    e.normal.enabled = false;
    uv.SetConfig(e);
    CHECK(uv.Apply().AllOk());
}

int main() {
    TestColor();
    TestJson();
    TestConfig();
    TestController();
    std::printf("%d checks, %d failed\n", g_run, g_fail);
    return g_fail ? 1 : 0;
}
