#include "d3d11_overlay_pass.h"
#include "d3d11_post_renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace routeb {

bool ExecuteP2FilterPass(const P2FilterRequest& req, PostLogFn logger) {
    if (!req.ctx || (req.filter_preset <= 0 && !req.vignette_enabled) ||
        req.vp_width <= 0 || req.vp_height <= 0) {
        return false;
    }

    D3D11StateGuard state_guard(req.ctx);
    ID3D11RenderTargetView* target_rtv = state_guard.om_rtv0() ? state_guard.om_rtv0() : req.fallback_rtv;
    if (!target_rtv) {
        return false;
    }

    ID3D11Resource* rtv_res = nullptr;
    target_rtv->GetResource(&rtv_res);
    ID3D11Texture2D* src_tex = nullptr;
    if (!rtv_res ||
        FAILED(rtv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src_tex))) ||
        !src_tex) {
        SafeRelease(src_tex);
        SafeRelease(rtv_res);
        return false;
    }
    SafeRelease(rtv_res);

    D3D11_TEXTURE2D_DESC rt_desc{};
    src_tex->GetDesc(&rt_desc);
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    target_rtv->GetDesc(&rtv_desc);
    if (rt_desc.Width == 0 || rt_desc.Height == 0 || rtv_desc.Format == DXGI_FORMAT_UNKNOWN) {
        SafeRelease(src_tex);
        return false;
    }

    ID3D11Device* dev = nullptr;
    req.ctx->GetDevice(&dev);
    if (!dev) {
        SafeRelease(src_tex);
        return false;
    }

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    struct LockGuard {
        CRITICAL_SECTION* cs;
        ID3D11Device* d;
        ID3D11Texture2D* t;
        ~LockGuard() {
            SafeRelease(t);
            SafeRelease(d);
            ::LeaveCriticalSection(cs);
        }
    } lock_guard{&g_d3d_lock, dev, src_tex};

    if (!EnsurePipelineObjects(dev, logger) ||
        !g_st.ps_p2_filter ||
        !g_st.cb_p2_filter ||
        !CopyRtvTextureToP2Copy(dev, req.ctx, src_tex, rt_desc, rtv_desc)) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(req.ctx->Map(g_st.cb_p2_filter, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        return false;
    }
    auto* cb = static_cast<CbP2FilterParams*>(mapped.pData);
    cb->params[0] = static_cast<float>(req.filter_preset);
    cb->params[1] = 1.0f;
    cb->params[2] = req.vignette_enabled ? (std::clamp)(req.vignette_strength_scale, 0.0f, 2.0f) : 0.0f;
    cb->params[3] = (std::clamp)(req.filter_strength_scale, 0.0f, 2.0f);
    cb->vp_rect[0] = static_cast<float>(req.vp_left);
    cb->vp_rect[1] = static_cast<float>(req.vp_top);
    cb->vp_rect[2] = static_cast<float>(req.vp_width);
    cb->vp_rect[3] = static_cast<float>(req.vp_height);
    req.ctx->Unmap(g_st.cb_p2_filter, 0);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(req.vp_left);
    vp.TopLeftY = static_cast<float>(req.vp_top);
    vp.Width = static_cast<float>(req.vp_width);
    vp.Height = static_cast<float>(req.vp_height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
    req.ctx->OMSetBlendState(g_st.blend_debug_rgb, nullptr, 0xFFFFFFFFU);
    req.ctx->OMSetDepthStencilState(g_st.dss_disabled, 0);
    req.ctx->RSSetState(g_st.rs_fullscreen);
    req.ctx->RSSetViewports(1, &vp);
    req.ctx->RSSetScissorRects(0, nullptr);
    req.ctx->IASetInputLayout(nullptr);
    req.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    req.ctx->VSSetShader(g_st.vs_fullscreen, nullptr, 0);
    req.ctx->GSSetShader(nullptr, nullptr, 0);
    req.ctx->PSSetShader(g_st.ps_p2_filter, nullptr, 0);
    req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_p2_filter);
    req.ctx->PSSetShaderResources(0, 1, &g_st.p2_copy_srv);
    req.ctx->Draw(3, 0);
    return true;
}

