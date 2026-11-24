#pragma once
// FULL OPTIMIZE inside the game, at the Vulkan level: every render pass the emulator records is seen here.
//  - measuring: per frame, how many passes of each kind (size, colour targets, depth) and how many draws they hold,
//    written to %APPDATA%\ASTRO-CTR\render_stats.json every 5 seconds
//  - hiding: passes of the kinds listed in %APPDATA%\ASTRO-CTR\optimize.json {"skip": ["2048x8192 c1 depth", ...]}
//    keep their clears but lose their draws, so what they draw disappears and the GPU skips that work. The shadows
//    are one such kind (seen in a race 2026-10-31: the cascade shadow atlas is a 2048x8192 pass with depth, 26 draws).
#include <vulkan/vulkan.h>

namespace astro {

struct DeviceFns;

// the hooked functions by name (nullptr: not one of them)
PFN_vkVoidFunction RenderHooks_Intercept(const char* name);
void RenderHooks_OnDevice(DeviceFns* d);   // after vkCreateDevice: the next layer's functions
void RenderHooks_OnPresent();              // frame boundary

}  // namespace astro
