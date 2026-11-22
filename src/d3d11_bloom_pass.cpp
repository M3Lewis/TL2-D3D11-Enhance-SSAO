#include "d3d11_bloom_pass.h"
#include "d3d11_post_renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace routeb {

namespace {

constexpr int kBloomMipCount = 5;

struct alignas(16) CbKawaseBloomParams {
    float dst_vp_rect[4];     // xy=dstOriginPx, zw=dstSizePx
    float src_uv_rect[4];     // xy=srcUvOrigin, zw=srcUvSize
    float texel_and_bias[4];  // xy=invSrcTexSize, z=threshold, w=knee
    float bloom_params[4];    // x=strengthOrScatter, y=radiusScale, z=maxClamp, w=coreLift
    float mip0_rect[4];       // xy=invMip0Size, z=reserved, w=reserved
};
static_assert(sizeof(CbKawaseBloomParams) == 80, "CbKawaseBloomParams must be 80 bytes");

const char kKawaseBloomShaderSource[] = R"HLSL(
cbuffer CbKawaseBloomParams : register(b0)
{
    float4 gDstVpRect;     // xy=dstOriginPx, zw=dstSizePx
    float4 gSrcUvRect;     // xy=srcUvOrigin, zw=srcUvSize
    float4 gTexelAndBias;  // xy=invSrcTexSize, z=threshold, w=knee
    float4 gBloomParams;   // x=strengthOrScatter, y=radiusScale, z=maxClamp, w=coreLift
    float4 gMip0Rect;      // xy=invMip0Size, zw=reserved
};

Texture2D<float4> gTex0     : register(t0);
Texture2D<float4> gBloomTex : register(t1);
SamplerState gSamp          : register(s0);

struct VS_OUT
{
    float4 pos : SV_Position;
};

float2 ComputeNormUv(float2 svPos)
{
    return saturate((svPos - gDstVpRect.xy) / max(gDstVpRect.zw, 1.0f));
}

float2 ComputeSrcUv(float2 svPos)
{
    return gSrcUvRect.xy + ComputeNormUv(svPos) * gSrcUvRect.zw;
}

float2 ClampSrcUv(float2 uv)
{
    float2 pad = gTexelAndBias.xy * 0.5f;
    return clamp(uv, gSrcUvRect.xy + pad, gSrcUvRect.xy + gSrcUvRect.zw - pad);
}

float KarisWeight(float3 linRgb)
{
    float luma = dot(linRgb, float3(0.2126f, 0.7152f, 0.0722f));
    return rcp(1.0f + luma);
}

float3 ExtractBrightLinear(float3 srgb, float threshold, float knee, float maxClamp, float coreLift)
{
    float3 lin = pow(max(srgb, 0.0f), 2.2f);
    float peak = max(lin.r, max(lin.g, lin.b));
    // Gentle HDR-like expansion for white-hot cores (torches, lava, spell flashes)
    lin *= (1.0f + coreLift * smoothstep(0.62f, 1.0f, peak));
    lin = min(lin, maxClamp);

    float luma = dot(lin, float3(0.2126f, 0.7152f, 0.0722f));
    float minCh = min(lin.r, min(lin.g, lin.b));
    float sat = (peak > 1e-4f) ? ((peak - minCh) / peak) : 0.0f;
    // Favor saturated emissives (fire, magic, crystals) while restraining neutral sand/snow
    float energy = lerp(luma, peak, 0.42f + 0.25f * sat);

    float soft = clamp(energy - threshold + knee, 0.0f, 2.0f * knee);
    soft = (soft * soft) / (4.0f * knee + 1e-5f);
    float contrib = max(soft, energy - threshold) / max(energy, 1e-4f);
    return lin * saturate(contrib);
}

float4 PS_BloomPrefilter(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float2 hp = gTexelAndBias.xy * 0.5f;
    float th = gTexelAndBias.z;
    float kn = gTexelAndBias.w;
    float mc = gBloomParams.z;
    float cl = gBloomParams.w;

    float3 c0 = ExtractBrightLinear(gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-hp.x, -hp.y)), 0).rgb, th, kn, mc, cl);
    float3 c1 = ExtractBrightLinear(gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( hp.x, -hp.y)), 0).rgb, th, kn, mc, cl);
    float3 c2 = ExtractBrightLinear(gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-hp.x,  hp.y)), 0).rgb, th, kn, mc, cl);
    float3 c3 = ExtractBrightLinear(gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( hp.x,  hp.y)), 0).rgb, th, kn, mc, cl);

    float w0 = KarisWeight(c0);
    float w1 = KarisWeight(c1);
    float w2 = KarisWeight(c2);
    float w3 = KarisWeight(c3);
    float3 sum = (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3) / max(w0 + w1 + w2 + w3, 1e-4f);
    return float4(sum, 1.0f);
}

