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
    uint64_t rimlit_3cube_binds = 0;
    uint64_t vs_texcoord_injections = 0;
    uint64_t vs_missing_vcol_fixes = 0;
    uint64_t ff_explicit_syncs = 0;
    uint64_t sampler_hook_calls = 0;
    uint64_t sampler_af_upgrades = 0;
    uint64_t p1_soft_shader_patches = 0;
    uint64_t p1_soft_particle_binds = 0;
    bool hooked = false;
    bool sampler_hooked = false;
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

// Screen-space soft particles (P1): smoothly softens intersections between unlit
// FixedFunc transparent particles/glows and opaque world depth while protecting
// floor-parallel decals via pixel gradient comparison.
// Uses Texture2D<float4> (t6) instead of a second cbuffer because Ogre 1.7's
// D3D11HLSLProgram::loadFromSource rejects shaders with >1 constant buffer
// ("Use one global constant buffer for Ogre HLSL programs").
inline bool PatchFixedFuncSoftParticles(std::string& hlsl_src) {
    if (hlsl_src.find("gP1SceneDepthTex") != std::string::npos ||
        hlsl_src.find("gP1SoftParamsTex") != std::string::npos ||
        hlsl_src.find("register(t6)") != std::string::npos ||
        hlsl_src.find("register(t7)") != std::string::npos ||
        hlsl_src.find("register(b6)") != std::string::npos) {
        return false;
    }
    if (hlsl_src.find("Pos:SV_POSITION") == std::string::npos &&
        hlsl_src.find("Pos : SV_POSITION") == std::string::npos &&
        hlsl_src.find("Pos : SV_Position") == std::string::npos) {
        return false;
    }
    static constexpr const char kPsAnchor[] = "float4 result=input.Color;";
    const size_t ps_pos = hlsl_src.find(kPsAnchor);
    if (ps_pos == std::string::npos) {
        return false;
    }
    if (hlsl_src.find("AlphaRejectValue", ps_pos) != std::string::npos ||
        hlsl_src.find("discard", ps_pos) != std::string::npos) {
        return false;
    }
    static constexpr const char kReturnHdr[] =
        "HdrOverlayOutput output; output.colour=result; output.coverage=1; return output; }";
    static constexpr const char kReturnStd[] = "return result; }";
    size_t ret_pos = hlsl_src.find(kReturnHdr, ps_pos);
    if (ret_pos == std::string::npos) {
        ret_pos = hlsl_src.find(kReturnStd, ps_pos);
        if (ret_pos == std::string::npos) {
            return false;
        }
    }

    static constexpr const char kGlobalDecl[] =
        "Texture2D<float4> gP1SoftParamsTex : register(t6);\n"
        "Texture2D<float> gP1SceneDepthTex : register(t7);\n";
    static constexpr const char kPsSoftBody[] =
        "\n{ float4 p1SoftParams = gP1SoftParamsTex.Load(int3(0, 0, 0));\n"
        "  float p1PartDist = p1SoftParams.w / min(input.Pos.z + p1SoftParams.z, -1e-6f);\n"
        "  float2 p1GradP = float2(ddx(p1PartDist), ddy(p1PartDist));\n"
        "  [branch] if (p1SoftParams.x > 0.5f) {\n"
        "    int4 p1DepthClamp = int4(gP1SoftParamsTex.Load(int3(1, 0, 0)));\n"
        "    int2 p1Pix = clamp(int2(input.Pos.xy), p1DepthClamp.xy, p1DepthClamp.zw);\n"
        "    float p1SceneRaw = gP1SceneDepthTex.Load(int3(p1Pix, 0));\n"
        "    if (p1SceneRaw < 0.99998f) {\n"
        "      float p1SceneDist = p1SoftParams.w / min(p1SceneRaw + p1SoftParams.z, -1e-6f);\n"
        "      float p1Diff = p1SceneDist - p1PartDist;\n"
        "      int2 p1PixR = clamp(p1Pix + int2(1, 0), p1DepthClamp.xy, p1DepthClamp.zw);\n"
        "      int2 p1PixL = clamp(p1Pix - int2(1, 0), p1DepthClamp.xy, p1DepthClamp.zw);\n"
        "      int2 p1PixD = clamp(p1Pix + int2(0, 1), p1DepthClamp.xy, p1DepthClamp.zw);\n"
        "      int2 p1PixU = clamp(p1Pix - int2(0, 1), p1DepthClamp.xy, p1DepthClamp.zw);\n"
        "      float p1RawR = gP1SceneDepthTex.Load(int3(p1PixR, 0));\n"
        "      float p1RawL = gP1SceneDepthTex.Load(int3(p1PixL, 0));\n"
        "      float p1RawD = gP1SceneDepthTex.Load(int3(p1PixD, 0));\n"
        "      float p1RawU = gP1SceneDepthTex.Load(int3(p1PixU, 0));\n"
        "      float p1DistR = (p1RawR < 0.99998f) ? (p1SoftParams.w / min(p1RawR + p1SoftParams.z, -1e-6f)) : p1SceneDist;\n"
        "      float p1DistL = (p1RawL < 0.99998f) ? (p1SoftParams.w / min(p1RawL + p1SoftParams.z, -1e-6f)) : p1SceneDist;\n"
        "      float p1DistD = (p1RawD < 0.99998f) ? (p1SoftParams.w / min(p1RawD + p1SoftParams.z, -1e-6f)) : p1SceneDist;\n"
        "      float p1DistU = (p1RawU < 0.99998f) ? (p1SoftParams.w / min(p1RawU + p1SoftParams.z, -1e-6f)) : p1SceneDist;\n"
        "      float p1GxR = p1DistR - p1SceneDist;\n"
        "      float p1GxL = p1SceneDist - p1DistL;\n"
        "      float p1GyD = p1DistD - p1SceneDist;\n"
        "      float p1GyU = p1SceneDist - p1DistU;\n"
        "      float2 p1GradS = float2(\n"
        "          (abs(p1GxR - p1GradP.x) < abs(p1GxL - p1GradP.x)) ? p1GxR : p1GxL,\n"
        "          (abs(p1GyD - p1GradP.y) < abs(p1GyU - p1GradP.y)) ? p1GyD : p1GyU);\n"
        "      float p1GradLen = length(p1GradP);\n"
        "      bool p1ParallelFloor = (p1GradLen > 0.004f && length(p1GradS - p1GradP) < p1GradLen * 0.50f && p1Diff > 0.005f);\n"
        "      if (!p1ParallelFloor) {\n"
        "        if (p1SoftParams.x < 1.5f) {\n"
        "          float p1T = saturate(p1Diff * p1SoftParams.y);\n"
        "          float p1Soft = p1T * p1T * (3.0f - 2.0f * p1T);\n"
        "          result.a *= p1Soft;\n"
        "        } else {\n"
        "          float p1T = saturate(p1Diff * (p1SoftParams.y * 2.8f));\n"
        "          float p1Soft = 1.0f - (1.0f - p1T) * (1.0f - p1T);\n"
        "          float p1Peak = max(result.r, max(result.g, result.b));\n"
        "          if (p1SoftParams.x > 2.5f) { p1Peak *= saturate(result.a); }\n"
        "          float p1MinGlow = lerp(0.45f, 0.88f, smoothstep(0.28f, 0.85f, p1Peak));\n"
        "          result.rgb *= lerp(p1MinGlow, 1.0f, p1Soft);\n"
        "        }\n"
        "      }\n"
        "    }\n"
        "  }\n"
        "}\n";

    hlsl_src.insert(ret_pos, kPsSoftBody);
    hlsl_src.insert(0, kGlobalDecl);
    return true;
}

