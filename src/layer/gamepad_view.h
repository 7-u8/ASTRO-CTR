#pragma once
// Gamepad view on the game's screen (MISC page of the ASTRO app): the pad drawn with the game's own button art,
// lit by what you press. Options come from misc.json (written by the app).
namespace astro {

void GamepadView_Setup();     // once, after the ImGui context exists: puts the button art into the font atlas
bool GamepadView_Enabled();   // misc.json says the pad is shown (read once a second)
void GamepadView_Draw();      // inside an ImGui frame

}  // namespace astro
