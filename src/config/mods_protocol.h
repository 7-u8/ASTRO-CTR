#pragma once
// MODS page: executable patches in the shadPS4 XML format (<Line Type="bytes" Address Value Signature SignatureOffset>)
// applied live by the layer, and kept in the launcher's patches.json so it applies them at the next start too.
//   app -> layer : mods_live.json {"mods": [{"id", "on", "lines": [{"addr", "value", "sig", "sig_off"}]}]}
//                  then kModsApplyEvent
//   layer -> app : mods_status.json {"<id>": {"state": "on|off|pending|mismatch|error", "detail"}} then kModsStatusEvent
// An XML address is an offset in the executable as the launcher numbers it: in game memory it is at
// address + kGuestDelta (seen in game 2026-10-31: SPECIAL KARTS EDITOR 0x0160b830 patched at 0x80120b830).
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "config.h"

namespace astro::mods {

inline constexpr wchar_t kModsApplyEvent[] = L"Local\\ASTRO_CTR_ModsApply";
inline constexpr wchar_t kModsStatusEvent[] = L"Local\\ASTRO_CTR_ModsStatus";
inline constexpr std::uint64_t kGuestDelta = 0x800000000ull - 0x400000ull;

struct Line {
    std::uint64_t addr = 0;            // as written in the XML
    std::vector<std::uint8_t> value;
    std::vector<int> sig;              // bytes, -1 = ?? (any)
    std::uint64_t sig_off = 0;         // the patch is at (signature start + sig_off)
};
struct Mod {
    std::string id;                    // "<file>/<Name>", the launcher's patches.json key
    std::string file, name, author, version, note;
    std::vector<Line> lines;
    bool gameplay = false;             // changes how the game plays (warned about online)
};
struct Status { std::string state, detail; };

// every <Metadata> of an XML file (one mod each)
std::vector<Mod> ParseXml(const std::filesystem::path& file);

inline std::filesystem::path Dir() { return DefaultConfigPath().parent_path(); }
inline std::filesystem::path LivePath() { return Dir() / "mods_live.json"; }
inline std::filesystem::path StatusPath() { return Dir() / "mods_status.json"; }

bool SaveLive(const std::vector<Mod>& mods, const std::map<std::string, bool>& on);
bool LoadLive(std::vector<Mod>& mods, std::map<std::string, bool>& on);
bool SaveStatus(const std::map<std::string, Status>& s);
std::map<std::string, Status> LoadStatus();

}  // namespace astro::mods