float4 PS_BloomDownsample(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float2 o = gTexelAndBias.xy * gBloomParams.y;
    float3 c  = gTex0.SampleLevel(gSamp, ClampSrcUv(uv), 0).rgb * 4.0f;
    c += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-o.x, -o.y)), 0).rgb;
    c += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( o.x, -o.y)), 0).rgb;
    c += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-o.x,  o.y)), 0).rgb;
    c += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( o.x,  o.y)), 0).rgb;
    return float4(c * 0.125f, 1.0f);
}

float4 PS_BloomUpsample(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float2 hp = gTexelAndBias.xy * gBloomParams.y;
    float3 sum = 0.0f;
    sum += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-hp.x * 2.0f, 0.0f)), 0).rgb;
    sum += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-hp.x,        hp.y)), 0).rgb * 2.0f;
    sum += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 0.0f,        hp.y * 2.0f)), 0).rgb;
    sum += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( hp.x,        hp.y)), 0).rgb * 2.0f;
    sum += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( hp.x * 2.0f, 0.0f)), 0).rgb;
    sum += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( hp.x,       -hp.y)), 0).rgb * 2.0f;
    sum += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 0.0f,       -hp.y * 2.0f)), 0).rgb;
    sum += gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-hp.x,       -hp.y)), 0).rgb * 2.0f;
    return float4((sum / 12.0f) * gBloomParams.x, 1.0f);
}

float4 PS_BloomComposite(VS_OUT input) : SV_Target0
{
    float2 normUv = ComputeNormUv(input.pos.xy);
    float2 sceneUv = ClampSrcUv(gSrcUvRect.xy + normUv * gSrcUvRect.zw);
    float2 mip0Pad = gMip0Rect.xy * 0.5f;
    float2 bloomUv = clamp(normUv, mip0Pad, 1.0f - mip0Pad);

    float3 sceneSrgb = saturate(gTex0.SampleLevel(gSamp, sceneUv, 0).rgb);
    float3 bloomLin = max(gBloomTex.SampleLevel(gSamp, bloomUv, 0).rgb, 0.0f) * gBloomParams.x;

    // Preserve chromatic richness on warm fire & cool magic halos
    float bLum = dot(bloomLin, float3(0.2126f, 0.7152f, 0.0722f));
    bloomLin = max(lerp(float3(bLum, bLum, bLum), bloomLin, 1.15f), 0.0f);

    float3 sceneLin = pow(sceneSrgb, 2.2f);
    // Energy-attenuated addition + soft highlight shoulder prevents clipping bright areas to flat white
    float3 combinedLin = sceneLin + bloomLin * rcp(1.0f + sceneLin * 0.45f);
    combinedLin = combinedLin / (1.0f + max(combinedLin - 0.85f, 0.0f) * 0.42f);
    return float4(saturate(pow(max(combinedLin, 0.0f), 1.0f / 2.2f)), 1.0f);
}
)HLSL";
constexpr size_t kKawaseBloomShaderSourceLength = sizeof(kKawaseBloomShaderSource) - 1;

ID3D11Texture2D* g_mip_tex[kBloomMipCount]{};
ID3D11RenderTargetView* g_mip_rtv[kBloomMipCount]{};
ID3D11ShaderResourceView* g_mip_srv[kBloomMipCount]{};
uint32_t g_mip_w[kBloomMipCount]{};
uint32_t g_mip_h[kBloomMipCount]{};
uint32_t g_pyramid_vp_w = 0;
uint32_t g_pyramid_vp_h = 0;
DXGI_FORMAT g_pyramid_fmt = DXGI_FORMAT_UNKNOWN;

ID3D11Buffer* g_cb_bloom = nullptr;
ID3D11SamplerState* g_samp_bloom_linear = nullptr;
ID3D11BlendState* g_blend_bloom_add = nullptr;
ID3D11BlendState* g_blend_bloom_rgb_write = nullptr;

ID3D11PixelShader* g_ps_bloom_prefilter = nullptr;
ID3D11PixelShader* g_ps_bloom_downsample = nullptr;
ID3D11PixelShader* g_ps_bloom_upsample = nullptr;
ID3D11PixelShader* g_ps_bloom_composite = nullptr;
bool g_bloom_init_failed = false;

