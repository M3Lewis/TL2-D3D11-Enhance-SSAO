#include "d3d11_smaa_pass.h"
#include "d3d11_smaa_shaders.h"
#include "smaa_area_tex.h"
#include "smaa_search_tex.h"
#include "d3d11_post_renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace routeb {

namespace {

std::atomic<int> g_p2_smaa_mode{0}; // 0=OFF, 1=Color, 2=Depth+Color

ID3D11Texture2D* g_smaa_area_tex = nullptr;
ID3D11ShaderResourceView* g_smaa_area_srv = nullptr;

ID3D11Texture2D* g_smaa_search_tex = nullptr;
ID3D11ShaderResourceView* g_smaa_search_srv = nullptr;

ID3D11Texture2D* g_smaa_edges_tex = nullptr;
ID3D11RenderTargetView* g_smaa_edges_rtv = nullptr;
ID3D11ShaderResourceView* g_smaa_edges_srv = nullptr;
uint32_t g_smaa_edges_w = 0;
uint32_t g_smaa_edges_h = 0;

ID3D11Texture2D* g_smaa_blend_tex = nullptr;
ID3D11RenderTargetView* g_smaa_blend_rtv = nullptr;
ID3D11ShaderResourceView* g_smaa_blend_srv = nullptr;
uint32_t g_smaa_blend_w = 0;
uint32_t g_smaa_blend_h = 0;

ID3D11SamplerState* g_samp_smaa_linear = nullptr;
ID3D11SamplerState* g_samp_smaa_point = nullptr;

ID3D11Buffer* g_cb_smaa = nullptr;

ID3D11PixelShader* g_ps_smaa_edge = nullptr;
ID3D11PixelShader* g_ps_smaa_weight = nullptr;
ID3D11PixelShader* g_ps_smaa_blend = nullptr;
std::atomic<bool> g_smaa_shaders_ready{false};
std::atomic<uint64_t> g_smaa_dispatches{0};
std::atomic<uint64_t> g_smaa_executed{0};

struct CbSmaaParams {
    float rt_metrics[4];       // (1.0f/W, 1.0f/H, (float)W, (float)H)
    float proj_z_and_mode[4];  // x=proj_22, y=proj_23, z=mode(1=Color, 2=Depth+Color), w=unused
    int32_t vp_clamp[4];       // (minX, minY, maxX, maxY)
};
static_assert(sizeof(CbSmaaParams) == 48, "CbSmaaParams must be 48 bytes");

}  // namespace

void SetSmaaMode(int mode) {
    const int clamped = (mode < 0 || mode > 2) ? 0 : mode;
    g_p2_smaa_mode.store(clamped, std::memory_order_relaxed);
}

int GetSmaaMode() {
    return g_p2_smaa_mode.load(std::memory_order_relaxed);
}

int CycleSmaaMode() {
    int cur = g_p2_smaa_mode.load(std::memory_order_relaxed);
    int next = 0;
    do {
        next = (cur + 1) % 3;
    } while (!g_p2_smaa_mode.compare_exchange_weak(cur, next, std::memory_order_relaxed));
    return next;
}

const char* SmaaModeName(int mode) {
    switch (mode) {
        case 1: return "Color";
        case 2: return "Depth+Color";
        default: return "OFF";
    }
}

SmaaStats GetSmaaStats() {
    SmaaStats s{};
    s.dispatches = g_smaa_dispatches.load(std::memory_order_relaxed);
    s.executed = g_smaa_executed.load(std::memory_order_relaxed);
    return s;
}

void ReleaseSmaaTargets() {
    SafeRelease(g_smaa_edges_srv);
    SafeRelease(g_smaa_edges_rtv);
    SafeRelease(g_smaa_edges_tex);
    g_smaa_edges_w = 0;
    g_smaa_edges_h = 0;

    SafeRelease(g_smaa_blend_srv);
    SafeRelease(g_smaa_blend_rtv);
    SafeRelease(g_smaa_blend_tex);
    g_smaa_blend_w = 0;
    g_smaa_blend_h = 0;
}

void ReleaseSmaaAllObjects() {
    ReleaseSmaaTargets();

    SafeRelease(g_smaa_area_srv);
    SafeRelease(g_smaa_area_tex);

    SafeRelease(g_smaa_search_srv);
    SafeRelease(g_smaa_search_tex);

    SafeRelease(g_samp_smaa_linear);
    SafeRelease(g_samp_smaa_point);

    SafeRelease(g_cb_smaa);

    SafeRelease(g_ps_smaa_edge);
    SafeRelease(g_ps_smaa_weight);
    SafeRelease(g_ps_smaa_blend);
    g_smaa_shaders_ready.store(false, std::memory_order_relaxed);
}