bool ExecuteToastOverlay(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    bool dll_enhance_on,
    bool ao_on,
    int filter_preset,
    bool ap_on,
    float fade_alpha,
    PostLogFn logger) {
    if (!ctx || !target_rtv || fade_alpha <= 0.001f) {
        return false;
    }

    ID3D11Resource* rtv_res = nullptr;
    target_rtv->GetResource(&rtv_res);
    if (!rtv_res) {
        return false;
    }
    ID3D11Texture2D* rtv_tex = nullptr;
    const HRESULT tex_hr = rtv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rtv_tex));
    SafeRelease(rtv_res);
    if (FAILED(tex_hr) || !rtv_tex) {
        return false;
    }
    D3D11_TEXTURE2D_DESC rt_desc{};
    rtv_tex->GetDesc(&rt_desc);
    SafeRelease(rtv_tex);
    if (rt_desc.Width < 160 || rt_desc.Height < 64) {
        return false;
    }

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) {
        return false;
    }

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    struct LockGuard {
        CRITICAL_SECTION* cs;
        ID3D11Device* d;
        ~LockGuard() {
            SafeRelease(d);
            ::LeaveCriticalSection(cs);
        }
    } lock_guard{&g_d3d_lock, dev};

    if (!EnsurePipelineObjects(dev, logger) || !g_st.ps_toast || !g_st.cb_toast || !g_st.blend_toast_alpha) {
        return false;
    }

    float scale = (rt_desc.Height >= 1400U) ? 3.0f : 2.0f;
    if (24.0f + (45.0f * 6.0f - 1.0f) * scale + 16.0f > static_cast<float>(rt_desc.Width) ||
        16.0f + 7.0f * scale + 16.0f > static_cast<float>(rt_desc.Height)) {
        scale = 1.0f;
    }
    const float box_w = 24.0f + (45.0f * 6.0f - 1.0f) * scale;
    const float box_h = 16.0f + 7.0f * scale;
    const float box_x = std::floor((std::max)(8.0f, (static_cast<float>(rt_desc.Width) - box_w) * 0.5f));
    const float box_y = (scale >= 3.0f) ? 26.0f : ((scale >= 2.0f) ? 18.0f : 8.0f);
    if (box_x + box_w > static_cast<float>(rt_desc.Width) || box_y + box_h > static_cast<float>(rt_desc.Height)) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g_st.cb_toast, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        return false;
    }
    auto* cb = static_cast<CbToastParams*>(mapped.pData);
    cb->rect[0] = box_x;
    cb->rect[1] = box_y;
    cb->rect[2] = box_w;
    cb->rect[3] = box_h;
    cb->params[0] = dll_enhance_on ? 1.0f : 0.0f;
    cb->params[1] = ao_on ? 1.0f : 0.0f;
    cb->params[2] = fade_alpha;
    cb->params[3] = scale;
    cb->extra[0] = static_cast<float>(filter_preset);
    cb->extra[1] = ap_on ? 1.0f : 0.0f;
    cb->extra[2] = 0.0f;
    cb->extra[3] = 0.0f;
    ctx->Unmap(g_st.cb_toast, 0);

    D3D11StateGuard state_guard(ctx);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = box_x;
    vp.TopLeftY = box_y;
    vp.Width = box_w;
    vp.Height = box_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, null_srvs);
    ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
    ctx->OMSetBlendState(g_st.blend_toast_alpha, nullptr, 0xFFFFFFFFU);
    ctx->OMSetDepthStencilState(g_st.dss_disabled, 0);
    ctx->RSSetState(g_st.rs_fullscreen);
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetScissorRects(0, nullptr);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_st.vs_fullscreen, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_st.ps_toast, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_st.cb_toast);
    ctx->Draw(3, 0);
    return true;
}

