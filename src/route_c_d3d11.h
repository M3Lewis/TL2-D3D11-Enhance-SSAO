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
    float blur_and_mode[4];        // (invVpW, invVpH, depthEdgeScale, debugMode)
    float sscs_params[4];          // (sscsEnabled, sscsStrength, tintStrength, aerialMode: 0=off, 1=Hob/neutral, 2=DarkFilm)
    float sscs_key_and_range[4];   // (keyDirX, keyDirY, aerialStart, invAerialSpan)
    float aerial_fog_params[4];    // (hazeR/gTimeSsr, hazeG, hazeB, maxAerialAlpha)
    float view_up_and_water[4];    // (worldUpVs.xyz, waterHint)
    float inv_view_row0[4];        // (worldXBasisVs.xyz, camPosVsToWorldOffsetX)
    float inv_view_row2[4];        // (worldZBasisVs.xyz, camPosVsToWorldOffsetZ)
};
static_assert(sizeof(CbAoParams) == 192, "CbAoParams must match 12 float4 HLSL registers");

struct RouteCAoRenderRequest {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* fallback_rtv = nullptr;  // Borrowed, optional
    ID3D11DepthStencilView* fallback_dsv = nullptr;  // Borrowed, optional
    const float* proj_4x4 = nullptr;                 // Row-major Ogre::Matrix4 (16 floats)
    const float* view_4x4 = nullptr;                 // Optional row-major Ogre::Matrix4 (16 floats)
    float near_clip = 1.0f;
    float far_clip = 100.0f;
    int vp_left = 0;
    int vp_top = 0;
    int vp_width = 0;
    int vp_height = 0;
    int debug_mode = 0;
    bool ao_enabled = true;
    bool sscs_enabled = true;
    bool capture_particle_depth = false;
    bool aerial_enabled = false;
    bool ssr_enabled = false;
    int lut_preset = 0;     // 0=OFF, 1=CinemaTealOrange, 2=ColdNordic, 3=Custom(.cube)
    bool water_hint = false;
    int filter_preset = 1;  // 0=OFF, 1=HOB, 2=DRK
    float ao_strength_scale = 1.0f;
    float sscs_strength_scale = 1.0f;
    float aerial_strength_scale = 1.0f;
    float ssr_strength_scale = 1.0f;
    float rain_strength = 0.0f;
    float time_of_day = -1.0f;
    float sscs_key_dir[2] = {0.42f, -0.907f};
    bool has_sscs_key_dir = false;
    int map_fog_mode = 0;  // Ogre::FogMode: 0=NONE, 1=EXP, 2=EXP2, 3=LINEAR
    float map_fog_rgb[3] = {0.0f, 0.0f, 0.0f};
    float map_fog_start = 0.0f;
    float map_fog_end = 0.0f;
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
struct RouteCPostWorldRequest {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* fallback_rtv = nullptr;  // World RT (e.g. c2/rt_output or main backbuffer)
    int vp_left = 0;
    int vp_top = 0;
    int vp_width = 0;
    int vp_height = 0;
    int debug_mode = 0;
    bool ssr_enabled = false;
    int lut_preset = 0;     // 0=OFF, 1=CinemaTealOrange, 2=ColdNordic, 3=Custom(.cube)
    bool water_hint = false;
    int filter_preset = 1;  // 0=OFF, 1=HOB, 2=DRK
    float ssr_strength_scale = 1.0f;
    float rain_strength = 0.0f;
    float time_of_day = -1.0f;
    float lut_strength_scale = 1.0f;
};
bool ExecuteRouteCPostWorldPass(const RouteCPostWorldRequest& req, RouteCD3DLogFn logger);
void ResetRouteCPostWorldDepthSnapshot();
struct RouteCP2FilterRequest {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* fallback_rtv = nullptr;
    int vp_left = 0;
    int vp_top = 0;
    int vp_width = 0;
    int vp_height = 0;
    int filter_preset = 1;  // 0=OFF, 1=HOB, 2=DRK
    bool vignette_enabled = false;
    float filter_strength_scale = 1.0f;
    float vignette_strength_scale = 1.0f;
};
bool ExecuteRouteCP2FilterPass(const RouteCP2FilterRequest& req, RouteCD3DLogFn logger);
struct RouteCPanelOverlayRequest {
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* target_rtv = nullptr;
    const void* bgra_pixels = nullptr;
    uint32_t tex_w = 0;
    uint32_t tex_h = 0;
    bool upload_dirty = false;
    float box_x = 0.0f;
    float box_y = 0.0f;
    float box_w = 0.0f;
    float box_h = 0.0f;
    float cursor_x = 0.0f;
    float cursor_y = 0.0f;
    bool draw_cursor = false;
    float panel_alpha = 1.0f;
};

bool ExecuteRouteCPanelOverlay(const RouteCPanelOverlayRequest& req, RouteCD3DLogFn logger);
bool ExecuteRouteCToastOverlay(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    bool dll_enhance_on,
    bool ao_on,
    int filter_preset,
    bool ap_on,
    float fade_alpha,
    RouteCD3DLogFn logger);
bool VerifyRouteCShadersCompile(RouteCD3DLogFn logger);
void ShutdownRouteCD3D11Resources();
RouteCD3DStats GetRouteCD3DStats();
bool IsCustomCubeLutLoaded();

// P1 Screen-Space Soft Particles depth sharing & b6/t7 state binding.
struct RouteCParticleDepthSnapshot {
    float proj_22 = 0.0f;
    float proj_23 = 0.0f;
    int32_t clamp_rect[4]{};
    bool valid = false;
};
bool GetRouteCParticleDepthSnapshot(RouteCParticleDepthSnapshot* out);
bool BindRouteCParticleState(
    ID3D11DeviceContext* ctx,
    int soft_mode,
    float inv_soft_range,
    const RouteCParticleDepthSnapshot* snap);
void ResetRouteCParticleDepthSnapshot();

}  // namespace routeb
