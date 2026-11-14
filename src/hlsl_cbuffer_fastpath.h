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
    uint64_t p3_param_forwards = 0;
};

bool InstallPhase1FastPaths(void* d3d11_base, void* ogre_main_base, LogFn logger);
void RemovePhase1FastPaths();
FastPathStats GetFastPathStats();

void SetActiveExplicitPsForwardTarget(uintptr_t explicit_ps_params);
bool WriteExplicitPsNamedFloat4(uintptr_t params, const char* name, const float v[4]);
bool WriteExplicitPsNamedFloat(uintptr_t params, const char* name, float v);
void ResetExplicitPsLightingScratch(uintptr_t params);

}  // namespace routeb