bool ExecutePanelOverlay(const PanelOverlayRequest& req, PostLogFn logger) {
    if (!req.ctx || !req.target_rtv || req.tex_w == 0 || req.tex_h == 0 ||
        req.box_w <= 1.0f || req.box_h <= 1.0f || !req.bgra_pixels) {
        return false;
    }

    ID3D11Resource* rtv_res = nullptr;
    req.target_rtv->GetResource(&rtv_res);
    ID3D11Texture2D* rt_tex = nullptr;
    D3D11_TEXTURE2D_DESC rt_desc{};
    const bool got_rt =
        rtv_res &&
        SUCCEEDED(rtv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rt_tex))) &&
        rt_tex;
    if (got_rt) {
        rt_tex->GetDesc(&rt_desc);
    }
    SafeRelease(rt_tex);
    SafeRelease(rtv_res);
    if (!got_rt || rt_desc.Width < 128U || rt_desc.Height < 128U) {
        return false;
    }

    ID3D11Device* dev = nullptr;
    req.ctx->GetDevice(&dev);
    if (!dev) {
        return false;
    }

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    struct LockGuard {
        CRITICAL_SECTION* cs;
        ID3D11Device* d;
        ~LockGuard() {
            SafeRelease(d);
            ::LeaveCriticalSection(cs);
        }
    } lock_guard{&g_d3d_lock, dev};

    if (!EnsurePipelineObjects(dev, logger) || !g_st.ps_panel || !g_st.ps_cursor ||
        !g_st.cb_panel || !g_st.samp_linear || !g_st.blend_toast_alpha) {
        return false;
    }
    const bool tex_recreated = (!g_st.panel_tex || g_st.panel_tex_w != req.tex_w || g_st.panel_tex_h != req.tex_h);
    if (!EnsurePanelTexture(dev, req.tex_w, req.tex_h)) {
        return false;
    }

    if (req.upload_dirty || tex_recreated) {
        D3D11_MAPPED_SUBRESOURCE tex_map{};
        if (SUCCEEDED(req.ctx->Map(g_st.panel_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &tex_map)) && tex_map.pData) {
            const auto* src_bytes = static_cast<const uint8_t*>(req.bgra_pixels);
            auto* dst_bytes = static_cast<uint8_t*>(tex_map.pData);
            const size_t row_bytes = static_cast<size_t>(req.tex_w) * 4U;
            for (uint32_t y = 0; y < req.tex_h; ++y) {
                std::memcpy(dst_bytes + y * tex_map.RowPitch, src_bytes + y * row_bytes, row_bytes);
            }
            req.ctx->Unmap(g_st.panel_tex, 0);
        }
    }

    D3D11_MAPPED_SUBRESOURCE cb_map{};
    if (FAILED(req.ctx->Map(g_st.cb_panel, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb_map)) || !cb_map.pData) {
        return false;
    }
    auto* cb = static_cast<CbPanelParams*>(cb_map.pData);
    cb->rect[0] = req.box_x;
    cb->rect[1] = req.box_y;
    cb->rect[2] = req.box_w;
    cb->rect[3] = req.box_h;
    cb->extra[0] = req.cursor_x;
    cb->extra[1] = req.cursor_y;
    cb->extra[2] = req.panel_alpha;
    cb->extra[3] = 0.0f;
    req.ctx->Unmap(g_st.cb_panel, 0);

    D3D11StateGuard state_guard(req.ctx);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = req.box_x;
    vp.TopLeftY = req.box_y;
    vp.Width = req.box_w;
    vp.Height = req.box_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    req.ctx->OMSetRenderTargets(1, &req.target_rtv, nullptr);
    req.ctx->OMSetBlendState(g_st.blend_toast_alpha, nullptr, 0xFFFFFFFFU);
    req.ctx->OMSetDepthStencilState(g_st.dss_disabled, 0);
    req.ctx->RSSetState(g_st.rs_fullscreen);
    req.ctx->RSSetViewports(1, &vp);
    req.ctx->RSSetScissorRects(0, nullptr);
    req.ctx->IASetInputLayout(nullptr);
    req.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    req.ctx->VSSetShader(g_st.vs_fullscreen, nullptr, 0);
    req.ctx->GSSetShader(nullptr, nullptr, 0);
    req.ctx->PSSetShader(g_st.ps_panel, nullptr, 0);
    req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_panel);
    req.ctx->PSSetShaderResources(0, 1, &g_st.panel_srv);
    req.ctx->PSSetSamplers(0, 1, &g_st.samp_linear);
    req.ctx->Draw(3, 0);

    if (req.draw_cursor) {
        const float cx = std::floor(req.cursor_x) - 3.0f;
        const float cy = std::floor(req.cursor_y) - 3.0f;
        const float max_x = static_cast<float>(rt_desc.Width);
        const float max_y = static_cast<float>(rt_desc.Height);
        const float vx0 = (std::max)(0.0f, cx);
        const float vy0 = (std::max)(0.0f, cy);
        const float vx1 = (std::min)(max_x, cx + 28.0f);
        const float vy1 = (std::min)(max_y, cy + 28.0f);
        if (vx1 > vx0 + 1.0f && vy1 > vy0 + 1.0f) {
            D3D11_VIEWPORT cvp{vx0, vy0, vx1 - vx0, vy1 - vy0, 0.0f, 1.0f};
            req.ctx->RSSetViewports(1, &cvp);
            req.ctx->PSSetShader(g_st.ps_cursor, nullptr, 0);
            req.ctx->Draw(3, 0);
        }
    }
    return true;
}

