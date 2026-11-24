#include "perf.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

#include "../config/config.h"

namespace astro {

namespace {

LARGE_INTEGER g_freq{}, g_last{}, g_window{};
double g_worst = 0, g_sum = 0;
unsigned g_count = 0;
unsigned long long g_total = 0;

double Ms(LONGLONG ticks) { return double(ticks) * 1000.0 / double(g_freq.QuadPart); }

void Flush(double seconds) {
    char buf[160];
    const double avg = g_count ? g_sum / g_count : 0;
    std::snprintf(buf, sizeof buf, "{\"fps\": %.1f, \"avg_ms\": %.2f, \"worst_ms\": %.2f, \"frames\": %llu}\n",
                  seconds > 0 ? g_count / seconds : 0.0, avg, g_worst, g_total);
    const auto dir = DefaultConfigPath().parent_path();
    const auto tmp = dir / "perf.json.tmp";
    if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        std::fputs(buf, f);
        std::fclose(f);
        MoveFileExW(tmp.c_str(), (dir / "perf.json").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

}  // namespace

void Perf_OnPresent() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_freq.QuadPart) {
        QueryPerformanceFrequency(&g_freq);
        g_last = g_window = now;
        return;
    }
    const double dt = Ms(now.QuadPart - g_last.QuadPart);
    g_last = now;
    g_sum += dt;
    g_worst = std::max(g_worst, dt);
    ++g_count;
    ++g_total;
    const double window = Ms(now.QuadPart - g_window.QuadPart) / 1000.0;
    if (window >= 1.0) {   // once a second
        Flush(window);
        g_window = now;
        g_sum = g_worst = 0;
        g_count = 0;
    }
}

}  // namespace astro
