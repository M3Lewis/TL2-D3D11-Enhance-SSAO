#include "d3d11_shadow_pass.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "d3d11_hdr_hook.h"
#include "d3d11_post_renderer.h"
#include "hlsl_cbuffer_fastpath.h"

namespace routeb {
namespace {

constexpr UINT kShadowMapSize = 2048;
constexpr float kShadowOrthoHalfExtent = 26.0f;  // 52x52 world units => ~2.5cm/texel
constexpr float kShadowNearHalfDepth = 65.0f;
constexpr float kCameraFocusDist = 20.0f;

struct alignas(16) CbShadowReproject {
    float cam_clip_to_light_clip[4][4];
    float alpha_params[4];  // x = alphaCutoutRef, yzw = 0
};
static_assert(sizeof(CbShadowReproject) == 80, "CbShadowReproject must be 80 bytes");

struct alignas(16) CbShadowResolve {
    float view_to_light_clip[4][4];
    float proj_xy[4];              // p00, p11, p02, p12
    float proj_z_light[4];         // p22, p23, texelSize, normalOffset
    float light_dir_strength[4];   // xyz = unit dir TO light in VS, w = strength
    float vp_rect[4];              // left, top, width, height
    int32_t vp_clamp[4];           // minX, minY, maxX, maxY
};
static_assert(sizeof(CbShadowResolve) == 144, "CbShadowResolve must be 144 bytes");

const char kShadowShadersSource[] = R"HLSL(
cbuffer CbShadowReproject : register(b0)
{
    float4 gCamClipToLightClip0;
    float4 gCamClipToLightClip1;
    float4 gCamClipToLightClip2;
    float4 gCamClipToLightClip3;
    float4 gAlphaParams;
};

struct GS_IN_POS
{
    float4 pos : SV_Position;
};

struct GS_IN_UV
{
    float4 pos : SV_Position;
    float4 col0 : COLOR0;
    float4 col1 : COLOR1;
    float  fog : FOG;
    float2 uv0 : TEXCOORD0;
};

struct GS_OUT_POS
{
    float4 pos : SV_Position;
};

struct GS_OUT_UV
{
    float4 pos : SV_Position;
    float2 uv0 : TEXCOORD0;
};

float4 ReprojectToLight(float4 camClip)
{
    return float4(
        dot(gCamClipToLightClip0, camClip),
        dot(gCamClipToLightClip1, camClip),
        dot(gCamClipToLightClip2, camClip),
        dot(gCamClipToLightClip3, camClip));
}

[maxvertexcount(3)]
void GS_ShadowReproject(triangle GS_IN_POS input[3], inout TriangleStream<GS_OUT_POS> triStream)
{
    GS_OUT_POS o;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        o.pos = ReprojectToLight(input[i].pos);
        triStream.Append(o);
    }
    triStream.RestartStrip();
}

[maxvertexcount(3)]
void GS_ShadowReprojectUv(triangle GS_IN_UV input[3], inout TriangleStream<GS_OUT_UV> triStream)
{
    GS_OUT_UV o;
    [unroll]
    for (int i = 0; i < 3; ++i)
    {
        o.pos = ReprojectToLight(input[i].pos);
        o.uv0 = input[i].uv0;
        triStream.Append(o);
    }
    triStream.RestartStrip();
}

Texture2D<float4> gAlphaTex : register(t0);
SamplerState gAlphaSamp : register(s0);

void PS_ShadowAlphaCutout(GS_OUT_UV input)
{
    float a = gAlphaTex.Sample(gAlphaSamp, input.uv0).a;
    clip(a - gAlphaParams.x);
}

cbuffer CbShadowResolve : register(b0)
{
    float4 gViewToLightClip0;
    float4 gViewToLightClip1;
    float4 gViewToLightClip2;
    float4 gViewToLightClip3;
    float4 gProjXY;
    float4 gProjZLight;         // x=p22, y=p23, z=texelSize, w=normalOffset
    float4 gLightDirStrength;   // xyz=unit light dir in VS, w=strength
    float4 gVpRect;             // left, top, width, height
    int4   gVpClamp;            // minX, minY, maxX, maxY
};

Texture2D<float> gSceneDepthTex : register(t0);
Texture2D<float> gShadowMapTex  : register(t1);
SamplerComparisonState gShadowCmpSamp : register(s0);

static const float2 kPoisson16[16] =
{
    float2(-0.94201624f, -0.39906216f),
    float2( 0.94558609f, -0.76890725f),
    float2(-0.09418410f, -0.92938870f),
    float2( 0.34495938f,  0.29387760f),
    float2(-0.91588581f,  0.45771432f),
    float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f,  0.27676845f),
    float2( 0.97484398f,  0.75648379f),
    float2( 0.44323325f, -0.97511554f),
    float2( 0.53742981f, -0.47373420f),
    float2(-0.26496911f, -0.41893023f),
    float2( 0.79197514f,  0.19090188f),
    float2(-0.24188840f,  0.99706507f),
    float2(-0.81409955f,  0.91437590f),
    float2( 0.19984126f,  0.78641367f),
    float2( 0.14383161f, -0.14100790f)
};

float2 PixelToNdc(float2 svPos)
{
    float2 uv = (svPos - gVpRect.xy) / max(gVpRect.zw, 1.0f);
    return float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
}

float3 ReconstructPosVS(int2 pix, float rawZ)
{
    float2 ndc = PixelToNdc(float2(pix) + 0.5f);
    float denom = min(rawZ + gProjZLight.x, -1e-6f);
    float zView = gProjZLight.y / denom;
    float xView = (ndc.x + gProjXY.z) * (-zView) / gProjXY.x;
    float yView = (ndc.y + gProjXY.w) * (-zView) / gProjXY.y;
    return float3(xView, yView, zView);
}

