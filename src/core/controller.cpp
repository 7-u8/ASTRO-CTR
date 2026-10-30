#include "controller.h"

#include <cstring>

namespace astro {

VisualController::VisualController(std::unique_ptr<IVisualBackend> backend) : backend_(std::move(backend)) {}

bool VisualController::Push(Applied& st, bool want, const Rgba& color, bool (IVisualBackend::*fn)(const Rgba*)) {
    if (st.known && st.on == want && (!want || st.color == color)) return true;  // nothing changed
    if (!st.known && !want) { st.known = true; st.on = false; return true; }     // never touched, stay untouched
    bool ok = (backend_.get()->*fn)(want ? &color : nullptr);
    if (ok) { st.known = true; st.on = want; st.color = color; }
    return ok;
}

// Same rules as Push, for the three normal-nitro colours sent together.
bool VisualController::PushNormal() {
    const NitroNormal& n = cfg_.normal;
    Applied& st = a_normal_;
    const bool same = st.color == n.color && a_normal_rest_[0] == n.color2 && a_normal_rest_[1] == n.color3 &&
                      a_normal_rest_[2] == n.smoke && a_normal_brightness_ == n.brightness;
    if (st.known && st.on == n.enabled && (!n.enabled || same)) return true;
    if (!st.known && !n.enabled) { st.known = true; st.on = false; return true; }
    const Rgba cols[4] = {n.color, n.color2, n.color3, n.smoke};
    bool ok = backend_->SetNormalNitroLayers(n.enabled ? cols : nullptr, n.brightness);
    if (ok) {
        st.known = true; st.on = n.enabled; st.color = n.color;
        a_normal_rest_[0] = n.color2; a_normal_rest_[1] = n.color3; a_normal_rest_[2] = n.smoke;
        a_normal_brightness_ = n.brightness;
    }
    return ok;
}

// Same rules as Push, for the three kart paint colours sent together.
bool VisualController::PushPaint() {
    const KartPaint& p = cfg_.paint;
    Applied& st = a_paint_;
    const bool same = !std::memcmp(a_paint_sent_, p.layer, sizeof a_paint_sent_);
    if (st.known && st.on == p.enabled && (!p.enabled || same)) return true;
    if (!st.known && !p.enabled) { st.known = true; st.on = false; return true; }
    bool ok = backend_->SetKartPaint(p.enabled ? &p.layer[0][0] : nullptr);
    if (ok) { st.known = true; st.on = p.enabled; std::memcpy(a_paint_sent_, p.layer, sizeof a_paint_sent_); }
    return ok;
}

ApplyReport VisualController::Apply() {
    ApplyReport r;
    r.normal_ok = PushNormal();
    r.blue_primary_ok = Push(a_primary_, cfg_.blue.enabled && cfg_.blue.primary_custom, cfg_.blue.primary,
                             &IVisualBackend::SetBlueNitroPrimary);
    r.blue_secondary_ok = Push(a_secondary_, cfg_.blue.enabled && cfg_.blue.secondary_custom, cfg_.blue.secondary,
                               &IVisualBackend::SetBlueNitroSecondary);
    r.champion_ok = Push(a_champion_, cfg_.champion.enabled, cfg_.champion.lower_region, &IVisualBackend::SetChampionLowerRegion);
    r.paint_ok = PushPaint();
    return r;
}

void VisualController::ResetNormal() { cfg_.normal = NitroNormal{}; Apply(); }

void VisualController::ResetBluePrimary() {
    cfg_.blue.primary = Rgba{};
    cfg_.blue.primary_custom = false;   // slot returns to the game's original; secondary untouched
    Apply();
}
void VisualController::ResetBlueSecondary() {
    cfg_.blue.secondary = Rgba{};
    cfg_.blue.secondary_custom = false;
    Apply();
}
void VisualController::ResetBlueAll() { cfg_.blue = NitroBlue{}; Apply(); }
void VisualController::ResetPaint() { cfg_.paint.enabled = false; Apply(); }
void VisualController::ResetChampion() { cfg_.champion = Champion{}; Apply(); }

void VisualController::Shutdown() {
    backend_->RestoreAll();
    a_normal_ = a_primary_ = a_secondary_ = a_champion_ = a_paint_ = Applied{};
}

}  // namespace astro
