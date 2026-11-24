#pragma once
// MODS page (inside the game): applies and removes executable patches live. See config/mods_protocol.h.
namespace astro {

void ModEngine_Start();
void ModEngine_Stop();   // leaves the patches as they are (the launcher applies the same ones at every start)

}  // namespace astro