float SampleRawDepthClamped(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gSceneDepthTex.Load(int3(c, 0));
}

float3 ReconstructNormalVS(int2 pix, float3 pC)
{
    float zL = SampleRawDepthClamped(pix + int2(-1, 0));
    float zR = SampleRawDepthClamped(pix + int2( 1, 0));
    float zU = SampleRawDepthClamped(pix + int2( 0,-1));
    float zD = SampleRawDepthClamped(pix + int2( 0, 1));

    float3 dL = pC - ReconstructPosVS(pix + int2(-1, 0), zL);
    float3 dR = ReconstructPosVS(pix + int2( 1, 0), zR) - pC;
    float3 dU = pC - ReconstructPosVS(pix + int2( 0,-1), zU);
    float3 dD = ReconstructPosVS(pix + int2( 0, 1), zD) - pC;

    float3 dH = (abs(dL.z) < abs(dR.z)) ? dL : dR;
    float3 dV = (abs(dU.z) < abs(dD.z)) ? dU : dD;
    float3 n = cross(dH, dV);
    float lenSq = dot(n, n);
    if (lenSq < 1e-10f)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }
    n *= rsqrt(lenSq);
    if (n.z < 0.0f)
    {
        n = -n;
    }
    return n;
}

float4 VS_ShadowFullscreen(uint vid : SV_VertexID) : SV_Position
{
    float2 pos[3] = {
        float2(-1.0f, -1.0f),
        float2(-1.0f,  3.0f),
        float2( 3.0f, -1.0f)
    };
    return float4(pos[vid], 0.0f, 1.0f);
}

float4 PS_ShadowMapResolve(float4 svPos : SV_Position) : SV_Target0
{
    int2 pix = int2(svPos.xy);
    float rawZ = SampleRawDepthClamped(pix);
    if (rawZ >= 0.99995f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float3 pVS = ReconstructPosVS(pix, rawZ);
    float3 nVS = ReconstructNormalVS(pix, pVS);
    float ndotl = dot(nVS, gLightDirStrength.xyz);

    // Smoothly fade directional shadow mask on geometry facing away from the sun.
    float facingMask = smoothstep(-0.08f, 0.14f, ndotl);
    if (facingMask <= 0.001f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float slopeScale = 1.0f - saturate(ndotl);
    float3 pBiased = pVS + nVS * (gProjZLight.w * (0.55f + 0.85f * slopeScale));

    float4 hp = float4(pBiased, 1.0f);
    float4 lp = float4(
        dot(gViewToLightClip0, hp),
        dot(gViewToLightClip1, hp),
        dot(gViewToLightClip2, hp),
        dot(gViewToLightClip3, hp));
    if (lp.w <= 1e-5f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float3 ndc = lp.xyz / lp.w;
    float2 shadowUv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);

    // Edge fade near ortho shadow frustum boundary so edges never pop.
    float2 edgeDist = min(shadowUv, 1.0f - shadowUv);
    float frustumMask = smoothstep(0.0f, 0.06f, min(edgeDist.x, edgeDist.y));
    if (ndc.z <= 0.0f || ndc.z >= 1.0f || frustumMask <= 0.001f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float receiverBias = 0.00045f + 0.00110f * slopeScale;
    float refZ = saturate(ndc.z - receiverBias);

    float angle = frac(sin(dot(svPos.xy, float2(12.9898f, 78.233f))) * 43758.5453f) * 6.2831853f;
    float s;
    float c;
    sincos(angle, s, c);
    float2x2 rot = float2x2(c, -s, s, c);
    float pcfRadius = gProjZLight.z * 2.15f;

    float lit = 0.0f;
    [unroll]
    for (int i = 0; i < 16; ++i)
    {
        float2 off = mul(rot, kPoisson16[i]) * pcfRadius;
        lit += gShadowMapTex.SampleCmpLevelZero(gShadowCmpSamp, shadowUv + off, refZ);
    }
    lit *= (1.0f / 16.0f);

    float occlusion = (1.0f - lit) * facingMask * frustumMask;
    float shadeAmount = saturate(occlusion * saturate(gLightDirStrength.w * 0.62f));
    float3 shadowColor = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.42f, 0.47f, 0.58f), shadeAmount);
    return float4(shadowColor, 1.0f);
}
)HLSL";

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

struct ShadowState {
    std::mutex mu;
    ID3D11Device* dev = nullptr;
    bool initialized = false;
    bool init_failed = false;

    ID3D11Texture2D* tex = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;

    ID3D11GeometryShader* gs_reproj = nullptr;
    ID3D11GeometryShader* gs_reproj_uv = nullptr;
    ID3D11PixelShader* ps_alpha_cutout = nullptr;
    ID3D11VertexShader* vs_resolve = nullptr;
    ID3D11PixelShader* ps_resolve = nullptr;

    ID3D11Buffer* cb_reproj = nullptr;
    ID3D11Buffer* cb_resolve = nullptr;

    ID3D11RasterizerState* rs_shadow = nullptr;
    ID3D11DepthStencilState* dss_shadow = nullptr;
    ID3D11BlendState* bs_shadow = nullptr;
    ID3D11SamplerState* samp_alpha = nullptr;

    ID3D11SamplerState* samp_cmp = nullptr;
    ID3D11RasterizerState* rs_resolve = nullptr;
    ID3D11DepthStencilState* dss_resolve = nullptr;
    ID3D11BlendState* bs_multiply = nullptr;

