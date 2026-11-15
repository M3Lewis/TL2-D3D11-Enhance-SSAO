#pragma once

#include <cstdint>
#include <string>

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
    uint64_t vs_missing_vcol_fixes = 0;
    uint64_t ff_explicit_syncs = 0;
    bool hooked = false;
    bool shared_params_ready = false;
};

// Matches D3D9 FFP semantics (D3DMCS_COLOR1 fallback to D3DMATERIAL9): when the vertex
// declaration omits COLOR0, ignore MaterialTrack and keep MaterialAmbient/Diffuse/Specular/Emissive.
inline bool PatchFixedFuncMissingVertexColour(std::string& hlsl_src) {
    if (hlsl_src.find("input.Diffuse0") != std::string::npos ||
        hlsl_src.find(" : COLOR") != std::string::npos) {
        return false;
    }
    static constexpr const char kTarget[] =
        "float4 ambient=lerp(MaterialAmbient,vertexColour,MaterialTrack.x); "
        "float4 diffuse=lerp(MaterialDiffuse,vertexColour,MaterialTrack.y);\n"
        "float4 specular=lerp(MaterialSpecular,vertexColour,MaterialTrack.z); "
        "float4 emissive=lerp(MaterialEmissive,vertexColour,MaterialTrack.w);\n";
    static constexpr const char kReplacement[] =
        "float4 ambient=MaterialAmbient; float4 diffuse=MaterialDiffuse;\n"
        "float4 specular=MaterialSpecular; float4 emissive=MaterialEmissive;\n";
    const size_t pos = hlsl_src.find(kTarget);
    if (pos == std::string::npos) {
        return false;
    }
    hlsl_src.replace(pos, sizeof(kTarget) - 1, kReplacement);
    return true;
}

bool InstallPhase3ScenePipeline(void* d3d11_base, void* ogre_main_base, Phase3LogFn logger);
void RemovePhase3ScenePipeline();
Phase3Stats GetPhase3Stats();
void LogPhase3PassDiagnostics(Phase3LogFn logger);

void SetPhase3RuntimeEnhanceEnabled(bool enabled);
bool IsPhase3RuntimeEnhanceEnabled();
bool TogglePhase3RuntimeEnhance();

void SetP2FilterPreset(int preset);
int GetP2FilterPreset();
int CycleP2FilterPreset();
const char* P2FilterPresetName(int preset);
bool WasP2PostShaderSeenRecently();

using Phase3PassObserverFn = void (*)(
    const char* pass_name,
    bool lighting,
    bool depth_write,
    bool transparent,
    bool alpha_reject,
    bool has_vp,
    bool has_fp);
void SetPhase3PassObserver(Phase3PassObserverFn observer);

}  // namespace routeb
