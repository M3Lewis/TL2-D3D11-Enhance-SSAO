#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "..\src\route_c_d3d11.h"

namespace {

using PluginFn = void (*)();
using VerifyRouteCShadersFn = bool (*)();

constexpr uint8_t kExpectedRenderScene[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
constexpr uint8_t kExpectedFireQueue[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};

static_assert(sizeof(routeb::CbAoParams) == 96, "CbAoParams must be 96 bytes");
static_assert(alignof(routeb::CbAoParams) == 16, "CbAoParams must be 16-byte aligned");
static_assert(offsetof(routeb::CbAoParams, proj_xy) == 0, "b0[0] mismatch");
static_assert(offsetof(routeb::CbAoParams, proj_z_fade) == 16, "b0[1] mismatch");
static_assert(offsetof(routeb::CbAoParams, vp_rect) == 32, "b0[2] mismatch");
static_assert(offsetof(routeb::CbAoParams, vp_clamp) == 48, "b0[3] mismatch");
static_assert(offsetof(routeb::CbAoParams, ao_params) == 64, "b0[4] mismatch");
static_assert(offsetof(routeb::CbAoParams, blur_and_mode) == 80, "b0[5] mismatch");

float RawToLinearDistCpu(float z_ndc, float p22, float p23) {
    const float denom = (std::min)(z_ndc + p22, -1e-6f);
    return p23 / denom;
}

bool TestProjectionMathBoundaries() {
    // Verified TL2 C0 runtime projection parameters (near=1.0, far=99.25).
    const float n = 1.0f;
    const float f = 99.25f;
    const float p00 = 1.8133f;
    const float p11 = 3.1716f;
    const float p02 = 0.0025f;  // include non-zero asymmetric offset test
    const float p12 = -0.0015f;
    const float p22 = -f / (f - n);
    const float p23 = -(f * n) / (f - n);

    const float d_near = RawToLinearDistCpu(0.0f, p22, p23);
    const float d_far = RawToLinearDistCpu(1.0f, p22, p23);
    if (std::fabs(d_near - n) > 1e-4f || std::fabs(d_far - f) > 1e-2f) {
        std::printf("FAILED near/far linearization check: d_near=%.6f d_far=%.6f\n", d_near, d_far);
        return false;
    }

    // Sub-viewport roundtrip with non-zero origin (left=120, top=60, w=1342, h=960).
    const float vp_x = 120.0f, vp_y = 60.0f, vp_w = 1342.0f, vp_h = 960.0f;
    const float px = 713.5f, py = 540.5f;
    const float true_d = 28.5f;
    const float z_ndc = -p22 + p23 / true_d;
    const float u = (px - vp_x) / vp_w;
    const float v = (py - vp_y) / vp_h;
    const float x_ndc = u * 2.0f - 1.0f;
    const float y_ndc = 1.0f - v * 2.0f;

    const float rec_d = RawToLinearDistCpu(z_ndc, p22, p23);
    const float x_vs = rec_d * (x_ndc + p02) / p00;
    const float y_vs = rec_d * (y_ndc + p12) / p11;
    const float z_vs = -rec_d;

    const float reproj_x_ndc = (p00 * x_vs) / (-z_vs) - p02;
    const float reproj_y_ndc = (p11 * y_vs) / (-z_vs) - p12;
    const float reproj_px = vp_x + (0.5f + 0.5f * reproj_x_ndc) * vp_w;
    const float reproj_py = vp_y + (0.5f - 0.5f * reproj_y_ndc) * vp_h;

    if (std::fabs(rec_d - true_d) > 1e-4f ||
        std::fabs(reproj_px - px) > 1e-3f ||
        std::fabs(reproj_py - py) > 1e-3f) {
        std::printf("FAILED asymmetric VS roundtrip: d=%.5f px=%.4f py=%.4f\n", rec_d, reproj_px, reproj_py);
        return false;
    }

    // Infinite far plane clamping check (denom cannot divide by zero or go NaN).
    const float d_inf = RawToLinearDistCpu(1.0f, -1.0f, -1.0f);
    if (!std::isfinite(d_inf) || d_inf <= 0.0f) {
        std::printf("FAILED infinite far clamp: d_inf=%.6f\n", d_inf);
        return false;
    }
    return true;
}

std::string ReadWholeLog(const char* path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    if (!TestProjectionMathBoundaries()) {
        return 10;
    }

    ::SetDllDirectoryW(LR"(H:\Steam\steamapps\Common\Torchlight II)");
    ::SetCurrentDirectoryW(LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\obj)");

    HMODULE ogre = ::LoadLibraryW(L"OgreMain.dll");
    HMODULE gui = ::LoadLibraryW(L"OgreGUIRenderer.dll");
    HMODULE d3d = ::LoadLibraryW(L"RenderSystem_Direct3D11.dll");
    if (!ogre || !gui || !d3d) {
        std::printf("FAILED LoadLibrary game DLLs (err=%lu)\n", ::GetLastError());
        return 1;
    }

    const auto* p_render_scene = reinterpret_cast<const uint8_t*>(::GetProcAddress(
        ogre, "?_renderScene@SceneManager@Ogre@@UEAAXPEAVCamera@2@PEAVViewport@2@_N@Z"));
    const auto* p_q_start = reinterpret_cast<const uint8_t*>(::GetProcAddress(
        ogre, "?fireRenderQueueStarted@SceneManager@Ogre@@MEAA_NEAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"));
    const auto* p_q_end = reinterpret_cast<const uint8_t*>(::GetProcAddress(
        ogre, "?fireRenderQueueEnded@SceneManager@Ogre@@MEAA_NEAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z"));

    if (!p_render_scene || !p_q_start || !p_q_end) {
        std::printf("FAILED locating OgreMain Route C export targets\n");
        return 2;
    }
    if (std::memcmp(p_render_scene, kExpectedRenderScene, sizeof(kExpectedRenderScene)) != 0 ||
        std::memcmp(p_q_start, kExpectedFireQueue, sizeof(kExpectedFireQueue)) != 0 ||
        std::memcmp(p_q_end, kExpectedFireQueue, sizeof(kExpectedFireQueue)) != 0) {
        std::printf("FAILED pre-hook Route C OgreMain prologue signature mismatch\n");
        return 3;
    }

    HMODULE plugin = ::LoadLibraryW(
        LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\bin\Plugin_TL2_DX11_Enhance_SSAO.dll)");
    if (!plugin) {
        std::printf("FAILED LoadLibrary Plugin_TL2_DX11_Enhance_SSAO.dll (err=%lu)\n", ::GetLastError());
        return 4;
    }

    auto start_fn = reinterpret_cast<PluginFn>(::GetProcAddress(plugin, "dllStartPlugin"));
    auto stop_fn = reinterpret_cast<PluginFn>(::GetProcAddress(plugin, "dllStopPlugin"));
    auto verify_shaders_fn = reinterpret_cast<VerifyRouteCShadersFn>(::GetProcAddress(plugin, "RouteB_VerifyRouteCShaders"));
    if (!start_fn || !stop_fn || !verify_shaders_fn) {
        std::printf("FAILED resolving Route C plugin exports\n");
        return 5;
    }

    start_fn();

    if (*reinterpret_cast<const uint16_t*>(p_render_scene) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_q_start) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_q_end) != 0x25FF) {
        std::printf("FAILED Route C detours were not patched with FF 25 jmp\n");
        stop_fn();
        return 6;
    }