    // Frame state (set by PrepareShadowMapForFrame on render thread)
    ID3D11RenderTargetView* frame_scene_rtv = nullptr;
    bool frame_valid = false;
    float cam_clip_to_light_clip[4][4]{};
    float view_to_light_clip[4][4]{};
    float light_dir_vs[3] = {0.35f, 0.85f, 0.38f};
    uint32_t frame_replay_draws = 0;
    uint32_t frame_skipped_blend = 0;
    bool logged_ready = false;
};

ShadowState g_sh;
std::atomic<bool> g_capture_open{false};
std::atomic<bool> g_alpha_cutout{false};
std::atomic<uint32_t> g_alpha_ref_bits{0};
thread_local bool g_in_shadow_replay = false;

// Small per-thread cache to avoid calling BlendState/DSS->GetDesc() hundreds of times per frame.
struct CasterFilterTls {
    std::unordered_map<const ID3D11BlendState*, bool> blend_opaque;
    std::unordered_map<const ID3D11DepthStencilState*, bool> dss_depth_write;
};
thread_local CasterFilterTls g_tls_filter;

bool CompileHlslStage(
    const char* entry,
    const char* target,
    ID3DBlob** out_blob,
    PostLogFn logger) {
    if (!out_blob) {
        return false;
    }
    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        return false;
    }
    const D3D_SHADER_MACRO macros[] = {{nullptr, nullptr}};
    return CompileShaderBlobFromSource(
        compile_fn,
        kShadowShadersSource,
        sizeof(kShadowShadersSource) - 1,
        "tl2_shadow_shaders.hlsl",
        entry,
        target,
        macros,
        out_blob,
        logger);
}

void ReleaseAllShadowUnlocked() {
    SafeRelease(g_sh.tex);
    SafeRelease(g_sh.dsv);
    SafeRelease(g_sh.srv);
    SafeRelease(g_sh.gs_reproj);
    SafeRelease(g_sh.gs_reproj_uv);
    SafeRelease(g_sh.ps_alpha_cutout);
    SafeRelease(g_sh.vs_resolve);
    SafeRelease(g_sh.ps_resolve);
    SafeRelease(g_sh.cb_reproj);
    SafeRelease(g_sh.cb_resolve);
    SafeRelease(g_sh.rs_shadow);
    SafeRelease(g_sh.dss_shadow);
    SafeRelease(g_sh.bs_shadow);
    SafeRelease(g_sh.samp_alpha);
    SafeRelease(g_sh.samp_cmp);
    SafeRelease(g_sh.rs_resolve);
    SafeRelease(g_sh.dss_resolve);
    SafeRelease(g_sh.bs_multiply);
    SafeRelease(g_sh.dev);
    g_sh.initialized = false;
    g_sh.init_failed = false;
    g_sh.frame_scene_rtv = nullptr;
    g_sh.frame_valid = false;
    g_sh.logged_ready = false;
}

