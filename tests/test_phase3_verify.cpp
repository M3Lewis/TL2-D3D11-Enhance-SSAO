#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3dcommon.h>
#include <d3d11shader.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "../src/fixed_func_manual_uniforms.h"
#include "../src/phase3_scene_pipeline.h"

namespace {

using PluginFn = void (*)();
using GenLayerBlendOpFn = std::string*(__fastcall*)(std::string* out, const routeb::OgreLayerBlendModeExRaw* blend, uint8_t is_alpha);

constexpr uintptr_t kRvaGetOrCreateSamplerState = 0x11240U;
constexpr uintptr_t kRvaSetPass = 0x1903C0U;
constexpr uintptr_t kRvaGetFixedFuncSrc = 0x352B0U;
constexpr uintptr_t kRvaSetFixedFuncParams = 0x37ED0U;
constexpr uintptr_t kRvaGetFfShaderPrograms = 0x32480U;
constexpr uintptr_t kRvaGenLayerBlendOp = 0x33580U;
constexpr uintptr_t kRvaGenLayerBlendOperand = 0x34F10U;

constexpr uint8_t kExpectedGetSamplerState[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20};
constexpr uint8_t kExpectedSetPass[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr uint8_t kExpectedGetShaderSrc[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57};
constexpr uint8_t kExpectedSetFfParams[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr uint8_t kExpectedGetFfPrograms[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
constexpr uint8_t kExpectedGenBlendOp[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr uint8_t kExpectedGenBlendOperand[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08};

std::string ReadWholeLog(const char* path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

bool TestManualNormalizationAndDeclInjection() {
    routeb::OgreTextureLayerStateRaw frame_a[2]{};
    routeb::OgreTextureLayerStateRaw frame_b[2]{};

    // Stage 0: animated LBS_MANUAL colour arg2 + animated LBS_MANUAL alpha arg1 + LBX_BLEND_MANUAL colour factor.
    frame_a[0].tex_type = 2;
    frame_a[0].colour.operation = routeb::kLayerBlendOpManual;
    frame_a[0].colour.source1 = 1;  // LBS_TEXTURE
    frame_a[0].colour.source2 = routeb::kLayerBlendSrcManual;
    frame_a[0].colour.colour_arg2[0] = 0.639f;
    frame_a[0].colour.colour_arg2[1] = 0.745f;
    frame_a[0].colour.colour_arg2[2] = 0.784f;
    frame_a[0].colour.colour_arg2[3] = 1.0f;
    frame_a[0].colour.factor = 0.25f;
    frame_a[0].alpha.operation = 2;  // LBX_MODULATE
    frame_a[0].alpha.source1 = routeb::kLayerBlendSrcManual;
    frame_a[0].alpha.source2 = 0;  // LBS_CURRENT
    frame_a[0].alpha.alpha_arg1 = 0.42f;

    // Stage 1: standard non-manual stage should retain its values identically.
    frame_a[1].tex_type = 2;
    frame_a[1].colour.operation = 2;
    frame_a[1].colour.source1 = 1;
    frame_a[1].colour.source2 = 0;
    frame_a[1].colour.factor = 0.5f;

    std::memcpy(frame_b, frame_a, sizeof(frame_a));
    frame_b[0].colour.colour_arg2[0] = 0.637f;
    frame_b[0].colour.factor = 0.87f;
    frame_b[0].alpha.alpha_arg1 = 0.91f;

    routeb::ManualFrameSnapshot snap_a{};
    routeb::ManualFrameSnapshot snap_b{};
    routeb::CaptureAndNormalizeManualLayers(frame_a, 2, &snap_a);
    routeb::CaptureAndNormalizeManualLayers(frame_b, 2, &snap_b);

    if (!snap_a.has_any || !snap_b.has_any) {
        std::printf("FAILED: expected has_any=true for manual layers\n");
        return false;
    }
    if (std::memcmp(frame_a, frame_b, sizeof(frame_a)) != 0) {
        std::printf("FAILED: normalized layer keys must match across animated manual float values\n");
        return false;
    }

    routeb::RestoreManualLayers(frame_a, 2, snap_a);
    if (frame_a[0].colour.colour_arg2[0] != 0.639f ||
        frame_a[0].colour.factor != 0.25f ||
        frame_a[0].alpha.alpha_arg1 != 0.42f ||
        frame_a[1].colour.factor != 0.5f) {
        std::printf("FAILED: RestoreManualLayers did not preserve exact original float values\n");
        return false;
    }

    routeb::ManualUniformMask mask{};
    mask.col_arg2 = 0x1;
    mask.col_factor = 0x1;
    mask.alpha_arg1 = 0x1;
    std::string dummy_hlsl =
        "float FogDensity, FogStart, FogEnd; float4 FogColor; float AlphaRejectValue;\n"
        "float4 ps_main() : SV_Target { return float4(1,1,1,1); }\n";
    if (!routeb::InjectManualUniformDeclarations(dummy_hlsl, mask)) {
        std::printf("FAILED: InjectManualUniformDeclarations returned false\n");
        return false;
    }
    if (dummy_hlsl.find("float4 FFManC0A2;\n") == std::string::npos ||
        dummy_hlsl.find("float FFManC0F;\n") == std::string::npos ||
        dummy_hlsl.find("float FFManA0A1;\n") == std::string::npos) {
        std::printf("FAILED: injected uniform declarations missing:\n%s\n", dummy_hlsl.c_str());
        return false;
    }
    return true;
}

bool TestMissingVertexColourFallback() {
    // Case 1: Mesh without COLOR0 stream + lit shader -> MaterialTrack lerp must fall back to Material*.
    std::string no_vcol_lit =
        "struct VS_INPUT { float3 Position0 : POSITION0; float3 Normal0 : NORMAL0; float2 Uv0 : TEXCOORD0; };\n"
        "VS_OUTPUT vs_main(VS_INPUT input) { VS_OUTPUT output=(VS_OUTPUT)0;\n"
        "float4 vertexColour=float4(1,1,1,1);\n"
        "float4 ambient=lerp(MaterialAmbient,vertexColour,MaterialTrack.x); "
        "float4 diffuse=lerp(MaterialDiffuse,vertexColour,MaterialTrack.y);\n"
        "float4 specular=lerp(MaterialSpecular,vertexColour,MaterialTrack.z); "
        "float4 emissive=lerp(MaterialEmissive,vertexColour,MaterialTrack.w);\n"
        "return output; }\n";
    if (!routeb::PatchFixedFuncMissingVertexColour(no_vcol_lit)) {
        std::printf("FAILED: expected PatchFixedFuncMissingVertexColour to patch no-COLOR0 lit VS\n");
        return false;
    }
    if (no_vcol_lit.find("float4 diffuse=MaterialDiffuse;") == std::string::npos ||
        no_vcol_lit.find("float4 ambient=MaterialAmbient;") == std::string::npos ||
        no_vcol_lit.find("MaterialTrack") != std::string::npos) {
        std::printf("FAILED: patched no-COLOR0 lit VS mismatch:\n%s\n", no_vcol_lit.c_str());
        return false;
    }

    // Case 2: Mesh WITH COLOR0 stream -> must preserve MaterialTrack lerp untouched.
    std::string with_vcol_lit =
        "struct VS_INPUT { float3 Position0 : POSITION0; uint Diffuse0 : COLOR0; };\n"
        "VS_OUTPUT vs_main(VS_INPUT input) { VS_OUTPUT output=(VS_OUTPUT)0;\n"
        "float4 vertexColour=float4(1,1,1,1);\n"
        "vertexColour=float4((input.Diffuse0>>16)&255,0,0,255)/255.0;\n"
        "float4 ambient=lerp(MaterialAmbient,vertexColour,MaterialTrack.x); "
        "float4 diffuse=lerp(MaterialDiffuse,vertexColour,MaterialTrack.y);\n"
        "float4 specular=lerp(MaterialSpecular,vertexColour,MaterialTrack.z); "
        "float4 emissive=lerp(MaterialEmissive,vertexColour,MaterialTrack.w);\n"
        "return output; }\n";
    const std::string with_vcol_orig = with_vcol_lit;
    if (routeb::PatchFixedFuncMissingVertexColour(with_vcol_lit) || with_vcol_lit != with_vcol_orig) {
        std::printf("FAILED: VS with COLOR0 stream must not be modified\n");
        return false;
    }

    // Case 3: Unlit shader without COLOR0 -> keeps vertexColour = float4(1,1,1,1) untouched.
    std::string no_vcol_unlit =
        "struct VS_INPUT { float3 Position0 : POSITION0; };\n"
        "VS_OUTPUT vs_main(VS_INPUT input) { VS_OUTPUT output=(VS_OUTPUT)0;\n"
        "float4 vertexColour=float4(1,1,1,1);\n"
        "output.Color=vertexColour;\n"
        "return output; }\n";
    const std::string no_vcol_unlit_orig = no_vcol_unlit;
    if (routeb::PatchFixedFuncMissingVertexColour(no_vcol_unlit) || no_vcol_unlit != no_vcol_unlit_orig) {
        std::printf("FAILED: unlit VS must not be modified\n");
        return false;
    }
    return true;
}

bool TestSoftParticleShaderPatching() {
    using D3DCompileFn = HRESULT(WINAPI*)(
        LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    using D3DReflectFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, REFIID, void**);
    HMODULE d3dcomp = ::LoadLibraryW(L"d3dcompiler_47.dll");
    D3DCompileFn compile_fn = d3dcomp ? reinterpret_cast<D3DCompileFn>(::GetProcAddress(d3dcomp, "D3DCompile")) : nullptr;
    D3DReflectFn reflect_fn = d3dcomp ? reinterpret_cast<D3DReflectFn>(::GetProcAddress(d3dcomp, "D3DReflect")) : nullptr;

    std::string std_src =
        "struct VS_OUTPUT { float4 Pos:SV_POSITION; float4 Color:COLOR0; };\n"
        "float4 ps_main(VS_OUTPUT input):SV_Target { float4 result=input.Color;\n"
        "return result; }\n";
    if (!routeb::PatchFixedFuncSoftParticles(std_src) ||
        std_src.find("gP1SceneDepthTex") == std::string::npos ||
        std_src.find("gP1SoftParamsTex") == std::string::npos ||
        std_src.find("register(t6)") == std::string::npos ||
        std_src.find("register(t7)") == std::string::npos ||
        std_src.find("cbuffer P1SoftParticleCb") != std::string::npos ||
        std_src.find("p1ParallelFloor") == std::string::npos) {
        std::printf("FAILED: PatchFixedFuncSoftParticles failed on standard FixedFunc PS\n");
        return false;
    }
    const std::string std_patched = std_src;
    if (routeb::PatchFixedFuncSoftParticles(std_src) || std_src != std_patched) {
        std::printf("FAILED: PatchFixedFuncSoftParticles must be idempotent\n");
        return false;
    }

    // Fogged / $Globals FixedFunc PS must compile cleanly with at most ONE cbuffer ($Globals)
    // because Ogre 1.7 D3D11HLSLProgram throws when ConstantBuffers > 1.
    std::string fog_src =
        "float FogStart;\n"
        "float FogEnd;\n"
        "float4 FogColor;\n"
        "float AlphaRejectValue;\n"
        "struct VS_OUTPUT { float4 Pos:SV_POSITION; float4 Color:COLOR0; float FogDist:FOGDISTANCE0; };\n"
        "float4 ps_main(VS_OUTPUT input):SV_Target { float4 result=input.Color;\n"
        "result.rgb=lerp(FogColor.rgb, result.rgb, saturate((FogEnd-abs(input.FogDist))/max(FogEnd-FogStart,1e-5f)));\n"
        "return result; }\n";
    if (!routeb::PatchFixedFuncSoftParticles(fog_src)) {
        std::printf("FAILED: PatchFixedFuncSoftParticles failed on fogged FixedFunc PS\n");
        return false;
    }

    // Alpha-rejected FixedFunc shaders (UI / foliage cutout) must be skipped cleanly.
    std::string alpha_reject_src =
        "float AlphaRejectValue;\n"
        "struct VS_OUTPUT { float4 Pos:SV_POSITION; float4 Color:COLOR0; };\n"
        "float4 ps_main(VS_OUTPUT input):SV_Target { float4 result=input.Color;\n"
        "if (result.a <= AlphaRejectValue) discard;\n"
        "return result; }\n";
    const std::string alpha_reject_orig = alpha_reject_src;
    if (routeb::PatchFixedFuncSoftParticles(alpha_reject_src) || alpha_reject_src != alpha_reject_orig) {
        std::printf("FAILED: shader using AlphaRejectValue must not be patched\n");
        return false;
    }

    std::string hdr_src =
        "struct VS_OUTPUT { float4 Pos:SV_POSITION; float4 Color:COLOR0; };\n"
        "struct HdrOverlayOutput { float4 colour:SV_Target0; float coverage:SV_Target1; };\n"
        "HdrOverlayOutput ps_main(VS_OUTPUT input):SV_Target { float4 result=input.Color;\n"
        "HdrOverlayOutput output; output.colour=result; output.coverage=1; return output; }\n";
    if (!routeb::PatchFixedFuncSoftParticles(hdr_src)) {
        std::printf("FAILED: PatchFixedFuncSoftParticles failed on HdrOverlayOutput PS\n");
        return false;
    }

    std::string conflict_src =
        "struct VS_OUTPUT { float4 Pos:SV_POSITION; float4 Color:COLOR0; };\n"
        "Texture2D<float4> userTex :register(t7);\n"
        "float4 ps_main(VS_OUTPUT input):SV_Target { float4 result=input.Color;\n"
        "return result; }\n";
    const std::string conflict_orig = conflict_src;
    if (routeb::PatchFixedFuncSoftParticles(conflict_src) || conflict_src != conflict_orig) {
        std::printf("FAILED: shader using :register(t7) must not be patched\n");
        return false;
    }

    if (compile_fn) {
        constexpr UINT kWarnAsErr = 1U << 15;  // D3DCOMPILE_WARNINGS_ARE_ERRORS
        for (const std::string* s : {&std_src, &fog_src, &hdr_src}) {
            ID3DBlob* code = nullptr;
            ID3DBlob* err = nullptr;
            const HRESULT hr = compile_fn(
                s->data(), s->size(), "ff_soft_test.hlsl", nullptr, nullptr,
                "ps_main", "ps_4_0", kWarnAsErr, 0, &code, &err);
            if (FAILED(hr) || !code) {
                const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "<none>";
                std::printf("FAILED: patched HLSL compile failed (hr=0x%08X): %s\n",
                            static_cast<unsigned>(hr), msg);
                if (err) err->Release();
                if (code) code->Release();
                return false;
            }
            if (err) err->Release();
            if (reflect_fn) {
                ID3D11ShaderReflection* refl = nullptr;
                if (FAILED(reflect_fn(code->GetBufferPointer(), code->GetBufferSize(), __uuidof(ID3D11ShaderReflection), reinterpret_cast<void**>(&refl))) || !refl) {
                    std::printf("FAILED: D3DReflect failed on patched shader\n");
                    code->Release();
                    return false;
                }
                D3D11_SHADER_DESC sh_desc{};
                refl->GetDesc(&sh_desc);
                refl->Release();
                if (sh_desc.ConstantBuffers > 1) {
                    std::printf("FAILED: patched shader uses %u cbuffers (>1 breaks Ogre 1.7 D3D11HLSLProgram)\n",
                                sh_desc.ConstantBuffers);
                    code->Release();
                    return false;
                }
            }
            code->Release();
        }
    }
    if (d3dcomp) {
        ::FreeLibrary(d3dcomp);
    }
    return true;
}

}  // namespace

int main() {
    if (!TestManualNormalizationAndDeclInjection() ||
        !TestMissingVertexColourFallback() ||
        !TestSoftParticleShaderPatching()) {
        return 10;
    }

    ::SetDllDirectoryW(LR"(H:\Steam\steamapps\Common\Torchlight II)");
    ::SetCurrentDirectoryW(LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\obj)");

    HMODULE ogre = ::LoadLibraryW(L"OgreMain.dll");
    HMODULE gui = ::LoadLibraryW(L"OgreGUIRenderer.dll");
    HMODULE d3d = ::LoadLibraryW(L"RenderSystem_Direct3D11.dll");
    if (!ogre || !gui || !d3d) {
        std::printf("FAILED LoadLibrary game DLLs (ogre=%p gui=%p d3d=%p err=%lu)\n",
                    ogre, gui, d3d, ::GetLastError());
        return 1;
    }

    HMODULE plugin = ::LoadLibraryW(
        LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\bin\Plugin_TL2_DX11_Enhance_SSAO.dll)");
    if (!plugin) {
        std::printf("FAILED LoadLibrary Plugin_TL2_DX11_Enhance_SSAO.dll err=%lu\n", ::GetLastError());
        return 1;
    }

    auto start_fn = reinterpret_cast<PluginFn>(::GetProcAddress(plugin, "dllStartPlugin"));
    auto stop_fn = reinterpret_cast<PluginFn>(::GetProcAddress(plugin, "dllStopPlugin"));
    if (!start_fn || !stop_fn) {
        std::printf("FAILED GetProcAddress dllStartPlugin/dllStopPlugin\n");
        return 1;
    }

    const auto ogre_base = reinterpret_cast<uintptr_t>(ogre);
    const auto d3d_base = reinterpret_cast<uintptr_t>(d3d);
    const auto* p_sampler = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGetOrCreateSamplerState);
    const auto* p_setpass = reinterpret_cast<const uint8_t*>(ogre_base + kRvaSetPass);
    const auto* p_getsrc = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGetFixedFuncSrc);
    const auto* p_setff = reinterpret_cast<const uint8_t*>(d3d_base + kRvaSetFixedFuncParams);
    const auto* p_getprog = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGetFfShaderPrograms);
    const auto* p_blendop = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGenLayerBlendOp);
    const auto* p_operand = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGenLayerBlendOperand);

