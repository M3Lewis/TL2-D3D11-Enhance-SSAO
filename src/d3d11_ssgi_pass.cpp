#include "d3d11_ssgi_pass.h"
#include "d3d11_post_renderer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace routeb {
namespace {

constexpr int kSsgiMipCount = 4;

struct alignas(16) CbSsgiParams {
    float ssgi_dims[4];  // halfW, halfH, 1/halfW, 1/halfH
    float src_dims[4];   // rtW, rtH, 1/rtW, 1/rtH
    float params0[4];    // giStrength, radiusWorld, depthEdgeScale, maxBounceGain
    float params1[4];    // isDesert, desertAtten, curMipIdx, mipInvW_or_mipInvH
};
static_assert(sizeof(CbSsgiParams) == 64, "CbSsgiParams must be 64 bytes");

const char kSsgiShaderSource[] = R"HLSL(
cbuffer CbAoParams : register(b0)
{
    float4 gProjXY;          // x=p00, y=p11, z=p02, w=p12
    float4 gProjZAndFade;    // x=p22, y=p23, z=fadeEnd, w=invFadeRange
    float4 gVpRect;          // x=vpOriginX, y=vpOriginY, z=vpWidth, w=vpHeight
    int4   gVpClamp;         // x=minX, y=minY, z=maxX, w=maxY
    float4 gAoParams;
    float4 gBlurAndMode;     // x=invVpW, y=invVpH, z=depthEdgeScale, w=debugMode
    float4 gSscsParams;
    float4 gSscsKeyAndRange;
    float4 gAerialFogParams;
    float4 gViewUpAndWater;
    float4 gInvViewRow0;
    float4 gInvViewRow2;
    float4 gInvViewRow1;
    float4 gHeightFogColor;
    float4 gHeightFogShape;
};

cbuffer CbSsgiParams : register(b1)
{
    float4 gSsgiDims;        // x=curW, y=curH, z=invCurW, w=invCurH
    float4 gSrcDims;         // x=rtW, y=rtH, z=invRtW, w=invRtH
    float4 gSsgiParams0;     // x=strength, y=radiusWorld, z=depthEdgeScale, w=maxBounceGain
    float4 gSsgiParams1;     // x=isDesert, y=desertAtten, z=unused, w=prevTexelScale
};

Texture2D<float4> gSceneTex : register(t0);
Texture2D<float4> gColorPyr : register(t1);
Texture2D<float4> gSsgiAux  : register(t2);
SamplerState gSamp : register(s0);

#if MSAA_DEPTH
Texture2DMS<float4> gDepthTex : register(t3);
Texture2DMS<float4> gWaterDepthTex : register(t4);
float LoadRawDepth(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gDepthTex.Load(c, 0).r;
}
float LoadWaterDepth(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gWaterDepthTex.Load(c, 0).r;
}
#else
Texture2D<float4> gDepthTex : register(t3);
Texture2D<float4> gWaterDepthTex : register(t4);
float LoadRawDepth(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gDepthTex.Load(int3(c, 0)).r;
}
float LoadWaterDepth(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gWaterDepthTex.Load(int3(c, 0)).r;
}
#endif

struct VsOut
{
    float4 pos : SV_Position;
};

float RawToLinearDist(float zNdc)
{
    float denom = min(zNdc + gProjZAndFade.x, -1e-6f);
    return gProjZAndFade.y / denom;
}

float3 PixelToViewPos(int2 pix, float rawZ)
{
    float d = max(RawToLinearDist(rawZ), 1e-3f);
    float2 uv = (float2(pix) + 0.5f - gVpRect.xy) / max(gVpRect.zw, float2(1.0f, 1.0f));
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    return float3(
        d * (ndc.x + gProjXY.z) / gProjXY.x,
        d * (ndc.y + gProjXY.w) / gProjXY.y,
        -d);
}

float3 ReconstructViewNormal(int2 pix, float3 P, float dC)
{
    int2 pL = clamp(pix + int2(-1, 0), gVpClamp.xy, gVpClamp.zw);
    int2 pR = clamp(pix + int2(1, 0), gVpClamp.xy, gVpClamp.zw);
    int2 pU = clamp(pix + int2(0, -1), gVpClamp.xy, gVpClamp.zw);
    int2 pD = clamp(pix + int2(0, 1), gVpClamp.xy, gVpClamp.zw);

    float rawL = LoadRawDepth(pL);
    float rawR = LoadRawDepth(pR);
    float rawU = LoadRawDepth(pU);
    float rawD = LoadRawDepth(pD);

    float dL = RawToLinearDist(rawL);
    float dR = RawToLinearDist(rawR);
    float dU = RawToLinearDist(rawU);
    float dD = RawToLinearDist(rawD);

    float3 dx = (abs(dR - dC) < abs(dC - dL))
        ? (PixelToViewPos(pR, rawR) - P)
        : (P - PixelToViewPos(pL, rawL));
    float3 dy = (abs(dD - dC) < abs(dC - dU))
        ? (PixelToViewPos(pD, rawD) - P)
        : (P - PixelToViewPos(pU, rawU));

    float3 N = cross(dy, dx);
    float lenSq = dot(N, N);
    if (lenSq < 1e-10f)
        return float3(0.0f, 0.0f, 1.0f);
    return N * rsqrt(lenSq);
}

float SsgiIgnHash(float2 pos)
{
    return frac(52.9829189f * frac(dot(pos, float2(0.06711056f, 0.00583715f))));
}

float ComputeWaterExclusion(int2 pix, float rawC)
{
    if (gViewUpAndWater.w < 0.5f)
        return 1.0f;
    float wRaw = LoadWaterDepth(pix);
    if (wRaw >= 0.99998f)
        return 1.0f;
    float dScene = RawToLinearDist(rawC);
    float dWater = RawToLinearDist(wRaw);
    // Keep water reflections crisp: mask indirect light off actual water plane.
    return smoothstep(0.08f, 0.55f, abs(dScene - dWater));
}

float4 PS_SsgiDownsampleColor(VsOut input) : SV_Target0
{
    float2 halfUv = clamp(input.pos.xy * gSsgiDims.zw, float2(0.0f, 0.0f), float2(1.0f, 1.0f));
    float2 srcUv = (gVpRect.xy + halfUv * gVpRect.zw) * gSrcDims.zw;
    float3 col = gSceneTex.SampleLevel(gSamp, srcUv, 0).rgb;
    float lum = dot(col, float3(0.2126f, 0.7152f, 0.0722f));
    col /= (1.0f + max(lum - 0.80f, 0.0f) * 0.45f);
    float lAfter = dot(col, float3(0.2126f, 0.7152f, 0.0722f));
    col = max(lerp(float3(lAfter, lAfter, lAfter), col, 1.15f), 0.0f);
    return float4(col, 1.0f);
}

float4 PS_SsgiDownsampleMip(VsOut input) : SV_Target0
{
    float2 uv = input.pos.xy * gSsgiDims.zw;
    float2 t = gSsgiParams1.w * 0.5f * gSsgiDims.zw;
    float4 c0 = gColorPyr.SampleLevel(gSamp, uv + float2(-t.x, -t.y), 0);
    float4 c1 = gColorPyr.SampleLevel(gSamp, uv + float2( t.x, -t.y), 0);
    float4 c2 = gColorPyr.SampleLevel(gSamp, uv + float2(-t.x,  t.y), 0);
    float4 c3 = gColorPyr.SampleLevel(gSamp, uv + float2( t.x,  t.y), 0);
    return (c0 + c1 + c2 + c3) * 0.25f;
}

float4 PS_SsgiTrace(VsOut input) : SV_Target0
{
    float2 halfUv = clamp(input.pos.xy * gSsgiDims.zw, float2(0.0f, 0.0f), float2(1.0f, 1.0f));
    int2 fullPix = clamp(int2(gVpRect.xy + halfUv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(fullPix);
    if (rawC >= 0.99998f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float dC = max(RawToLinearDist(rawC), 1e-3f);
    float fadeDist = saturate((gProjZAndFade.z - dC) * gProjZAndFade.w);
    if (fadeDist <= 1e-3f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float waterMask = ComputeWaterExclusion(fullPix, rawC);
    if (waterMask <= 0.02f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float3 P = PixelToViewPos(fullPix, rawC);
    float3 V = normalize(-P);
    float3 N = ReconstructViewNormal(fullPix, P, dC);

    float radiusWorld = gSsgiParams0.y;
    float screenScale = clamp(gVpRect.w / 1080.0f, 0.65f, 2.0f) * clamp(28.0f / max(dC, 8.0f), 0.55f, 1.55f);
    float baseAngle = SsgiIgnHash(input.pos.xy) * 6.2831853f;

    static const float kSteps[4] = { 12.0f, 26.0f, 48.0f, 82.0f };
    static const int kDirs = 6;

    float3 accumGi = float3(0.0f, 0.0f, 0.0f);
    float weightSum = 0.0f;

    [unroll]
    for (int d = 0; d < kDirs; ++d)
    {
        float ang = baseAngle + (float(d) * 1.04719755f);
        float2 dir2d = float2(cos(ang), sin(ang));
        float maxSinElev = -0.35f;

        [unroll]
        for (int s = 0; s < 4; ++s)
        {
            float2 stepOffPx = dir2d * (kSteps[s] * screenScale);
            float2 sampleVpUv = halfUv + stepOffPx / max(gSsgiDims.xy * 2.0f, float2(1.0f, 1.0f));
            if (sampleVpUv.x < 0.002f || sampleVpUv.x > 0.998f ||
                sampleVpUv.y < 0.002f || sampleVpUv.y > 0.998f)
            {
                continue;
            }

            int2 sPix = clamp(int2(gVpRect.xy + sampleVpUv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
            float sRaw = LoadRawDepth(sPix);
            if (sRaw >= 0.99998f)
                continue;

            float sDist = max(RawToLinearDist(sRaw), 1e-3f);
            float3 Q = PixelToViewPos(sPix, sRaw);
            float3 delta = Q - P;
            float distSq = dot(delta, delta);
            if (distSq < 0.04f)
                continue;

            float distQ = sqrt(distSq);
            if (distQ > radiusWorld * 1.45f)
                continue;

            float3 dirQ = delta / distQ;
            float sinElev = dot(dirQ, V);
            float ndotDir = dot(N, dirQ);

            float zJumpFade = saturate(1.0f - max((dC - sDist) - 2.6f, 0.0f) * 0.45f);
            float distFade = 1.0f - smoothstep(radiusWorld * 0.35f, radiusWorld * 1.40f, distQ);

            if (ndotDir > 0.05f && sinElev > maxSinElev && zJumpFade > 0.01f && distFade > 0.01f)
            {
                float horizonGain = saturate(sinElev - max(maxSinElev, -0.25f));
                float mipLevel = clamp(0.35f + float(s) * 0.80f, 0.0f, 3.0f);
                float3 emitRgb = gColorPyr.SampleLevel(gSamp, sampleVpUv, mipLevel).rgb;
                float emitLuma = dot(emitRgb, float3(0.2126f, 0.7152f, 0.0722f));
                float emitBoost = smoothstep(0.06f, 0.48f, emitLuma) * 0.85f + 0.15f;
                float w = ndotDir * (0.25f + horizonGain * 1.25f) * distFade * zJumpFade;
                accumGi += emitRgb * (w * emitBoost);
                weightSum += w;
                maxSinElev = sinElev;
            }
            else if (sinElev > maxSinElev)
            {
                maxSinElev = sinElev;
            }
        }
    }

    float3 outGi = (weightSum > 1e-4f) ? (accumGi / max(weightSum, 0.65f)) : float3(0.0f, 0.0f, 0.0f);
    outGi *= fadeDist * waterMask * gSsgiParams1.y;
    return float4(max(outGi, 0.0f), fadeDist * waterMask);
}

float4 RunBilateralBlur(int2 halfPix, float2 dirStep)
{
    float2 centerUv = (float2(halfPix) + 0.5f) * gSsgiDims.zw;
    int2 fullCenter = clamp(int2(gVpRect.xy + centerUv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(fullCenter);
    float4 c0 = gSsgiAux.Load(int3(clamp(halfPix, int2(0, 0), int2(gSsgiDims.xy) - 1), 0));
    if (rawC >= 0.99998f)
        return c0;

    float dC = max(RawToLinearDist(rawC), 1e-3f);
    float edgeScale = gSsgiParams0.z;
    static const float kWeights[4] = { 0.28f, 0.22f, 0.10f, 0.04f };

    float4 sum = c0 * kWeights[0];
    float wSum = kWeights[0];
    int2 maxHalf = max(int2(gSsgiDims.xy) - 1, int2(0, 0));

    [unroll]
    for (int i = -3; i <= 3; ++i)
    {
        if (i == 0)
            continue;
        int ai = abs(i);
        float kW = kWeights[ai];
        int2 sp = clamp(halfPix + int2(dirStep * float(i)), int2(0, 0), maxHalf);
        float2 suv = (float2(sp) + 0.5f) * gSsgiDims.zw;
        int2 fpix = clamp(int2(gVpRect.xy + suv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
        float dS = max(RawToLinearDist(LoadRawDepth(fpix)), 1e-3f);
        float depthW = exp(-abs(dS - dC) * edgeScale);
        float w = kW * depthW;
        sum += gSsgiAux.Load(int3(sp, 0)) * w;
        wSum += w;
    }

    return sum / max(wSum, 1e-4f);
}

float4 PS_SsgiBlurH(VsOut input) : SV_Target0
{
    return RunBilateralBlur(int2(input.pos.xy), float2(1.0f, 0.0f));
}

float4 PS_SsgiBlurV(VsOut input) : SV_Target0
{
    return RunBilateralBlur(int2(input.pos.xy), float2(0.0f, 1.0f));
}

float4 PS_SsgiComposite(VsOut input) : SV_Target0
{
    int2 pix = clamp(int2(input.pos.xy), gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(pix);
    if (rawC >= 0.99998f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float waterMask = ComputeWaterExclusion(pix, rawC);
    if (waterMask <= 0.02f)
        return float4(0.0f, 0.0f, 0.0f, 0.0f);

    float dC = max(RawToLinearDist(rawC), 1e-3f);
    float2 vpUv = (float2(pix) + 0.5f - gVpRect.xy) / max(gVpRect.zw, float2(1.0f, 1.0f));
    float2 halfF = vpUv * gSsgiDims.xy - 0.5f;
    int2 h0 = int2(floor(halfF));
    float2 f = saturate(halfF - float2(h0));
    int2 maxHalf = max(int2(gSsgiDims.xy) - 1, int2(0, 0));

    float edgeScale = gSsgiParams0.z * 1.15f;
    float3 giSum = float3(0.0f, 0.0f, 0.0f);
    float wSum = 0.0f;

    [unroll]
    for (int oy = 0; oy <= 1; ++oy)
    {
        [unroll]
        for (int ox = 0; ox <= 1; ++ox)
        {
            int2 hp = clamp(h0 + int2(ox, oy), int2(0, 0), maxHalf);
            float2 suv = (float2(hp) + 0.5f) * gSsgiDims.zw;
            int2 fpix = clamp(int2(gVpRect.xy + suv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
            float dS = max(RawToLinearDist(LoadRawDepth(fpix)), 1e-3f);
            float bW = (ox == 0 ? (1.0f - f.x) : f.x) * (oy == 0 ? (1.0f - f.y) : f.y);
            float dW = exp(-abs(dS - dC) * edgeScale);
            float w = max(bW * dW, 1e-4f);
            giSum += gSsgiAux.Load(int3(hp, 0)).rgb * w;
            wSum += w;
        }
    }

    float3 gi = giSum / max(wSum, 1e-4f);
    float3 sceneCol = gSceneTex.Load(int3(pix, 0)).rgb;
    float scenePeak = max(sceneCol.r, max(sceneCol.g, sceneCol.b));
    float3 albedoProxy = clamp(sceneCol / max(scenePeak, 0.14f), float3(0.16f, 0.16f, 0.16f), float3(1.0f, 1.0f, 1.0f));
    float3 surfaceReflect = lerp(float3(0.55f, 0.55f, 0.55f), albedoProxy, 0.72f);

    float sceneLuma = dot(sceneCol, float3(0.2126f, 0.7152f, 0.0722f));
    float headroom = saturate(1.10f - sceneLuma * 0.85f);
    float gain = gSsgiParams0.x * gSsgiParams0.w;
    float3 addRgb = gi * surfaceReflect * (gain * waterMask * headroom);
    return float4(min(addRgb, float3(0.45f, 0.45f, 0.45f)), 0.0f);
}
)HLSL";
constexpr size_t kSsgiShaderSourceLength = sizeof(kSsgiShaderSource) - 1;

struct SsgiState {
    ID3D11Device* device = nullptr;
    ID3D11PixelShader* ps_downsample_color = nullptr;
    ID3D11PixelShader* ps_downsample_mip = nullptr;
    ID3D11PixelShader* ps_trace[2]{};
    ID3D11PixelShader* ps_blur_h[2]{};
    ID3D11PixelShader* ps_blur_v[2]{};
    ID3D11PixelShader* ps_composite[2]{};
    ID3D11Buffer* cb_ssgi = nullptr;
    ID3D11BlendState* blend_add_rgb = nullptr;

    ID3D11Texture2D* color_pyr_tex = nullptr;
    ID3D11ShaderResourceView* color_pyr_srv = nullptr;
    ID3D11ShaderResourceView* color_pyr_mip_srv[kSsgiMipCount]{};
    ID3D11RenderTargetView* color_pyr_rtv[kSsgiMipCount]{};

    ID3D11Texture2D* raw_tex = nullptr;
    ID3D11RenderTargetView* raw_rtv = nullptr;
    ID3D11ShaderResourceView* raw_srv = nullptr;

    ID3D11Texture2D* blur_tex = nullptr;
    ID3D11RenderTargetView* blur_rtv = nullptr;
    ID3D11ShaderResourceView* blur_srv = nullptr;

    uint32_t half_w = 0;
    uint32_t half_h = 0;
    bool msaa_ready = false;
    bool init_failed = false;
};

SsgiState g_ssgi{};
std::atomic<uint64_t> g_ssgi_executed{0};

void ReleaseSsgiTargetsInternal() {
    for (int i = 0; i < kSsgiMipCount; ++i) {
        SafeRelease(g_ssgi.color_pyr_rtv[i]);
        SafeRelease(g_ssgi.color_pyr_mip_srv[i]);
    }
    SafeRelease(g_ssgi.color_pyr_srv);
    SafeRelease(g_ssgi.color_pyr_tex);
    SafeRelease(g_ssgi.raw_srv);
    SafeRelease(g_ssgi.raw_rtv);
    SafeRelease(g_ssgi.raw_tex);
    SafeRelease(g_ssgi.blur_srv);
    SafeRelease(g_ssgi.blur_rtv);
    SafeRelease(g_ssgi.blur_tex);
    g_ssgi.half_w = 0;
    g_ssgi.half_h = 0;
}

bool EnsureSsgiPipelineObjects(ID3D11Device* dev, PostLogFn logger) {
    if (!dev) {
        return false;
    }
    if (g_ssgi.device && g_ssgi.device != dev) {
        ReleaseSsgiAllObjects();
    }
    if (g_ssgi.init_failed) {
        return false;
    }
    if (g_ssgi.ps_downsample_color && g_ssgi.ps_downsample_mip &&
        g_ssgi.ps_trace[0] && g_ssgi.ps_blur_h[0] && g_ssgi.ps_blur_v[0] &&
        g_ssgi.ps_composite[0] && g_ssgi.cb_ssgi && g_ssgi.blend_add_rgb) {
        return true;
    }

    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        g_ssgi.init_failed = true;
        return false;
    }

    if (!g_ssgi.device) {
        g_ssgi.device = dev;
        g_ssgi.device->AddRef();
    }

    const D3D_SHADER_MACRO macro_single[] = {{"MSAA_DEPTH", "0"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO macro_msaa[] = {{"MSAA_DEPTH", "1"}, {nullptr, nullptr}};

    bool ok = CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                  "PS_SsgiDownsampleColor", "ps_4_0", macro_single, &g_ssgi.ps_downsample_color, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                  "PS_SsgiDownsampleMip", "ps_4_0", macro_single, &g_ssgi.ps_downsample_mip, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                  "PS_SsgiTrace", "ps_4_0", macro_single, &g_ssgi.ps_trace[0], logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                  "PS_SsgiBlurH", "ps_4_0", macro_single, &g_ssgi.ps_blur_h[0], logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                  "PS_SsgiBlurV", "ps_4_0", macro_single, &g_ssgi.ps_blur_v[0], logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                  "PS_SsgiComposite", "ps_4_0", macro_single, &g_ssgi.ps_composite[0], logger);

    if (ok && dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1) {
        const bool msaa_ok =
            CompileAndCreatePixelShaderFromSource(
                dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                "PS_SsgiTrace", "ps_4_1", macro_msaa, &g_ssgi.ps_trace[1], logger) &&
            CompileAndCreatePixelShaderFromSource(
                dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                "PS_SsgiBlurH", "ps_4_1", macro_msaa, &g_ssgi.ps_blur_h[1], logger) &&
            CompileAndCreatePixelShaderFromSource(
                dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                "PS_SsgiBlurV", "ps_4_1", macro_msaa, &g_ssgi.ps_blur_v[1], logger) &&
            CompileAndCreatePixelShaderFromSource(
                dev, compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
                "PS_SsgiComposite", "ps_4_1", macro_msaa, &g_ssgi.ps_composite[1], logger);
        if (!msaa_ok) {
            SafeRelease(g_ssgi.ps_trace[1]);
            SafeRelease(g_ssgi.ps_blur_h[1]);
            SafeRelease(g_ssgi.ps_blur_v[1]);
            SafeRelease(g_ssgi.ps_composite[1]);
        }
        g_ssgi.msaa_ready = msaa_ok;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(CbSsgiParams);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BLEND_DESC bd_add{};
    bd_add.RenderTarget[0].BlendEnable = TRUE;
    bd_add.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd_add.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd_add.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_add.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_add.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_add.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_add.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED |
                                                 D3D11_COLOR_WRITE_ENABLE_GREEN |
                                                 D3D11_COLOR_WRITE_ENABLE_BLUE;

    ok = ok &&
         SUCCEEDED(dev->CreateBuffer(&cb_desc, nullptr, &g_ssgi.cb_ssgi)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_add, &g_ssgi.blend_add_rgb));

    if (!ok) {
        ReleaseSsgiAllObjects();
        g_ssgi.init_failed = true;
        return false;
    }
    return true;
}

bool EnsureSsgiTargets(ID3D11Device* dev, uint32_t rt_w, uint32_t rt_h) {
    const uint32_t hw = (std::max)(1U, rt_w / 2U);
    const uint32_t hh = (std::max)(1U, rt_h / 2U);
    if (g_ssgi.color_pyr_tex && g_ssgi.raw_tex && g_ssgi.blur_tex &&
        g_ssgi.half_w == hw && g_ssgi.half_h == hh) {
        return true;
    }

    ReleaseSsgiTargetsInternal();

    D3D11_TEXTURE2D_DESC pyr_desc{};
    pyr_desc.Width = hw;
    pyr_desc.Height = hh;
    pyr_desc.MipLevels = kSsgiMipCount;
    pyr_desc.ArraySize = 1;
    pyr_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pyr_desc.SampleDesc.Count = 1;
    pyr_desc.Usage = D3D11_USAGE_DEFAULT;
    pyr_desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(dev->CreateTexture2D(&pyr_desc, nullptr, &g_ssgi.color_pyr_tex)) || !g_ssgi.color_pyr_tex) {
        ReleaseSsgiTargetsInternal();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_pyr{};
    srv_pyr.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srv_pyr.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_pyr.Texture2D.MostDetailedMip = 0;
    srv_pyr.Texture2D.MipLevels = kSsgiMipCount;
    if (FAILED(dev->CreateShaderResourceView(g_ssgi.color_pyr_tex, &srv_pyr, &g_ssgi.color_pyr_srv))) {
        ReleaseSsgiTargetsInternal();
        return false;
    }

    for (int m = 0; m < kSsgiMipCount; ++m) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_mip{};
        srv_mip.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv_mip.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_mip.Texture2D.MostDetailedMip = static_cast<UINT>(m);
        srv_mip.Texture2D.MipLevels = 1;

        D3D11_RENDER_TARGET_VIEW_DESC rtv_mip{};
        rtv_mip.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtv_mip.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rtv_mip.Texture2D.MipSlice = static_cast<UINT>(m);
        if (FAILED(dev->CreateShaderResourceView(g_ssgi.color_pyr_tex, &srv_mip, &g_ssgi.color_pyr_mip_srv[m])) ||
            FAILED(dev->CreateRenderTargetView(g_ssgi.color_pyr_tex, &rtv_mip, &g_ssgi.color_pyr_rtv[m]))) {
            ReleaseSsgiTargetsInternal();
            return false;
        }
    }

    auto make_half_rt = [&](ID3D11Texture2D*& out_tex, ID3D11RenderTargetView*& out_rtv, ID3D11ShaderResourceView*& out_srv) -> bool {
        D3D11_TEXTURE2D_DESC td = pyr_desc;
        td.MipLevels = 1;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &out_tex)) || !out_tex) {
            return false;
        }
        if (FAILED(dev->CreateRenderTargetView(out_tex, nullptr, &out_rtv)) ||
            FAILED(dev->CreateShaderResourceView(out_tex, nullptr, &out_srv))) {
            return false;
        }
        return true;
    };

    if (!make_half_rt(g_ssgi.raw_tex, g_ssgi.raw_rtv, g_ssgi.raw_srv) ||
        !make_half_rt(g_ssgi.blur_tex, g_ssgi.blur_rtv, g_ssgi.blur_srv)) {
        ReleaseSsgiTargetsInternal();
        return false;
    }

    g_ssgi.half_w = hw;
    g_ssgi.half_h = hh;
    return true;
}

bool UploadSsgiCb(
    ID3D11DeviceContext* ctx,
    float cur_w,
    float cur_h,
    float rt_w,
    float rt_h,
    float strength,
    bool is_desert,
    float prev_texel_scale) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g_ssgi.cb_ssgi, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        return false;
    }
    CbSsgiParams cb{};
    cb.ssgi_dims[0] = cur_w;
    cb.ssgi_dims[1] = cur_h;
    cb.ssgi_dims[2] = 1.0f / (std::max)(cur_w, 1.0f);
    cb.ssgi_dims[3] = 1.0f / (std::max)(cur_h, 1.0f);
    cb.src_dims[0] = rt_w;
    cb.src_dims[1] = rt_h;
    cb.src_dims[2] = 1.0f / (std::max)(rt_w, 1.0f);
    cb.src_dims[3] = 1.0f / (std::max)(rt_h, 1.0f);
    cb.params0[0] = (std::clamp)(strength, 0.0f, 2.0f);
    cb.params0[1] = 4.6f;     // World-space bounce gather radius (meters)
    cb.params0[2] = 1.45f;    // Bilateral depth edge scale
    cb.params0[3] = 0.85f;    // Bounce albedo gain
    cb.params1[0] = is_desert ? 1.0f : 0.0f;
    cb.params1[1] = is_desert ? 0.72f : 1.0f;
    cb.params1[2] = 0.0f;
    cb.params1[3] = prev_texel_scale;
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    ctx->Unmap(g_ssgi.cb_ssgi, 0);
    return true;
}

}  // namespace

void ReleaseSsgiTargets() {
    ReleaseSsgiTargetsInternal();
}

void ReleaseSsgiAllObjects() {
    ReleaseSsgiTargetsInternal();
    SafeRelease(g_ssgi.blend_add_rgb);
    SafeRelease(g_ssgi.cb_ssgi);
    for (int i = 0; i < 2; ++i) {
        SafeRelease(g_ssgi.ps_trace[i]);
        SafeRelease(g_ssgi.ps_blur_h[i]);
        SafeRelease(g_ssgi.ps_blur_v[i]);
        SafeRelease(g_ssgi.ps_composite[i]);
    }
    SafeRelease(g_ssgi.ps_downsample_mip);
    SafeRelease(g_ssgi.ps_downsample_color);
    SafeRelease(g_ssgi.device);
    g_ssgi.msaa_ready = false;
    g_ssgi.init_failed = false;
}

bool VerifySsgiShadersCompile(PostLogFn logger) {
    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        return false;
    }
    const D3D_SHADER_MACRO macro_single[] = {{"MSAA_DEPTH", "0"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO macro_msaa[] = {{"MSAA_DEPTH", "1"}, {nullptr, nullptr}};
    const char* single_entries[] = {
        "PS_SsgiDownsampleColor",
        "PS_SsgiDownsampleMip",
        "PS_SsgiTrace",
        "PS_SsgiBlurH",
        "PS_SsgiBlurV",
        "PS_SsgiComposite",
    };
    for (const char* entry : single_entries) {
        ID3DBlob* blob = nullptr;
        const bool ok = CompileShaderBlobFromSource(
            compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
            entry, "ps_4_0", macro_single, &blob, logger);
        SafeRelease(blob);
        if (!ok) {
            return false;
        }
    }
    const char* msaa_entries[] = {"PS_SsgiTrace", "PS_SsgiBlurH", "PS_SsgiBlurV", "PS_SsgiComposite"};
    for (const char* entry : msaa_entries) {
        ID3DBlob* blob = nullptr;
        const bool ok = CompileShaderBlobFromSource(
            compile_fn, kSsgiShaderSource, kSsgiShaderSourceLength, "route_b_ssgi.hlsl",
            entry, "ps_4_1", macro_msaa, &blob, logger);
        SafeRelease(blob);
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool ExecuteSsgiPass(const SsgiRequest& req, PostLogFn logger) {
    if (req.is_menu || !req.dev || !req.ctx || !req.target_rtv || !req.scene_srv || !req.depth_srv ||
        req.vp_width <= 0 || req.vp_height <= 0 || req.strength_scale <= 0.001f) {
        return false;
    }
    if (!EnsureSsgiPipelineObjects(req.dev, logger)) {
        return false;
    }
    if (req.use_msaa_depth && !g_ssgi.msaa_ready) {
        return false;
    }
    if (!EnsureSsgiTargets(req.dev, req.rt_w, req.rt_h)) {
        return false;
    }

    const int vi = req.use_msaa_depth ? 1 : 0;
    const float hw = static_cast<float>(g_ssgi.half_w);
    const float hh = static_cast<float>(g_ssgi.half_h);
    const float rtw = static_cast<float>(req.rt_w);
    const float rth = static_cast<float>(req.rt_h);

    ID3D11Buffer* cbs[2] = {g_st.cb_params, g_ssgi.cb_ssgi};
    req.ctx->PSSetConstantBuffers(0, 2, cbs);
    req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);

    ID3D11ShaderResourceView* null_srvs[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};

    // Pass 1: Downsample scene color into mip 0
    if (!UploadSsgiCb(req.ctx, hw, hh, rtw, rth, req.strength_scale, req.is_desert, 1.0f)) {
        return false;
    }
    D3D11_VIEWPORT vp{};
    vp.Width = hw;
    vp.Height = hh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    req.ctx->RSSetViewports(1, &vp);
    req.ctx->PSSetShaderResources(0, 5, null_srvs);
    req.ctx->OMSetRenderTargets(1, &g_ssgi.color_pyr_rtv[0], nullptr);
    req.ctx->PSSetShader(g_ssgi.ps_downsample_color, nullptr, 0);
    ID3D11ShaderResourceView* srvs_c0[1] = {req.scene_srv};
    req.ctx->PSSetShaderResources(0, 1, srvs_c0);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 5, null_srvs);

    // Passes 2..4: Build mip 1..3 pyramid via per-mip SRV (zero subresource overlap)
    uint32_t cur_w = g_ssgi.half_w;
    uint32_t cur_h = g_ssgi.half_h;
    for (int m = 1; m < kSsgiMipCount; ++m) {
        const uint32_t prev_w = cur_w;
        cur_w = (std::max)(1U, cur_w >> 1);
        cur_h = (std::max)(1U, cur_h >> 1);
        const float texel_scale = static_cast<float>(cur_w) / static_cast<float>((std::max)(prev_w, 1U));
        if (!UploadSsgiCb(req.ctx, static_cast<float>(cur_w), static_cast<float>(cur_h), rtw, rth,
                          req.strength_scale, req.is_desert, texel_scale)) {
            req.ctx->OMSetRenderTargets(0, nullptr, nullptr);
            return false;
        }
        vp.Width = static_cast<float>(cur_w);
        vp.Height = static_cast<float>(cur_h);
        req.ctx->RSSetViewports(1, &vp);
        req.ctx->OMSetRenderTargets(1, &g_ssgi.color_pyr_rtv[m], nullptr);
        req.ctx->PSSetShader(g_ssgi.ps_downsample_mip, nullptr, 0);
        ID3D11ShaderResourceView* srvs_m[2] = {nullptr, g_ssgi.color_pyr_mip_srv[m - 1]};
        req.ctx->PSSetShaderResources(0, 2, srvs_m);
        req.ctx->Draw(3, 0);
        req.ctx->PSSetShaderResources(0, 5, null_srvs);
    }

    // Pass 5: Horizon-aware 1-bounce SSGI trace at half resolution -> raw_rtv
    if (!UploadSsgiCb(req.ctx, hw, hh, rtw, rth, req.strength_scale, req.is_desert, 1.0f)) {
        req.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        return false;
    }
    vp.Width = hw;
    vp.Height = hh;
    req.ctx->RSSetViewports(1, &vp);
    req.ctx->OMSetRenderTargets(1, &g_ssgi.raw_rtv, nullptr);
    req.ctx->PSSetShader(g_ssgi.ps_trace[vi], nullptr, 0);
    ID3D11ShaderResourceView* srvs_trace[5] = {
        nullptr, g_ssgi.color_pyr_srv, nullptr, req.depth_srv, req.water_depth_srv};
    req.ctx->PSSetShaderResources(0, 5, srvs_trace);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 5, null_srvs);

    // Pass 6: Separable bilateral H blur (raw_srv -> blur_rtv)
    req.ctx->OMSetRenderTargets(1, &g_ssgi.blur_rtv, nullptr);
    req.ctx->PSSetShader(g_ssgi.ps_blur_h[vi], nullptr, 0);
    ID3D11ShaderResourceView* srvs_bh[5] = {nullptr, nullptr, g_ssgi.raw_srv, req.depth_srv, nullptr};
    req.ctx->PSSetShaderResources(0, 5, srvs_bh);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 5, null_srvs);

    // Pass 7: Separable bilateral V blur (blur_srv -> raw_rtv)
    req.ctx->OMSetRenderTargets(1, &g_ssgi.raw_rtv, nullptr);
    req.ctx->PSSetShader(g_ssgi.ps_blur_v[vi], nullptr, 0);
    ID3D11ShaderResourceView* srvs_bv[5] = {nullptr, nullptr, g_ssgi.blur_srv, req.depth_srv, nullptr};
    req.ctx->PSSetShaderResources(0, 5, srvs_bv);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 5, null_srvs);

    // Pass 8: Full-res joint-bilateral upsample & additive albedo-aware composite -> target_rtv
    D3D11_VIEWPORT full_vp{};
    full_vp.TopLeftX = static_cast<float>(req.vp_left);
    full_vp.TopLeftY = static_cast<float>(req.vp_top);
    full_vp.Width = static_cast<float>(req.vp_width);
    full_vp.Height = static_cast<float>(req.vp_height);
    full_vp.MinDepth = 0.0f;
    full_vp.MaxDepth = 1.0f;
    req.ctx->RSSetViewports(1, &full_vp);
    req.ctx->OMSetRenderTargets(1, &req.target_rtv, nullptr);
    req.ctx->OMSetBlendState(g_ssgi.blend_add_rgb, nullptr, 0xFFFFFFFFU);
    req.ctx->PSSetShader(g_ssgi.ps_composite[vi], nullptr, 0);
    ID3D11ShaderResourceView* srvs_comp[5] = {
        req.scene_srv, nullptr, g_ssgi.raw_srv, req.depth_srv, req.water_depth_srv};
    req.ctx->PSSetShaderResources(0, 5, srvs_comp);
    req.ctx->Draw(3, 0);
    req.ctx->PSSetShaderResources(0, 5, null_srvs);

    ID3D11Buffer* null_cbs[2] = {nullptr, nullptr};
    req.ctx->PSSetConstantBuffers(0, 2, null_cbs);
    g_ssgi_executed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

SsgiStats GetSsgiStats() {
    SsgiStats s{};
    s.executed = g_ssgi_executed.load(std::memory_order_relaxed);
    return s;
}

}  // namespace routeb