    if (!verify_shaders_fn()) {
        std::printf("FAILED Route C HLSL shader verification\n");
        stop_fn();
        return 7;
    }

    stop_fn();

    if (std::memcmp(p_render_scene, kExpectedRenderScene, sizeof(kExpectedRenderScene)) != 0 ||
        std::memcmp(p_q_start, kExpectedFireQueue, sizeof(kExpectedFireQueue)) != 0 ||
        std::memcmp(p_q_end, kExpectedFireQueue, sizeof(kExpectedFireQueue)) != 0) {
        std::printf("FAILED clean unhook restoration of Route C OgreMain prologues\n");
        return 8;
    }

    const std::string log = ReadWholeLog("TL2_RouteB_D3D11.log");
    if (log.find("[RouteC][C2] Hooks installed") == std::string::npos ||
        log.find("[RouteC][C2][Stats]") == std::string::npos) {
        std::printf("FAILED log check: Route C C2 logs missing\n");
        return 9;
    }

    ::FreeLibrary(plugin);
    ::FreeLibrary(d3d);
    ::FreeLibrary(gui);
    ::FreeLibrary(ogre);

    std::printf("[PASS] Route C verification succeeded (CbAoParams=96B, projection roundtrip/clamp, 3 OgreMain detours & clean restore, all HLSL shaders compiled)!\n");
    return 0;
}
