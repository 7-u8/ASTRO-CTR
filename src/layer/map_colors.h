#pragma once
// MAPS page (inside the game): the colours of the map being raced, set live. Options come from maps.json (written by
// the ASTRO app); what was found is written to maps_status.json.
namespace astro {

void MapColors_Start();
void MapColors_Stop();

}  // namespace astro