std::atomic<uint64_t> g_bloom_dispatches{0};
std::atomic<uint64_t> g_bloom_executed{0};

DXGI_FORMAT SelectBloomTextureFormat(ID3D11Device* dev) {
    if (!dev) return DXGI_FORMAT_R8G8B8A8_UNORM;
    constexpr UINT kRequired = D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    const DXGI_FORMAT candidates[] = {
        DXGI_FORMAT_R16G16B16A16_FLOAT,
        DXGI_FORMAT_R11G11B10_FLOAT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
    };
    for (DXGI_FORMAT fmt : candidates) {
        UINT support = 0;
        if (SUCCEEDED(dev->CheckFormatSupport(fmt, &support)) && ((support & kRequired) == kRequired)) {
            return fmt;
        }
    }
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

bool EnsureBloomObjects(ID3D11Device* dev, PostLogFn logger) {
    if (!dev) return false;
    if (g_bloom_init_failed) return false;
    if (g_ps_bloom_prefilter && g_ps_bloom_downsample && g_ps_bloom_upsample &&
        g_ps_bloom_composite && g_cb_bloom && g_samp_bloom_linear &&
        g_blend_bloom_add && g_blend_bloom_rgb_write) {
        return true;
    }

    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        g_bloom_init_failed = true;
        return false;
    }

    const D3D_SHADER_MACRO macros[] = {{nullptr, nullptr}};
    const bool shaders_ok =
        CompileAndCreatePixelShaderFromSource(
            dev, compile_fn, kKawaseBloomShaderSource, kKawaseBloomShaderSourceLength,
            "route_b_kawase_bloom.hlsl", "PS_BloomPrefilter", "ps_4_0", macros, &g_ps_bloom_prefilter, logger) &&
        CompileAndCreatePixelShaderFromSource(
            dev, compile_fn, kKawaseBloomShaderSource, kKawaseBloomShaderSourceLength,
            "route_b_kawase_bloom.hlsl", "PS_BloomDownsample", "ps_4_0", macros, &g_ps_bloom_downsample, logger) &&
        CompileAndCreatePixelShaderFromSource(
            dev, compile_fn, kKawaseBloomShaderSource, kKawaseBloomShaderSourceLength,
            "route_b_kawase_bloom.hlsl", "PS_BloomUpsample", "ps_4_0", macros, &g_ps_bloom_upsample, logger) &&
        CompileAndCreatePixelShaderFromSource(
            dev, compile_fn, kKawaseBloomShaderSource, kKawaseBloomShaderSourceLength,
            "route_b_kawase_bloom.hlsl", "PS_BloomComposite", "ps_4_0", macros, &g_ps_bloom_composite, logger);
    if (!shaders_ok) {
        ReleaseBloomAllObjects();
        g_bloom_init_failed = true;
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(CbKawaseBloomParams);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SAMPLER_DESC samp_desc{};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    D3D11_BLEND_DESC bd_add{};
    bd_add.RenderTarget[0].BlendEnable = TRUE;
    bd_add.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd_add.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd_add.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_add.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_add.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_add.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_add.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_BLEND_DESC bd_rgb{};
    bd_rgb.RenderTarget[0].BlendEnable = FALSE;
    bd_rgb.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    const bool objects_ok =
        SUCCEEDED(dev->CreateBuffer(&cb_desc, nullptr, &g_cb_bloom)) &&
        SUCCEEDED(dev->CreateSamplerState(&samp_desc, &g_samp_bloom_linear)) &&
        SUCCEEDED(dev->CreateBlendState(&bd_add, &g_blend_bloom_add)) &&
        SUCCEEDED(dev->CreateBlendState(&bd_rgb, &g_blend_bloom_rgb_write));
    if (!objects_ok) {
        ReleaseBloomAllObjects();
        g_bloom_init_failed = true;
        return false;
    }

    if (logger) {
        logger("[RouteB][Bloom] Initialized 5-Mip Dual-Kawase Bloom shaders and D3D11 state objects.");
    }
    return true;
}

bool EnsureBloomTargets(ID3D11Device* dev, uint32_t vp_w, uint32_t vp_h, PostLogFn logger) {
    if (!dev || vp_w < 2 || vp_h < 2) return false;

    const DXGI_FORMAT fmt = SelectBloomTextureFormat(dev);
    if (g_mip_tex[0] && g_pyramid_vp_w == vp_w && g_pyramid_vp_h == vp_h && g_pyramid_fmt == fmt) {
        return true;
    }

    ReleaseBloomTargets();

    uint32_t w = (std::max)(2U, vp_w / 2U);
    uint32_t h = (std::max)(2U, vp_h / 2U);
    for (int i = 0; i < kBloomMipCount; ++i) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w;
        td.Height = h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_mip_tex[i])) || !g_mip_tex[i] ||
            FAILED(dev->CreateRenderTargetView(g_mip_tex[i], nullptr, &g_mip_rtv[i])) || !g_mip_rtv[i] ||
            FAILED(dev->CreateShaderResourceView(g_mip_tex[i], nullptr, &g_mip_srv[i])) || !g_mip_srv[i]) {
            ReleaseBloomTargets();
            return false;
        }
        g_mip_w[i] = w;
        g_mip_h[i] = h;
        w = (std::max)(1U, w / 2U);
        h = (std::max)(1U, h / 2U);
    }

    g_pyramid_vp_w = vp_w;
    g_pyramid_vp_h = vp_h;
    g_pyramid_fmt = fmt;
    if (logger) {
        logger("[RouteB][Bloom] Built 5-mip pyramid (%ux%u .. %ux%u, fmt=%u).",
               g_mip_w[0], g_mip_h[0], g_mip_w[kBloomMipCount - 1], g_mip_h[kBloomMipCount - 1],
               static_cast<unsigned>(fmt));
    }
    return true;
}

