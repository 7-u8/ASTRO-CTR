#pragma once
// Engine-facing boundary. Everything that touches the game's memory/rendering lives behind this interface,
// so reverse-engineering discoveries never leak into UI/config/lifecycle code.
#include <string>
#include <vector>

#include "../common/color.h"

namespace astro {

enum class BackendStatus {
    NotDiscovered,  // engine layout for the property has not been reverse-engineered/validated
    Ready,
    Error,
};

struct BackendInfo {
    BackendStatus status = BackendStatus::NotDiscovered;
    std::string detail;
    // Alpha is exposed in the UI only for effects whose renderer path was verified to honour it.
    bool normal_alpha = false, blue_primary_alpha = false, blue_secondary_alpha = false;
};

// Contract for every Set*: color != nullptr -> apply that color; color == nullptr -> restore the game's
// original state for that single property. Return false if the change could not be applied.
class IVisualBackend {
public:
    virtual ~IVisualBackend() = default;
    virtual BackendInfo Probe() = 0;
    virtual bool SetNormalNitro(const Rgba* color) = 0;
    // flame colours {color, color2, color3, smoke} x brightness, or nullptr to restore; default: the first colour only
    virtual bool SetNormalNitroLayers(const Rgba* colors4, float /*brightness*/) { return SetNormalNitro(colors4); }
    virtual bool SetBlueNitroPrimary(const Rgba* color) = 0;
    virtual bool SetBlueNitroSecondary(const Rgba* color) = 0;
    virtual bool SetChampionLowerRegion(const Rgba* color) = 0;
    // Player kart paint: kPaintSlots x kPaintLayers colours (slot-major, layer 0 = tint), or nullptr to restore
    // the kart's own paint job.
    virtual bool SetKartPaint(const Rgba* /*layers*/) { return false; }
    // The kart's own colours (same layout) once they have been read from memory (false until then).
    virtual bool KartPaintOriginal(Rgba* /*out_layers*/) { return false; }
    // Paint jobs that have a live copy group in memory (each: 3 colours + number of groups). When there is more
    // than one, the user picks the kart's own with ChooseKartPaint.
    virtual std::vector<std::vector<Rgba>> KartPaintCandidates() { return {}; }
    virtual void ChooseKartPaint(const std::vector<Rgba>& /*colours3*/) {}
    virtual void RestoreAll() = 0;
    // Diagnostics only: 1 = paint every loaded kart paint colour with a test colour using in-process stores,
    // 0 = restore. Status() describes the last result.
    virtual void PaintTest(int /*mode*/) {}
    virtual std::string PaintTestStatus() { return "not available"; }
};

// Honest placeholder used until the engine properties are discovered: never modifies anything,
// reports exactly why, and treats "restore original" as a successful no-op.
class UndiscoveredBackend final : public IVisualBackend {
public:
    BackendInfo Probe() override;
    bool SetNormalNitro(const Rgba* c) override { return c == nullptr; }
    bool SetBlueNitroPrimary(const Rgba* c) override { return c == nullptr; }
    bool SetBlueNitroSecondary(const Rgba* c) override { return c == nullptr; }
    bool SetChampionLowerRegion(const Rgba* c) override { return c == nullptr; }
    void RestoreAll() override {}
};

}  // namespace astro