bool EnsureSmaaStaticTextures(ID3D11Device* dev, PostLogFn logger) {
    if (!dev) return false;

    // 1. Area texture (160x560 R8G8_UNORM, Pitch=320)
    if (!g_smaa_area_tex || !g_smaa_area_srv) {
        SafeRelease(g_smaa_area_srv);
        SafeRelease(g_smaa_area_tex);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = AREATEX_WIDTH;
        td.Height = AREATEX_HEIGHT;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = areaTexBytes;
        sd.SysMemPitch = AREATEX_PITCH;

        if (FAILED(dev->CreateTexture2D(&td, &sd, &g_smaa_area_tex)) || !g_smaa_area_tex) {
            if (logger) logger("[RouteC][SMAA] FAILED CreateTexture2D for AreaTex");
            return false;
        }
        if (FAILED(dev->CreateShaderResourceView(g_smaa_area_tex, nullptr, &g_smaa_area_srv)) || !g_smaa_area_srv) {
            if (logger) logger("[RouteC][SMAA] FAILED CreateShaderResourceView for AreaTex");
            return false;
        }
    }

    // 2. Search texture (64x16 R8_UNORM, Pitch=64)
    if (!g_smaa_search_tex || !g_smaa_search_srv) {
        SafeRelease(g_smaa_search_srv);
        SafeRelease(g_smaa_search_tex);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = SEARCHTEX_WIDTH;
        td.Height = SEARCHTEX_HEIGHT;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

        D3D11_SUBRESOURCE_DATA sd{};
        sd.pSysMem = searchTexBytes;
        sd.SysMemPitch = SEARCHTEX_PITCH;

        if (FAILED(dev->CreateTexture2D(&td, &sd, &g_smaa_search_tex)) || !g_smaa_search_tex) {
            if (logger) logger("[RouteC][SMAA] FAILED CreateTexture2D for SearchTex");
            return false;
        }
        if (FAILED(dev->CreateShaderResourceView(g_smaa_search_tex, nullptr, &g_smaa_search_srv)) || !g_smaa_search_srv) {
            if (logger) logger("[RouteC][SMAA] FAILED CreateShaderResourceView for SearchTex");
            return false;
        }
    }

    // 3. Dedicated samplers:
    // Linear: MIN_MAG_LINEAR_MIP_POINT + CLAMP
    if (!g_samp_smaa_linear) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sd, &g_samp_smaa_linear)) || !g_samp_smaa_linear) {
            return false;
        }
    }

    // Point: MIN_MAG_MIP_POINT + CLAMP
    if (!g_samp_smaa_point) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sd, &g_samp_smaa_point)) || !g_samp_smaa_point) {
            return false;
        }
    }

    // 4. Constant buffer
    if (!g_cb_smaa) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(CbSmaaParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_cb_smaa)) || !g_cb_smaa) {
            return false;
        }
    }

    return true;
}