bool UploadBloomCb(
    ID3D11DeviceContext* ctx,
    float dst_x,
    float dst_y,
    float dst_w,
    float dst_h,
    float src_u0,
    float src_v0,
    float src_uw,
    float src_vh,
    float inv_src_w,
    float inv_src_h,
    float threshold,
    float knee,
    float strength_or_scatter,
    float radius_scale,
    float max_clamp,
    float core_lift,
    float inv_mip0_w,
    float inv_mip0_h) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g_cb_bloom, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        return false;
    }
    auto* cb = static_cast<CbKawaseBloomParams*>(mapped.pData);
    cb->dst_vp_rect[0] = dst_x;
    cb->dst_vp_rect[1] = dst_y;
    cb->dst_vp_rect[2] = dst_w;
    cb->dst_vp_rect[3] = dst_h;
    cb->src_uv_rect[0] = src_u0;
    cb->src_uv_rect[1] = src_v0;
    cb->src_uv_rect[2] = src_uw;
    cb->src_uv_rect[3] = src_vh;
    cb->texel_and_bias[0] = inv_src_w;
    cb->texel_and_bias[1] = inv_src_h;
    cb->texel_and_bias[2] = threshold;
    cb->texel_and_bias[3] = knee;
    cb->bloom_params[0] = strength_or_scatter;
    cb->bloom_params[1] = radius_scale;
    cb->bloom_params[2] = max_clamp;
    cb->bloom_params[3] = core_lift;
    cb->mip0_rect[0] = inv_mip0_w;
    cb->mip0_rect[1] = inv_mip0_h;
    cb->mip0_rect[2] = 0.0f;
    cb->mip0_rect[3] = 0.0f;
    ctx->Unmap(g_cb_bloom, 0);
    return true;
}

void SetSimpleViewport(ID3D11DeviceContext* ctx, float x, float y, float w, float h) {
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = w;
    vp.Height = h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);
}

}  // namespace

void ReleaseBloomTargets() {
    for (int i = 0; i < kBloomMipCount; ++i) {
        SafeRelease(g_mip_srv[i]);
        SafeRelease(g_mip_rtv[i]);
        SafeRelease(g_mip_tex[i]);
        g_mip_w[i] = 0;
        g_mip_h[i] = 0;
    }
    g_pyramid_vp_w = 0;
    g_pyramid_vp_h = 0;
    g_pyramid_fmt = DXGI_FORMAT_UNKNOWN;
}

void ReleaseBloomAllObjects() {
    ReleaseBloomTargets();
    SafeRelease(g_ps_bloom_composite);
    SafeRelease(g_ps_bloom_upsample);
    SafeRelease(g_ps_bloom_downsample);
    SafeRelease(g_ps_bloom_prefilter);
    SafeRelease(g_blend_bloom_rgb_write);
    SafeRelease(g_blend_bloom_add);
    SafeRelease(g_samp_bloom_linear);
    SafeRelease(g_cb_bloom);
    g_bloom_init_failed = false;
}

