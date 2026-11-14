#pragma once

#include <cstdint>

namespace routeb {

using LogFn = void (*)(const char* fmt, ...);

struct FastPathStats {
    uint64_t cb_fast_hits = 0;
    uint64_t cb_fallback_calls = 0;
    uint64_t ff_param_hits = 0;
    uint64_t ff_param_misses = 0;
    uint64_t ff_param_skipped_missing = 0;
};

bool InstallPhase1FastPaths(void* d3d11_base, void* ogre_main_base, LogFn logger);
void RemovePhase1FastPaths();
FastPathStats GetFastPathStats();

}  // namespace routeb
