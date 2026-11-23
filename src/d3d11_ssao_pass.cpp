#include "d3d11_ssao_pass.h"
#include "d3d11_post_renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace routeb {

float ReadEnvFloat(const char* name, float fallback, float min_v, float max_v) {
    char buf[32] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0 || len >= sizeof(buf)) {
        return fallback;
    }
    const float val = static_cast<float>(std::atof(buf));
    return (std::max)(min_v, (std::min)(max_v, val));
}

bool EnvFlagIsOne(const char* name) {
    char buf[16] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return (len == 1 && (buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T'));
}

bool EnvFlagIsZero(const char* name) {
    char buf[16] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return (len == 1 && (buf[0] == '0' || buf[0] == 'n' || buf[0] == 'N' || buf[0] == 'f' || buf[0] == 'F'));
}

const AoTuneConfig& GetAoTuneConfig(PostLogFn logger) {
    static AoTuneConfig cfg{};
    if (!cfg.initialized) {
        cfg.initialized = true;
        cfg.radius_world = ReadEnvFloat("TL2_C_AO_RADIUS", 0.95f, 0.10f, 8.0f);
        cfg.bias_cos = ReadEnvFloat("TL2_C_AO_BIAS", 0.24f, 0.01f, 0.50f);
        cfg.strength = ReadEnvFloat("TL2_C_AO_STRENGTH", 1.20f, 0.0f, 4.0f);
        cfg.min_ao_floor = ReadEnvFloat("TL2_C_AO_FLOOR", 0.58f, 0.20f, 0.98f);
        cfg.depth_edge_scale = ReadEnvFloat("TL2_C_AO_EDGE", 55.0f, 5.0f, 300.0f);
        cfg.fade_start = ReadEnvFloat("TL2_C_AO_FADESTART", 42.0f, 5.0f, 150.0f);
        cfg.fade_end_cap = ReadEnvFloat("TL2_C_AO_FADEEND", 78.0f, 10.0f, 250.0f);
        if (EnvFlagIsZero("TL2_C_AO_BENCH")) {
            cfg.bench_mode = 0;
        } else if (EnvFlagIsOne("TL2_C_AO_BENCH")) {
            cfg.bench_mode = 2;
        } else {
            cfg.bench_mode = 1;
        }
        if (logger) {
            logger(
                "[RouteC][C3] Config halfRes=1 radius=%.2f bias=%.2f strength=%.2f floor=%.2f edge=%.1f fade=(%.1f,%.1f) bench=%d",
                cfg.radius_world,
                cfg.bias_cos,
                cfg.strength,
                cfg.min_ao_floor,
                cfg.depth_edge_scale,
                cfg.fade_start,
                cfg.fade_end_cap,
                cfg.bench_mode);
        }
    }
    return cfg;
}
void PopulateDynamicAoAerialParams(const AoRenderRequest& req, CbAoParams& cb) {
    float kx = req.sscs_key_dir[0];
    float ky = req.sscs_key_dir[1];
    const float klen = std::sqrt(kx * kx + ky * ky);
    if (std::isfinite(klen) && klen > 1e-4f) {
        kx /= klen;
        ky /= klen;
    } else {
        kx = 0.42f;
        ky = -0.907f;
    }

    const bool is_dark = (req.filter_preset == 2);
    const float base_rgb[3] = {
        is_dark ? 0.055f : 0.115f,
        is_dark ? 0.085f : 0.170f,
        is_dark ? 0.135f : 0.255f};
    const float max_alpha = is_dark ? 0.25f : 0.21f;

    float haze_rgb[3] = {base_rgb[0], base_rgb[1], base_rgb[2]};
    if (req.map_fog_mode != 0 &&
        std::isfinite(req.map_fog_rgb[0]) &&
        std::isfinite(req.map_fog_rgb[1]) &&
        std::isfinite(req.map_fog_rgb[2])) {
        const float fr = (std::max)(0.0f, (std::min)(1.0f, req.map_fog_rgb[0]));
        const float fg = (std::max)(0.0f, (std::min)(1.0f, req.map_fog_rgb[1]));
        const float fb = (std::max)(0.0f, (std::min)(1.0f, req.map_fog_rgb[2]));
        const float fog_lum = 0.299f * fr + 0.587f * fg + 0.114f * fb;
        if (fog_lum > 0.015f) {
            const float target_lum = is_dark
                ? (std::max)(0.07f, (std::min)(0.16f, fog_lum * 0.45f))
                : (std::max)(0.14f, (std::min)(0.28f, fog_lum * 0.65f));
            const float scale = target_lum / fog_lum;
            const float mr = (std::min)(0.45f, fr * scale);
            const float mg = (std::min)(0.45f, fg * scale);
            const float mb = (std::min)(0.45f, fb * scale);
            constexpr float kMapTintWeight = 0.72f;
            haze_rgb[0] = base_rgb[0] * (1.0f - kMapTintWeight) + mr * kMapTintWeight;
            haze_rgb[1] = base_rgb[1] * (1.0f - kMapTintWeight) + mg * kMapTintWeight;
            haze_rgb[2] = base_rgb[2] * (1.0f - kMapTintWeight) + mb * kMapTintWeight;
        }
    }

    float aerial_start = 24.0f;
    float aerial_end = 54.0f;
    if (req.map_fog_mode == 3 &&
        std::isfinite(req.map_fog_start) &&
        std::isfinite(req.map_fog_end) &&
        req.map_fog_end > req.map_fog_start + 5.0f) {
        aerial_start = (std::max)(14.0f, (std::min)(36.0f, 24.0f * 0.45f + req.map_fog_start * 0.55f));
        aerial_end = (std::max)(aerial_start + 18.0f, (std::min)(75.0f, 54.0f * 0.55f + req.map_fog_end * 0.45f));
    }

    cb.sscs_key_and_range[0] = kx;
    cb.sscs_key_and_range[1] = ky;
    cb.sscs_key_and_range[2] = aerial_start;
    cb.sscs_key_and_range[3] = 1.0f / (std::max)(aerial_end - aerial_start, 1e-3f);
    cb.aerial_fog_params[0] = haze_rgb[0];
    cb.aerial_fog_params[1] = haze_rgb[1];
    cb.aerial_fog_params[2] = haze_rgb[2];
    const float aerial_scale = (std::clamp)(req.aerial_strength_scale, 0.0f, 2.0f);
    cb.aerial_fog_params[3] = req.aerial_enabled ? (std::clamp)(max_alpha * aerial_scale, 0.0f, 0.65f) : 0.0f;

    if (req.view_4x4) {
        const float* V = req.view_4x4;
        // View matrix rows: row0=right_ws, row1=up_ws, row2=back_ws, col3=t_vs.
        // World X basis in view space = col 0 (V[0], V[4], V[8])
        // World Y (Up) in view space = col 1 (V[1], V[5], V[9])
        // World Z basis in view space = col 2 (V[2], V[6], V[10])
        float ux = V[1], uy = V[5], uz = V[9];
        const float ulen = std::sqrt(ux * ux + uy * uy + uz * uz);
        if (std::isfinite(ulen) && ulen > 1e-4f) {
            ux /= ulen;
            uy /= ulen;
            uz /= ulen;
        } else {
            ux = 0.0f;
            uy = 0.7071f;
            uz = 0.7071f;
        }
        const float tx = V[3], ty = V[7], tz = V[11];
        cb.view_up_and_water[0] = ux;
        cb.view_up_and_water[1] = uy;
        cb.view_up_and_water[2] = uz;
        cb.view_up_and_water[3] = req.water_hint ? 1.0f : 0.0f;

        cb.inv_view_row0[0] = V[0];
        cb.inv_view_row0[1] = V[4];
        cb.inv_view_row0[2] = V[8];
        cb.inv_view_row0[3] = -(V[0] * tx + V[4] * ty + V[8] * tz);

        cb.inv_view_row1[0] = V[1];
        cb.inv_view_row1[1] = V[5];
        cb.inv_view_row1[2] = V[9];
        cb.inv_view_row1[3] = -(V[1] * tx + V[5] * ty + V[9] * tz);

        cb.inv_view_row2[0] = V[2];
        cb.inv_view_row2[1] = V[6];
        cb.inv_view_row2[2] = V[10];
        cb.inv_view_row2[3] = -(V[2] * tx + V[6] * ty + V[10] * tz);
    } else {
        cb.view_up_and_water[0] = 0.0f;
        cb.view_up_and_water[1] = 0.7071f;
        cb.view_up_and_water[2] = 0.7071f;
        cb.view_up_and_water[3] = req.water_hint ? 1.0f : 0.0f;
        cb.inv_view_row0[0] = 1.0f;
        cb.inv_view_row1[1] = 1.0f;
        cb.inv_view_row2[2] = 1.0f;
    }

    // Continuous ToD color LUT + World Ground Height Fog parameters
    if (req.height_fog_enabled && !req.is_menu && req.height_fog_strength_scale > 0.001f) {
        static const float kTodKeyColors[4][3] = {
            {0.055f, 0.095f, 0.175f},  // 00:00 Midnight deep blue
            {0.360f, 0.275f, 0.245f},  // 06:00 Dawn warm rose gold
            {0.175f, 0.235f, 0.305f},  // 12:00 Noon clear sky blue
            {0.420f, 0.240f, 0.165f},  // 18:00 Dusk amber violet
        };
        float tod = (req.time_of_day >= 0.0f) ? std::fmod(req.time_of_day, 24.0f) : 12.0f;
        if (tod < 0.0f) tod += 24.0f;
        const float scaled = tod / 6.0f;
        const int seg0 = static_cast<int>(scaled) & 3;
        const int seg1 = (seg0 + 1) & 3;
        const float frac = scaled - std::floor(scaled);
        const float smooth_t = frac * frac * (3.0f - 2.0f * frac);

        float hf_r = kTodKeyColors[seg0][0] + (kTodKeyColors[seg1][0] - kTodKeyColors[seg0][0]) * smooth_t;
        float hf_g = kTodKeyColors[seg0][1] + (kTodKeyColors[seg1][1] - kTodKeyColors[seg0][1]) * smooth_t;
        float hf_b = kTodKeyColors[seg0][2] + (kTodKeyColors[seg1][2] - kTodKeyColors[seg0][2]) * smooth_t;

        if (req.is_desert) {
            hf_r = hf_r * 0.65f + 0.380f * 0.35f;
            hf_g = hf_g * 0.65f + 0.290f * 0.35f;
            hf_b = hf_b * 0.65f + 0.180f * 0.35f;
        }
        if (is_dark) {
            hf_r *= 0.72f;
            hf_g *= 0.78f;
            hf_b *= 0.88f;
        }
        if (req.map_fog_mode != 0 &&
            std::isfinite(req.map_fog_rgb[0]) &&
            std::isfinite(req.map_fog_rgb[1]) &&
            std::isfinite(req.map_fog_rgb[2])) {
            const float fr = (std::clamp)(req.map_fog_rgb[0], 0.0f, 1.0f);
            const float fg = (std::clamp)(req.map_fog_rgb[1], 0.0f, 1.0f);
            const float fb = (std::clamp)(req.map_fog_rgb[2], 0.0f, 1.0f);
            const float fl = 0.299f * fr + 0.587f * fg + 0.114f * fb;
            if (fl > 0.015f) {
                const float target_l = is_dark ? (std::clamp)(fl * 0.52f, 0.08f, 0.22f)
                                               : (std::clamp)(fl * 0.72f, 0.16f, 0.34f);
                const float sc = target_l / fl;
                hf_r = hf_r * 0.55f + (std::min)(0.52f, fr * sc) * 0.45f;
                hf_g = hf_g * 0.55f + (std::min)(0.52f, fg * sc) * 0.45f;
                hf_b = hf_b * 0.55f + (std::min)(0.52f, fb * sc) * 0.45f;
            }
        }
        const float rain_clamp = (std::clamp)(req.rain_strength, 0.0f, 1.5f);
        if (rain_clamp > 0.01f) {
            hf_r = hf_r * (1.0f - 0.22f * rain_clamp) + 0.16f * (0.22f * rain_clamp);
            hf_g = hf_g * (1.0f - 0.22f * rain_clamp) + 0.21f * (0.22f * rain_clamp);
            hf_b = hf_b * (1.0f - 0.22f * rain_clamp) + 0.27f * (0.22f * rain_clamp);
        }

        const float base_max_alpha = (req.is_desert ? 0.16f : 0.22f) * (1.0f + 0.28f * rain_clamp);
        const float h_scale = (std::clamp)(req.height_fog_strength_scale, 0.0f, 2.0f);
        cb.height_fog_color[0] = hf_r;
        cb.height_fog_color[1] = hf_g;
        cb.height_fog_color[2] = hf_b;
        cb.height_fog_color[3] = (std::clamp)(base_max_alpha * h_scale, 0.0f, 0.55f);
        cb.height_fog_shape[0] = req.is_desert ? 0.22f : 0.32f;   // heightFalloffK
        cb.height_fog_shape[1] = req.is_desert ? 0.014f : 0.020f; // distDensity
        cb.height_fog_shape[2] = -0.35f;                          // baseOffsetY
        cb.height_fog_shape[3] = (tod >= 5.0f && tod <= 19.5f && !is_dark) ? 0.25f : 0.08f;
    } else {
        cb.height_fog_color[0] = 0.0f;
        cb.height_fog_color[1] = 0.0f;
        cb.height_fog_color[2] = 0.0f;
        cb.height_fog_color[3] = 0.0f;
        cb.height_fog_shape[0] = 0.0f;
        cb.height_fog_shape[1] = 0.0f;
        cb.height_fog_shape[2] = 0.0f;
        cb.height_fog_shape[3] = 0.0f;
    }
}
bool ExecuteAoPass(const AoRenderRequest& req, PostLogFn logger) {
    const bool has_atmos = req.aerial_enabled || req.height_fog_enabled;
    const bool need_depth = req.ao_enabled || req.capture_particle_depth || has_atmos || req.ssr_enabled || req.ssgi_enabled;
    if (!req.ctx || !req.proj_4x4 || req.vp_width <= 0 || req.vp_height <= 0 || !need_depth) {
        return false;
    }

    PostLockScope lock_guard;

    g_st.particle_depth_valid = false;
    g_st.post_world_depth_valid = false;
    g_st.post_world_msaa = false;
    D3D11StateGuard state_guard(req.ctx);

    ID3D11RenderTargetView* target_rtv = state_guard.om_rtv0() ? state_guard.om_rtv0() : req.fallback_rtv;
    ID3D11DepthStencilView* target_dsv = state_guard.om_dsv() ? state_guard.om_dsv() : req.fallback_dsv;
    if (!target_rtv || (need_depth && !target_dsv)) {
        g_inject_skip_no_om.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ID3D11Resource* rtv_res = nullptr;
    target_rtv->GetResource(&rtv_res);
    ID3D11Texture2D* src_rt_tex = nullptr;
    if (!rtv_res ||
        FAILED(rtv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src_rt_tex))) ||
        !src_rt_tex) {
        SafeRelease(src_rt_tex);
        SafeRelease(rtv_res);
        g_inject_skip_no_om.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    SafeRelease(rtv_res);
    D3D11_TEXTURE2D_DESC rt_desc{};
    src_rt_tex->GetDesc(&rt_desc);
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    target_rtv->GetDesc(&rtv_desc);

    ID3D11Device* dev = nullptr;
    req.ctx->GetDevice(&dev);
    if (!dev) {
        SafeRelease(src_rt_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    struct DevRtGuard {
        ID3D11Device* d = nullptr;
        ID3D11Texture2D* rt = nullptr;
        ~DevRtGuard() {
            SafeRelease(rt);
            SafeRelease(d);
        }
    } res_guard{dev, src_rt_tex};

    if (!EnsurePipelineObjects(dev, logger)) {
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    D3D11_TEXTURE2D_DESC depth_desc{};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    bool use_msaa = false;
    const DepthVariantShaders* ds = &g_st.shaders_single;
    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    const AoTuneConfig& cfg = GetAoTuneConfig(logger);
    BenchSlot* bench_slot = (req.ao_enabled || req.aerial_enabled) ? BeginBenchSlot(dev, req.ctx, cfg) : nullptr;
    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};

    if (need_depth) {
        ID3D11Resource* depth_res = nullptr;
        target_dsv->GetResource(&depth_res);
        ID3D11Texture2D* src_depth_tex = nullptr;
        if (!depth_res || FAILED(depth_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src_depth_tex))) || !src_depth_tex) {
            SafeRelease(src_depth_tex);
            SafeRelease(depth_res);
            if (bench_slot) {
                req.ctx->End(bench_slot->disjoint);
                bench_slot->in_flight = false;
            }
            g_inject_skip_no_om.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        SafeRelease(depth_res);

        src_depth_tex->GetDesc(&depth_desc);
        target_dsv->GetDesc(&dsv_desc);
        const DepthCopyFormats fmts = MapDepthCopyFormats(depth_desc.Format, dsv_desc.Format);
        if (!fmts.supported) {
            SafeRelease(src_depth_tex);
            if (bench_slot) {
                req.ctx->End(bench_slot->disjoint);
                bench_slot->in_flight = false;
            }
            g_inject_skip_fmt.fetch_add(1, std::memory_order_relaxed);
            if (!g_st.logged_unsupported_fmt && logger) {
                g_st.logged_unsupported_fmt = true;
                logger("[RouteC][D3D11] Unsupported depth format texFmt=%u dsvFmt=%u; skipping AO.",
                       static_cast<unsigned>(depth_desc.Format),
                       static_cast<unsigned>(dsv_desc.Format));
            }
            return false;
        }

        UINT src_mip = 0;
        const UINT src_subres = ComputeDsvSubresource(dsv_desc, depth_desc.MipLevels, &src_mip);
        if (src_mip != 0 || !EnsureAoTargets(dev, depth_desc, fmts, logger)) {
            SafeRelease(src_depth_tex);
            if (bench_slot) {
                req.ctx->End(bench_slot->disjoint);
                bench_slot->in_flight = false;
            }
            g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        use_msaa = (depth_desc.SampleDesc.Count > 1);
        if (use_msaa && !g_st.msaa_shaders_ready) {
            SafeRelease(src_depth_tex);
            if (bench_slot) {
                req.ctx->End(bench_slot->disjoint);
                bench_slot->in_flight = false;
            }
            g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        ds = use_msaa ? &g_st.shaders_msaa : &g_st.shaders_single;

        min_x = (std::max)(0, req.vp_left);
        min_y = (std::max)(0, req.vp_top);
        max_x = (std::min)(static_cast<int>(depth_desc.Width) - 1, req.vp_left + req.vp_width - 1);
        max_y = (std::min)(static_cast<int>(depth_desc.Height) - 1, req.vp_top + req.vp_height - 1);
        if (min_x > max_x || min_y > max_y) {
            SafeRelease(src_depth_tex);
            if (bench_slot) {
                req.ctx->End(bench_slot->disjoint);
                bench_slot->in_flight = false;
            }
            g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        req.ctx->PSSetShaderResources(0, 2, null_srvs);
        req.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        req.ctx->CopySubresourceRegion(g_st.depth_copy_tex, 0, 0, 0, 0, src_depth_tex, src_subres, nullptr);
        SafeRelease(src_depth_tex);
        RecordTimestamp(req.ctx, bench_slot, 1);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(req.ctx->Map(g_st.cb_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
            if (bench_slot) {
                req.ctx->End(bench_slot->disjoint);
                bench_slot->in_flight = false;
            }
            g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const float* P = req.proj_4x4;
        const float fade_start = cfg.fade_start;
        const float fade_end = (std::max)(fade_start + 5.0f, (std::min)(req.far_clip * 0.92f, cfg.fade_end_cap));
        CbAoParams cb{};
        cb.proj_xy[0] = P[0];   // m[0][0]
        cb.proj_xy[1] = P[5];   // m[1][1]
        cb.proj_xy[2] = P[2];   // m[0][2]
        cb.proj_xy[3] = P[6];   // m[1][2]
        cb.proj_z_fade[0] = P[10];  // m[2][2]
        cb.proj_z_fade[1] = P[11];  // m[2][3]
        cb.proj_z_fade[2] = fade_end;
        cb.proj_z_fade[3] = 1.0f / (std::max)(fade_end - fade_start, 1e-3f);
        cb.vp_rect[0] = static_cast<float>(req.vp_left);
        cb.vp_rect[1] = static_cast<float>(req.vp_top);
        cb.vp_rect[2] = static_cast<float>(req.vp_width);
        cb.vp_rect[3] = static_cast<float>(req.vp_height);
        cb.vp_clamp[0] = min_x;
        cb.vp_clamp[1] = min_y;
        cb.vp_clamp[2] = max_x;
        cb.vp_clamp[3] = max_y;
        const float ao_scale = (std::clamp)(req.ao_strength_scale, 0.0f, 2.0f);
        const float sscs_scale = (std::clamp)(req.sscs_strength_scale, 0.0f, 2.0f);
        const float extra_ao_floor = (ao_scale > 1.0f) ? (ao_scale - 1.0f) * 0.22f : 0.0f;
        cb.ao_params[0] = cfg.radius_world;
        cb.ao_params[1] = cfg.bias_cos;
        cb.ao_params[2] = cfg.strength * ao_scale;
        cb.ao_params[3] = (std::clamp)(cfg.min_ao_floor - extra_ao_floor, 0.30f, 0.90f);
        cb.blur_and_mode[0] = 1.0f / static_cast<float>(req.vp_width);
        cb.blur_and_mode[1] = 1.0f / static_cast<float>(req.vp_height);
        cb.blur_and_mode[2] = cfg.depth_edge_scale;
        cb.blur_and_mode[3] = static_cast<float>(req.debug_mode);
        cb.sscs_params[0] = req.sscs_enabled ? 1.0f : 0.0f;
        cb.sscs_params[1] = 1.15f * sscs_scale;
        cb.sscs_params[2] = (std::clamp)(0.90f * sscs_scale, 0.0f, 1.0f);
        cb.sscs_params[3] = req.aerial_enabled ? ((req.filter_preset == 2) ? 2.0f : 1.0f) : 0.0f;
        PopulateDynamicAoAerialParams(req, cb);
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        req.ctx->Unmap(g_st.cb_params, 0);
        std::memcpy(g_st.post_world_cb_bytes, &cb, sizeof(cb));
        g_st.post_world_depth_valid = true;
        g_st.post_world_msaa = use_msaa;

        g_last_tex_w.store(depth_desc.Width, std::memory_order_relaxed);
        g_last_tex_h.store(depth_desc.Height, std::memory_order_relaxed);
        g_last_dsv_tex_fmt.store(static_cast<uint32_t>(depth_desc.Format), std::memory_order_relaxed);
        g_last_sample_count.store(depth_desc.SampleDesc.Count, std::memory_order_relaxed);
    } else {
        min_x = (std::max)(0, req.vp_left);
        min_y = (std::max)(0, req.vp_top);
        max_x = (std::min)(static_cast<int>(rt_desc.Width) - 1, req.vp_left + req.vp_width - 1);
        max_y = (std::min)(static_cast<int>(rt_desc.Height) - 1, req.vp_top + req.vp_height - 1);
        if (min_x > max_x || min_y > max_y) {
            if (bench_slot) {
                req.ctx->End(bench_slot->disjoint);
                bench_slot->in_flight = false;
            }
            g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    g_last_rtv_fmt.store(static_cast<uint32_t>(rtv_desc.Format), std::memory_order_relaxed);

    req.ctx->OMSetDepthStencilState(g_st.dss_disabled, 0);
    req.ctx->RSSetState(g_st.rs_fullscreen);
    req.ctx->RSSetScissorRects(0, nullptr);
    req.ctx->IASetInputLayout(nullptr);
    req.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    req.ctx->VSSetShader(g_st.vs_fullscreen, nullptr, 0);
    req.ctx->GSSetShader(nullptr, nullptr, 0);
    req.ctx->PSSetSamplers(0, 1, &g_st.samp_linear);

    if (need_depth && req.capture_particle_depth) {
        const float* P = req.proj_4x4;
        bool resolved_ok = true;
        if (use_msaa) {
            resolved_ok = g_st.ps_resolve_depth_msaa != nullptr &&
                          EnsureDepthResolveTarget(dev, depth_desc.Width, depth_desc.Height, logger);
            if (resolved_ok) {
                D3D11_VIEWPORT full_vp{};
                full_vp.Width = static_cast<float>(depth_desc.Width);
                full_vp.Height = static_cast<float>(depth_desc.Height);
                full_vp.MaxDepth = 1.0f;
                req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_params);
                req.ctx->RSSetViewports(1, &full_vp);
                req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
                req.ctx->OMSetRenderTargets(1, &g_st.depth_resolve_rtv, nullptr);
                req.ctx->PSSetShader(g_st.ps_resolve_depth_msaa, nullptr, 0);
                ID3D11ShaderResourceView* resolve_srvs[2] = {g_st.depth_copy_srv, nullptr};
                req.ctx->PSSetShaderResources(0, 2, resolve_srvs);
                req.ctx->Draw(3, 0);
                req.ctx->OMSetRenderTargets(0, nullptr, nullptr);
                req.ctx->PSSetShaderResources(0, 2, null_srvs);
            }
        }
        if (resolved_ok) {
            g_st.particle_depth_valid = true;
            g_st.particle_proj_22 = P[10];
            g_st.particle_proj_23 = P[11];
            g_st.particle_clamp[0] = min_x;
            g_st.particle_clamp[1] = min_y;
            g_st.particle_clamp[2] = max_x;
            g_st.particle_clamp[3] = max_y;
        }
    }

    D3D11_VIEWPORT ao_vp{};
    ao_vp.TopLeftX = static_cast<float>(req.vp_left);
    ao_vp.TopLeftY = static_cast<float>(req.vp_top);
    ao_vp.Width = static_cast<float>(req.vp_width);
    ao_vp.Height = static_cast<float>(req.vp_height);
    ao_vp.MinDepth = 0.0f;
    ao_vp.MaxDepth = 1.0f;

    if (need_depth) {
        req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_params);
    }

    if (req.ao_enabled) {
        const int half_w = (std::max)(1, ((req.vp_left + req.vp_width - 1) >> 1) - (req.vp_left >> 1) + 1);
        const int half_h = (std::max)(1, ((req.vp_top + req.vp_height - 1) >> 1) - (req.vp_top >> 1) + 1);
        D3D11_VIEWPORT ao_vp_half{};
        ao_vp_half.TopLeftX = static_cast<float>(req.vp_left >> 1);
        ao_vp_half.TopLeftY = static_cast<float>(req.vp_top >> 1);
        ao_vp_half.Width = static_cast<float>(half_w);
        ao_vp_half.Height = static_cast<float>(half_h);
        ao_vp_half.MinDepth = 0.0f;
        ao_vp_half.MaxDepth = 1.0f;

        if (req.debug_mode == 3) {
            req.ctx->RSSetViewports(1, &ao_vp);
            req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
            req.ctx->OMSetBlendState(g_st.blend_debug_rgb, nullptr, 0xFFFFFFFFU);
            req.ctx->PSSetShader(ds->ps_c1_debug, nullptr, 0);
            ID3D11ShaderResourceView* srvs_c1[2] = {g_st.depth_copy_srv, nullptr};
            req.ctx->PSSetShaderResources(0, 2, srvs_c1);
            req.ctx->Draw(3, 0);
            RecordTimestamp(req.ctx, bench_slot, 2);
            RecordTimestamp(req.ctx, bench_slot, 3);
            RecordTimestamp(req.ctx, bench_slot, 4);
        } else {
            // Pass 1 (Half-Res): HBAO/GTAO-Lite Raw -> ao_rt_rtv
            req.ctx->RSSetViewports(1, &ao_vp_half);
            req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
            req.ctx->OMSetRenderTargets(1, &g_st.ao_rt_rtv, nullptr);
            req.ctx->PSSetShader(ds->ps_ao_raw, nullptr, 0);
            ID3D11ShaderResourceView* srvs_p1[2] = {g_st.depth_copy_srv, nullptr};
            req.ctx->PSSetShaderResources(0, 2, srvs_p1);
            req.ctx->Draw(3, 0);
            RecordTimestamp(req.ctx, bench_slot, 2);

            if (req.debug_mode != 1) {
                // Pass 2 (Half-Res): Bilateral Blur Horizontal -> ao_blur_rtv
                req.ctx->OMSetRenderTargets(1, &g_st.ao_blur_rtv, nullptr);
                req.ctx->PSSetShader(ds->ps_blur_h, nullptr, 0);
                ID3D11ShaderResourceView* srvs_p2[2] = {g_st.depth_copy_srv, g_st.ao_rt_srv};
                req.ctx->PSSetShaderResources(0, 2, srvs_p2);
                req.ctx->Draw(3, 0);
                RecordTimestamp(req.ctx, bench_slot, 3);

                // Pass 3 (Half-Res): Bilateral Blur Vertical -> ao_rt_rtv
                ID3D11ShaderResourceView* unbind_t1[2] = {g_st.depth_copy_srv, nullptr};
                req.ctx->PSSetShaderResources(0, 2, unbind_t1);
                req.ctx->OMSetRenderTargets(1, &g_st.ao_rt_rtv, nullptr);
                req.ctx->PSSetShader(ds->ps_blur_v, nullptr, 0);
                ID3D11ShaderResourceView* srvs_p3[2] = {g_st.depth_copy_srv, g_st.ao_blur_srv};
                req.ctx->PSSetShaderResources(0, 2, srvs_p3);
                req.ctx->Draw(3, 0);
                RecordTimestamp(req.ctx, bench_slot, 4);
            } else {
                RecordTimestamp(req.ctx, bench_slot, 3);
                RecordTimestamp(req.ctx, bench_slot, 4);
            }

            // Pass 4 (Full-Res Joint-Bilateral Upsample): single-pass Dual-Source AO+Atmos or RGB-only AO Composite
            const bool use_dual_composite =
                (req.debug_mode == 0) && has_atmos && ds->ps_comp_dual && g_st.blend_dual_ao;
            req.ctx->PSSetShaderResources(0, 2, null_srvs);
            req.ctx->RSSetViewports(1, &ao_vp);
            req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
            req.ctx->OMSetBlendState(
                use_dual_composite ? g_st.blend_dual_ao : ((req.debug_mode == 0) ? g_st.blend_mul_rgb : g_st.blend_debug_rgb),
                nullptr,
                0xFFFFFFFFU);
            req.ctx->PSSetShader(use_dual_composite ? ds->ps_comp_dual : ds->ps_composite, nullptr, 0);
            ID3D11ShaderResourceView* srvs_p4[2] = {g_st.depth_copy_srv, g_st.ao_rt_srv};
            req.ctx->PSSetShaderResources(0, 2, srvs_p4);
            req.ctx->Draw(3, 0);
        }
    } else if (has_atmos && bench_slot) {
        RecordTimestamp(req.ctx, bench_slot, 2);
        RecordTimestamp(req.ctx, bench_slot, 3);
        RecordTimestamp(req.ctx, bench_slot, 4);
    }

    const bool ran_dual_composite =
        req.ao_enabled && (req.debug_mode == 0) && has_atmos && ds->ps_comp_dual && g_st.blend_dual_ao;
    if (need_depth && has_atmos && req.debug_mode == 0 && !ran_dual_composite && ds->ps_aerial && g_st.blend_toast_alpha) {
        req.ctx->RSSetViewports(1, &ao_vp);
        req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
        req.ctx->OMSetBlendState(g_st.blend_toast_alpha, nullptr, 0xFFFFFFFFU);
        req.ctx->PSSetShader(ds->ps_aerial, nullptr, 0);
        ID3D11ShaderResourceView* srvs_aerial[2] = {g_st.depth_copy_srv, nullptr};
        req.ctx->PSSetShaderResources(0, 2, srvs_aerial);
        req.ctx->Draw(3, 0);
    }

    if (bench_slot) {
        RecordTimestamp(req.ctx, bench_slot, 5);
        req.ctx->End(bench_slot->disjoint);
    }

    if (!req.ao_enabled) {
        return g_st.particle_depth_valid || has_atmos || g_st.post_world_depth_valid;
    }

    g_inject_ok.fetch_add(1, std::memory_order_relaxed);
    return true;
}


}  // namespace routeb
