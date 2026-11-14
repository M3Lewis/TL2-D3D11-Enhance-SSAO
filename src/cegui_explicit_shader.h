#pragma once

#include <cstdint>

namespace routeb {

struct CeguiShaderStats {
    uint64_t hook_calls;
    uint64_t explicit_binds;
    uint64_t fallback_skips;
    bool hooked;
    bool shaders_ready;
};

using LogCallbackFn = void (*)(const char* fmt, ...);

bool InstallCeguiExplicitShaderHook(void* d3d11_module_base, void* ogre_main_base, LogCallbackFn log_fn);
void RemoveCeguiExplicitShaderHook();
CeguiShaderStats GetCeguiShaderStats();

}  // namespace routeb
