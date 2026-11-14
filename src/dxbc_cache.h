#pragma once

#include <cstdint>

namespace routeb {

using LogFn = void (*)(const char* fmt, ...);

struct DxbcCacheStats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t bypassed = 0;
    uint64_t write_errors = 0;
};

bool InstallDxbcCacheHook(void* d3d11_module_base, LogFn logger);
void RemoveDxbcCacheHook();
DxbcCacheStats GetDxbcCacheStats();

}  // namespace routeb