BloomStats GetBloomStats() {
    BloomStats s{};
    s.dispatches = g_bloom_dispatches.load(std::memory_order_relaxed);
    s.executed = g_bloom_executed.load(std::memory_order_relaxed);
    return s;
}

bool VerifyBloomShadersCompile(PostLogFn logger) {
    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        return false;
    }
    const D3D_SHADER_MACRO macros[] = {{nullptr, nullptr}};
    const char* entries[] = {
        "PS_BloomPrefilter",
        "PS_BloomDownsample",
        "PS_BloomUpsample",
        "PS_BloomComposite",
    };
    bool all_ok = true;
    for (const char* entry : entries) {
        ID3DBlob* blob = nullptr;
        const bool ok = CompileShaderBlobFromSource(
            compile_fn, kKawaseBloomShaderSource, kKawaseBloomShaderSourceLength,
            "route_b_kawase_bloom.hlsl", entry, "ps_4_0", macros, &blob, logger);
        SafeRelease(blob);
        all_ok = all_ok && ok;
    }
    return all_ok;
}

bool ExecuteKawaseBloomPass(const KawaseBloomRequest& req, PostLogFn logger) {
    g_bloom_dispatches.fetch_add(1, std::memory_order_relaxed);
    if (!req.dev || !req.ctx || !req.target_rtv || !req.scene_srv ||
        req.rt_w == 0 || req.rt_h == 0 || req.vp_width < 4 || req.vp_height < 4 ||
        req.strength_scale <= 0.001f) {
        return false;
    }

    if (!EnsureBloomObjects(req.dev, logger) ||
        !EnsureBloomTargets(req.dev, static_cast<uint32_t>(req.vp_width), static_cast<uint32_t>(req.vp_height), logger)) {
        return false;
    }

    const float rt_w = static_cast<float>(req.rt_w);
    const float rt_h = static_cast<float>(req.rt_h);
    const float vp_l = static_cast<float>(req.vp_left);
    const float vp_t = static_cast<float>(req.vp_top);
    const float vp_w = static_cast<float>(req.vp_width);
    const float vp_h = static_cast<float>(req.vp_height);
    const float src_u0 = vp_l / rt_w;
    const float src_v0 = vp_t / rt_h;
    const float src_uw = vp_w / rt_w;
    const float src_vh = vp_h / rt_h;

    // Linear-space soft-knee threshold calibrated per filter preset & desert brightness
    float threshold = (req.filter_preset == 2) ? 0.38f : ((req.filter_preset == 1) ? 0.34f : 0.36f);
    if (req.is_desert) {
        threshold += 0.08f;
    }
    constexpr float kKnee = 0.16f;
    constexpr float kMaxClamp = 2.20f;
    constexpr float kCoreLift = 0.55f;

    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    req.ctx->PSSetConstantBuffers(0, 1, &g_cb_bloom);
    req.ctx->PSSetSamplers(0, 1, &g_samp_bloom_linear);
    req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);

    // 1. Prefilter: Scene (p2_copy_srv) -> Mip 0 (with 4-tap Karis average)
    if (!UploadBloomCb(
            req.ctx,
            0.0f, 0.0f, static_cast<float>(g_mip_w[0]), static_cast<float>(g_mip_h[0]),
            src_u0, src_v0, src_uw, src_vh,
            1.0f / rt_w, 1.0f / rt_h,
            threshold, kKnee, 1.0f, 1.0f, kMaxClamp, kCoreLift,
            1.0f / static_cast<float>(g_mip_w[0]), 1.0f / static_cast<float>(g_mip_h[0]))) {
        return false;
    }
    SetSimpleViewport(req.ctx, 0.0f, 0.0f, static_cast<float>(g_mip_w[0]), static_cast<float>(g_mip_h[0]));
    req.ctx->OMSetRenderTargets(1, &g_mip_rtv[0], nullptr);
    req.ctx->PSSetShader(g_ps_bloom_prefilter, nullptr, 0);
    req.ctx->PSSetShaderResources(0, 1, &req.scene_srv);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 1, null_srvs);

    // 2. Dual-Kawase 5-Tap Downsample Pyramid: Mip 0 -> 1 -> 2 -> 3 -> 4
    req.ctx->PSSetShader(g_ps_bloom_downsample, nullptr, 0);
    for (int i = 0; i < kBloomMipCount - 1; ++i) {
        const float dst_w = static_cast<float>(g_mip_w[i + 1]);
        const float dst_h = static_cast<float>(g_mip_h[i + 1]);
        const float src_w = static_cast<float>(g_mip_w[i]);
        const float src_h = static_cast<float>(g_mip_h[i]);
        if (!UploadBloomCb(
                req.ctx,
                0.0f, 0.0f, dst_w, dst_h,
                0.0f, 0.0f, 1.0f, 1.0f,
                1.0f / src_w, 1.0f / src_h,
                0.0f, 0.0f, 1.0f, 1.0f, kMaxClamp, 0.0f,
                1.0f / static_cast<float>(g_mip_w[0]), 1.0f / static_cast<float>(g_mip_h[0]))) {
            return false;
        }
        SetSimpleViewport(req.ctx, 0.0f, 0.0f, dst_w, dst_h);
        req.ctx->OMSetRenderTargets(1, &g_mip_rtv[i + 1], nullptr);
        req.ctx->PSSetShaderResources(0, 1, &g_mip_srv[i]);
        req.ctx->Draw(3, 0);
        req.ctx->PSSetShaderResources(0, 1, null_srvs);
    }

    // 3. Dual-Kawase 8-Tap Additive Upsample Pyramid: Mip 4 -> 3 -> 2 -> 1 -> 0
    static constexpr float kUpsampleScatter[kBloomMipCount - 1] = {
        0.60f,  // 1 -> 0
        0.66f,  // 2 -> 1
        0.72f,  // 3 -> 2
        0.78f,  // 4 -> 3
    };
    req.ctx->PSSetShader(g_ps_bloom_upsample, nullptr, 0);
    req.ctx->OMSetBlendState(g_blend_bloom_add, nullptr, 0xFFFFFFFFU);
    for (int i = kBloomMipCount - 1; i >= 1; --i) {
        const float dst_w = static_cast<float>(g_mip_w[i - 1]);
        const float dst_h = static_cast<float>(g_mip_h[i - 1]);
        const float src_w = static_cast<float>(g_mip_w[i]);
        const float src_h = static_cast<float>(g_mip_h[i]);
        if (!UploadBloomCb(
                req.ctx,
                0.0f, 0.0f, dst_w, dst_h,
                0.0f, 0.0f, 1.0f, 1.0f,
                1.0f / src_w, 1.0f / src_h,
                0.0f, 0.0f, kUpsampleScatter[i - 1], 0.85f, kMaxClamp, 0.0f,
                1.0f / static_cast<float>(g_mip_w[0]), 1.0f / static_cast<float>(g_mip_h[0]))) {
            return false;
        }
        SetSimpleViewport(req.ctx, 0.0f, 0.0f, dst_w, dst_h);
        req.ctx->OMSetRenderTargets(1, &g_mip_rtv[i - 1], nullptr);
        req.ctx->PSSetShaderResources(0, 1, &g_mip_srv[i]);
        req.ctx->Draw(3, 0);
        req.ctx->PSSetShaderResources(0, 1, null_srvs);
    }

    // 4. Composite Mip 0 + Original Scene -> target_rtv (RGB write, Alpha preserved)
    const float menu_scale = req.is_menu ? 0.65f : 1.0f;
    const float composite_strength = 0.145f * (std::clamp)(req.strength_scale, 0.0f, 2.0f) * menu_scale;
    if (!UploadBloomCb(
            req.ctx,
            vp_l, vp_t, vp_w, vp_h,
            src_u0, src_v0, src_uw, src_vh,
            1.0f / rt_w, 1.0f / rt_h,
            0.0f, 0.0f, composite_strength, 1.0f, kMaxClamp, 0.0f,
            1.0f / static_cast<float>(g_mip_w[0]), 1.0f / static_cast<float>(g_mip_h[0]))) {
        return false;
    }
    SetSimpleViewport(req.ctx, vp_l, vp_t, vp_w, vp_h);
    req.ctx->OMSetRenderTargets(1, &req.target_rtv, nullptr);
    req.ctx->OMSetBlendState(g_blend_bloom_rgb_write, nullptr, 0xFFFFFFFFU);
    req.ctx->PSSetShader(g_ps_bloom_composite, nullptr, 0);
    ID3D11ShaderResourceView* srvs_comp[2] = {req.scene_srv, g_mip_srv[0]};
    req.ctx->PSSetShaderResources(0, 2, srvs_comp);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 2, null_srvs);

    g_bloom_executed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

}  // namespace routeb