bool EnsureShadowObjectsUnlocked(ID3D11Device* dev, PostLogFn logger) {
    if (!dev) {
        return false;
    }
    if (g_sh.dev != dev) {
        ReleaseAllShadowUnlocked();
        g_sh.dev = dev;
        g_sh.dev->AddRef();
    }
    if (g_sh.initialized) {
        return true;
    }
    if (g_sh.init_failed) {
        return false;
    }

    HdrUpgradeBypassScope bypass;

    // 1. 2048x2048 R32_TYPELESS depth map + D32_FLOAT DSV + R32_FLOAT SRV
    D3D11_TEXTURE2D_DESC td{};
    td.Width = kShadowMapSize;
    td.Height = kShadowMapSize;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_sh.tex)) || !g_sh.tex) {
        g_sh.init_failed = true;
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvd{};
    dsvd.Format = DXGI_FORMAT_D32_FLOAT;
    dsvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    if (FAILED(dev->CreateDepthStencilView(g_sh.tex, &dsvd, &g_sh.dsv)) || !g_sh.dsv) {
        ReleaseAllShadowUnlocked();
        g_sh.init_failed = true;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_sh.tex, &srvd, &g_sh.srv)) || !g_sh.srv) {
        ReleaseAllShadowUnlocked();
        g_sh.init_failed = true;
        return false;
    }

    // 2. Shaders
    ID3DBlob* b_gs = nullptr;
    ID3DBlob* b_gs_uv = nullptr;
    ID3DBlob* b_ps_alpha = nullptr;
    ID3DBlob* b_vs_res = nullptr;
    ID3DBlob* b_ps_res = nullptr;
    const bool comp_ok =
        CompileHlslStage("GS_ShadowReproject", "gs_4_0", &b_gs, logger) &&
        CompileHlslStage("GS_ShadowReprojectUv", "gs_4_0", &b_gs_uv, logger) &&
        CompileHlslStage("PS_ShadowAlphaCutout", "ps_4_0", &b_ps_alpha, logger) &&
        CompileHlslStage("VS_ShadowFullscreen", "vs_4_0", &b_vs_res, logger) &&
        CompileHlslStage("PS_ShadowMapResolve", "ps_4_0", &b_ps_res, logger);

    bool created = false;
    if (comp_ok) {
        created =
            SUCCEEDED(dev->CreateGeometryShader(b_gs->GetBufferPointer(), b_gs->GetBufferSize(), nullptr, &g_sh.gs_reproj)) &&
            SUCCEEDED(dev->CreateGeometryShader(b_gs_uv->GetBufferPointer(), b_gs_uv->GetBufferSize(), nullptr, &g_sh.gs_reproj_uv)) &&
            SUCCEEDED(dev->CreatePixelShader(b_ps_alpha->GetBufferPointer(), b_ps_alpha->GetBufferSize(), nullptr, &g_sh.ps_alpha_cutout)) &&
            SUCCEEDED(dev->CreateVertexShader(b_vs_res->GetBufferPointer(), b_vs_res->GetBufferSize(), nullptr, &g_sh.vs_resolve)) &&
            SUCCEEDED(dev->CreatePixelShader(b_ps_res->GetBufferPointer(), b_ps_res->GetBufferSize(), nullptr, &g_sh.ps_resolve));
    }
    SafeRelease(b_gs);
    SafeRelease(b_gs_uv);
    SafeRelease(b_ps_alpha);
    SafeRelease(b_vs_res);
    SafeRelease(b_ps_res);
    if (!created) {
        ReleaseAllShadowUnlocked();
        g_sh.init_failed = true;
        return false;
    }

    // 3. Constant buffers
    auto create_cb = [dev](UINT byte_width, ID3D11Buffer** out_cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = byte_width;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(dev->CreateBuffer(&bd, nullptr, out_cb)) && *out_cb;
    };
    if (!create_cb(sizeof(CbShadowReproject), &g_sh.cb_reproj) ||
        !create_cb(sizeof(CbShadowResolve), &g_sh.cb_resolve)) {
        ReleaseAllShadowUnlocked();
        g_sh.init_failed = true;
        return false;
    }

    // 4. Shadow capture states
    D3D11_RASTERIZER_DESC rsd{};
    rsd.FillMode = D3D11_FILL_SOLID;
    rsd.CullMode = D3D11_CULL_NONE;
    rsd.DepthBias = 650;
    rsd.DepthBiasClamp = 0.01f;
    rsd.SlopeScaledDepthBias = 2.0f;
    rsd.DepthClipEnable = TRUE;

    D3D11_DEPTH_STENCIL_DESC dssd{};
    dssd.DepthEnable = TRUE;
    dssd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dssd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;

    D3D11_BLEND_DESC bsd{};
    bsd.RenderTarget[0].RenderTargetWriteMask = 0;

    D3D11_SAMPLER_DESC sampd{};
    sampd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampd.MaxLOD = D3D11_FLOAT32_MAX;

    // 5. Shadow resolve states
    D3D11_SAMPLER_DESC cmpd{};
    cmpd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    cmpd.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    cmpd.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    cmpd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    cmpd.BorderColor[0] = 1.0f;
    cmpd.BorderColor[1] = 1.0f;
    cmpd.BorderColor[2] = 1.0f;
    cmpd.BorderColor[3] = 1.0f;
    cmpd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    cmpd.MaxLOD = D3D11_FLOAT32_MAX;

    D3D11_RASTERIZER_DESC rsr{};
    rsr.FillMode = D3D11_FILL_SOLID;
    rsr.CullMode = D3D11_CULL_NONE;
    rsr.DepthClipEnable = FALSE;

    D3D11_DEPTH_STENCIL_DESC dssr{};
    dssr.DepthEnable = FALSE;
    dssr.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;

    D3D11_BLEND_DESC bsm{};
    bsm.RenderTarget[0].BlendEnable = TRUE;
    bsm.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bsm.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bsm.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bsm.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bsm.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bsm.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bsm.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
                                               D3D11_COLOR_WRITE_ENABLE_GREEN |
                                               D3D11_COLOR_WRITE_ENABLE_BLUE;

    const bool states_ok =
        SUCCEEDED(dev->CreateRasterizerState(&rsd, &g_sh.rs_shadow)) &&
        SUCCEEDED(dev->CreateDepthStencilState(&dssd, &g_sh.dss_shadow)) &&
        SUCCEEDED(dev->CreateBlendState(&bsd, &g_sh.bs_shadow)) &&
        SUCCEEDED(dev->CreateSamplerState(&sampd, &g_sh.samp_alpha)) &&
        SUCCEEDED(dev->CreateSamplerState(&cmpd, &g_sh.samp_cmp)) &&
        SUCCEEDED(dev->CreateRasterizerState(&rsr, &g_sh.rs_resolve)) &&
        SUCCEEDED(dev->CreateDepthStencilState(&dssr, &g_sh.dss_resolve)) &&
        SUCCEEDED(dev->CreateBlendState(&bsm, &g_sh.bs_multiply));

    if (!states_ok) {
        ReleaseAllShadowUnlocked();
        g_sh.init_failed = true;
        return false;
    }

    g_sh.initialized = true;
    if (logger && !g_sh.logged_ready) {
        g_sh.logged_ready = true;
        logger("[Shadow] Initialized %ux%u D32_FLOAT Shadow Map pipeline (GS reprojection + 16-tap PCF)",
               kShadowMapSize, kShadowMapSize);
    }
    return true;
}

void MulMat4D(const double a[16], const double b[16], double out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r * 4 + c] =
                a[r * 4 + 0] * b[0 * 4 + c] +
                a[r * 4 + 1] * b[1 * 4 + c] +
                a[r * 4 + 2] * b[2 * 4 + c] +
                a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