bool EnsureSmaaDynamicTargets(ID3D11Device* dev, uint32_t width, uint32_t height, PostLogFn logger) {
    if (!dev || width == 0 || height == 0) return false;

    // 1. Edges target (R8G8_UNORM)
    if (!g_smaa_edges_tex || !g_smaa_edges_rtv || !g_smaa_edges_srv ||
        g_smaa_edges_w != width || g_smaa_edges_h != height) {
        SafeRelease(g_smaa_edges_srv);
        SafeRelease(g_smaa_edges_rtv);
        SafeRelease(g_smaa_edges_tex);
        g_smaa_edges_w = 0;
        g_smaa_edges_h = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_smaa_edges_tex)) || !g_smaa_edges_tex) {
            if (logger) logger("[RouteC][SMAA] FAILED CreateTexture2D for edges (%ux%u)", width, height);
            return false;
        }
        if (FAILED(dev->CreateRenderTargetView(g_smaa_edges_tex, nullptr, &g_smaa_edges_rtv)) || !g_smaa_edges_rtv) {
            SafeRelease(g_smaa_edges_tex);
            return false;
        }
        if (FAILED(dev->CreateShaderResourceView(g_smaa_edges_tex, nullptr, &g_smaa_edges_srv)) || !g_smaa_edges_srv) {
            SafeRelease(g_smaa_edges_rtv);
            SafeRelease(g_smaa_edges_tex);
            return false;
        }
        g_smaa_edges_w = width;
        g_smaa_edges_h = height;
    }

    // 2. Blend weights target (R8G8B8A8_UNORM)
    if (!g_smaa_blend_tex || !g_smaa_blend_rtv || !g_smaa_blend_srv ||
        g_smaa_blend_w != width || g_smaa_blend_h != height) {
        SafeRelease(g_smaa_blend_srv);
        SafeRelease(g_smaa_blend_rtv);
        SafeRelease(g_smaa_blend_tex);
        g_smaa_blend_w = 0;
        g_smaa_blend_h = 0;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = width;
        td.Height = height;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_smaa_blend_tex)) || !g_smaa_blend_tex) {
            if (logger) logger("[RouteC][SMAA] FAILED CreateTexture2D for blend (%ux%u)", width, height);
            return false;
        }
        if (FAILED(dev->CreateRenderTargetView(g_smaa_blend_tex, nullptr, &g_smaa_blend_rtv)) || !g_smaa_blend_rtv) {
            SafeRelease(g_smaa_blend_tex);
            return false;
        }
        if (FAILED(dev->CreateShaderResourceView(g_smaa_blend_tex, nullptr, &g_smaa_blend_srv)) || !g_smaa_blend_srv) {
            SafeRelease(g_smaa_blend_rtv);
            SafeRelease(g_smaa_blend_tex);
            return false;
        }
        g_smaa_blend_w = width;
        g_smaa_blend_h = height;
    }

    return true;
}

bool VerifySmaaShadersCompile(ID3D11Device* dev, PostLogFn logger) {
    if (g_smaa_shaders_ready.load(std::memory_order_relaxed)) {
        return true;
    }
    if (!dev) return false;

    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) return false;

    bool ok = true;
    ok &= CompileAndCreatePixelShaderFromSource(
        dev, compile_fn, kSmaaShadersSource, kSmaaShadersSourceLength,
        "PS_SmaaEdgeDetection", "PS_SmaaEdgeDetection", "ps_4_0", nullptr,
        &g_ps_smaa_edge, logger);

    ok &= CompileAndCreatePixelShaderFromSource(
        dev, compile_fn, kSmaaShadersSource, kSmaaShadersSourceLength,
        "PS_SmaaBlendingWeight", "PS_SmaaBlendingWeight", "ps_4_0", nullptr,
        &g_ps_smaa_weight, logger);

    ok &= CompileAndCreatePixelShaderFromSource(
        dev, compile_fn, kSmaaShadersSource, kSmaaShadersSourceLength,
        "PS_SmaaNeighborhoodBlend", "PS_SmaaNeighborhoodBlend", "ps_4_0", nullptr,
        &g_ps_smaa_blend, logger);

    if (ok) {
        g_smaa_shaders_ready.store(true, std::memory_order_release);
        if (logger) {
            logger("[RouteC][SMAA] Compiled SMAA 1x shaders (Edge, Weight, Blend) successfully.");
        }
    } else {
        if (logger) {
            logger("[RouteC][SMAA] FAILED compiling one or more SMAA shaders.");
        }
    }
    return ok;
}

