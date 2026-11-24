#pragma once

#include <cstddef>
#include <cstdint>
#include <dxgiformat.h>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11DepthStencilView;

namespace routeb {

using PostLogFn = void (*)(const char* fmt, ...);
using RouteCD3DLogFn = PostLogFn;
using RouteCLogFn = PostLogFn;

// 192 bytes = 12 float4 HLSL registers matching cbuffer CbAoParams in hlsl
struct alignas(16) CbAoParams {
    float proj_xy[4];              // (p00, p11, p02, p12)
    float proj_z_fade[4];          // (p22, p23, fadeEnd, invFadeRange)
    float vp_rect[4];              // (vpOriginX, vpOriginY, vpWidth, vpHeight)
    int32_t vp_clamp[4];           // (minX, minY, maxX, maxY)
    float ao_params[4];            // (radiusWorld, biasCos, strength, minAoFloor)
    float blur_and_mode[4];        // (invVpW, invVpH, depthEdgeScale, debugMode)
    float sscs_params[4];          // (sscsEnabled, sscsStrength, tintStrength, aerialMode)
    float sscs_key_and_range[4];   // (keyDirX, keyDirY, aerialStart, invAerialSpan)
    float aerial_fog_params[4];    // (hazeR/gTimeSsr, hazeG, hazeB, maxAerialAlpha)
    float view_up_and_water[4];    // (worldUpVs.xyz, waterHint)
    float inv_view_row0[4];        // (worldXBasisVs.xyz, camPosVsToWorldOffsetX)
    float inv_view_row2[4];        // (worldZBasisVs.xyz, camPosVsToWorldOffsetZ)
};
static_assert(sizeof(CbAoParams) == 192, "CbAoParams must match 12 float4 HLSL registers");

struct alignas(16) CbPanelParams {
    float rect[4];   // box_x, box_y, box_w, box_h
    float extra[4];  // cursor_x, cursor_y, panel_alpha, 0
};
static_assert(sizeof(CbPanelParams) == 32, "CbPanelParams must be 32 bytes");

struct alignas(16) CbToastParams {
    float rect[4];    // left, top, width, height
    float params[4];  // dllOn, aoOn, fadeAlpha, scale
    float extra[4];   // filterMode, 0, 0, 0
};
static_assert(sizeof(CbToastParams) == 48, "CbToastParams must be 48 bytes");

struct alignas(16) CbP2FilterParams {
    float params[4];   // preset, enable, vignetteEnable, 0
    float vp_rect[4];  // vpLeft, vpTop, vpWidth, vpHeight
};
static_assert(sizeof(CbP2FilterParams) == 32, "CbP2FilterParams must be 32 bytes");

struct alignas(16) CbBloomLutParams {
    float dst_vp_rect[4];     // xy=dstOriginPx, zw=dstSizePx
    float src_uv_rect[4];     // xy=srcUvOrigin, zw=srcUvSize
    float texel_and_bias[4];  // xy=invSrcTexSize, z=threshold, w=knee
    float mix_and_lut[4];     // x=upsampleMixOrBloomStrength, y=lutSize, z=lutInvSize, w=lutMix
};
static_assert(sizeof(CbBloomLutParams) == 64, "CbBloomLutParams must be 64 bytes");

struct ParticleParamsTexData {
    float soft_params[4];   // mode (0=off, 1=alpha, 2=rgb), invSoftRange, proj22, proj23
    float clamp_rect[4];    // minX, minY, maxX, maxY
};
static_assert(sizeof(ParticleParamsTexData) == 32, "ParticleParamsTexData must be 32 bytes");

struct DepthCopyFormats {
    bool supported = false;
    DXGI_FORMAT tex_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
};

struct AoRenderRequest {
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
    bool is_desert = false;
    bool is_menu = false;
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
using RouteCAoRenderRequest = AoRenderRequest;

struct PostWorldRequest {
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
    bool is_desert = false;
    bool is_menu = false;
};
using RouteCPostWorldRequest = PostWorldRequest;

struct P2FilterRequest {
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
using RouteCP2FilterRequest = P2FilterRequest;

struct PanelOverlayRequest {
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
using RouteCPanelOverlayRequest = PanelOverlayRequest;

struct ParticleDepthSnapshot {
    float proj_22 = 0.0f;
    float proj_23 = 0.0f;
    int32_t clamp_rect[4]{};
    bool valid = false;
};
using RouteCParticleDepthSnapshot = ParticleDepthSnapshot;

struct PostRenderStats {
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
using RouteCD3DStats = PostRenderStats;

struct PipelineStats {
    uint64_t render_scene_calls = 0;
    uint64_t perspective_world_candidates = 0;
    uint64_t queue_started_calls = 0;
    uint64_t queue_ended_calls = 0;
    uint64_t survey_unique_targets = 0;
    uint64_t survey_unique_queues = 0;
    bool hooked = false;
    bool c0_survey_active = false;
};
using RouteCStats = PipelineStats;

}  // namespace routeb
