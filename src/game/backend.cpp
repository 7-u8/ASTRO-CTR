#include "backend.h"

namespace astro {

BackendInfo UndiscoveredBackend::Probe() {
    BackendInfo i;
    i.status = BackendStatus::NotDiscovered;
    i.detail =
        "Live colour write is not available: the runtime layout of the boost-flame material / VFX colour "
        "operators, the blue-nitro parameters and the Champion lower region have not been discovered in the "
        "sacredfire.exe guest memory yet (see docs/reverse-engineering-notes.md).";
    return i;
}

}  // namespace astro
