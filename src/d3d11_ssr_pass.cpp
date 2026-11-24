#include "d3d11_ssr_pass.h"
#include "d3d11_post_renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace routeb {

void ResetPostWorldDepthSnapshot() {
    PostLockScope lock_guard;
    g_st.post_world_depth_valid = false;
    g_st.post_world_msaa = false;
}

bool ExecutePostWorldPass(const PostWorldRequest& req, PostLogFn logger) {
    if (!req.ctx || req.vp_width <= 0 || req.vp_height <= 0 || req.debug_mode > 0 ||
        (!req.ssr_enabled && req.hdr_tonemap_mode <= 0 && req.lut_preset <= 0)) {
        return false;
    }

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    struct LockGuard {
        ~LockGuard() {
            g_st.post_world_depth_valid = false;
            ::LeaveCriticalSection(&g_d3d_lock);
        }
    } lock_guard;

    D3D11StateGuard state_guard(req.ctx);
    ID3D11RenderTargetView* target_rtv = req.fallback_rtv ? req.fallback_rtv : state_guard.om_rtv0();
    if (!target_rtv) {
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
        return false;
    }

    const int min_x = (std::max)(0, req.vp_left);
    const int min_y = (std::max)(0, req.vp_top);
    const int max_x = (std::min)(static_cast<int>(rt_desc.Width) - 1, req.vp_left + req.vp_width - 1);
    const int max_y = (std::min)(static_cast<int>(rt_desc.Height) - 1, req.vp_top + req.vp_height - 1);
    if (min_x > max_x || min_y > max_y) {
        return false;
    }

    req.ctx->OMSetDepthStencilState(g_st.dss_disabled, 0);
    req.ctx->RSSetState(g_st.rs_fullscreen);
    req.ctx->RSSetScissorRects(0, nullptr);
    req.ctx->IASetInputLayout(nullptr);
    req.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    req.ctx->VSSetShader(g_st.vs_fullscreen, nullptr, 0);
    req.ctx->GSSetShader(nullptr, nullptr, 0);
    req.ctx->PSSetSamplers(0, 1, &g_st.samp_linear);

    D3D11_VIEWPORT ao_vp{};
    ao_vp.TopLeftX = static_cast<float>(req.vp_left);
    ao_vp.TopLeftY = static_cast<float>(req.vp_top);
    ao_vp.Width = static_cast<float>(req.vp_width);
    ao_vp.Height = static_cast<float>(req.vp_height);
    ao_vp.MinDepth = 0.0f;
    ao_vp.MaxDepth = 1.0f;
    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    bool did_any = false;

    // Item 5: Post-water SSR using depth snapshot from Ended86 and post-translucent color buffer (queues 0..94).
    const DepthVariantShaders* ds = g_st.post_world_msaa ? &g_st.shaders_msaa : &g_st.shaders_single;
    if (!req.is_menu && req.ssr_enabled && g_st.post_world_depth_valid && g_st.depth_copy_srv &&
        ds->ps_ssr && ds->ps_ssr_resolve && g_st.cb_params && g_st.blend_toast_alpha &&
        EnsureSsrTarget(dev, rt_desc.Width, rt_desc.Height) &&
        EnsureWaterDepthTarget(dev, rt_desc.Width, rt_desc.Height, rt_desc.SampleDesc.Count, rt_desc.SampleDesc.Quality) &&
        CopyRtvTextureToP2Copy(dev, req.ctx, src_rt_tex, rt_desc, rtv_desc)) {
        CbAoParams cb_ssr{};
        std::memcpy(&cb_ssr, g_st.post_world_cb_bytes, sizeof(cb_ssr));
        const float want_hint = req.water_hint ? 1.0f : 0.0f;
        if (std::fabs(cb_ssr.view_up_and_water[3] - want_hint) > 0.25f) {
            cb_ssr.view_up_and_water[3] = want_hint;
            std::memcpy(g_st.post_world_cb_bytes, &cb_ssr, sizeof(cb_ssr));
        }
        cb_ssr.ao_params[2] = (std::clamp)(req.ssr_strength_scale, 0.0f, 2.0f);
        cb_ssr.aerial_fog_params[0] = static_cast<float>(::GetTickCount64() % 3600000ULL) * 0.001f;
        cb_ssr.aerial_fog_params[1] = (std::clamp)(req.rain_strength, 0.0f, 2.0f);
        cb_ssr.aerial_fog_params[2] = req.time_of_day;
        D3D11_MAPPED_SUBRESOURCE mapped_ao{};
        if (SUCCEEDED(req.ctx->Map(g_st.cb_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_ao)) && mapped_ao.pData) {
            std::memcpy(mapped_ao.pData, &cb_ssr, sizeof(cb_ssr));
            req.ctx->Unmap(g_st.cb_params, 0);
        }
        req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_params);
        req.ctx->RSSetViewports(1, &ao_vp);

        // Pass 1: Raymarch SSR to offscreen target (ssr_rt_rtv) with overwrite blend
        const float clear_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        req.ctx->ClearRenderTargetView(g_st.ssr_rt_rtv, clear_zero);
        req.ctx->OMSetRenderTargets(1, &g_st.ssr_rt_rtv, nullptr);
        req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
        req.ctx->PSSetShader(ds->ps_ssr, nullptr, 0);
        ID3D11ShaderResourceView* srvs_ssr[3] = {g_st.depth_copy_srv, g_st.p2_copy_srv, g_st.water_depth_srv};
        req.ctx->PSSetShaderResources(0, 3, srvs_ssr);
        req.ctx->Draw(3, 0);
        ID3D11ShaderResourceView* null_srvs3[3] = {nullptr, nullptr, nullptr};
        req.ctx->PSSetShaderResources(0, 3, null_srvs3);

        // Pass 2: 3x3 Bilateral Spatial Resolve & Composite to target_rtv
        // Unbind ssr_rt_rtv by setting target_rtv FIRST to avoid D3D11 hazard
        req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
        req.ctx->OMSetBlendState(g_st.blend_toast_alpha, nullptr, 0xFFFFFFFFU);
        req.ctx->PSSetShader(ds->ps_ssr_resolve, nullptr, 0);
        ID3D11ShaderResourceView* srvs_resolve[2] = {g_st.depth_copy_srv, g_st.ssr_rt_srv};
        req.ctx->PSSetShaderResources(0, 2, srvs_resolve);
        req.ctx->Draw(3, 0);
        req.ctx->PSSetShaderResources(0, 2, null_srvs);
        did_any = true;
    }

    // FP16 HDR -> Filmic Tonemap (ACES Fitted / AgX Punchy), right after SSR and before LUT/SMAA/Bloom2
    if (!req.is_menu && req.hdr_tonemap_mode > 0 &&
        g_st.ps_hdr_tonemap && g_st.cb_bloom_lut && g_st.blend_debug_rgb &&
        CopyRtvTextureToP2Copy(dev, req.ctx, src_rt_tex, rt_desc, rtv_desc)) {
        const float rt_w = static_cast<float>(rt_desc.Width);
        const float rt_h = static_cast<float>(rt_desc.Height);
        if (UploadBloomLutCb(req.ctx,
                             static_cast<float>(req.vp_left), static_cast<float>(req.vp_top),
                             static_cast<float>(req.vp_width), static_cast<float>(req.vp_height),
                             static_cast<float>(req.vp_left) / rt_w, static_cast<float>(req.vp_top) / rt_h,
                             static_cast<float>(req.vp_width) / rt_w, static_cast<float>(req.vp_height) / rt_h,
                             1.0f / rt_w, 1.0f / rt_h,
                             0.0f, 0.0f,
                             static_cast<float>(req.hdr_tonemap_mode),
                             (std::max)(0.1f, req.hdr_exposure_scale),
                             0.0f)) {
            req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_bloom_lut);
            req.ctx->RSSetViewports(1, &ao_vp);
            req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
            req.ctx->OMSetBlendState(g_st.blend_debug_rgb, nullptr, 0xFFFFFFFFU);
            req.ctx->PSSetShader(g_st.ps_hdr_tonemap, nullptr, 0);
            ID3D11ShaderResourceView* s[2] = {g_st.p2_copy_srv, nullptr};
            req.ctx->PSSetShaderResources(0, 2, s);
            req.ctx->Draw(3, 0);
            req.ctx->PSSetShaderResources(0, 2, null_srvs);
            did_any = true;
        }
    }

    // Item 7: 3D LUT Color Grading (post-world + post-SSR + post-HDR, strictly pre-UI)
    if (req.lut_preset > 0 &&
        g_st.ps_lut_grade && g_st.cb_bloom_lut && g_st.blend_debug_rgb &&
        EnsureLutReady(dev, req.lut_preset, logger) &&
        CopyRtvTextureToP2Copy(dev, req.ctx, src_rt_tex, rt_desc, rtv_desc)) {
        const float rt_w = static_cast<float>(rt_desc.Width);
        const float rt_h = static_cast<float>(rt_desc.Height);
        const float lut_sz = static_cast<float>(g_st.lut_size[req.lut_preset]);
        const float base_mix = (req.lut_preset == 3) ? 1.0f : 0.88f;
        const float lut_mix = (std::clamp)(base_mix * req.lut_strength_scale, 0.0f, 1.35f);
        if (UploadBloomLutCb(req.ctx,
                             static_cast<float>(req.vp_left), static_cast<float>(req.vp_top),
                             static_cast<float>(req.vp_width), static_cast<float>(req.vp_height),
                             static_cast<float>(req.vp_left) / rt_w, static_cast<float>(req.vp_top) / rt_h,
                             static_cast<float>(req.vp_width) / rt_w, static_cast<float>(req.vp_height) / rt_h,
                             1.0f / rt_w, 1.0f / rt_h,
                             0.0f, 0.0f, 0.0f, lut_sz, lut_mix)) {
            req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_bloom_lut);
            req.ctx->RSSetViewports(1, &ao_vp);
            req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
            req.ctx->OMSetBlendState(g_st.blend_debug_rgb, nullptr, 0xFFFFFFFFU);
            req.ctx->PSSetShader(g_st.ps_lut_grade, nullptr, 0);
            ID3D11ShaderResourceView* s[2] = {g_st.p2_copy_srv, g_st.lut_srv[req.lut_preset]};
            req.ctx->PSSetShaderResources(0, 2, s);
            req.ctx->Draw(3, 0);
            req.ctx->PSSetShaderResources(0, 2, null_srvs);
            did_any = true;
        }
    }

    return did_any;
}


}  // namespace routeb