bool ExecuteSmaaPass(const SmaaRequest& req, PostLogFn logger) {
    if (!req.ctx || req.vp_width <= 0 || req.vp_height <= 0 || req.smaa_mode <= 0) {
        return false;
    }
    g_smaa_dispatches.fetch_add(1, std::memory_order_relaxed);

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    struct LockGuard {
        ~LockGuard() {
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

    if (!EnsurePipelineObjects(dev, logger) ||
        !EnsureSmaaStaticTextures(dev, logger) ||
        !EnsureSmaaDynamicTargets(dev, rt_desc.Width, rt_desc.Height, logger) ||
        !VerifySmaaShadersCompile(dev, logger)) {
        return false;
    }

    // Copy current target_rtv into p2_copy_tex (handles MSAA resolve if needed)
    if (!CopyRtvTextureToP2Copy(dev, req.ctx, src_rt_tex, rt_desc, rtv_desc)) {
        return false;
    }

    // Upload CbSmaaParams
    CbSmaaParams cb{};
    cb.rt_metrics[0] = 1.0f / static_cast<float>(rt_desc.Width);
    cb.rt_metrics[1] = 1.0f / static_cast<float>(rt_desc.Height);
    cb.rt_metrics[2] = static_cast<float>(rt_desc.Width);
    cb.rt_metrics[3] = static_cast<float>(rt_desc.Height);
    cb.proj_z_and_mode[0] = req.proj_22;
    cb.proj_z_and_mode[1] = req.proj_23;
    cb.proj_z_and_mode[2] = static_cast<float>(req.smaa_mode);
    cb.proj_z_and_mode[3] = 0.0f;
    cb.vp_clamp[0] = (std::max)(0, req.vp_left);
    cb.vp_clamp[1] = (std::max)(0, req.vp_top);
    cb.vp_clamp[2] = (std::min)(static_cast<int>(rt_desc.Width) - 1, req.vp_left + req.vp_width - 1);
    cb.vp_clamp[3] = (std::min)(static_cast<int>(rt_desc.Height) - 1, req.vp_top + req.vp_height - 1);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(req.ctx->Map(g_cb_smaa, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) && mapped.pData) {
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        req.ctx->Unmap(g_cb_smaa, 0);
    }

    // Red line 2: Clear edges and blend targets to 0 before execution
    const float clear_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    req.ctx->ClearRenderTargetView(g_smaa_edges_rtv, clear_zero);
    req.ctx->ClearRenderTargetView(g_smaa_blend_rtv, clear_zero);

    // Red line 3: Pipeline state setup
    req.ctx->RSSetState(g_st.rs_fullscreen);
    req.ctx->RSSetScissorRects(0, nullptr);
    req.ctx->IASetInputLayout(nullptr);
    req.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    req.ctx->VSSetShader(g_st.vs_fullscreen, nullptr, 0);
    req.ctx->GSSetShader(nullptr, nullptr, 0);
    req.ctx->OMSetDepthStencilState(g_st.dss_disabled, 0);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = 0.0f;
    vp.TopLeftY = 0.0f;
    vp.Width = static_cast<float>(rt_desc.Width);
    vp.Height = static_cast<float>(rt_desc.Height);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    req.ctx->RSSetViewports(1, &vp);

    ID3D11SamplerState* samps[2] = {g_samp_smaa_linear, g_samp_smaa_point};
    req.ctx->PSSetSamplers(0, 2, samps);
    req.ctx->PSSetConstantBuffers(0, 1, &g_cb_smaa);

    ID3D11ShaderResourceView* null_srvs[3] = {nullptr, nullptr, nullptr};

    // ------------------------------------------------------------------------
    // PASS 1: Edge Detection -> smaa_edges_rtv
    // ------------------------------------------------------------------------
    req.ctx->OMSetRenderTargets(1, &g_smaa_edges_rtv, nullptr);
    req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    req.ctx->PSSetShader(g_ps_smaa_edge, nullptr, 0);

    ID3D11ShaderResourceView* srvs_pass1[2] = {
        g_st.p2_copy_srv,
        (req.smaa_mode == 2 && g_st.depth_copy_srv) ? g_st.depth_copy_srv : g_st.p2_copy_srv
    };
    req.ctx->PSSetShaderResources(0, 2, srvs_pass1);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 2, null_srvs);

    // ------------------------------------------------------------------------
    // PASS 2: Blending Weight Calculation -> smaa_blend_rtv
    // ------------------------------------------------------------------------
    req.ctx->OMSetRenderTargets(1, &g_smaa_blend_rtv, nullptr);
    req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    req.ctx->PSSetShader(g_ps_smaa_weight, nullptr, 0);

    ID3D11ShaderResourceView* srvs_pass2[3] = {
        g_smaa_edges_srv,
        g_smaa_area_srv,
        g_smaa_search_srv
    };
    req.ctx->PSSetShaderResources(0, 3, srvs_pass2);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 3, null_srvs);

    // ------------------------------------------------------------------------
    // PASS 3: Neighborhood Blending -> target_rtv (Blend OFF overwrite)
    // ------------------------------------------------------------------------
    req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
    req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
    req.ctx->PSSetShader(g_ps_smaa_blend, nullptr, 0);

    ID3D11ShaderResourceView* srvs_pass3[3] = {
        g_st.p2_copy_srv,
        g_smaa_blend_srv,
        g_smaa_edges_srv
    };
    req.ctx->PSSetShaderResources(0, 3, srvs_pass3);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 3, null_srvs);

    g_smaa_executed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace routeb