bool InvertMat4D(const double m[16], double inv_out[16]) {
    double aug[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            aug[r][c] = m[r * 4 + c];
            aug[r][c + 4] = (r == c) ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        double best = std::abs(aug[col][col]);
        for (int r = col + 1; r < 4; ++r) {
            const double v = std::abs(aug[r][col]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-12) {
            return false;
        }
        if (piv != col) {
            for (int c = 0; c < 8; ++c) {
                std::swap(aug[col][c], aug[piv][c]);
            }
        }
        const double inv_piv = 1.0 / aug[col][col];
        for (int c = 0; c < 8; ++c) {
            aug[col][c] *= inv_piv;
        }
        for (int r = 0; r < 4; ++r) {
            if (r == col) {
                continue;
            }
            const double factor = aug[r][col];
            for (int c = 0; c < 8; ++c) {
                aug[r][c] -= factor * aug[col][c];
            }
        }
    }
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            inv_out[r * 4 + c] = aug[r][c + 4];
        }
    }
    return true;
}

bool BuildLightViewProj(
    const double inv_view[16],
    double out_light_vp[16],
    float out_to_light_ws[3]) {
    // Ogre's DirectionalLight0_Direction points FROM light TOWARD surface.
    float raw_dir[3] = {0.38f, -0.78f, 0.49f};
    float lum = 0.0f;
    double lx = 0.38;
    double ly = 0.78;
    double lz = -0.49;
    if (GetCapturedDirectionalLight(raw_dir, &lum) && lum > 1e-3f) {
        double tx = -static_cast<double>(raw_dir[0]);
        double ty = -static_cast<double>(raw_dir[1]);
        double tz = -static_cast<double>(raw_dir[2]);
        const double len = std::sqrt(tx * tx + ty * ty + tz * tz);
        if (std::isfinite(len) && len > 1e-4) {
            tx /= len;
            ty /= len;
            tz /= len;
            // Clamp near-horizontal underground lights to at least ~22 deg elevation
            if (ty < 0.38) {
                ty = 0.38;
                const double renorm = std::sqrt(tx * tx + ty * ty + tz * tz);
                tx /= renorm;
                ty /= renorm;
                tz /= renorm;
            }
            lx = tx;
            ly = ty;
            lz = tz;
        }
    }
    out_to_light_ws[0] = static_cast<float>(lx);
    out_to_light_ws[1] = static_cast<float>(ly);
    out_to_light_ws[2] = static_cast<float>(lz);

    // Focus point = Camera View Space (0, 0, -kCameraFocusDist) transformed to World Space
    const double focus_x = inv_view[0] * 0.0 + inv_view[1] * 0.0 + inv_view[2] * (-kCameraFocusDist) + inv_view[3];
    const double focus_y = inv_view[4] * 0.0 + inv_view[5] * 0.0 + inv_view[6] * (-kCameraFocusDist) + inv_view[7];
    const double focus_z = inv_view[8] * 0.0 + inv_view[9] * 0.0 + inv_view[10] * (-kCameraFocusDist) + inv_view[11];

    // Right-handed view matrix matching Ogre's column-vector convention:
    // forward = dir FROM focus TOWARD -light (i.e. +Z_light points TOWARD the light, so -Z_light points along light rays).
    const double fx = -lx;
    const double fy = -ly;
    const double fz = -lz;
    double upx = 0.0, upy = 1.0, upz = 0.0;
    if (std::abs(fy) > 0.96) {
        upx = 0.0; upy = 0.0; upz = 1.0;
    }
    // right = normalize(cross(forward, up)) -> s = f x up, u = s x f
    double rx = fy * upz - fz * upy;
    double ry = fz * upx - fx * upz;
    double rz = fx * upy - fy * upx;
    const double rlen = std::sqrt(rx * rx + ry * ry + rz * rz);
    if (rlen < 1e-8) {
        return false;
    }
    rx /= rlen; ry /= rlen; rz /= rlen;

    const double ux = ry * fz - rz * fy;
    const double uy = rz * fx - rx * fz;
    const double uz = rx * fy - ry * fx;

    // Light eye positioned at focus + l * 0 (ortho uses signed Z range around focus)
    // Basis rows map world point -> light view coordinates (Right, Up, Back=-Forward)
    double l_view[16] = {
         rx,  ry,  rz, -(rx * focus_x + ry * focus_y + rz * focus_z),
         ux,  uy,  uz, -(ux * focus_x + uy * focus_y + uz * focus_z),
        -fx, -fy, -fz, -(-fx * focus_x - fy * focus_y - fz * focus_z),
        0.0, 0.0, 0.0, 1.0
    };

    // Texel snapping: shift light view X/Y translation to exact shadow texel increments
    const double texel_world = (2.0 * kShadowOrthoHalfExtent) / static_cast<double>(kShadowMapSize);
    const double world_origin_lx = l_view[3];
    const double world_origin_ly = l_view[7];
    const double snapped_lx = std::floor(world_origin_lx / texel_world) * texel_world;
    const double snapped_ly = std::floor(world_origin_ly / texel_world) * texel_world;
    l_view[3] = snapped_lx;
    l_view[7] = snapped_ly;

    // D3D RH orthographic projection with z in [-kShadowNearHalfDepth, +kShadowNearHalfDepth]:
    // Notice camera looks down -Z_light (so nearer to light = larger +Z_light -> depth=0; farther along -Z_light -> depth=1).
    // If nearDist = -kShadowNearHalfDepth (towards light, +Z_light) and farDist = +kShadowNearHalfDepth (away, -Z_light):
    // depth = (-Z_light + kShadowNearHalfDepth) / (2 * kShadowNearHalfDepth)
    const double inv_half = 1.0 / static_cast<double>(kShadowOrthoHalfExtent);
    const double inv_z_span = 1.0 / (2.0 * static_cast<double>(kShadowNearHalfDepth));
    const double l_ortho[16] = {
        inv_half, 0.0, 0.0, 0.0,
        0.0, inv_half, 0.0, 0.0,
        0.0, 0.0, -inv_z_span, 0.5,
        0.0, 0.0, 0.0, 1.0
    };
    MulMat4D(l_ortho, l_view, out_light_vp);
    return true;
}

