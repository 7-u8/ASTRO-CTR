#pragma once
#include "overlay.h"

namespace astro {

void Menu_DrawToast();
void Menu_Tick();   // every frame: animated (rainbow) colours
void Menu_Draw(bool* open, const OverlayDiag& diag);
void Menu_DiagPage();   // the Diagnostics page of the F8 menu
void Menu_Shutdown();   // restores everything the mod changed

// the ASTRO app's pages, built into the layer (app/main.cpp with ASTRO_IN_GAME)
void AstroUi_Setup();            // once, after the ImGui context exists: style and fonts
void AstroUi_Draw(bool* open);   // the F8 window
void AstroUi_Flush();            // the menu was closed: write what is still pending

}  // namespace astro
