#pragma once
#include "../game/backend.h"

namespace astro {

// Live boost-flame colour. The flame effect vfx/CTR/share/ctr_kart_exhaust_fire_1 (permanent.pak) holds four
// igVfxColorCurveOperator objects; each has an R, G and B block of 16 floats = channel value * fixed curve
// (value at float [2]). Writing a block scaled to a new channel value recolours the flame for every kart at once
// (verified in game 2026-10-30). The objects are found by a background search of guest memory.
IVisualBackend* CreateGuestBackend();   // owned by the caller (wrap in unique_ptr)

}  // namespace astro