bool IsOpaqueBlendCaster(ID3D11BlendState* bs) {
    if (!bs) {
        return true;
    }
    auto it = g_tls_filter.blend_opaque.find(bs);
    if (it != g_tls_filter.blend_opaque.end()) {
        return it->second;
    }
    D3D11_BLEND_DESC d{};
    bs->GetDesc(&d);
    const bool opaque = (d.RenderTarget[0].BlendEnable == FALSE);
    if (g_tls_filter.blend_opaque.size() > 256) {
        g_tls_filter.blend_opaque.clear();
    }
    g_tls_filter.blend_opaque.emplace(bs, opaque);
    return opaque;
}

bool IsDepthWriteCaster(ID3D11DepthStencilState* dss) {
    if (!dss) {
        return true;  // Default D3D11 DSS enables depth test + depth write
    }
    auto it = g_tls_filter.dss_depth_write.find(dss);
    if (it != g_tls_filter.dss_depth_write.end()) {
        return it->second;
    }
    D3D11_DEPTH_STENCIL_DESC d{};
    dss->GetDesc(&d);
    const bool writes_depth =
        (d.DepthEnable == TRUE) &&
        (d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL);
    if (g_tls_filter.dss_depth_write.size() > 256) {
        g_tls_filter.dss_depth_write.clear();
    }
    g_tls_filter.dss_depth_write.emplace(dss, writes_depth);
    return writes_depth;
}

struct SavedReplayState {
    ID3D11GeometryShader* gs = nullptr;
    ID3D11ClassInstance* gs_inst[8]{};
    UINT gs_inst_count = 8;
    ID3D11Buffer* gs_cb0 = nullptr;

    ID3D11PixelShader* ps = nullptr;
    ID3D11ClassInstance* ps_inst[8]{};
    UINT ps_inst_count = 8;
    ID3D11Buffer* ps_cb0 = nullptr;
    ID3D11SamplerState* ps_samp0 = nullptr;

    ID3D11RasterizerState* rs = nullptr;
    UINT num_vps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};

    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ID3D11BlendState* bs = nullptr;
    float blend_factor[4]{};
    UINT sample_mask = 0xFFFFFFFFu;
    ID3D11DepthStencilState* dss = nullptr;
    UINT stencil_ref = 0;

    ID3D11ShaderResourceView* ps_srv0 = nullptr;
};

template <typename DrawFnCall>
void ReplayShadowDrawCommon(ID3D11DeviceContext* ctx, DrawFnCall&& draw_fn) {
    if (!ctx || !IsWorldShadowCaptureOpen() || g_in_shadow_replay) {
        return;
    }
    if (!g_sh.frame_valid || !g_sh.initialized) {
        return;
    }

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);
    if (topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST &&
        topo != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP) {
        return;
    }

    SavedReplayState st{};
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, st.rtvs, &st.dsv);
    if (st.rtvs[0] != g_sh.frame_scene_rtv || !st.dsv || st.dsv == g_sh.dsv) {
        for (auto*& r : st.rtvs) SafeRelease(r);
        SafeRelease(st.dsv);
        return;
    }

    ctx->OMGetBlendState(&st.bs, st.blend_factor, &st.sample_mask);
    ctx->OMGetDepthStencilState(&st.dss, &st.stencil_ref);
    if (!IsOpaqueBlendCaster(st.bs) || !IsDepthWriteCaster(st.dss)) {
        ++g_sh.frame_skipped_blend;
        for (auto*& r : st.rtvs) SafeRelease(r);
        SafeRelease(st.dsv);
        SafeRelease(st.bs);
        SafeRelease(st.dss);
        return;
    }

    ctx->PSGetShaderResources(0, 1, &st.ps_srv0);
    const bool use_alpha_cutout = g_alpha_cutout.load(std::memory_order_relaxed) && (st.ps_srv0 != nullptr);

    ctx->GSGetShader(&st.gs, st.gs_inst, &st.gs_inst_count);
    ctx->GSGetConstantBuffers(0, 1, &st.gs_cb0);
    ctx->PSGetShader(&st.ps, st.ps_inst, &st.ps_inst_count);
    ctx->PSGetConstantBuffers(0, 1, &st.ps_cb0);
    ctx->PSGetSamplers(0, 1, &st.ps_samp0);
    ctx->RSGetState(&st.rs);
    ctx->RSGetViewports(&st.num_vps, st.vps);

    if (use_alpha_cutout) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(g_sh.cb_reproj, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            CbShadowReproject cb{};
            std::memcpy(cb.cam_clip_to_light_clip, g_sh.cam_clip_to_light_clip, sizeof(cb.cam_clip_to_light_clip));
            const uint32_t bits = g_alpha_ref_bits.load(std::memory_order_relaxed);
            std::memcpy(&cb.alpha_params[0], &bits, sizeof(float));
            std::memcpy(mapped.pData, &cb, sizeof(cb));
            ctx->Unmap(g_sh.cb_reproj, 0);
        }
    }

    D3D11_VIEWPORT vp{};
    vp.Width = static_cast<float>(kShadowMapSize);
    vp.Height = static_cast<float>(kShadowMapSize);
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* null_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, null_rtvs, g_sh.dsv);
    ctx->OMSetBlendState(g_sh.bs_shadow, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(g_sh.dss_shadow, 0);
    ctx->RSSetState(g_sh.rs_shadow);
    ctx->RSSetViewports(1, &vp);

    ctx->GSSetConstantBuffers(0, 1, &g_sh.cb_reproj);
    if (use_alpha_cutout) {
        ctx->GSSetShader(g_sh.gs_reproj_uv, nullptr, 0);
        ctx->PSSetShader(g_sh.ps_alpha_cutout, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &g_sh.cb_reproj);
        ctx->PSSetSamplers(0, 1, &g_sh.samp_alpha);
    } else {
        ctx->GSSetShader(g_sh.gs_reproj, nullptr, 0);
        ctx->PSSetShader(nullptr, nullptr, 0);
    }

    g_in_shadow_replay = true;
    draw_fn();
    g_in_shadow_replay = false;
    ++g_sh.frame_replay_draws;

    // Restore original pipeline state cleanly
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, st.rtvs, st.dsv);
    ctx->OMSetBlendState(st.bs, st.blend_factor, st.sample_mask);
    ctx->OMSetDepthStencilState(st.dss, st.stencil_ref);
    ctx->RSSetState(st.rs);
    ctx->RSSetViewports(st.num_vps, st.vps);
    ctx->GSSetShader(st.gs, st.gs_inst, st.gs_inst_count);
    ctx->GSSetConstantBuffers(0, 1, &st.gs_cb0);
    ctx->PSSetShader(st.ps, st.ps_inst, st.ps_inst_count);
    ctx->PSSetConstantBuffers(0, 1, &st.ps_cb0);
    ctx->PSSetSamplers(0, 1, &st.ps_samp0);

    SafeRelease(st.ps_srv0);
    for (auto*& r : st.rtvs) SafeRelease(r);
    SafeRelease(st.dsv);
    SafeRelease(st.bs);
    SafeRelease(st.dss);
    SafeRelease(st.rs);
    SafeRelease(st.gs);
    SafeRelease(st.gs_cb0);
    SafeRelease(st.ps);
    SafeRelease(st.ps_cb0);
    SafeRelease(st.ps_samp0);
    for (UINT i = 0; i < st.gs_inst_count; ++i) SafeRelease(st.gs_inst[i]);
    for (UINT i = 0; i < st.ps_inst_count; ++i) SafeRelease(st.ps_inst[i]);
}

}  // namespace

