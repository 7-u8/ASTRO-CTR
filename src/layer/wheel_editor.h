#pragma once
// Wheel Editor (inside the game): finds the loaded wheel textures (wheel_catalog.json next to the DLL) and writes
// the recoloured textures the ASTRO app asks for. See config/wheel_protocol.h for the files and events.
namespace astro {

void WheelEditor_Start();
void WheelEditor_Stop();   // puts every texture it changed back

}  // namespace astro
