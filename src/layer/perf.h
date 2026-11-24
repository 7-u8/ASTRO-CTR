#pragma once
// Frame timing measured where the game presents a frame (QueuePresentKHR), written once a second to
// %APPDATA%\ASTRO-CTR\perf.json for the ASTRO app: {"fps", "avg_ms", "worst_ms", "frames"}.
namespace astro {

void Perf_OnPresent();

}  // namespace astro
