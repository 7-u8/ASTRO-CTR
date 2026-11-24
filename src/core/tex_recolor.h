#pragma once
// Recolouring of BC1 textures without decoding them: every 4x4 block keeps its two RGB565 end colours and its
// indices; only the end colours are changed. Used by the Wheel Editor (the layer writes the result into the game's
// copy of the texture, which the emulator re-uploads; the app uses RecolorRgb for its live preview).
#include <cstddef>
#include <cstdint>

#include "../common/color.h"

namespace astro {

struct RecolorParams {
    Rgba target{255, 90, 31, 255};        // new colour of the coloured parts
    Rgba grey_target{255, 90, 31, 255};   // colour the grey/white parts are tinted with (when greys is on)
    float strength = 1.0f;                // 0 = original, 1 = fully the new colour
    float brightness = 1.0f;              // multiplies the result
    bool colours = true;                  // recolour the coloured parts
    bool greys = false;                   // tint the grey/white parts (silver, glow masks)
    bool blacks = false;                  // dark parts take black_target (a pad's black body): tinting cannot, black x colour = black
    Rgba black_target{255, 90, 31, 255};
};

// one colour (0..255 per channel) -> recoloured
void RecolorRgb(const RecolorParams& p, const std::uint8_t in[3], std::uint8_t out[3]);

// BC1 payload (8 bytes per block, any tiling): writes the recoloured blocks to out (same size as in).
void RecolorBc1(const RecolorParams& p, const std::uint8_t* in, std::uint8_t* out, size_t len);

// BC3 payload (16 bytes per block: 8 alpha bytes, then a BC1-style colour block): only the colour half is changed
void RecolorBc3(const RecolorParams& p, const std::uint8_t* in, std::uint8_t* out, size_t len);

// uncompressed RGBA8 payload (any tiling: every pixel is recoloured on its own; alpha is kept)
void RecolorRgba8(const RecolorParams& p, const std::uint8_t* in, std::uint8_t* out, size_t len);

}  // namespace astro
