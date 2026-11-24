#pragma once
// PLAYERS page (inside the game): every kart in the race marked on the screen with its live data, the values the
// developers' "Inspect Kart" tool shows (speed, air time, boost, ground angle, surface, wall, wetness).
// Read only: nothing is written to the game. See docs/debug-menu-analysis.md for where each value comes from.
namespace astro {

bool PlayersEsp_Enabled();   // draw this frame even with the menu closed
void PlayersEsp_Draw();      // every frame, inside ImGui::NewFrame/Render
void PlayersEsp_Tab();       // the PLAYERS tab of the F8 menu

}  // namespace astro