bool GetParticleDepthSnapshot(ParticleDepthSnapshot* out) {
    if (!out) {
        return false;
    }
    *out = {};
    PostLockScope lock;
    const bool valid = g_st.particle_depth_valid && g_st.particle_params_srv != nullptr &&
                       ((g_st.depth_sample_count > 1) ? (g_st.depth_resolve_srv != nullptr)
                                                      : (g_st.depth_copy_srv != nullptr));
    if (valid) {
        out->proj_22 = g_st.particle_proj_22;
        out->proj_23 = g_st.particle_proj_23;
        out->clamp_rect[0] = g_st.particle_clamp[0];
        out->clamp_rect[1] = g_st.particle_clamp[1];
        out->clamp_rect[2] = g_st.particle_clamp[2];
        out->clamp_rect[3] = g_st.particle_clamp[3];
        out->valid = true;
    }
    
    return valid;
}

bool BindParticleState(
    ID3D11DeviceContext* ctx,
    int soft_mode,
    float inv_soft_range,
    const ParticleDepthSnapshot* snap) {
    if (!ctx) {
        return false;
    }
    PostLockScope lock;
    struct LockGuard {
        ~LockGuard() {  }
    } lock_guard;

    if (!g_st.particle_params_tex || !g_st.particle_params_srv) {
        return false;
    }

    const bool activate = (soft_mode > 0 && snap && snap->valid && g_st.particle_depth_valid);
    ID3D11ShaderResourceView* depth_srv =
        (g_st.depth_sample_count > 1) ? g_st.depth_resolve_srv : g_st.depth_copy_srv;
    if (activate && !depth_srv) {
        return false;
    }

    if (activate) {
        ParticleParamsTexData tex_data{};
        tex_data.soft_params[0] = static_cast<float>(soft_mode);
        tex_data.soft_params[1] = inv_soft_range;
        tex_data.soft_params[2] = snap->proj_22;
        tex_data.soft_params[3] = snap->proj_23;
        tex_data.clamp_rect[0] = static_cast<float>(snap->clamp_rect[0]);
        tex_data.clamp_rect[1] = static_cast<float>(snap->clamp_rect[1]);
        tex_data.clamp_rect[2] = static_cast<float>(snap->clamp_rect[2]);
        tex_data.clamp_rect[3] = static_cast<float>(snap->clamp_rect[3]);

        const bool changed = !g_st.last_cb_particle_valid ||
                             std::memcmp(&g_st.last_cb_particle, &tex_data, sizeof(tex_data)) != 0;
        if (changed) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(g_st.particle_params_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
                std::memcpy(mapped.pData, &tex_data, sizeof(tex_data));
                ctx->Unmap(g_st.particle_params_tex, 0);
                g_st.last_cb_particle = tex_data;
                g_st.last_cb_particle_valid = true;
            }
        }

        ID3D11ShaderResourceView* srvs[2] = {g_st.particle_params_srv, depth_srv};
        ctx->PSSetShaderResources(6, 2, srvs);
        return true;
    } else {
        ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(6, 2, null_srvs);
        return false;
    }
}

void ResetParticleDepthSnapshot() {
    PostLockScope lock;
    g_st.particle_depth_valid = false;
    
}

}  // namespace routeb
