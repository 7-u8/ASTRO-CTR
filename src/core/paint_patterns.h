#pragma once
// Colour helpers and the kart paint colour layouts, shared by the in-game menu and the ASTRO app.
#include "../config/config.h"

namespace astro {

struct Hsv { float h, s, v; };
Hsv ToHsv(const Rgba& c);
Rgba FromHsv(float h, float s, float v);   // h wraps around; s and v are clamped to 0..1
Rgba Mix(const Rgba& a, const Rgba& b, float t);
Rgba Scale(const Rgba& a, float k);

// A slot = tint (layer 0, the sheen) + a 6-colour ramp (layers 1..6, the base colour). A pattern builds the 7
// layers from one colour. These are ASTRO's own looks made from that structure, not copies of the game's schemes.
// (names chosen not to look like the game's own paint types such as Electron or Atomic: these are colour layouts only)
constexpr int kPatternCount = 8;
extern const char* const kPatterns[kPatternCount];
void MakePattern(int p, const Rgba& c, Rgba out[kPaintLayers]);

}  // namespace astro
