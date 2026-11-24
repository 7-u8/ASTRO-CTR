#pragma once
// Parts of the layer that can be switched off for a test run (to find which one a game problem comes from).
// %APPDATA%\ASTRO-CTR\off.txt lists the names to leave off, one per line (or separated by spaces/commas), read once
// when the game starts; "all" switches off every part below. No file = nothing off.
//   backend  nitro / underglow / kart paint (searches and writes colours in the game's memory)
//   nitro    only the nitro flame + underglow part of backend (the memory search and its writes)
//   paint    only the kart paint part of backend (direct writes to the shown kart's colours)
//   fx       effects colour editor
//   wheels   wheel editor (texture and tint writes)
//   mods     MODS page patches
//   maps     MAPS page (water)
//   light    MAPS page lighting: fog, sun, ambient, colour grading (zone_light.cpp)
//   render   command hooks: shadows, shaders, texture detail, engine switches, perf
// The overlay (menu drawing) is always on: it is the layer itself.
namespace astro {

bool Off(const char* name);

}  // namespace astro

#include <cstddef>
#include <cstdint>
namespace astro {
// Names the calling thread (shows in task tools and in tools/perf_probe.py), e.g. "astro:nitro"
void NameThread(const wchar_t* name);
// Write journal: every store ASTRO makes in the game's memory is appended (and flushed at once, so a crash keeps it)
// to %APPDATA%\ASTRO-CTR\writes_<pid>.log as "<ms since start> <who> <addr> <size> <old bytes> -> <new bytes>".
// Call it right before the store. Large stores (textures) log only address and size.
void JournalWrite(const char* who, std::uintptr_t addr, const void* data, std::size_t n);
// A race (or menu) was just loaded: the texture engine saw its textures go away. For 30 s after that every part looks
// for its targets quickly (1-2 s), then goes back to its slow pace (searching all the time cost frames, measured
// 2026-11-01: ~15 fps).
void NoteLoad();
bool Loading();
}  // namespace astro
