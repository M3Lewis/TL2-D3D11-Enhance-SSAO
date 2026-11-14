#pragma once

#include <cstdint>

namespace routeb {

using Phase3LogFn = void (*)(const char* fmt, ...);

struct Phase3Stats {
    uint64_t setpass_calls = 0;
    uint64_t diffpass_opaque_binds = 0;
    uint64_t diffpass_alpha_binds = 0;
    uint64_t skinned_diff_binds = 0;
    uint64_t lightpass_binds = 0;
    uint64_t rimlit_2d_binds = 0;
    uint64_t rimlit_cube_binds = 0;
    uint64_t vs_texcoord_injections = 0;
    uint64_t ff_explicit_syncs = 0;
    bool hooked = false;
    bool shared_params_ready = false;
};

bool InstallPhase3ScenePipeline(void* d3d11_base, void* ogre_main_base, Phase3LogFn logger);
void RemovePhase3ScenePipeline();
Phase3Stats GetPhase3Stats();
void LogPhase3PassDiagnostics(Phase3LogFn logger);

}  // namespace routeb