bool InstallPhase3ScenePipeline(void* d3d11_base, void* ogre_main_base, Phase3LogFn logger);
void RemovePhase3ScenePipeline();
Phase3Stats GetPhase3Stats();
void LogPhase3PassDiagnostics(Phase3LogFn logger);

void SetPhase3RuntimeEnhanceEnabled(bool enabled);
bool IsPhase3RuntimeEnhanceEnabled();
bool TogglePhase3RuntimeEnhance();

void SetF11VisualEnhanceEnabled(bool enabled);
bool IsF11VisualEnhanceEnabled();
bool ToggleF11VisualEnhance();

void SetP0RimLitEnhanceEnabled(bool enabled);
bool IsP0RimLitEnhanceEnabled();
bool ToggleP0RimLitEnhance();

void SetP2FilterPreset(int preset);
int GetP2FilterPreset();
int CycleP2FilterPreset();
const char* P2FilterPresetName(int preset);

void SetPhase3StrengthPct(int pct);
int GetPhase3StrengthPct();
void SetF11PointLightStrengthPct(int pct);
int GetF11PointLightStrengthPct();
void SetP0RimLitStrengthPct(int pct);
int GetP0RimLitStrengthPct();
void SetP2FilterStrengthPct(int pct);
int GetP2FilterStrengthPct();
void SetPhase3IblWrapEnabled(bool enabled);
bool IsPhase3IblWrapEnabled();
bool TogglePhase3IblWrap();
void SetPhase3IblWrapStrengthPct(int pct);
int GetPhase3IblWrapStrengthPct();

bool WasP2PostShaderSeenRecently();
bool ConsumeFrameWaterPassHint();

bool IsDynamicRainRecentlyObserved();
bool IsDesertSceneActive();
bool IsMainMenuActive();
float UpdateRainWetnessPerFrame();
float GetEffectiveRainStrength();

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