    if (std::memcmp(p_sampler, kExpectedGetSamplerState, sizeof(kExpectedGetSamplerState)) != 0 ||
        std::memcmp(p_setpass, kExpectedSetPass, sizeof(kExpectedSetPass)) != 0 ||
        std::memcmp(p_getsrc, kExpectedGetShaderSrc, sizeof(kExpectedGetShaderSrc)) != 0 ||
        std::memcmp(p_setff, kExpectedSetFfParams, sizeof(kExpectedSetFfParams)) != 0 ||
        std::memcmp(p_getprog, kExpectedGetFfPrograms, sizeof(kExpectedGetFfPrograms)) != 0 ||
        std::memcmp(p_blendop, kExpectedGenBlendOp, sizeof(kExpectedGenBlendOp)) != 0 ||
        std::memcmp(p_operand, kExpectedGenBlendOperand, sizeof(kExpectedGenBlendOperand)) != 0) {
        std::printf("FAILED pre-hook prologue signature mismatch on OgreMain/D3D11\n");
        return 2;
    }

    start_fn();

    if (*reinterpret_cast<const uint16_t*>(p_sampler) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_setpass) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_getsrc) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_setff) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_getprog) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_blendop) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_operand) != 0x25FF) {
        std::printf("FAILED Phase 3 / FFManual hook prologues were not patched with FF 25 jmp\n");
        stop_fn();
        return 3;
    }

    // Exercise live trampoline roundtrip through RenderSystem_Direct3D11::sub_180033580 when shader gen is inactive.
    routeb::OgreLayerBlendModeExRaw passthrough_blend{};
    passthrough_blend.operation = 2;  // LBX_MODULATE
    passthrough_blend.source1 = 1;    // LBS_TEXTURE
    passthrough_blend.source2 = 0;    // LBS_CURRENT
    std::string live_expr;
    auto gen_blend_op = reinterpret_cast<GenLayerBlendOpFn>(d3d_base + kRvaGenLayerBlendOp);
    gen_blend_op(&live_expr, &passthrough_blend, 0);
    if (live_expr.find("sampled.rgb") == std::string::npos ||
        live_expr.find("previous.rgb") == std::string::npos) {
        std::printf("FAILED live trampoline sub_180033580 returned unexpected expr: %s\n", live_expr.c_str());
        stop_fn();
        return 6;
    }

    using VerifyRouteCShadersFn = bool(*)();
    auto verify_rc_shaders = reinterpret_cast<VerifyRouteCShadersFn>(::GetProcAddress(plugin, "RouteB_VerifyRouteCShaders"));
    if (!verify_rc_shaders || !verify_rc_shaders()) {
        std::printf("FAILED Route C HLSL shader compilation verification (vs_4_0 / ps_4_0 / ps_4_1)\n");
        stop_fn();
        return 7;
    }

    stop_fn();

    if (std::memcmp(p_sampler, kExpectedGetSamplerState, sizeof(kExpectedGetSamplerState)) != 0 ||
        std::memcmp(p_setpass, kExpectedSetPass, sizeof(kExpectedSetPass)) != 0 ||
        std::memcmp(p_getsrc, kExpectedGetShaderSrc, sizeof(kExpectedGetShaderSrc)) != 0 ||
        std::memcmp(p_setff, kExpectedSetFfParams, sizeof(kExpectedSetFfParams)) != 0 ||
        std::memcmp(p_getprog, kExpectedGetFfPrograms, sizeof(kExpectedGetFfPrograms)) != 0 ||
        std::memcmp(p_blendop, kExpectedGenBlendOp, sizeof(kExpectedGenBlendOp)) != 0 ||
        std::memcmp(p_operand, kExpectedGenBlendOperand, sizeof(kExpectedGenBlendOperand)) != 0) {
        std::printf("FAILED clean unhook restoration of Phase 3 / FFManual prologues\n");
        return 4;
    }

    const std::string log = ReadWholeLog("TL2_RouteB_D3D11.log");
    if (log.find("scene_p3=ON") == std::string::npos ||
        log.find("samplerAF=1") == std::string::npos ||
        log.find("[RouteB][Phase3][Stats] Scene hooked=1 samplerHook=1") == std::string::npos ||
        log.find("vsVColFix=") == std::string::npos ||
        log.find("[RouteB][FFManual][Stats] hooked=1") == std::string::npos ||
        log.find("nullCb=") == std::string::npos) {
        std::printf("FAILED log check: Phase 3 / FFManual / CB breakdown stats missing from TL2_RouteB_D3D11.log\n");
        return 5;
    }

    ::FreeLibrary(plugin);
    ::FreeLibrary(d3d);
    ::FreeLibrary(gui);
    ::FreeLibrary(ogre);

    std::printf("[PASS] Phase 3 + FFManual verification succeeded (6 detours patched & restored cleanly, key normalization & decl injection passed)!\n");
    return 0;
}
