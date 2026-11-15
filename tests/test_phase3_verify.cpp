#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

constexpr uintptr_t kRvaSetPass = 0x1903C0U;
constexpr uintptr_t kRvaGetFixedFuncSrc = 0x352B0U;
constexpr uintptr_t kRvaSetFixedFuncParams = 0x37ED0U;
constexpr uintptr_t kRvaGetFfShaderPrograms = 0x32480U;
constexpr uintptr_t kRvaGenLayerBlendOp = 0x33580U;
constexpr uintptr_t kRvaGenLayerBlendOperand = 0x34F10U;

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

}  // namespace

int main() {
    if (!TestManualNormalizationAndDeclInjection() || !TestMissingVertexColourFallback()) {
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
        LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\bin\Plugin_TL2_RouteB.dll)");
    if (!plugin) {
        std::printf("FAILED LoadLibrary Plugin_TL2_RouteB.dll err=%lu\n", ::GetLastError());
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
    const auto* p_setpass = reinterpret_cast<const uint8_t*>(ogre_base + kRvaSetPass);
    const auto* p_getsrc = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGetFixedFuncSrc);
    const auto* p_setff = reinterpret_cast<const uint8_t*>(d3d_base + kRvaSetFixedFuncParams);
    const auto* p_getprog = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGetFfShaderPrograms);
    const auto* p_blendop = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGenLayerBlendOp);
    const auto* p_operand = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGenLayerBlendOperand);

    if (std::memcmp(p_setpass, kExpectedSetPass, sizeof(kExpectedSetPass)) != 0 ||
        std::memcmp(p_getsrc, kExpectedGetShaderSrc, sizeof(kExpectedGetShaderSrc)) != 0 ||
        std::memcmp(p_setff, kExpectedSetFfParams, sizeof(kExpectedSetFfParams)) != 0 ||
        std::memcmp(p_getprog, kExpectedGetFfPrograms, sizeof(kExpectedGetFfPrograms)) != 0 ||
        std::memcmp(p_blendop, kExpectedGenBlendOp, sizeof(kExpectedGenBlendOp)) != 0 ||
        std::memcmp(p_operand, kExpectedGenBlendOperand, sizeof(kExpectedGenBlendOperand)) != 0) {
        std::printf("FAILED pre-hook prologue signature mismatch on OgreMain/D3D11\n");
        return 2;
    }

    start_fn();

    if (*reinterpret_cast<const uint16_t*>(p_setpass) != 0x25FF ||
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

    if (std::memcmp(p_setpass, kExpectedSetPass, sizeof(kExpectedSetPass)) != 0 ||
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
        log.find("[RouteB][Phase3][Stats] Scene hooked=1") == std::string::npos ||
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
