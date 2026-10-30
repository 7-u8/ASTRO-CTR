#pragma once
#include <memory>

#include "../config/config.h"
#include "../game/backend.h"

namespace astro {

struct ApplyReport {
    bool normal_ok = true, blue_primary_ok = true, blue_secondary_ok = true, champion_ok = true, paint_ok = true;
    bool AllOk() const { return normal_ok && blue_primary_ok && blue_secondary_ok && champion_ok && paint_ok; }
};

// Maps Config -> backend calls. Each property is independent (changing one never touches another).
// Disabled property -> backend restore (nullptr). Only properties whose desired state changed are re-sent.
class VisualController {
public:
    explicit VisualController(std::unique_ptr<IVisualBackend> backend);

    const Config& GetConfig() const { return cfg_; }
    void SetConfig(const Config& c) { cfg_ = c; }

    ApplyReport Apply();                 // push the current config
    void ResetNormal();                  // sets default color + disabled, restores original
    void ResetBluePrimary();
    void ResetBlueSecondary();
    void ResetBlueAll();
    void ResetChampion();
    void ResetPaint();
    void Shutdown();                     // restore everything the mod changed

    IVisualBackend& Backend() { return *backend_; }

private:
    struct Applied { bool known = false; bool on = false; Rgba color; };
    bool Push(Applied& state, bool want, const Rgba& color, bool (IVisualBackend::*fn)(const Rgba*));
    std::unique_ptr<IVisualBackend> backend_;
    Config cfg_;
    bool PushNormal();
    bool PushPaint();
    Applied a_paint_;
    Rgba a_paint_sent_[kPaintSlots][kPaintLayers];
    Applied a_normal_, a_primary_, a_secondary_, a_champion_;
    Rgba a_normal_rest_[3];          // color2, color3, smoke last sent (color is a_normal_.color)
    float a_normal_brightness_ = 0;
};

}  // namespace astro
