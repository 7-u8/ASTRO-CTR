#pragma once
#include "overlay.h"

namespace astro {

void Menu_ApplyStyle();
void Menu_DrawToast();
void Menu_Tick();   // every frame: animated (rainbow) colours
void Menu_Draw(bool* open, const OverlayDiag& diag);
void Menu_Shutdown();   // restores everything the mod changed

}  // namespace astro
