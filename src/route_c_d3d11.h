#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace routeb {

using RouteCD3DLogFn = void (*)(const char* fmt, ...);

struct alignas(16) CbAoParams {
    float proj_xy[4];        // (p00, p11, p02, p12)
    float proj_z_fade[4];    // (p22, p23, fadeEnd, invFadeRange)
    float vp_rect[4];        // (vpOriginX, vpOriginY, vpWidth, vpHeight)
    int32_t vp_clamp[4];     // (minX, minY, maxX, maxY)
    float ao_params[4];      // (radiusWorld, biasCos, strength, minAoFloor)
    float blur_and_mode[4];  // (invVpW, invVpH, depthEdgeScale, debugMode)
};
static_assert(sizeof(CbAoParams) == 96, "CbAoParams must match 6 float4 HLSL registers");

struct RouteCAoRenderRequest {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* fallback_rtv = nullptr;  // Borrowed, optional
    ID3D11DepthStencilView* fallback_dsv = nullptr;  // Borrowed, optional
    const float* proj_4x4 = nullptr;                 // Row-major Ogre::Matrix4 (16 floats)
    float near_clip = 1.0f;
    float far_clip = 100.0f;
    int vp_left = 0;
    int vp_top = 0;
    int vp_width = 0;
    int vp_height = 0;
    int debug_mode = 0;
};

struct RouteCD3DStats {
    uint64_t inject_ok = 0;
    uint64_t inject_skip_no_om = 0;
    uint64_t inject_skip_fmt = 0;
    uint64_t inject_skip_fail = 0;
    uint32_t depth_rebuilds = 0;
    uint32_t last_tex_w = 0;
    uint32_t last_tex_h = 0;
    uint32_t last_rtv_fmt = 0;
    uint32_t last_dsv_tex_fmt = 0;
    uint32_t last_sample_count = 0;
    bool shaders_ready = false;

    uint64_t bench_samples = 0;
    double avg_copy_ms = 0.0;
    double avg_ao_ms = 0.0;
    double avg_blur_h_ms = 0.0;
    double avg_blur_v_ms = 0.0;
    double avg_comp_ms = 0.0;
    double avg_total_ms = 0.0;
};

bool ExecuteRouteCAoPass(const RouteCAoRenderRequest& req, RouteCD3DLogFn logger);
bool ExecuteRouteCToastOverlay(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    bool dll_enhance_on,
    bool ao_on,
    float fade_alpha,
    RouteCD3DLogFn logger);
bool VerifyRouteCShadersCompile(RouteCD3DLogFn logger);
void ShutdownRouteCD3D11Resources();
RouteCD3DStats GetRouteCD3DStats();

}  // namespace routeb
