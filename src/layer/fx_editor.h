#pragma once
// Effects editor (inside the game): lists every live VFX colour operator, names it from vfx_catalog.json (next to
// the DLL) and writes the colours the ASTRO app asks for. See config/fx_protocol.h for the files and events.
namespace astro {

void FxEditor_Start();
void FxEditor_Stop();   // restores every colour it changed

}  // namespace astro