void PrepareShadowMapForFrame(
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* scene_rtv,
    const float* view_4x4,
    const float* proj_4x4,
    PostLogFn logger) {
    g_capture_open.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_sh.mu);
    g_sh.frame_valid = false;
    g_sh.frame_scene_rtv = scene_rtv;
    g_sh.frame_replay_draws = 0;
    g_sh.frame_skipped_blend = 0;

    if (!dev || !ctx || !scene_rtv || !view_4x4 || !proj_4x4) {
        return;
    }
    if (!EnsureShadowObjectsUnlocked(dev, logger)) {
        return;
    }

    double v_d[16]{};
    double p_d[16]{};
    for (int i = 0; i < 16; ++i) {
        v_d[i] = static_cast<double>(view_4x4[i]);
        p_d[i] = static_cast<double>(proj_4x4[i]);
    }
    double inv_v[16]{};
    double cam_vp[16]{};
    double inv_cam_vp[16]{};
    MulMat4D(p_d, v_d, cam_vp);
    if (!InvertMat4D(v_d, inv_v) || !InvertMat4D(cam_vp, inv_cam_vp)) {
        return;
    }

    double light_vp[16]{};
    float to_light_ws[3]{};
    if (!BuildLightViewProj(inv_v, light_vp, to_light_ws)) {
        return;
    }

    double reproj[16]{};
    double view_to_light[16]{};
    MulMat4D(light_vp, inv_cam_vp, reproj);
    MulMat4D(light_vp, inv_v, view_to_light);

    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            g_sh.cam_clip_to_light_clip[r][c] = static_cast<float>(reproj[r * 4 + c]);
            g_sh.view_to_light_clip[r][c] = static_cast<float>(view_to_light[r * 4 + c]);
        }
    }

    // Light direction TO sun in Camera View Space = upper 3x3 of View * to_light_ws
    const float lvx = view_4x4[0] * to_light_ws[0] + view_4x4[1] * to_light_ws[1] + view_4x4[2] * to_light_ws[2];
    const float lvy = view_4x4[4] * to_light_ws[0] + view_4x4[5] * to_light_ws[1] + view_4x4[6] * to_light_ws[2];
    const float lvz = view_4x4[8] * to_light_ws[0] + view_4x4[9] * to_light_ws[1] + view_4x4[10] * to_light_ws[2];
    const float lv_len = std::sqrt(lvx * lvx + lvy * lvy + lvz * lvz);
    if (lv_len > 1e-5f) {
        g_sh.light_dir_vs[0] = lvx / lv_len;
        g_sh.light_dir_vs[1] = lvy / lv_len;
        g_sh.light_dir_vs[2] = lvz / lv_len;
    }

    ctx->ClearDepthStencilView(g_sh.dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(ctx->Map(g_sh.cb_reproj, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        CbShadowReproject cb{};
        std::memcpy(cb.cam_clip_to_light_clip, g_sh.cam_clip_to_light_clip, sizeof(cb.cam_clip_to_light_clip));
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        ctx->Unmap(g_sh.cb_reproj, 0);
    }

    g_sh.frame_valid = true;
}

void SetWorldShadowCaptureOpen(bool open) {
    g_capture_open.store(open, std::memory_order_relaxed);
}

bool IsWorldShadowCaptureOpen() {
    return g_capture_open.load(std::memory_order_relaxed);
}

void SetShadowCasterAlphaCutout(bool alpha_cutout, float alpha_ref) {
    g_alpha_cutout.store(alpha_cutout, std::memory_order_relaxed);
    uint32_t bits = 0;
    std::memcpy(&bits, &alpha_ref, sizeof(float));
    g_alpha_ref_bits.store(bits, std::memory_order_relaxed);
}

bool ShouldBypassDrawHookForShadowReplay() {
    return g_in_shadow_replay;
}

void ReplayShadowDrawIndexed(
    ID3D11DeviceContext* ctx,
    FnD3D11DrawIndexed orig_fn,
    uint32_t index_count,
    uint32_t start_index,
    int32_t base_vertex) {
    if (!orig_fn) {
        return;
    }
    ReplayShadowDrawCommon(ctx, [=]() {
        orig_fn(ctx, index_count, start_index, base_vertex);
    });
}

void ReplayShadowDraw(
    ID3D11DeviceContext* ctx,
    FnD3D11Draw orig_fn,
    uint32_t vertex_count,
    uint32_t start_vertex) {
    if (!orig_fn) {
        return;
    }
    ReplayShadowDrawCommon(ctx, [=]() {
        orig_fn(ctx, vertex_count, start_vertex);
    });
}

bool ExecuteShadowResolvePass(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    ID3D11ShaderResourceView* scene_depth_srv,
    const float* proj_4x4,
    int vp_left,
    int vp_top,
    int vp_width,
    int vp_height,
    float shadow_strength,
    PostLogFn logger) {
    if (!ctx || !target_rtv || !scene_depth_srv || !proj_4x4 || vp_width <= 0 || vp_height <= 0) {
        return false;
    }
    if (shadow_strength <= 0.001f) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_sh.mu);
    if (!g_sh.initialized || !g_sh.frame_valid || g_sh.frame_replay_draws == 0) {
        if (logger && !g_sh.initialized) {
            logger("[Shadow] Skip resolve: not initialized");
        }
        return false;
    }

    CbShadowResolve cb{};
    std::memcpy(cb.view_to_light_clip, g_sh.view_to_light_clip, sizeof(cb.view_to_light_clip));
    cb.proj_xy[0] = proj_4x4[0];
    cb.proj_xy[1] = proj_4x4[5];
    cb.proj_xy[2] = proj_4x4[2];
    cb.proj_xy[3] = proj_4x4[6];
    cb.proj_z_light[0] = proj_4x4[10];
    cb.proj_z_light[1] = proj_4x4[11];
    cb.proj_z_light[2] = 1.0f / static_cast<float>(kShadowMapSize);
    cb.proj_z_light[3] = (2.0f * kShadowOrthoHalfExtent / static_cast<float>(kShadowMapSize)) * 2.2f;
    cb.light_dir_strength[0] = g_sh.light_dir_vs[0];
    cb.light_dir_strength[1] = g_sh.light_dir_vs[1];
    cb.light_dir_strength[2] = g_sh.light_dir_vs[2];
    cb.light_dir_strength[3] = (std::max)(0.0f, (std::min)(2.0f, shadow_strength));
    cb.vp_rect[0] = static_cast<float>(vp_left);
    cb.vp_rect[1] = static_cast<float>(vp_top);
    cb.vp_rect[2] = static_cast<float>(vp_width);
    cb.vp_rect[3] = static_cast<float>(vp_height);
    cb.vp_clamp[0] = vp_left;
    cb.vp_clamp[1] = vp_top;
    cb.vp_clamp[2] = vp_left + vp_width - 1;
    cb.vp_clamp[3] = vp_top + vp_height - 1;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g_sh.cb_resolve, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(g_sh.cb_resolve, 0);

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = static_cast<float>(vp_left);
    vp.TopLeftY = static_cast<float>(vp_top);
    vp.Width = static_cast<float>(vp_width);
    vp.Height = static_cast<float>(vp_height);
    vp.MaxDepth = 1.0f;

    ID3D11ShaderResourceView* srvs[2] = {scene_depth_srv, g_sh.srv};
    ID3D11RenderTargetView* rtvs[1] = {target_rtv};
    ctx->OMSetRenderTargets(1, rtvs, nullptr);
    ctx->OMSetBlendState(g_sh.bs_multiply, nullptr, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(g_sh.dss_resolve, 0);
    ctx->RSSetState(g_sh.rs_resolve);
    ctx->RSSetViewports(1, &vp);

    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_sh.vs_resolve, nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(g_sh.ps_resolve, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_sh.cb_resolve);
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->PSSetSamplers(0, 1, &g_sh.samp_cmp);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, null_srvs);
    return true;
}

bool VerifyShadowShadersCompile(PostLogFn logger) {
    const struct {
        const char* entry;
        const char* target;
    } stages[5] = {
        {"GS_ShadowReproject", "gs_4_0"},
        {"GS_ShadowReprojectUv", "gs_4_0"},
        {"PS_ShadowAlphaCutout", "ps_4_0"},
        {"VS_ShadowFullscreen", "vs_4_0"},
        {"PS_ShadowMapResolve", "ps_4_0"},
    };
    for (const auto& s : stages) {
        ID3DBlob* b = nullptr;
        const bool ok = CompileHlslStage(s.entry, s.target, &b, logger);
        if (b) {
            b->Release();
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

void ReleaseShadowPassResources() {
    g_capture_open.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_sh.mu);
    ReleaseAllShadowUnlocked();
}

}  // namespace routeb
