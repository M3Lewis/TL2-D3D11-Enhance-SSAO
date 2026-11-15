#include "route_c_d3d11.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace routeb {

namespace {

constexpr size_t kBenchSlots = 4;

using D3DCompileFn = HRESULT(WINAPI*)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs);

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

void SafeReleaseClassInstances(ID3D11ClassInstance** arr, UINT count) {
    if (!arr) {
        return;
    }
    const UINT n = (std::min)(count, static_cast<UINT>(D3D11_SHADER_MAX_INTERFACES));
    for (UINT i = 0; i < n; ++i) {
        SafeRelease(arr[i]);
    }
}

float ReadEnvFloat(const char* name, float fallback, float min_v, float max_v) {
    char buf[32] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0 || len >= sizeof(buf)) {
        return fallback;
    }
    char* end = nullptr;
    const float val = std::strtof(buf, &end);
    if (end == buf || !std::isfinite(val)) {
        return fallback;
    }
    return (std::max)(min_v, (std::min)(max_v, val));
}

bool EnvFlagIsOne(const char* name) {
    char buf[8] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return len > 0 && buf[0] == '1';
}

bool EnvFlagIsZero(const char* name) {
    char buf[8] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return len > 0 && buf[0] == '0';
}

struct AoTuneConfig {
    bool initialized = false;
    float radius_world = 0.95f;
    float bias_cos = 0.10f;
    float strength = 1.35f;
    float min_ao_floor = 0.55f;
    float depth_edge_scale = 55.0f;
    float fade_start = 42.0f;
    float fade_end_cap = 78.0f;
    int bench_mode = 1;  // 0=off, 1=sample every 32 frames, 2=every frame
};

const AoTuneConfig& GetAoTuneConfig(RouteCD3DLogFn logger) {
    static AoTuneConfig cfg{};
    if (!cfg.initialized) {
        cfg.initialized = true;
        cfg.radius_world = ReadEnvFloat("TL2_C_AO_RADIUS", 0.95f, 0.10f, 8.0f);
        cfg.bias_cos = ReadEnvFloat("TL2_C_AO_BIAS", 0.10f, 0.01f, 0.50f);
        cfg.strength = ReadEnvFloat("TL2_C_AO_STRENGTH", 1.35f, 0.0f, 4.0f);
        cfg.min_ao_floor = ReadEnvFloat("TL2_C_AO_FLOOR", 0.55f, 0.20f, 0.98f);
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

D3DCompileFn ResolveD3DCompile(RouteCD3DLogFn logger) {
    static D3DCompileFn cached = nullptr;
    static bool attempted = false;
    if (attempted) {
        return cached;
    }
    attempted = true;

    const char* dlls[] = {"d3dcompiler_47.dll", "D3DCompiler_43.dll"};
    for (const char* dll_name : dlls) {
        HMODULE mod = ::GetModuleHandleA(dll_name);
        if (!mod) {
            mod = ::LoadLibraryA(dll_name);
        }
        if (mod) {
            auto fn = reinterpret_cast<D3DCompileFn>(::GetProcAddress(mod, "D3DCompile"));
            if (fn) {
                cached = fn;
                if (logger) {
                    logger("[RouteC][D3D11] Resolved D3DCompile from %s.", dll_name);
                }
                return cached;
            }
        }
    }
    if (logger) {
        logger("[RouteC][D3D11] FAILED to resolve D3DCompile from d3dcompiler_47/43.");
    }
    return nullptr;
}

constexpr const char kAoShaderSource[] = R"HLSL(
cbuffer CbAoParams : register(b0)
{
    float4 gProjXY;          // x=p00, y=p11, z=p02, w=p12
    float4 gProjZAndFade;    // x=p22, y=p23, z=fadeEnd, w=invFadeRange
    float4 gVpRect;          // x=vpOriginX, y=vpOriginY, z=vpWidth, w=vpHeight
    int4   gVpClamp;         // x=minX, y=minY, z=maxX, w=maxY
    float4 gAoParams;        // x=radiusWorld, y=biasCos, z=strength, w=minAoFloor
    float4 gBlurAndMode;     // x=invVpW, y=invVpH, z=depthEdgeScale, w=debugMode
    float4 gSscsParams;      // x=sscsEnabled, y=sscsStrength, z=tintStrength, w=aerialMode
    float4 gSscsKeyAndRange; // x=keyDirX, y=keyDirY, z=aerialStart, w=invAerialSpan
    float4 gAerialFogParams; // rgb=hazeRgb, a=maxAerialAlpha
    float4 gViewUpAndWater;  // xyz=worldUpVs, w=waterHint
    float4 gInvViewRow0;     // xyz=worldXBasisVs, w=worldXOffset
    float4 gInvViewRow2;     // xyz=worldZBasisVs, w=worldZOffset
};

#if MSAA_DEPTH
Texture2DMS<float4> gDepthTex : register(t0);
float LoadRawDepth(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gDepthTex.Load(c, 0).r;
}
#else
Texture2D<float4> gDepthTex : register(t0);
float LoadRawDepth(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gDepthTex.Load(int3(c, 0)).r;
}
#endif

Texture2D<float4> gAoInputTex : register(t1);
SamplerState gAoSamp : register(s0);

struct VsOut
{
    float4 pos : SV_Position;
};

VsOut VS_Fullscreen(uint vid : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

float RawToLinearDist(float zNdc)
{
    float denom = min(zNdc + gProjZAndFade.x, -1e-6f);
    return gProjZAndFade.y / denom; // positive view-space distance d = -z_vs
}

float3 ReconstructPosVS(int2 pix, out float rawDepth)
{
    rawDepth = LoadRawDepth(pix);
    float d = RawToLinearDist(rawDepth);
    float2 pixelCenter = float2(pix) + 0.5f;
    float2 uv = (pixelCenter - gVpRect.xy) / gVpRect.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float x = d * (ndc.x + gProjXY.z) / gProjXY.x;
    float y = d * (ndc.y + gProjXY.w) / gProjXY.y;
    return float3(x, y, -d);
}

float3 ReconstructNormalVS(int2 pix, float3 P)
{
    float dL, dR, dU, dD;
    float3 pL = ReconstructPosVS(pix + int2(-1,  0), dL);
    float3 pR = ReconstructPosVS(pix + int2( 1,  0), dR);
    float3 pU = ReconstructPosVS(pix + int2( 0, -1), dU);
    float3 pD = ReconstructPosVS(pix + int2( 0,  1), dD);

    float3 dxL = P - pL;
    float3 dxR = pR - P;
    float3 dPdx = (abs(dxL.z) < abs(dxR.z)) ? dxL : dxR;

    float3 dyU = P - pU;
    float3 dyD = pD - P;
    float3 dPdy = (abs(dyU.z) < abs(dyD.z)) ? dyU : dyD;

    float3 cp = cross(dPdy, dPdx);
    float lenSq = dot(cp, cp);
    float3 n = (lenSq > 1e-12f) ? (cp * rsqrt(lenSq)) : float3(0.0f, 0.0f, 1.0f);
    if (n.z < 0.0f)
    {
        n = -n;
    }
    return n;
}

float4 PS_C1DepthNormalDebug(VsOut input) : SV_Target0
{
    int2 pix = int2(input.pos.xy);
    float rawDepth = 1.0f;
    float3 P = ReconstructPosVS(pix, rawDepth);
    float2 uv = (input.pos.xy - gVpRect.xy) / gVpRect.zw;

    if (abs(uv.x - 0.5f) < gBlurAndMode.x * 1.5f)
    {
        return float4(0.95f, 0.80f, 0.15f, 1.0f);
    }
    if (rawDepth >= 0.99998f)
    {
        return float4(0.06f, 0.16f, 0.30f, 1.0f);
    }

    if (uv.x < 0.5f)
    {
        float d = -P.z;
        float normDist = saturate((d - 1.0f) / 48.0f);
        float phase = frac(d * 0.5f);
        float lineMask = 1.0f - smoothstep(0.0f, 0.09f, min(phase, 1.0f - phase));
        float baseShade = 1.0f - normDist * 0.85f;
        float3 col = lerp(float3(baseShade, baseShade * 0.98f, baseShade * 0.95f),
                          float3(0.08f, 0.85f, 0.95f),
                          lineMask * 0.8f);
        return float4(col, 1.0f);
    }
    else
    {
        float3 n = ReconstructNormalVS(pix, P);
        float3 vis = saturate(n * 0.5f + 0.5f);
        return float4(vis, 1.0f);
    }
}

float4 PS_AoRaw(VsOut input) : SV_Target0
{
    // Pass 1 executes at half resolution; map hpix -> exact even full-res depth pixel.
    int2 hpix = int2(input.pos.xy);
    int2 pix = clamp(hpix * 2, gVpClamp.xy, gVpClamp.zw);
    float rawDepth = 1.0f;
    float3 P = ReconstructPosVS(pix, rawDepth);
    if (rawDepth >= 0.99998f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float d = -P.z;
    float fade = saturate((gProjZAndFade.z - d) * gProjZAndFade.w);
    if (fade <= 0.001f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float3 n = ReconstructNormalVS(pix, P);
    float R = gAoParams.x;
    float pixPerUnit = 0.5f * gVpRect.w * gProjXY.y / max(d, 0.25f);
    float pixRadius = clamp(R * pixPerUnit, 5.0f, 48.0f);

    float ignA = frac(52.9829189f * frac(dot(float2(hpix), float2(0.06711056f, 0.00583715f))));
    float ignS = frac(52.9829189f * frac(dot(float2(hpix) + float2(19.0f, 37.0f), float2(0.06711056f, 0.00583715f))));

    float s0, c0;
    sincos(ignA * 1.5707963f, s0, c0);
    float2 dirs[4] = {
        float2( c0,  s0),
        float2(-s0,  c0),
        float2(-c0, -s0),
        float2( s0, -c0)
    };

    float occSum = 0.0f;
    float biasCos = gAoParams.y;
    float invOneMinusBias = 1.0f / max(1.0f - biasCos, 1e-3f);
    float invR2 = 1.0f / max(R * R, 1e-5f);

    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        float2 dir = dirs[k];
        float maxDirOcc = 0.0f;

        [unroll]
        for (int s = 1; s <= 4; ++s)
        {
            float tFrac = (float(s) - 0.5f + 0.65f * (ignS - 0.5f)) * 0.25f;
            float stepPx = max(tFrac * pixRadius, 2.0f);
            int2 off = int2(round(dir * stepPx));
            if (off.x != 0 || off.y != 0)
            {
                float sRawD = 1.0f;
                float3 S = ReconstructPosVS(pix + off, sRawD);
                if (sRawD < 0.99998f)
                {
                    float3 V = S - P;
                    float distSq = dot(V, V);
                    float invDist = rsqrt(max(distSq, 1e-8f));
                    float cosTheta = dot(n, V) * invDist;
                    float angleOcc = saturate((cosTheta - biasCos) * invOneMinusBias);
                    float falloff = saturate(1.0f - distSq * invR2);
                    maxDirOcc = max(maxDirOcc, angleOcc * falloff);
                }
            }
        }
        occSum += maxDirOcc;
    }

    float occ = occSum * 0.25f;
    float darkened = saturate(occ * gAoParams.z) * (1.0f - gAoParams.w) * fade;
    float ao = 1.0f - darkened;
    return float4(ao, ao, ao, 1.0f);
}

float BilateralBlur1D(int2 hpix, int2 axisDir)
{
    int2 vpHalfMin = gVpClamp.xy >> 1;
    int2 vpHalfMax = gVpClamp.zw >> 1;
    int2 chpix = clamp(hpix, vpHalfMin, vpHalfMax);
    int2 cpix = clamp(chpix * 2, gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(cpix);
    if (rawC >= 0.99998f)
    {
        return 1.0f;
    }

    float dCenter = max(RawToLinearDist(rawC), 1e-3f);
    float sumAo = gAoInputTex.Load(int3(chpix, 0)).r;
    float sumW = 1.0f;
    float edgeScale = gBlurAndMode.z;

    [unroll]
    for (int i = -3; i <= 3; ++i)
    {
        if (i != 0)
        {
            int2 shpix = clamp(hpix + axisDir * i, vpHalfMin, vpHalfMax);
            int2 spix = clamp(shpix * 2, gVpClamp.xy, gVpClamp.zw);
            float rawS = LoadRawDepth(spix);
            if (rawS < 0.99998f)
            {
                float dS = RawToLinearDist(rawS);
                float relDiff = abs(dS - dCenter) / dCenter;
                float ed = relDiff * edgeScale;
                float w = exp(-0.22f * float(i * i) - ed * ed);
                sumAo += gAoInputTex.Load(int3(shpix, 0)).r * w;
                sumW += w;
            }
        }
    }
    return sumAo / max(sumW, 1e-5f);
}

float4 PS_BlurH(VsOut input) : SV_Target0
{
    float v = BilateralBlur1D(int2(input.pos.xy), int2(1, 0));
    return float4(v, v, v, 1.0f);
}

float4 PS_BlurV(VsOut input) : SV_Target0
{
    float v = BilateralBlur1D(int2(input.pos.xy), int2(0, 1));
    return float4(v, v, v, 1.0f);
}

float4 PS_ResolveDepthMsaa(VsOut input) : SV_Target0
{
    return float4(LoadRawDepth(int2(input.pos.xy)), 0.0f, 0.0f, 1.0f);
}

float3 ComputeAoCompositeRgb(int2 pix, float rawC)
{
    if (rawC >= 0.99998f)
    {
        return float3(1.0f, 1.0f, 1.0f);
    }

    float dC = max(RawToLinearDist(rawC), 1e-3f);
    float edgeScale = gBlurAndMode.z;
    int2 vpHalfMin = gVpClamp.xy >> 1;
    int2 vpHalfMax = gVpClamp.zw >> 1;
    int2 h0 = pix >> 1;
    float fx = (pix.x & 1) ? 0.5f : 0.0f;
    float fy = (pix.y & 1) ? 0.5f : 0.0f;
    float wx[2] = {1.0f - fx, fx};
    float wy[2] = {1.0f - fy, fy};

    float sumAo = 0.0f;
    float sumW = 0.0f;

    [unroll]
    for (int dy = 0; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx)
        {
            float wSpat = wx[dx] * wy[dy];
            if (wSpat > 0.0f)
            {
                int2 shpix = clamp(h0 + int2(dx, dy), vpHalfMin, vpHalfMax);
                int2 spix = clamp(shpix * 2, gVpClamp.xy, gVpClamp.zw);
                float rawS = LoadRawDepth(spix);
                float dS = max(RawToLinearDist(rawS), 1e-3f);
                float relDiff = abs(dS - dC) / dC;
                float ed = relDiff * edgeScale;
                float wDepth = (rawS < 0.99998f) ? exp(-(ed * ed)) : 0.0f;
                float w = wSpat * (wDepth + 1e-4f);
                sumAo += gAoInputTex.Load(int3(shpix, 0)).r * w;
                sumW += w;
            }
        }
    }

    float ao = (sumW > 1e-5f) ? (sumAo / sumW) : 1.0f;

    if (gSscsParams.x > 0.5f && gBlurAndMode.w < 0.5f)
    {
        float fade = saturate((gProjZAndFade.z - dC) * gProjZAndFade.w);
        if (fade > 0.02f && dC < 38.0f)
        {
            float3 P = ReconstructPosVS(pix, rawC);
            float3 n = ReconstructNormalVS(pix, P);
            float ign = frac(52.9829189f * frac(dot(float2(pix), float2(0.06711056f, 0.00583715f))));

            // P0-2A: Full-res directional micro contact shadows (synced with DirectionalLight0)
            float2 keyDir = gSscsKeyAndRange.xy;
            float contactOcc = 0.0f;
            [unroll]
            for (int s = 1; s <= 5; ++s)
            {
                float stepPx = (float(s) - 0.35f + 0.50f * ign) * 1.95f;
                int2 spix = pix + int2(round(keyDir * stepPx));
                float sRaw = 1.0f;
                float3 S = ReconstructPosVS(spix, sRaw);
                if (sRaw < 0.99998f)
                {
                    float3 V = S - P;
                    float distSq = dot(V, V);
                    float invDist = rsqrt(max(distSq, 1e-8f));
                    float cosN = dot(n, V) * invDist;
                    float dz = dC - (-S.z);
                    float rangeW = saturate(1.0f - distSq * 3.2f);
                    float thickW = smoothstep(0.012f, 0.045f, dz) * (1.0f - smoothstep(0.22f, 0.45f, dz));
                    float angW = saturate((cosN - 0.12f) * 2.2f);
                    contactOcc = max(contactOcc, angW * rangeW * thickW * (1.10f - 0.14f * float(s)));
                }
            }

            // P0-2B: 2-pixel full-res micro-crevice detail sharpening for stone cracks & reliefs
            float microOcc = 0.0f;
            int2 microOffsets[4] = {
                int2(2, 0),
                int2(-2, 0),
                int2(0, 2),
                int2(0, -2)
            };
            [unroll]
            for (int m = 0; m < 4; ++m)
            {
                float mRaw = 1.0f;
                float3 M = ReconstructPosVS(pix + microOffsets[m], mRaw);
                if (mRaw < 0.99998f)
                {
                    float3 Vm = M - P;
                    float distSq = dot(Vm, Vm);
                    float invDist = rsqrt(max(distSq, 1e-8f));
                    float cosM = dot(n, Vm) * invDist;
                    float rangeM = saturate(1.0f - distSq * 22.0f);
                    microOcc += saturate((cosM - 0.18f) * 2.6f) * rangeM * 0.25f;
                }
            }

            float microAtten = 1.0f - saturate(contactOcc * gSscsParams.y + microOcc * (gSscsParams.y * 0.65f)) * 0.40f * fade;
            float floorMin = max(gAoParams.w * 0.78f, 0.36f);
            ao = max(min(ao, ao * microAtten), floorMin);
        }

        // P0-2C: Hob-style cool-tinted crevice AO curve (avoid neutral muddy gray)
        float3 coolAo = pow(max(ao, 1e-4f), float3(1.18f, 1.04f, 0.85f));
        return lerp(float3(ao, ao, ao), coolAo, saturate(gSscsParams.z));
    }

    return float3(ao, ao, ao);
}

void ComputeAerialPerspective(float rawC, out float3 hazeRgb, out float hazeA)
{
    hazeRgb = float3(0.0f, 0.0f, 0.0f);
    hazeA = 0.0f;
    if (rawC >= 0.99998f || gSscsParams.w < 0.5f || gAerialFogParams.a <= 1e-4f)
    {
        return;
    }

    float dC = max(RawToLinearDist(rawC), 1e-3f);
    float t = saturate((dC - gSscsKeyAndRange.z) * gSscsKeyAndRange.w);
    float smoothT = t * t * (3.0f - 2.0f * t);
    hazeRgb = gAerialFogParams.rgb;
    hazeA = smoothT * gAerialFogParams.a;
}

float4 PS_AoComposite(VsOut input) : SV_Target0
{
    int2 pix = clamp(int2(input.pos.xy), gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(pix);
    return float4(ComputeAoCompositeRgb(pix, rawC), 1.0f);
}

struct AoDualOut
{
    float4 hazePremul : SV_Target0;
    float4 destFactor : SV_Target1;
};

AoDualOut PS_AoCompositeDual(VsOut input)
{
    int2 pix = clamp(int2(input.pos.xy), gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(pix);
    float3 aoRgb = ComputeAoCompositeRgb(pix, rawC);
    float3 hazeRgb;
    float hazeA;
    ComputeAerialPerspective(rawC, hazeRgb, hazeA);
    AoDualOut o;
    o.hazePremul = float4(hazeRgb * hazeA, 0.0f);
    o.destFactor = float4(aoRgb * (1.0f - hazeA), 1.0f);
    return o;
}

float4 PS_AerialPerspective(VsOut input) : SV_Target0
{
    int2 pix = clamp(int2(input.pos.xy), gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(pix);
    float3 hazeRgb;
    float hazeA;
    ComputeAerialPerspective(rawC, hazeRgb, hazeA);
    return float4(hazeRgb, hazeA);
}
)HLSL";

constexpr const char kAoSsrTailSource[] = R"HLSL(
float SsrHash21(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}

float SsrValueNoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = SsrHash21(i);
    float b = SsrHash21(i + float2(1.0f, 0.0f));
    float c = SsrHash21(i + float2(0.0f, 1.0f));
    float d = SsrHash21(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float SsrFbm(float2 p)
{
    return 0.65f * SsrValueNoise(p) + 0.35f * SsrValueNoise(p * 2.13f + float2(13.7f, 7.1f));
}

float2 ProjectViewToVpUv(float3 pVS)
{
    float denom = max(-pVS.z, 0.05f);
    float xNdc = (gProjXY.x * pVS.x) / denom - gProjXY.z;
    float yNdc = (gProjXY.y * pVS.y) / denom - gProjXY.w;
    return float2(0.5f + 0.5f * xNdc, 0.5f - 0.5f * yNdc);
}

float4 PS_WetFloorSsr(VsOut input) : SV_Target0
{
    int2 pix = clamp(int2(input.pos.xy), gVpClamp.xy, gVpClamp.zw);
    float rawC = 0.0f;
    float3 P = ReconstructPosVS(pix, rawC);
    float dC = -P.z;
    if (rawC >= 0.99998f || dC >= gProjZAndFade.z)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float3 N = ReconstructNormalVS(pix, P);
    float3 upVS = normalize(gViewUpAndWater.xyz);
    float horizMask = smoothstep(0.91f, 0.965f, dot(N, upVS));
    if (horizMask <= 0.02f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float dL, dR, dU, dD;
    float3 pL = ReconstructPosVS(pix + int2(-2,  0), dL);
    float3 pR = ReconstructPosVS(pix + int2( 2,  0), dR);
    float3 pU = ReconstructPosVS(pix + int2( 0, -2), dU);
    float3 pD = ReconstructPosVS(pix + int2( 0,  2), dD);
    float curv = length((pL + pR) * 0.5f - P) + length((pU + pD) * 0.5f - P);
    float flatMask = 1.0f - smoothstep(0.028f, 0.135f, curv);
    if (flatMask <= 0.02f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float3 baseCol = gAoInputTex.Load(int3(pix, 0)).rgb;
    float luma = dot(baseCol, float3(0.2126f, 0.7152f, 0.0722f));
    float coolWater = saturate((baseCol.b - baseCol.r + 0.055f) * 3.4f);
    float darkFloor = 1.0f - smoothstep(0.16f, 0.44f, luma);
    float warmDry = smoothstep(0.09f, 0.24f, baseCol.r - baseCol.b) * smoothstep(0.20f, 0.46f, luma);

    float wx = dot(P, gInvViewRow0.xyz) + gInvViewRow0.w;
    float wz = dot(P, gInvViewRow2.xyz) + gInvViewRow2.w;
    float puddleNoise = SsrFbm(float2(wx, wz) * 0.16f);
    float puddlePatch = smoothstep(0.57f, 0.70f, puddleNoise);

    float wetMask = 0.0f;
    float maxOpacity = 0.30f;
    if (gViewUpAndWater.w > 0.5f)
    {
        float waterBody = saturate(coolWater * 0.85f + darkFloor * 0.35f) * (0.55f + 0.45f * puddleNoise);
        float stonePuddle = puddlePatch * darkFloor * 0.55f;
        wetMask = max(waterBody, stonePuddle);
        maxOpacity = (waterBody > stonePuddle) ? 0.52f : 0.28f;
    }
    else
    {
        wetMask = puddlePatch * darkFloor * 0.65f;
        maxOpacity = 0.24f;
    }
    wetMask *= horizMask * flatMask * (1.0f - warmDry * 0.78f);

    float distFade = saturate((gProjZAndFade.z - dC) * gProjZAndFade.w);
    wetMask *= distFade;
    if (wetMask <= 0.025f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    uint texW = 1, texH = 1;
    gAoInputTex.GetDimensions(texW, texH);
    float2 invTex = rcp(float2(max(texW, 1u), max(texH, 1u)));

    float ripScale = 0.016f * (1.0f - smoothstep(10.0f, 36.0f, dC));
    float2 rip = float2(sin(wx * 2.6f + wz * 1.4f), cos(wz * 2.8f - wx * 1.2f)) * ripScale;
    float3 Nrefl = normalize(N + float3(rip.x, 0.0f, rip.y));
    float3 V = normalize(P);
    float3 R = reflect(V, Nrefl);

    float jitter = SsrHash21(float2(pix)) * 0.35f;
    float t = 0.32f + jitter * 0.28f;
    bool hit = false;
    float2 hitTexUv = float2(0.0f, 0.0f);
    float2 hitVpUv = float2(0.5f, 0.5f);
    float prevT = 0.10f;

    [loop]
    for (int i = 0; i < 12; ++i)
    {
        float3 sampleVS = P + R * t;
        if (sampleVS.z >= -0.25f)
        {
            break;
        }
        float2 vpUv = ProjectViewToVpUv(sampleVS);
        if (vpUv.x <= 0.002f || vpUv.x >= 0.998f || vpUv.y <= 0.002f || vpUv.y >= 0.998f)
        {
            break;
        }
        int2 spix = clamp(int2(gVpRect.xy + vpUv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
        float zRaw = LoadRawDepth(spix);
        if (zRaw < 0.99995f)
        {
            float sceneD = RawToLinearDist(zRaw);
            float rayD = -sampleVS.z;
            float diff = rayD - sceneD;
            if (diff > 0.045f && diff < 1.45f)
            {
                float midT = 0.5f * (prevT + t);
                float3 midVS = P + R * midT;
                float2 midVpUv = ProjectViewToVpUv(midVS);
                int2 mpix = clamp(int2(gVpRect.xy + midVpUv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
                float midDiff = (-midVS.z) - RawToLinearDist(LoadRawDepth(mpix));
                float2 bestVpUv = (midDiff > 0.02f && midDiff < 1.45f) ? midVpUv : vpUv;
                hitVpUv = bestVpUv;
                hitTexUv = (gVpRect.xy + bestVpUv * gVpRect.zw) * invTex;
                hit = true;
                break;
            }
        }
        prevT = t;
        t += 0.28f + float(i) * 0.14f;
    }

    if (!hit)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float2 edgeDist = min(hitVpUv, 1.0f - hitVpUv);
    float edgeFade = smoothstep(0.0f, 0.08f, min(edgeDist.x, edgeDist.y));
    float fresnel = 0.18f + 0.82f * pow(1.0f - saturate(dot(-V, Nrefl)), 3.2f);
    float3 reflCol = gAoInputTex.SampleLevel(gAoSamp, hitTexUv, 0).rgb;
    float alpha = saturate(wetMask * fresnel * edgeFade) * maxOpacity;
    return float4(reflCol * float3(0.92f, 0.97f, 1.04f), alpha);
}
)HLSL";

constexpr const char kBloomLutShaderSource[] = R"HLSL(
cbuffer CbBloomLutParams : register(b0)
{
    float4 gDstVpRect;     // xy=dstOriginPx, zw=dstSizePx
    float4 gSrcUvRect;     // xy=srcUvOrigin, zw=srcUvSize
    float4 gTexelAndBias;  // xy=invSrcTexSize, z=threshold, w=knee
    float4 gMixAndLut;     // x=upsampleMixOrBloomStrength, y=lutSize, z=lutInvSize, w=lutMix
};

Texture2D<float4> gTex0 : register(t0);
Texture2D<float4> gTex1 : register(t1);
Texture3D<float4> gLutTex : register(t1);
SamplerState gSamp : register(s0);

struct VS_OUT
{
    float4 pos : SV_Position;
};

float2 ComputeSrcUv(float2 svPos)
{
    float2 normUv = saturate((svPos - gDstVpRect.xy) / max(gDstVpRect.zw, 1.0f));
    return gSrcUvRect.xy + normUv * gSrcUvRect.zw;
}

float2 ClampSrcUv(float2 uv)
{
    float2 pad = gTexelAndBias.xy * 0.5f;
    return clamp(uv, gSrcUvRect.xy + pad, gSrcUvRect.xy + gSrcUvRect.zw - pad);
}

float3 KarisSampleAvg(float3 a, float3 b, float3 c, float3 d)
{
    float wa = rcp(1.0f + max(a.r, max(a.g, a.b)));
    float wb = rcp(1.0f + max(b.r, max(b.g, b.b)));
    float wc = rcp(1.0f + max(c.r, max(c.g, c.b)));
    float wd = rcp(1.0f + max(d.r, max(d.g, d.b)));
    return (a * wa + b * wb + c * wc + d * wd) / max(wa + wb + wc + wd, 1e-4f);
}

float4 PS_BloomPrefilter(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float2 e = gTexelAndBias.xy;

    float3 A = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-2.0f, -2.0f) * e), 0).rgb;
    float3 B = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 0.0f, -2.0f) * e), 0).rgb;
    float3 C = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 2.0f, -2.0f) * e), 0).rgb;
    float3 D = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-1.0f, -1.0f) * e), 0).rgb;
    float3 E = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 1.0f, -1.0f) * e), 0).rgb;
    float3 F = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-2.0f,  0.0f) * e), 0).rgb;
    float3 G = gTex0.SampleLevel(gSamp, ClampSrcUv(uv), 0).rgb;
    float3 H = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 2.0f,  0.0f) * e), 0).rgb;
    float3 I = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-1.0f,  1.0f) * e), 0).rgb;
    float3 J = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 1.0f,  1.0f) * e), 0).rgb;
    float3 K = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-2.0f,  2.0f) * e), 0).rgb;
    float3 L = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 0.0f,  2.0f) * e), 0).rgb;
    float3 M = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 2.0f,  2.0f) * e), 0).rgb;

    float3 col =
        KarisSampleAvg(D, E, I, J) * 0.500f +
        KarisSampleAvg(A, B, G, F) * 0.125f +
        KarisSampleAvg(B, C, H, G) * 0.125f +
        KarisSampleAvg(F, G, L, K) * 0.125f +
        KarisSampleAvg(G, H, M, L) * 0.125f;

    col = pow(clamp(col, 0.0f, 4.0f), 2.2f);
    float peak = max(col.r, max(col.g, col.b));
    float luma = dot(col, float3(0.2126f, 0.7152f, 0.0722f));
    float energy = lerp(luma, peak, 0.50f);

    float thr = gTexelAndBias.z;
    float knee = gTexelAndBias.w;
    float soft = clamp(energy - thr + knee, 0.0f, 2.0f * knee);
    soft = (soft * soft) / max(4.0f * knee, 1e-5f);
    float contrib = max(soft, energy - thr) / max(energy, 1e-4f);
    return float4(min(col * saturate(contrib), 4.0f), 1.0f);
}

float4 PS_BloomDownsample(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float2 e = gTexelAndBias.xy;

    float3 A = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-2.0f, -2.0f) * e), 0).rgb;
    float3 B = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 0.0f, -2.0f) * e), 0).rgb;
    float3 C = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 2.0f, -2.0f) * e), 0).rgb;
    float3 D = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-1.0f, -1.0f) * e), 0).rgb;
    float3 E = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 1.0f, -1.0f) * e), 0).rgb;
    float3 F = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-2.0f,  0.0f) * e), 0).rgb;
    float3 G = gTex0.SampleLevel(gSamp, ClampSrcUv(uv), 0).rgb;
    float3 H = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 2.0f,  0.0f) * e), 0).rgb;
    float3 I = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-1.0f,  1.0f) * e), 0).rgb;
    float3 J = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 1.0f,  1.0f) * e), 0).rgb;
    float3 K = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-2.0f,  2.0f) * e), 0).rgb;
    float3 L = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 0.0f,  2.0f) * e), 0).rgb;
    float3 M = gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 2.0f,  2.0f) * e), 0).rgb;

    float3 outRgb =
        (D + E + I + J) * (0.500f * 0.25f) +
        (A + B + G + F) * (0.125f * 0.25f) +
        (B + C + H + G) * (0.125f * 0.25f) +
        (F + G + L + K) * (0.125f * 0.25f) +
        (G + H + M + L) * (0.125f * 0.25f);
    return float4(outRgb, 1.0f);
}

float4 PS_BloomUpsample(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float2 e = gTexelAndBias.xy * 1.15f;

    float3 s =
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-e.x, -e.y)), 0).rgb * 1.0f +
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 0.0f, -e.y)), 0).rgb * 2.0f +
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( e.x, -e.y)), 0).rgb * 1.0f +
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-e.x,  0.0f)), 0).rgb * 2.0f +
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv), 0).rgb * 4.0f +
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( e.x,  0.0f)), 0).rgb * 2.0f +
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2(-e.x,  e.y)), 0).rgb * 1.0f +
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( 0.0f,  e.y)), 0).rgb * 2.0f +
        gTex0.SampleLevel(gSamp, ClampSrcUv(uv + float2( e.x,  e.y)), 0).rgb * 1.0f;
    s *= (1.0f / 16.0f);

    float3 base = gTex1.SampleLevel(gSamp, ClampSrcUv(uv), 0).rgb;
    return float4(base + s * gMixAndLut.x, 1.0f);
}

float4 PS_BloomComposite(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float3 hdrGlow = max(gTex0.SampleLevel(gSamp, ClampSrcUv(uv), 0).rgb, 0.0f);
    float3 glow = pow(saturate(hdrGlow * gMixAndLut.x), 1.0f / 2.2f);
    return float4(glow, 1.0f);
}

float4 PS_LutGrade3D(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float3 src = saturate(gTex0.SampleLevel(gSamp, ClampSrcUv(uv), 0).rgb);
    float3 lutUv = src * ((gMixAndLut.y - 1.0f) * gMixAndLut.z) + (0.5f * gMixAndLut.z);
    float3 graded = saturate(gLutTex.SampleLevel(gSamp, lutUv, 0).rgb);
    return float4(lerp(src, graded, gMixAndLut.w), 1.0f);
}
)HLSL";

constexpr const char kToastShaderSource[] = R"HLSL(
cbuffer CbToastParams : register(b0) {
    float4 gToastRect;    // x=left, y=top, z=width, w=height
    float4 gToastParams;  // x=dllOn, y=aoOn, z=fadeAlpha, w=scale
    float4 gToastExtra;   // x=filterMode (0=OFF, 1=HOB, 2=DRK), y=apOn (F11: 16xAF + PointLight)
};

struct VS_OUT {
    float4 pos : SV_Position;
};

uint GetToastChar(int idx, bool dllOn, bool aoOn, int fxMode, bool apOn) {
    // Format (45 chars): "[F8]DL:ON  [F9]AO:ON  [F10]FX:HOB [F11]AP:ON "
    if (idx == 0 || idx == 11 || idx == 22 || idx == 34) return 91u;   // '['
    if (idx == 3 || idx == 14 || idx == 26 || idx == 38) return 93u;   // ']'
    if (idx == 1 || idx == 12 || idx == 23 || idx == 27 || idx == 35) return 70u;  // 'F'
    if (idx == 2) return 56u;                                          // '8'
    if (idx == 13) return 57u;                                         // '9'
    if (idx == 24 || idx == 36 || idx == 37) return 49u;               // '1'
    if (idx == 25) return 48u;                                         // '0'
    if (idx == 4) return 68u;                                          // 'D'
    if (idx == 5) return 76u;                                          // 'L'
    if (idx == 6 || idx == 17 || idx == 29 || idx == 41) return 58u;   // ':'
    if (idx == 7 || idx == 16 || idx == 18 || idx == 42) return 79u;   // 'O'
    if (idx == 8) return dllOn ? 78u : 70u;                            // 'N' : 'F'
    if (idx == 9) return dllOn ? 32u : 70u;                            // ' ' : 'F'
    if (idx == 15 || idx == 39) return 65u;                            // 'A'
    if (idx == 19) return aoOn ? 78u : 70u;                            // 'N' : 'F'
    if (idx == 20) return aoOn ? 32u : 70u;                            // ' ' : 'F'
    if (idx == 28) return 88u;                                         // 'X'
    if (idx == 30) {
        if (fxMode == 1) return 72u;                                   // 'H'
        if (fxMode == 2) return 68u;                                   // 'D'
        return 79u;                                                    // 'O'
    }
    if (idx == 31) {
        if (fxMode == 1) return 79u;                                   // 'O'
        if (fxMode == 2) return 82u;                                   // 'R'
        return 70u;                                                    // 'F'
    }
    if (idx == 32) {
        if (fxMode == 1) return 66u;                                   // 'B'
        if (fxMode == 2) return 75u;                                   // 'K'
        return 70u;                                                    // 'F'
    }
    if (idx == 40) return 80u;                                         // 'P'
    if (idx == 43) return apOn ? 78u : 70u;                            // 'N' : 'F'
    if (idx == 44) return apOn ? 32u : 70u;                            // ' ' : 'F'
    return 32u;                                                        // ' '
}

uint GetGlyph5x7Row(uint ch, uint row) {
    if (ch == 91u) { return (row == 0u || row == 6u) ? 14u : 8u; }                         // '['
    if (ch == 93u) { return (row == 0u || row == 6u) ? 14u : 2u; }                         // ']'
    if (ch == 58u) { return (row == 1u || row == 2u || row == 4u || row == 5u) ? 4u : 0u; }// ':'
    if (ch == 48u) {                                                                       // '0'
        if (row == 0u || row == 6u) return 14u;
        if (row == 2u || row == 3u) return 21u;
        return 17u;
    }
    if (ch == 49u) {                                                                       // '1'
        if (row == 1u) return 12u;
        if (row == 6u) return 14u;
        return 4u;
    }
    if (ch == 70u) { return (row == 0u) ? 31u : ((row == 3u) ? 30u : 16u); }               // 'F'
    if (ch == 56u) { return (row == 0u || row == 3u || row == 6u) ? 14u : 17u; }           // '8'
    if (ch == 57u) {                                                                       // '9'
        if (row == 0u) return 14u;
        if (row == 1u || row == 2u) return 17u;
        if (row == 3u) return 15u;
        if (row == 4u) return 1u;
        if (row == 5u) return 2u;
        return 12u;
    }
    if (ch == 68u) { return (row == 0u || row == 6u) ? 30u : 17u; }                        // 'D'
    if (ch == 76u) { return (row == 6u) ? 31u : 16u; }                                     // 'L'
    if (ch == 79u) { return (row == 0u || row == 6u) ? 14u : 17u; }                        // 'O'
    if (ch == 78u) {                                                                       // 'N'
        if (row == 1u) return 25u;
        if (row == 2u) return 21u;
        if (row == 3u) return 19u;
        return 17u;
    }
    if (ch == 65u) {                                                                       // 'A'
        if (row == 0u) return 14u;
        if (row == 3u) return 31u;
        return 17u;
    }
    if (ch == 88u) {                                                                       // 'X'
        if (row == 2u || row == 4u) return 10u;
        if (row == 3u) return 4u;
        return 17u;
    }
    if (ch == 72u) { return (row == 3u) ? 31u : 17u; }                                     // 'H'
    if (ch == 66u) { return (row == 0u || row == 3u || row == 6u) ? 30u : 17u; }           // 'B'
    if (ch == 82u) {                                                                       // 'R'
        if (row == 0u || row == 3u) return 30u;
        if (row == 4u) return 20u;
        if (row == 5u) return 18u;
        return 17u;
    }
    if (ch == 75u) {                                                                       // 'K'
        if (row == 1u || row == 5u) return 18u;
        if (row == 2u || row == 4u) return 20u;
        if (row == 3u) return 24u;
        return 17u;
    }
    if (ch == 80u) {                                                                       // 'P'
        if (row == 0u || row == 3u) return 30u;
        if (row <= 2u) return 17u;
        return 16u;
    }
    return 0u;
}

float4 PS_ToastOverlay(VS_OUT input) : SV_Target0 {
    float2 local = input.pos.xy - gToastRect.xy;
    float border = (local.x < 1.5f || local.y < 1.5f ||
                    local.x > gToastRect.z - 1.5f || local.y > gToastRect.w - 1.5f) ? 1.0f : 0.0f;
    float scale = max(gToastParams.w, 1.0f);
    float2 textPos = local - float2(12.0f, 8.0f);
    int2 cell = int2(floor(textPos / scale));
    int fxMode = (int)round(gToastExtra.x);
    bool apOn = (gToastExtra.y > 0.5f);

    bool hit = false;
    int charIdx = -1;
    if (cell.y >= 0 && cell.y < 7 && cell.x >= 0) {
        charIdx = cell.x / 6;
        int gx = cell.x - charIdx * 6;
        if (charIdx >= 0 && charIdx < 45 && gx >= 0 && gx < 5) {
            uint ch = GetToastChar(charIdx, gToastParams.x > 0.5f, gToastParams.y > 0.5f, fxMode, apOn);
            uint bits = GetGlyph5x7Row(ch, (uint)cell.y);
            hit = ((bits >> (4u - (uint)gx)) & 1u) != 0u;
        }
    }

    float fade = saturate(gToastParams.z);
    if (hit) {
        bool dllOn = (gToastParams.x > 0.5f);
        bool aoOn = (gToastParams.y > 0.5f);
        float3 fg = float3(0.95f, 0.96f, 0.98f);
        if (charIdx >= 7 && charIdx <= 9) {
            fg = dllOn ? float3(0.38f, 0.96f, 0.58f) : float3(1.00f, 0.44f, 0.42f);
        } else if (charIdx >= 18 && charIdx <= 20) {
            fg = aoOn ? float3(0.38f, 0.96f, 0.58f) : float3(1.00f, 0.44f, 0.42f);
        } else if (charIdx >= 30 && charIdx <= 32) {
            if (fxMode == 1) {
                fg = float3(1.00f, 0.84f, 0.36f);
            } else if (fxMode == 2) {
                fg = float3(0.72f, 0.68f, 1.00f);
            } else {
                fg = float3(1.00f, 0.44f, 0.42f);
            }
        } else if (charIdx >= 42 && charIdx <= 44) {
            fg = apOn ? float3(0.38f, 0.96f, 0.58f) : float3(1.00f, 0.44f, 0.42f);
        }
        return float4(fg, fade);
    }

    float3 bg = lerp(float3(0.06f, 0.08f, 0.12f), float3(0.45f, 0.52f, 0.62f), border);
    float bgA = lerp(0.84f, 0.92f, border) * fade;
    return float4(bg, bgA);
}
)HLSL";

constexpr const char kP2FilterShaderSource[] = R"HLSL(
Texture2D<float4> gSceneTex : register(t0);

cbuffer CbP2FilterParams : register(b0) {
    float4 p2FilterParams;  // x=preset (0=OFF, 1=HOB, 2=DRK), y=enable, z=vignetteEnable
    float4 p2VpRect;        // x=vpLeft, y=vpTop, z=vpWidth, w=vpHeight
};

struct VS_OUT {
    float4 pos : SV_Position;
};

float3 HableFilmicCurve(float3 x) {
    static const float A = 0.24f;
    static const float B = 0.34f;
    static const float C = 0.10f;
    static const float D = 0.22f;
    static const float E = 0.018f;
    static const float F = 0.32f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - (E / F);
}

float3 ApplyP2HobFilter(float3 rgb, float mode) {
    if (mode < 0.5f) {
        return rgb;
    }
    float3 c = max(rgb, 0.0f);
    float3 lin = pow(c, 2.2f);
    float exposure = (mode < 1.5f) ? 1.45f : 1.15f;
    float whiteScale = rcp(max(HableFilmicCurve(float3(1.60f, 1.60f, 1.60f)).x, 1e-4f));
    float3 mapped = saturate(HableFilmicCurve(lin * exposure) * whiteScale);
    float3 graded = pow(mapped, 1.0f / 2.2f);

    float luma = dot(graded, float3(0.2126f, 0.7152f, 0.0722f));
    float shadowW = pow(saturate(1.0f - luma * 1.45f), 1.75f);
    float hiW = smoothstep(0.38f, 0.96f, luma);

    if (mode < 1.5f) {
        float3 tint = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.90f, 0.98f, 1.12f), shadowW * 0.65f);
        tint *= lerp(float3(1.0f, 1.0f, 1.0f), float3(1.05f, 1.01f, 0.93f), hiW * 0.55f);
        graded *= tint;

        float maxC = max(graded.r, max(graded.g, graded.b));
        float minC = min(graded.r, min(graded.g, graded.b));
        float sat = maxC - minC;
        float vib = (1.0f - saturate(sat * 1.35f)) * 0.20f;
        float gLuma = dot(graded, float3(0.2126f, 0.7152f, 0.0722f));
        graded = lerp(float3(gLuma, gLuma, gLuma), graded, 1.0f + vib);
    } else {
        float3 tint = lerp(float3(1.0f, 1.0f, 1.0f), float3(0.84f, 0.93f, 1.14f), shadowW * 0.75f);
        tint *= lerp(float3(1.0f, 1.0f, 1.0f), float3(1.07f, 0.98f, 0.87f), hiW * 0.60f);
        graded *= tint;

        float maxC = max(graded.r, max(graded.g, graded.b));
        float minC = min(graded.r, min(graded.g, graded.b));
        float sat = maxC - minC;
        float vib = (1.0f - saturate(sat * 1.30f)) * 0.14f;
        float gLuma = dot(graded, float3(0.2126f, 0.7152f, 0.0722f));
        graded = lerp(float3(gLuma, gLuma, gLuma), graded, 1.0f + vib);
    }
    return saturate(graded);
}

float ComputeP2OpticalVignette(float2 uv) {
    float2 d = (uv - 0.5f) * float2(1.0f, 0.90f);
    float r2 = dot(d, d);
    return 1.0f - 0.16f * smoothstep(0.14f, 0.50f, r2);
}

float4 PS_P2FilterFallback(VS_OUT input) : SV_Target0 {
    int2 px = int2(input.pos.xy);
    float4 src = gSceneTex.Load(int3(px, 0));
    float3 outRgb = ApplyP2HobFilter(src.rgb, p2FilterParams.x);
    if (p2FilterParams.z > 0.5f) {
        float2 uv = saturate((input.pos.xy - p2VpRect.xy) / max(p2VpRect.zw, 1.0f));
        outRgb *= ComputeP2OpticalVignette(uv);
    }
    return float4(outRgb, 1.0f);
}
)HLSL";

constexpr const char kPanelShaderSource[] = R"HLSL(
Texture2D<float4> gPanelTex : register(t0);
SamplerState gPanelSamp : register(s0);

cbuffer CbPanelParams : register(b0) {
    float4 gPanelRect;    // x=boxX, y=boxY, z=boxW, w=boxH
    float4 gPanelExtra;   // x=cursorLocalX, y=cursorLocalY, z=panelAlpha, w=0
};

struct VS_OUT {
    float4 pos : SV_Position;
};

float SdRoundedBox(float2 p, float2 halfSize, float radius) {
    float2 q = abs(p) - halfSize + radius;
    return min(max(q.x, q.y), 0.0f) + length(max(q, 0.0f)) - radius;
}

float4 PS_PanelOverlay(VS_OUT input) : SV_Target0 {
    float2 local = input.pos.xy - gPanelRect.xy;
    float2 size = max(gPanelRect.zw, 1.0f);
    float2 halfSize = size * 0.5f;
    float sd = SdRoundedBox(local - halfSize, halfSize - 1.0f, 10.0f);
    if (sd > 1.2f) {
        discard;
    }

    float2 uv = saturate(local / size);
    float3 texRgb = gPanelTex.SampleLevel(gPanelSamp, uv, 0.0f).rgb;
    float edgeMask = 1.0f - smoothstep(-0.6f, 0.9f, sd);
    float rimGlow = saturate(1.0f - abs(sd + 0.5f) * 1.1f);
    float3 col = lerp(texRgb, float3(0.42f, 0.58f, 0.78f), rimGlow * 0.65f);
    float alpha = saturate(gPanelExtra.z) * 0.95f * edgeMask;
    return float4(col, alpha);
}

float DistPointToSegment(float2 p, float2 a, float2 b) {
    float2 pa = p - a;
    float2 ba = b - a;
    float h = saturate(dot(pa, ba) / max(dot(ba, ba), 1e-5f));
    return length(pa - ba * h);
}

float Sign2D(float2 p1, float2 p2, float2 p3) {
    return (p1.x - p3.x) * (p2.y - p3.y) - (p2.x - p3.x) * (p1.y - p3.y);
}

float SdTriangle(float2 p, float2 a, float2 b, float2 c) {
    float2 e0 = b - a, e1 = c - b, e2 = a - c;
    float2 v0 = p - a, v1 = p - b, v2 = p - c;
    float2 pq0 = v0 - e0 * saturate(dot(v0, e0) / max(dot(e0, e0), 1e-5f));
    float2 pq1 = v1 - e1 * saturate(dot(v1, e1) / max(dot(e1, e1), 1e-5f));
    float2 pq2 = v2 - e2 * saturate(dot(v2, e2) / max(dot(e2, e2), 1e-5f));
    float s = sign(e0.x * e2.y - e0.y * e2.x);
    float2 d = min(min(
        float2(dot(pq0, pq0), s * (v0.x * e0.y - v0.y * e0.x)),
        float2(dot(pq1, pq1), s * (v1.x * e1.y - v1.y * e1.x))),
        float2(dot(pq2, pq2), s * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

float4 PS_CursorOverlay(VS_OUT input) : SV_Target0 {
    float2 p = input.pos.xy - gPanelExtra.xy;
    // Crisp arrow pointer centered with hot-spot at (0,0)
    float dTri = min(
        SdTriangle(p, float2(0.0f, 0.0f), float2(0.0f, 19.0f), float2(6.0f, 14.0f)),
        SdTriangle(p, float2(0.0f, 0.0f), float2(6.0f, 14.0f), float2(14.0f, 13.0f)));
    float dTail = DistPointToSegment(p, float2(5.5f, 12.5f), float2(9.5f, 21.5f)) - 1.3f;
    float sd = min(dTri, dTail);
    if (sd > 1.8f) {
        discard;
    }
    float outlineMask = 1.0f - smoothstep(0.9f, 1.8f, sd);
    float fillMask = 1.0f - smoothstep(-0.4f, 0.5f, sd);
    float3 col = lerp(float3(0.05f, 0.07f, 0.10f), float3(1.00f, 0.94f, 0.72f), fillMask);
    return float4(col, outlineMask);
}
)HLSL";

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

DepthCopyFormats MapDepthCopyFormats(DXGI_FORMAT tex_fmt, DXGI_FORMAT dsv_fmt) {
    if (tex_fmt == DXGI_FORMAT_R24G8_TYPELESS || dsv_fmt == DXGI_FORMAT_D24_UNORM_S8_UINT) {
        return {true, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
    }
    if (tex_fmt == DXGI_FORMAT_R32_TYPELESS || dsv_fmt == DXGI_FORMAT_D32_FLOAT) {
        return {true, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT};
    }
    if (tex_fmt == DXGI_FORMAT_R16_TYPELESS || dsv_fmt == DXGI_FORMAT_D16_UNORM) {
        return {true, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM};
    }
    if (tex_fmt == DXGI_FORMAT_R32G8X24_TYPELESS || dsv_fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
        return {true, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
    }
    return {};
}

UINT ComputeDsvSubresource(
    const D3D11_DEPTH_STENCIL_VIEW_DESC& dsv_desc,
    UINT mip_levels,
    UINT* out_mip_slice) {
    UINT mip = 0;
    UINT slice = 0;
    switch (dsv_desc.ViewDimension) {
        case D3D11_DSV_DIMENSION_TEXTURE2D:
            mip = dsv_desc.Texture2D.MipSlice;
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
            mip = dsv_desc.Texture2DArray.MipSlice;
            slice = dsv_desc.Texture2DArray.FirstArraySlice;
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMS:
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
            slice = dsv_desc.Texture2DMSArray.FirstArraySlice;
            break;
        default:
            break;
    }
    if (out_mip_slice) {
        *out_mip_slice = mip;
    }
    return D3D11CalcSubresource(mip, slice, mip_levels > 0 ? mip_levels : 1);
}

class D3D11StateGuard {
public:
    explicit D3D11StateGuard(ID3D11DeviceContext* ctx) : ctx_(ctx) {
        if (!ctx_) {
            return;
        }
        ctx_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, &dsv_);
        ctx_->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
        ctx_->OMGetDepthStencilState(&dss_, &stencil_ref_);
        ctx_->RSGetState(&rs_);
        ctx_->RSGetViewports(&num_viewports_, viewports_);
        ctx_->RSGetScissorRects(&num_scissors_, scissors_);
        ctx_->IAGetInputLayout(&input_layout_);
        ctx_->IAGetPrimitiveTopology(&topology_);
        ctx_->VSGetShader(&vs_, vs_ci_, &vs_ci_cnt_);
        ctx_->GSGetShader(&gs_, gs_ci_, &gs_ci_cnt_);
        ctx_->PSGetShader(&ps_, ps_ci_, &ps_ci_cnt_);
        ctx_->PSGetConstantBuffers(0, 1, &ps_cb0_);
        ctx_->PSGetShaderResources(0, 2, ps_srvs_);
        ctx_->PSGetSamplers(0, 1, &ps_samp0_);
    }

    ~D3D11StateGuard() {
        if (ctx_) {
            ctx_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, dsv_);
            ctx_->OMSetBlendState(blend_, blend_factor_, sample_mask_);
            ctx_->OMSetDepthStencilState(dss_, stencil_ref_);
            ctx_->RSSetState(rs_);
            ctx_->RSSetViewports(num_viewports_, viewports_);
            ctx_->RSSetScissorRects(num_scissors_, scissors_);
            ctx_->IASetInputLayout(input_layout_);
            ctx_->IASetPrimitiveTopology(topology_);
            ctx_->VSSetShader(vs_, vs_ci_, vs_ci_cnt_);
            ctx_->GSSetShader(gs_, gs_ci_, gs_ci_cnt_);
            ctx_->PSSetShader(ps_, ps_ci_, ps_ci_cnt_);
            ctx_->PSSetConstantBuffers(0, 1, &ps_cb0_);
            ctx_->PSSetShaderResources(0, 2, ps_srvs_);
            ctx_->PSSetSamplers(0, 1, &ps_samp0_);
        }
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            SafeRelease(rtvs_[i]);
        }
        SafeRelease(dsv_);
        SafeRelease(blend_);
        SafeRelease(dss_);
        SafeRelease(rs_);
        SafeRelease(input_layout_);
        SafeRelease(vs_);
        SafeRelease(gs_);
        SafeRelease(ps_);
        SafeRelease(ps_cb0_);
        SafeRelease(ps_srvs_[0]);
        SafeRelease(ps_srvs_[1]);
        SafeRelease(ps_samp0_);
        SafeReleaseClassInstances(vs_ci_, vs_ci_cnt_);
        SafeReleaseClassInstances(gs_ci_, gs_ci_cnt_);
        SafeReleaseClassInstances(ps_ci_, ps_ci_cnt_);
    }

    D3D11StateGuard(const D3D11StateGuard&) = delete;
    D3D11StateGuard& operator=(const D3D11StateGuard&) = delete;

    ID3D11RenderTargetView* om_rtv0() const { return rtvs_[0]; }
    ID3D11DepthStencilView* om_dsv() const { return dsv_; }

private:
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11RenderTargetView* rtvs_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    FLOAT blend_factor_[4]{1.0f, 1.0f, 1.0f, 1.0f};
    UINT sample_mask_ = 0xFFFFFFFFU;
    ID3D11DepthStencilState* dss_ = nullptr;
    UINT stencil_ref_ = 0;
    ID3D11RasterizerState* rs_ = nullptr;
    UINT num_viewports_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT num_scissors_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11InputLayout* input_layout_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11ClassInstance* vs_ci_[D3D11_SHADER_MAX_INTERFACES]{};
    UINT vs_ci_cnt_ = D3D11_SHADER_MAX_INTERFACES;
    ID3D11GeometryShader* gs_ = nullptr;
    ID3D11ClassInstance* gs_ci_[D3D11_SHADER_MAX_INTERFACES]{};
    UINT gs_ci_cnt_ = D3D11_SHADER_MAX_INTERFACES;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11ClassInstance* ps_ci_[D3D11_SHADER_MAX_INTERFACES]{};
    UINT ps_ci_cnt_ = D3D11_SHADER_MAX_INTERFACES;
    ID3D11Buffer* ps_cb0_ = nullptr;
    ID3D11ShaderResourceView* ps_srvs_[2]{};
    ID3D11SamplerState* ps_samp0_ = nullptr;
};

struct DepthVariantShaders {
    ID3D11PixelShader* ps_c1_debug = nullptr;
    ID3D11PixelShader* ps_ao_raw = nullptr;
    ID3D11PixelShader* ps_blur_h = nullptr;
    ID3D11PixelShader* ps_blur_v = nullptr;
    ID3D11PixelShader* ps_composite = nullptr;
    ID3D11PixelShader* ps_comp_dual = nullptr;
    ID3D11PixelShader* ps_aerial = nullptr;
    ID3D11PixelShader* ps_ssr = nullptr;
};

struct BenchSlot {
    bool in_flight = false;
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* q[6]{};  // 0=start, 1=afterCopy, 2=afterAo, 3=afterBlurH, 4=afterBlurV, 5=afterComp
};

struct RouteCD3DState {
    ID3D11Device* device = nullptr;
    ID3D11VertexShader* vs_fullscreen = nullptr;
    DepthVariantShaders shaders_single{};
    DepthVariantShaders shaders_msaa{};
    bool msaa_shaders_ready = false;

    ID3D11Buffer* cb_params = nullptr;
    ID3D11Buffer* cb_toast = nullptr;
    ID3D11PixelShader* ps_toast = nullptr;
    ID3D11Buffer* cb_p2_filter = nullptr;
    ID3D11PixelShader* ps_p2_filter = nullptr;
    ID3D11Buffer* cb_bloom_lut = nullptr;
    ID3D11PixelShader* ps_bloom_prefilter = nullptr;
    ID3D11PixelShader* ps_bloom_downsample = nullptr;
    ID3D11PixelShader* ps_bloom_upsample = nullptr;
    ID3D11PixelShader* ps_bloom_composite = nullptr;
    ID3D11PixelShader* ps_lut_grade = nullptr;
    ID3D11Buffer* cb_panel = nullptr;
    ID3D11PixelShader* ps_panel = nullptr;
    ID3D11PixelShader* ps_cursor = nullptr;
    ID3D11SamplerState* samp_linear = nullptr;
    ID3D11Texture2D* panel_tex = nullptr;
    ID3D11ShaderResourceView* panel_srv = nullptr;
    uint32_t panel_tex_w = 0;
    uint32_t panel_tex_h = 0;
    ID3D11BlendState* blend_debug_rgb = nullptr;
    ID3D11BlendState* blend_mul_rgb = nullptr;
    ID3D11BlendState* blend_dual_ao = nullptr;
    ID3D11BlendState* blend_toast_alpha = nullptr;
    ID3D11BlendState* blend_screen_rgb = nullptr;
    ID3D11DepthStencilState* dss_disabled = nullptr;
    ID3D11RasterizerState* rs_fullscreen = nullptr;

    ID3D11Texture2D* p2_copy_tex = nullptr;
    ID3D11ShaderResourceView* p2_copy_srv = nullptr;
    uint32_t p2_copy_w = 0;
    uint32_t p2_copy_h = 0;
    DXGI_FORMAT p2_copy_fmt = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture2D* bloom_down_tex[3]{};
    ID3D11RenderTargetView* bloom_down_rtv[3]{};
    ID3D11ShaderResourceView* bloom_down_srv[3]{};
    ID3D11Texture2D* bloom_up_tex[2]{};
    ID3D11RenderTargetView* bloom_up_rtv[2]{};
    ID3D11ShaderResourceView* bloom_up_srv[2]{};
    uint32_t bloom_vp_w = 0;
    uint32_t bloom_vp_h = 0;
    uint32_t bloom_w[3]{};
    uint32_t bloom_h[3]{};

    ID3D11Texture3D* lut_tex[4]{};
    ID3D11ShaderResourceView* lut_srv[4]{};
    uint32_t lut_size[4]{};
    FILETIME custom_lut_mtime{};
    bool custom_lut_exists = false;
    uint32_t custom_lut_probe_seq = 0;
    wchar_t custom_lut_path[MAX_PATH]{};

    ID3D11Texture2D* depth_copy_tex = nullptr;
    ID3D11ShaderResourceView* depth_copy_srv = nullptr;
    ID3D11Texture2D* depth_resolve_tex = nullptr;
    ID3D11RenderTargetView* depth_resolve_rtv = nullptr;
    ID3D11ShaderResourceView* depth_resolve_srv = nullptr;
    uint32_t depth_resolve_w = 0;
    uint32_t depth_resolve_h = 0;
    ID3D11Texture2D* particle_params_tex = nullptr;
    ID3D11ShaderResourceView* particle_params_srv = nullptr;
    ID3D11PixelShader* ps_resolve_depth_msaa = nullptr;
    ParticleParamsTexData last_cb_particle{};
    bool last_cb_particle_valid = false;
    bool particle_depth_valid = false;
    float particle_proj_22 = 0.0f;
    float particle_proj_23 = 0.0f;
    int32_t particle_clamp[4]{};

    ID3D11Texture2D* ao_rt_tex = nullptr;
    ID3D11RenderTargetView* ao_rt_rtv = nullptr;
    ID3D11ShaderResourceView* ao_rt_srv = nullptr;
    ID3D11Texture2D* ao_blur_tex = nullptr;
    ID3D11RenderTargetView* ao_blur_rtv = nullptr;
    ID3D11ShaderResourceView* ao_blur_srv = nullptr;

    uint32_t depth_w = 0;
    uint32_t depth_h = 0;
    DXGI_FORMAT depth_tex_fmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depth_srv_fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t depth_sample_count = 0;
    uint32_t depth_sample_quality = 0;

    BenchSlot bench[kBenchSlots]{};
    uint32_t bench_frame_seq = 0;
    bool bench_queries_failed = false;

    bool init_failed = false;
    bool logged_unsupported_fmt = false;
};

CRITICAL_SECTION g_d3d_lock;
INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK InitLockOnce(PINIT_ONCE, PVOID, PVOID*) {
    ::InitializeCriticalSection(&g_d3d_lock);
    return TRUE;
}
void EnsureLockInit() {
    ::InitOnceExecuteOnce(&g_lock_once, &InitLockOnce, nullptr, nullptr);
}

RouteCD3DState g_st{};

std::atomic<uint64_t> g_inject_ok{0};
std::atomic<uint64_t> g_inject_skip_no_om{0};
std::atomic<uint64_t> g_inject_skip_fmt{0};
std::atomic<uint64_t> g_inject_skip_fail{0};
std::atomic<uint32_t> g_depth_rebuilds{0};
std::atomic<uint32_t> g_last_tex_w{0};
std::atomic<uint32_t> g_last_tex_h{0};
std::atomic<uint32_t> g_last_rtv_fmt{0};
std::atomic<uint32_t> g_last_dsv_tex_fmt{0};
std::atomic<uint32_t> g_last_sample_count{0};
std::atomic<bool> g_shaders_ready{false};
std::atomic<bool> g_custom_cube_lut_loaded{false};

uint64_t g_bench_samples = 0;
double g_sum_copy_ms = 0.0;
double g_sum_ao_ms = 0.0;
double g_sum_blur_h_ms = 0.0;
double g_sum_blur_v_ms = 0.0;
double g_sum_comp_ms = 0.0;
double g_sum_total_ms = 0.0;

void ReleaseDepthVariantShaders(DepthVariantShaders& s) {
    SafeRelease(s.ps_ssr);
    SafeRelease(s.ps_aerial);
    SafeRelease(s.ps_comp_dual);
    SafeRelease(s.ps_composite);
    SafeRelease(s.ps_blur_v);
    SafeRelease(s.ps_blur_h);
    SafeRelease(s.ps_ao_raw);
    SafeRelease(s.ps_c1_debug);
}

void ReleaseBloomTargets() {
    for (int i = 0; i < 3; ++i) {
        SafeRelease(g_st.bloom_down_srv[i]);
        SafeRelease(g_st.bloom_down_rtv[i]);
        SafeRelease(g_st.bloom_down_tex[i]);
        g_st.bloom_w[i] = 0;
        g_st.bloom_h[i] = 0;
    }
    for (int i = 0; i < 2; ++i) {
        SafeRelease(g_st.bloom_up_srv[i]);
        SafeRelease(g_st.bloom_up_rtv[i]);
        SafeRelease(g_st.bloom_up_tex[i]);
    }
    g_st.bloom_vp_w = 0;
    g_st.bloom_vp_h = 0;
}

void ReleaseLutResources() {
    for (int i = 0; i < 4; ++i) {
        SafeRelease(g_st.lut_srv[i]);
        SafeRelease(g_st.lut_tex[i]);
        g_st.lut_size[i] = 0;
    }
    g_st.custom_lut_mtime = {};
    g_st.custom_lut_exists = false;
    g_st.custom_lut_probe_seq = 0;
    g_custom_cube_lut_loaded.store(false, std::memory_order_relaxed);
}

void ReleaseTargets() {
    ReleaseBloomTargets();
    SafeRelease(g_st.panel_srv);
    SafeRelease(g_st.panel_tex);
    g_st.panel_tex_w = 0;
    g_st.panel_tex_h = 0;

    SafeRelease(g_st.p2_copy_srv);
    SafeRelease(g_st.p2_copy_tex);
    g_st.p2_copy_w = 0;
    g_st.p2_copy_h = 0;
    g_st.p2_copy_fmt = DXGI_FORMAT_UNKNOWN;

    SafeRelease(g_st.ao_blur_srv);
    SafeRelease(g_st.ao_blur_rtv);
    SafeRelease(g_st.ao_blur_tex);
    SafeRelease(g_st.ao_rt_srv);
    SafeRelease(g_st.ao_rt_rtv);
    SafeRelease(g_st.ao_rt_tex);
    SafeRelease(g_st.depth_resolve_srv);
    SafeRelease(g_st.depth_resolve_rtv);
    SafeRelease(g_st.depth_resolve_tex);
    g_st.depth_resolve_w = 0;
    g_st.depth_resolve_h = 0;
    SafeRelease(g_st.depth_copy_srv);
    SafeRelease(g_st.depth_copy_tex);
    g_st.depth_w = 0;
    g_st.depth_h = 0;
    g_st.depth_tex_fmt = DXGI_FORMAT_UNKNOWN;
    g_st.depth_srv_fmt = DXGI_FORMAT_UNKNOWN;
    g_st.depth_sample_count = 0;
    g_st.depth_sample_quality = 0;
    g_st.particle_depth_valid = false;
}

void ReleaseBenchQueries() {
    for (BenchSlot& b : g_st.bench) {
        b.in_flight = false;
        SafeRelease(b.disjoint);
        for (ID3D11Query*& q : b.q) {
            SafeRelease(q);
        }
    }
    g_st.bench_queries_failed = false;
}

void ReleaseAllDeviceObjects() {
    ReleaseBenchQueries();
    ReleaseTargets();
    ReleaseLutResources();
    SafeRelease(g_st.rs_fullscreen);
    SafeRelease(g_st.dss_disabled);
    SafeRelease(g_st.blend_screen_rgb);
    SafeRelease(g_st.blend_toast_alpha);
    SafeRelease(g_st.blend_dual_ao);
    SafeRelease(g_st.blend_mul_rgb);
    SafeRelease(g_st.blend_debug_rgb);
    SafeRelease(g_st.samp_linear);
    SafeRelease(g_st.ps_cursor);
    SafeRelease(g_st.ps_panel);
    SafeRelease(g_st.cb_panel);
    SafeRelease(g_st.ps_lut_grade);
    SafeRelease(g_st.ps_bloom_composite);
    SafeRelease(g_st.ps_bloom_upsample);
    SafeRelease(g_st.ps_bloom_downsample);
    SafeRelease(g_st.ps_bloom_prefilter);
    SafeRelease(g_st.cb_bloom_lut);
    SafeRelease(g_st.cb_toast);
    SafeRelease(g_st.ps_toast);
    SafeRelease(g_st.cb_p2_filter);
    SafeRelease(g_st.ps_p2_filter);
    SafeRelease(g_st.particle_params_srv);
    SafeRelease(g_st.particle_params_tex);
    SafeRelease(g_st.ps_resolve_depth_msaa);
    SafeRelease(g_st.cb_params);
    ReleaseDepthVariantShaders(g_st.shaders_msaa);
    ReleaseDepthVariantShaders(g_st.shaders_single);
    SafeRelease(g_st.vs_fullscreen);
    SafeRelease(g_st.device);
    g_st.last_cb_particle_valid = false;
    g_st.msaa_shaders_ready = false;
    g_st.init_failed = false;
    g_shaders_ready.store(false, std::memory_order_relaxed);
}

bool CompileShaderBlobFromSource(
    D3DCompileFn compile_fn,
    const char* source,
    size_t source_bytes,
    const char* source_name,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3DBlob** out_blob,
    RouteCD3DLogFn logger) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile_fn(
        source,
        source_bytes,
        source_name,
        defines,
        nullptr,
        entry,
        target,
        0,
        0,
        out_blob,
        &err);
    if (FAILED(hr) || !out_blob || !*out_blob) {
        if (logger) {
            const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "<no error msg>";
            logger("[RouteC][D3D11] HLSL compile failed (%s %s, hr=0x%08X): %s",
                   entry, target, static_cast<unsigned>(hr), msg);
        }
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    return true;
}

bool CompileShaderBlob(
    D3DCompileFn compile_fn,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3DBlob** out_blob,
    RouteCD3DLogFn logger) {
    static const std::string kCombined = std::string(kAoShaderSource) + kAoSsrTailSource;
    return CompileShaderBlobFromSource(
        compile_fn, kCombined.data(), kCombined.size(), "route_c_ao.hlsl",
        entry, target, defines, out_blob, logger);
}

bool CompileAndCreatePixelShaderFromSource(
    ID3D11Device* dev,
    D3DCompileFn compile_fn,
    const char* source,
    size_t source_bytes,
    const char* source_name,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3D11PixelShader** out_ps,
    RouteCD3DLogFn logger) {
    ID3DBlob* blob = nullptr;
    if (!CompileShaderBlobFromSource(compile_fn, source, source_bytes, source_name, entry, target, defines, &blob, logger)) {
        return false;
    }
    const HRESULT hr = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out_ps);
    SafeRelease(blob);
    return SUCCEEDED(hr) && *out_ps != nullptr;
}

bool CompileAndCreatePixelShader(
    ID3D11Device* dev,
    D3DCompileFn compile_fn,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3D11PixelShader** out_ps,
    RouteCD3DLogFn logger) {
    ID3DBlob* blob = nullptr;
    if (!CompileShaderBlob(compile_fn, entry, target, defines, &blob, logger)) {
        return false;
    }
    const HRESULT hr = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out_ps);
    SafeRelease(blob);
    return SUCCEEDED(hr) && *out_ps != nullptr;
}

bool CreateDepthVariantShaders(
    ID3D11Device* dev,
    D3DCompileFn compile_fn,
    const char* ps_profile,
    const D3D_SHADER_MACRO* defines,
    DepthVariantShaders& out,
    RouteCD3DLogFn logger) {
    const bool ok =
        CompileAndCreatePixelShader(dev, compile_fn, "PS_C1DepthNormalDebug", ps_profile, defines, &out.ps_c1_debug, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AoRaw", ps_profile, defines, &out.ps_ao_raw, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_BlurH", ps_profile, defines, &out.ps_blur_h, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_BlurV", ps_profile, defines, &out.ps_blur_v, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AoComposite", ps_profile, defines, &out.ps_composite, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AoCompositeDual", ps_profile, defines, &out.ps_comp_dual, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AerialPerspective", ps_profile, defines, &out.ps_aerial, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_WetFloorSsr", ps_profile, defines, &out.ps_ssr, logger);
    if (!ok) {
        ReleaseDepthVariantShaders(out);
    }
    return ok;
}

bool EnsurePipelineObjects(ID3D11Device* dev, RouteCD3DLogFn logger) {
    if (g_st.device && g_st.device != dev) {
        if (logger) {
            logger("[RouteC][D3D11] ID3D11Device changed; rebuilding Route C D3D11 objects.");
        }
        ReleaseAllDeviceObjects();
    }
    if (g_st.init_failed) {
        return false;
    }
    if (g_st.vs_fullscreen && g_st.shaders_single.ps_ao_raw && g_st.shaders_single.ps_composite &&
        g_st.shaders_single.ps_comp_dual && g_st.shaders_single.ps_aerial && g_st.shaders_single.ps_ssr &&
        g_st.ps_bloom_prefilter && g_st.ps_bloom_downsample && g_st.ps_bloom_upsample &&
        g_st.ps_bloom_composite && g_st.ps_lut_grade && g_st.cb_bloom_lut && g_st.blend_screen_rgb &&
        g_st.ps_toast && g_st.cb_toast && g_st.blend_toast_alpha &&
        g_st.ps_panel && g_st.ps_cursor && g_st.cb_panel && g_st.samp_linear &&
        g_st.cb_params && g_st.blend_debug_rgb && g_st.blend_mul_rgb && g_st.blend_dual_ao &&
        g_st.dss_disabled && g_st.rs_fullscreen) {
        return true;
    }

    if (!g_st.device) {
        g_st.device = dev;
        g_st.device->AddRef();
    }

    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        g_st.init_failed = true;
        return false;
    }

    const D3D_SHADER_MACRO macro_single[] = {{"MSAA_DEPTH", "0"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO macro_msaa[] = {{"MSAA_DEPTH", "1"}, {nullptr, nullptr}};

    ID3DBlob* vs_blob = nullptr;
    const size_t bloom_src_bytes = sizeof(kBloomLutShaderSource) - 1;
    bool ok = CompileShaderBlob(compile_fn, "VS_Fullscreen", "vs_4_0", macro_single, &vs_blob, logger) &&
              SUCCEEDED(dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_st.vs_fullscreen)) &&
              CreateDepthVariantShaders(dev, compile_fn, "ps_4_0", macro_single, g_st.shaders_single, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kToastShaderSource, sizeof(kToastShaderSource) - 1, "route_c_toast.hlsl",
                  "PS_ToastOverlay", "ps_4_0", macro_single, &g_st.ps_toast, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kP2FilterShaderSource, sizeof(kP2FilterShaderSource) - 1, "route_b_p2_filter.hlsl",
                  "PS_P2FilterFallback", "ps_4_0", macro_single, &g_st.ps_p2_filter, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kPanelShaderSource, sizeof(kPanelShaderSource) - 1, "route_c_panel.hlsl",
                  "PS_PanelOverlay", "ps_4_0", macro_single, &g_st.ps_panel, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kPanelShaderSource, sizeof(kPanelShaderSource) - 1, "route_c_panel.hlsl",
                  "PS_CursorOverlay", "ps_4_0", macro_single, &g_st.ps_cursor, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kBloomLutShaderSource, bloom_src_bytes, "route_c_bloom_lut.hlsl",
                  "PS_BloomPrefilter", "ps_4_0", macro_single, &g_st.ps_bloom_prefilter, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kBloomLutShaderSource, bloom_src_bytes, "route_c_bloom_lut.hlsl",
                  "PS_BloomDownsample", "ps_4_0", macro_single, &g_st.ps_bloom_downsample, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kBloomLutShaderSource, bloom_src_bytes, "route_c_bloom_lut.hlsl",
                  "PS_BloomUpsample", "ps_4_0", macro_single, &g_st.ps_bloom_upsample, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kBloomLutShaderSource, bloom_src_bytes, "route_c_bloom_lut.hlsl",
                  "PS_BloomComposite", "ps_4_0", macro_single, &g_st.ps_bloom_composite, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kBloomLutShaderSource, bloom_src_bytes, "route_c_bloom_lut.hlsl",
                  "PS_LutGrade3D", "ps_4_0", macro_single, &g_st.ps_lut_grade, logger);
    SafeRelease(vs_blob);

    if (ok && dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1) {
        g_st.msaa_shaders_ready = CreateDepthVariantShaders(dev, compile_fn, "ps_4_1", macro_msaa, g_st.shaders_msaa, logger);
        if (g_st.msaa_shaders_ready) {
            if (!CompileAndCreatePixelShader(
                    dev, compile_fn, "PS_ResolveDepthMsaa", "ps_4_1", macro_msaa, &g_st.ps_resolve_depth_msaa, logger)) {
                SafeRelease(g_st.ps_resolve_depth_msaa);
                g_st.msaa_shaders_ready = false;
            }
        }
    }

    if (!ok) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED creating Route C D3D11 shaders.");
        }
        ReleaseAllDeviceObjects();
        g_st.init_failed = true;
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(CbAoParams);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BLEND_DESC bd_dbg{};
    bd_dbg.RenderTarget[0].BlendEnable = FALSE;
    bd_dbg.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_BLEND_DESC bd_mul{};
    bd_mul.RenderTarget[0].BlendEnable = TRUE;
    bd_mul.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bd_mul.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bd_mul.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_mul.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_mul.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_mul.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_mul.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_BLEND_DESC bd_dual{};
    bd_dual.RenderTarget[0].BlendEnable = TRUE;
    bd_dual.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd_dual.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
    bd_dual.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_dual.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_dual.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_dual.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_dual.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_BUFFER_DESC cb_toast_desc{};
    cb_toast_desc.ByteWidth = sizeof(CbToastParams);
    cb_toast_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_toast_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_toast_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BUFFER_DESC cb_p2_desc{};
    cb_p2_desc.ByteWidth = sizeof(CbP2FilterParams);
    cb_p2_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_p2_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_p2_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BUFFER_DESC cb_bloom_desc{};
    cb_bloom_desc.ByteWidth = sizeof(CbBloomLutParams);
    cb_bloom_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_bloom_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_bloom_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BLEND_DESC bd_screen{};
    bd_screen.RenderTarget[0].BlendEnable = TRUE;
    bd_screen.RenderTarget[0].SrcBlend = D3D11_BLEND_INV_DEST_COLOR;
    bd_screen.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    bd_screen.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_screen.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_screen.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_screen.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_screen.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_BLEND_DESC bd_toast{};
    bd_toast.RenderTarget[0].BlendEnable = TRUE;
    bd_toast.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd_toast.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd_toast.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_toast.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_toast.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_toast.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_toast.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_DEPTH_STENCIL_DESC dss_desc{};
    dss_desc.DepthEnable = FALSE;
    dss_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dss_desc.StencilEnable = FALSE;

    D3D11_RASTERIZER_DESC rs_desc{};
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE;
    rs_desc.DepthClipEnable = FALSE;
    rs_desc.ScissorEnable = FALSE;
    rs_desc.MultisampleEnable = FALSE;

    D3D11_BUFFER_DESC cb_panel_desc{};
    cb_panel_desc.ByteWidth = sizeof(CbPanelParams);
    cb_panel_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_panel_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_panel_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_TEXTURE2D_DESC particle_tex_desc{};
    particle_tex_desc.Width = 2;
    particle_tex_desc.Height = 1;
    particle_tex_desc.MipLevels = 1;
    particle_tex_desc.ArraySize = 1;
    particle_tex_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    particle_tex_desc.SampleDesc.Count = 1;
    particle_tex_desc.Usage = D3D11_USAGE_DYNAMIC;
    particle_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    particle_tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SHADER_RESOURCE_VIEW_DESC particle_srv_desc{};
    particle_srv_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    particle_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    particle_srv_desc.Texture2D.MipLevels = 1;

    D3D11_SAMPLER_DESC samp_desc{};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    ok = SUCCEEDED(dev->CreateBuffer(&cb_desc, nullptr, &g_st.cb_params)) &&
         SUCCEEDED(dev->CreateBuffer(&cb_toast_desc, nullptr, &g_st.cb_toast)) &&
         SUCCEEDED(dev->CreateBuffer(&cb_p2_desc, nullptr, &g_st.cb_p2_filter)) &&
         SUCCEEDED(dev->CreateBuffer(&cb_bloom_desc, nullptr, &g_st.cb_bloom_lut)) &&
         SUCCEEDED(dev->CreateBuffer(&cb_panel_desc, nullptr, &g_st.cb_panel)) &&
         SUCCEEDED(dev->CreateTexture2D(&particle_tex_desc, nullptr, &g_st.particle_params_tex)) &&
         SUCCEEDED(dev->CreateShaderResourceView(g_st.particle_params_tex, &particle_srv_desc, &g_st.particle_params_srv)) &&
         SUCCEEDED(dev->CreateSamplerState(&samp_desc, &g_st.samp_linear)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_dbg, &g_st.blend_debug_rgb)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_mul, &g_st.blend_mul_rgb)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_dual, &g_st.blend_dual_ao)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_toast, &g_st.blend_toast_alpha)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_screen, &g_st.blend_screen_rgb)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dss_desc, &g_st.dss_disabled)) &&
         SUCCEEDED(dev->CreateRasterizerState(&rs_desc, &g_st.rs_fullscreen));

    if (!ok) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED creating Route C D3D11 state/CB objects.");
        }
        ReleaseAllDeviceObjects();
        g_st.init_failed = true;
        return false;
    }

    g_shaders_ready.store(true, std::memory_order_relaxed);
    if (logger) {
        logger("[RouteC][D3D11] Compiled C2 AO+Blur+Composite shaders (msaa=%d, WriteMask=RGB).",
               g_st.msaa_shaders_ready ? 1 : 0);
    }
    return true;
}

bool CreateR8AoTarget(
    ID3D11Device* dev,
    uint32_t w,
    uint32_t h,
    ID3D11Texture2D** out_tex,
    ID3D11RenderTargetView** out_rtv,
    ID3D11ShaderResourceView** out_srv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(dev->CreateTexture2D(&td, nullptr, out_tex)) || !*out_tex) {
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(*out_tex, nullptr, out_rtv)) || !*out_rtv) {
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(*out_tex, nullptr, out_srv)) || !*out_srv) {
        return false;
    }
    return true;
}

bool EnsureAoTargets(
    ID3D11Device* dev,
    const D3D11_TEXTURE2D_DESC& src_desc,
    const DepthCopyFormats& fmts,
    RouteCD3DLogFn logger) {
    if (g_st.depth_copy_tex && g_st.depth_copy_srv &&
        g_st.ao_rt_tex && g_st.ao_rt_rtv && g_st.ao_rt_srv &&
        g_st.ao_blur_tex && g_st.ao_blur_rtv && g_st.ao_blur_srv &&
        g_st.depth_w == src_desc.Width &&
        g_st.depth_h == src_desc.Height &&
        g_st.depth_tex_fmt == fmts.tex_format &&
        g_st.depth_srv_fmt == fmts.srv_format &&
        g_st.depth_sample_count == src_desc.SampleDesc.Count &&
        g_st.depth_sample_quality == src_desc.SampleDesc.Quality) {
        return true;
    }

    ReleaseTargets();

    D3D11_TEXTURE2D_DESC copy_desc{};
    copy_desc.Width = src_desc.Width;
    copy_desc.Height = src_desc.Height;
    copy_desc.MipLevels = 1;
    copy_desc.ArraySize = 1;
    copy_desc.Format = fmts.tex_format;
    copy_desc.SampleDesc = src_desc.SampleDesc;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(dev->CreateTexture2D(&copy_desc, nullptr, &g_st.depth_copy_tex)) || !g_st.depth_copy_tex) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED CreateTexture2D for depth copy (%ux%u).", src_desc.Width, src_desc.Height);
        }
        ReleaseTargets();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = fmts.srv_format;
    if (src_desc.SampleDesc.Count > 1) {
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = 1;
    }
    if (FAILED(dev->CreateShaderResourceView(g_st.depth_copy_tex, &srv_desc, &g_st.depth_copy_srv)) || !g_st.depth_copy_srv) {
        ReleaseTargets();
        return false;
    }

    const uint32_t ao_w = (std::max)(1U, (src_desc.Width + 1U) / 2U);
    const uint32_t ao_h = (std::max)(1U, (src_desc.Height + 1U) / 2U);
    if (!CreateR8AoTarget(dev, ao_w, ao_h, &g_st.ao_rt_tex, &g_st.ao_rt_rtv, &g_st.ao_rt_srv) ||
        !CreateR8AoTarget(dev, ao_w, ao_h, &g_st.ao_blur_tex, &g_st.ao_blur_rtv, &g_st.ao_blur_srv)) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED creating R8_UNORM half-res AO targets (%ux%u).", ao_w, ao_h);
        }
        ReleaseTargets();
        return false;
    }

    g_st.depth_w = src_desc.Width;
    g_st.depth_h = src_desc.Height;
    g_st.depth_tex_fmt = fmts.tex_format;
    g_st.depth_srv_fmt = fmts.srv_format;
    g_st.depth_sample_count = src_desc.SampleDesc.Count;
    g_st.depth_sample_quality = src_desc.SampleDesc.Quality;

    const uint32_t rebuilds = g_depth_rebuilds.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logger && rebuilds <= 8) {
        logger("[RouteC][D3D11] Built Route C targets #%u: %ux%u texFmt=%u srvFmt=%u msaa=%ux%u.",
               rebuilds,
               g_st.depth_w,
               g_st.depth_h,
               static_cast<unsigned>(g_st.depth_tex_fmt),
               static_cast<unsigned>(g_st.depth_srv_fmt),
               g_st.depth_sample_count,
               g_st.depth_sample_quality);
    }
    return true;
}

bool EnsureDepthResolveTarget(ID3D11Device* dev, uint32_t width, uint32_t height, RouteCD3DLogFn logger) {
    if (g_st.depth_resolve_tex && g_st.depth_resolve_rtv && g_st.depth_resolve_srv &&
        g_st.depth_resolve_w == width && g_st.depth_resolve_h == height) {
        return true;
    }
    SafeRelease(g_st.depth_resolve_srv);
    SafeRelease(g_st.depth_resolve_rtv);
    SafeRelease(g_st.depth_resolve_tex);
    g_st.depth_resolve_w = 0;
    g_st.depth_resolve_h = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_st.depth_resolve_tex)) || !g_st.depth_resolve_tex) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED creating R32_FLOAT MSAA depth resolve target (%ux%u).", width, height);
        }
        SafeRelease(g_st.depth_resolve_tex);
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(g_st.depth_resolve_tex, nullptr, &g_st.depth_resolve_rtv)) || !g_st.depth_resolve_rtv) {
        SafeRelease(g_st.depth_resolve_rtv);
        SafeRelease(g_st.depth_resolve_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_st.depth_resolve_tex, &srvd, &g_st.depth_resolve_srv)) || !g_st.depth_resolve_srv) {
        SafeRelease(g_st.depth_resolve_srv);
        SafeRelease(g_st.depth_resolve_rtv);
        SafeRelease(g_st.depth_resolve_tex);
        return false;
    }
    g_st.depth_resolve_w = width;
    g_st.depth_resolve_h = height;
    return true;
}

bool EnsureBenchQueries(ID3D11Device* dev) {
    if (g_st.bench_queries_failed) {
        return false;
    }
    if (g_st.bench[0].disjoint) {
        return true;
    }
    D3D11_QUERY_DESC dq{};
    dq.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC tq{};
    tq.Query = D3D11_QUERY_TIMESTAMP;
    for (BenchSlot& b : g_st.bench) {
        if (FAILED(dev->CreateQuery(&dq, &b.disjoint)) || !b.disjoint) {
            ReleaseBenchQueries();
            g_st.bench_queries_failed = true;
            return false;
        }
        for (ID3D11Query*& q : b.q) {
            if (FAILED(dev->CreateQuery(&tq, &q)) || !q) {
                ReleaseBenchQueries();
                g_st.bench_queries_failed = true;
                return false;
            }
        }
    }
    return true;
}

void HarvestBenchSlot(ID3D11DeviceContext* ctx, BenchSlot& b) {
    if (!b.in_flight || !b.disjoint) {
        return;
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (ctx->GetData(b.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        return;
    }
    uint64_t ts[6]{};
    for (size_t i = 0; i < 6; ++i) {
        if (ctx->GetData(b.q[i], &ts[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            return;
        }
    }
    b.in_flight = false;
    if (dj.Disjoint || dj.Frequency == 0) {
        return;
    }
    const double to_ms = 1000.0 / static_cast<double>(dj.Frequency);
    const auto delta_ms = [to_ms](uint64_t a, uint64_t c) -> double {
        return (c >= a) ? static_cast<double>(c - a) * to_ms : 0.0;
    };
    const double copy_ms = delta_ms(ts[0], ts[1]);
    const double ao_ms = delta_ms(ts[1], ts[2]);
    const double bh_ms = delta_ms(ts[2], ts[3]);
    const double bv_ms = delta_ms(ts[3], ts[4]);
    const double comp_ms = delta_ms(ts[4], ts[5]);
    const double total_ms = delta_ms(ts[0], ts[5]);

    ++g_bench_samples;
    g_sum_copy_ms += copy_ms;
    g_sum_ao_ms += ao_ms;
    g_sum_blur_h_ms += bh_ms;
    g_sum_blur_v_ms += bv_ms;
    g_sum_comp_ms += comp_ms;
    g_sum_total_ms += total_ms;
}

BenchSlot* BeginBenchSlot(ID3D11Device* dev, ID3D11DeviceContext* ctx, const AoTuneConfig& cfg) {
    if (cfg.bench_mode <= 0) {
        return nullptr;
    }
    const uint32_t seq = g_st.bench_frame_seq++;
    if (!EnsureBenchQueries(dev)) {
        return nullptr;
    }
    for (BenchSlot& b : g_st.bench) {
        HarvestBenchSlot(ctx, b);
    }
    if (cfg.bench_mode == 1 && (seq & 31U) != 0U) {
        return nullptr;
    }
    for (BenchSlot& b : g_st.bench) {
        if (!b.in_flight) {
            b.in_flight = true;
            ctx->Begin(b.disjoint);
            ctx->End(b.q[0]);
            return &b;
        }
    }
    return nullptr;
}

void RecordTimestamp(ID3D11DeviceContext* ctx, BenchSlot* slot, size_t idx) {
    if (slot && idx < 6) {
        ctx->End(slot->q[idx]);
    }
}

void PopulateDynamicAoAerialParams(const RouteCAoRenderRequest& req, CbAoParams& cb) {
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
    cb.aerial_fog_params[3] = req.aerial_enabled ? max_alpha : 0.0f;

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
        cb.inv_view_row2[2] = 1.0f;
    }
}

bool EnsureP2CopyTarget(ID3D11Device* dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt) {
    if (g_st.p2_copy_tex && g_st.p2_copy_srv &&
        g_st.p2_copy_w == width && g_st.p2_copy_h == height && g_st.p2_copy_fmt == fmt) {
        return true;
    }
    SafeRelease(g_st.p2_copy_srv);
    SafeRelease(g_st.p2_copy_tex);
    g_st.p2_copy_w = 0;
    g_st.p2_copy_h = 0;
    g_st.p2_copy_fmt = DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_st.p2_copy_tex)) || !g_st.p2_copy_tex) {
        SafeRelease(g_st.p2_copy_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = fmt;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_st.p2_copy_tex, &srvd, &g_st.p2_copy_srv)) || !g_st.p2_copy_srv) {
        SafeRelease(g_st.p2_copy_srv);
        SafeRelease(g_st.p2_copy_tex);
        return false;
    }
    g_st.p2_copy_w = width;
    g_st.p2_copy_h = height;
    g_st.p2_copy_fmt = fmt;
    return true;
}

bool CopyRtvTextureToP2Copy(
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* src_tex,
    const D3D11_TEXTURE2D_DESC& rt_desc,
    const D3D11_RENDER_TARGET_VIEW_DESC& rtv_desc) {
    if (!dev || !ctx || !src_tex || rt_desc.Width == 0 || rt_desc.Height == 0 ||
        rtv_desc.Format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    if (!EnsureP2CopyTarget(dev, rt_desc.Width, rt_desc.Height, rtv_desc.Format)) {
        return false;
    }
    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, null_srvs);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (rt_desc.SampleDesc.Count > 1) {
        ctx->ResolveSubresource(g_st.p2_copy_tex, 0, src_tex, 0, rtv_desc.Format);
    } else {
        ctx->CopySubresourceRegion(g_st.p2_copy_tex, 0, 0, 0, 0, src_tex, 0, nullptr);
    }
    return true;
}

bool CreateFp16Target(
    ID3D11Device* dev,
    uint32_t width,
    uint32_t height,
    ID3D11Texture2D** out_tex,
    ID3D11RenderTargetView** out_rtv,
    ID3D11ShaderResourceView** out_srv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, out_tex)) || !*out_tex) {
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(*out_tex, nullptr, out_rtv)) || !*out_rtv) {
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(*out_tex, nullptr, out_srv)) || !*out_srv) {
        return false;
    }
    return true;
}

bool EnsureBloomTargets(ID3D11Device* dev, uint32_t vp_w, uint32_t vp_h, RouteCD3DLogFn logger) {
    if (vp_w == 0 || vp_h == 0) {
        return false;
    }
    if (g_st.bloom_down_srv[0] && g_st.bloom_up_srv[0] &&
        g_st.bloom_vp_w == vp_w && g_st.bloom_vp_h == vp_h) {
        return true;
    }
    ReleaseBloomTargets();
    uint32_t w0 = (std::max)(1U, (vp_w + 1U) / 2U);
    uint32_t h0 = (std::max)(1U, (vp_h + 1U) / 2U);
    uint32_t w1 = (std::max)(1U, (w0 + 1U) / 2U);
    uint32_t h1 = (std::max)(1U, (h0 + 1U) / 2U);
    uint32_t w2 = (std::max)(1U, (w1 + 1U) / 2U);
    uint32_t h2 = (std::max)(1U, (h1 + 1U) / 2U);
    g_st.bloom_w[0] = w0;
    g_st.bloom_h[0] = h0;
    g_st.bloom_w[1] = w1;
    g_st.bloom_h[1] = h1;
    g_st.bloom_w[2] = w2;
    g_st.bloom_h[2] = h2;

    for (int i = 0; i < 3; ++i) {
        if (!CreateFp16Target(dev, g_st.bloom_w[i], g_st.bloom_h[i],
                              &g_st.bloom_down_tex[i], &g_st.bloom_down_rtv[i], &g_st.bloom_down_srv[i])) {
            ReleaseBloomTargets();
            if (logger) {
                logger("[RouteC][Bloom] FAILED creating down mip %d (%ux%u).", i, g_st.bloom_w[i], g_st.bloom_h[i]);
            }
            return false;
        }
    }
    for (int i = 0; i < 2; ++i) {
        if (!CreateFp16Target(dev, g_st.bloom_w[i], g_st.bloom_h[i],
                              &g_st.bloom_up_tex[i], &g_st.bloom_up_rtv[i], &g_st.bloom_up_srv[i])) {
            ReleaseBloomTargets();
            if (logger) {
                logger("[RouteC][Bloom] FAILED creating up mip %d (%ux%u).", i, g_st.bloom_w[i], g_st.bloom_h[i]);
            }
            return false;
        }
    }
    g_st.bloom_vp_w = vp_w;
    g_st.bloom_vp_h = vp_h;
    return true;
}

bool UploadBloomLutCb(
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
    float mix_or_strength,
    float lut_size,
    float lut_mix) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g_st.cb_bloom_lut, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        return false;
    }
    auto* cb = static_cast<CbBloomLutParams*>(mapped.pData);
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
    cb->mix_and_lut[0] = mix_or_strength;
    cb->mix_and_lut[1] = lut_size;
    cb->mix_and_lut[2] = (lut_size > 1.0f) ? (1.0f / lut_size) : 0.03125f;
    cb->mix_and_lut[3] = lut_mix;
    ctx->Unmap(g_st.cb_bloom_lut, 0);
    return true;
}

uint16_t FloatToHalf(float value) {
    const float clamped = (std::max)(0.0f, (std::min)(65504.0f, std::isfinite(value) ? value : 0.0f));
    uint32_t fbits = 0;
    std::memcpy(&fbits, &clamped, sizeof(fbits));
    const uint32_t exp = (fbits >> 23) & 0xFFU;
    const uint32_t mant = fbits & 0x7FFFFFU;
    if (exp == 0) {
        return 0;
    }
    const int half_exp = static_cast<int>(exp) - 127 + 15;
    if (half_exp <= 0) {
        if (half_exp < -10) {
            return 0;
        }
        const uint32_t m = mant | 0x800000U;
        const uint32_t shift = static_cast<uint32_t>(14 - half_exp);
        return static_cast<uint16_t>((m + (1U << (shift - 1U))) >> shift);
    }
    if (half_exp >= 31) {
        return 0x7BFFU;
    }
    const uint32_t rounded_mant = mant + 0x1000U;
    if (rounded_mant >= 0x800000U) {
        return static_cast<uint16_t>((static_cast<uint32_t>(half_exp + 1) << 10) & 0x7FFFU);
    }
    return static_cast<uint16_t>((static_cast<uint32_t>(half_exp) << 10) | (rounded_mant >> 13));
}

float SmoothStep(float e0, float e1, float x) {
    const float t = (std::max)(0.0f, (std::min)(1.0f, (x - e0) / (std::max)(e1 - e0, 1e-5f)));
    return t * t * (3.0f - 2.0f * t);
}

void BuildBuiltinLutData(int preset, uint32_t size, std::vector<float>& out_rgb) {
    const size_t n = static_cast<size_t>(size);
    out_rgb.resize(n * n * n * 3U);
    const float inv_max = 1.0f / static_cast<float>(size - 1U);
    size_t idx = 0;
    for (uint32_t bi = 0; bi < size; ++bi) {
        const float b0 = static_cast<float>(bi) * inv_max;
        for (uint32_t gi = 0; gi < size; ++gi) {
            const float g0 = static_cast<float>(gi) * inv_max;
            for (uint32_t ri = 0; ri < size; ++ri) {
                const float r0 = static_cast<float>(ri) * inv_max;
                float r = r0, g = g0, b = b0;
                if (preset == 1) {
                    // Teal & Orange Cinema: gentle S-curve + teal shadow + golden highlight warmth
                    auto scurve = [](float x) {
                        const float s = x * x * (3.0f - 2.0f * x);
                        return x * 0.72f + s * 0.28f;
                    };
                    r = scurve(r);
                    g = scurve(g);
                    b = scurve(b);
                    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    const float sw = std::pow((std::max)(0.0f, 1.0f - luma * 1.35f), 1.6f);
                    const float hw = SmoothStep(0.32f, 0.92f, luma);
                    r += (-0.042f * sw) + (0.055f * hw);
                    g += (0.012f * sw) + (0.018f * hw);
                    b += (0.062f * sw) - (0.048f * hw);
                    const float nl = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = nl + (r - nl) * 1.12f;
                    g = nl + (g - nl) * 1.12f;
                    b = nl + (b - nl) * 1.12f;
                } else if (preset == 2) {
                    // Cold Nordic Slate: restrained saturation, cool steel shadows, crisp highlights
                    auto scurve = [](float x) {
                        const float s = x * x * (3.0f - 2.0f * x);
                        return x * 0.65f + s * 0.35f;
                    };
                    r = scurve(r);
                    g = scurve(g);
                    b = scurve(b);
                    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    const float sw = std::pow((std::max)(0.0f, 1.0f - luma * 1.15f), 1.5f);
                    const float hw = SmoothStep(0.45f, 0.98f, luma);
                    r *= (1.0f - 0.14f * sw);
                    g *= (1.0f - 0.04f * sw);
                    b *= (1.0f + 0.09f * sw);
                    r += 0.025f * hw;
                    g += 0.032f * hw;
                    b += 0.045f * hw;
                    const float nl = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = nl + (r - nl) * 0.86f;
                    g = nl + (g - nl) * 0.86f;
                    b = nl + (b - nl) * 0.86f;
                }
                out_rgb[idx++] = (std::max)(0.0f, (std::min)(1.0f, r));
                out_rgb[idx++] = (std::max)(0.0f, (std::min)(1.0f, g));
                out_rgb[idx++] = (std::max)(0.0f, (std::min)(1.0f, b));
            }
        }
    }
}

bool ParseCubeLutFile(const wchar_t* path, uint32_t& out_size, std::vector<float>& out_rgb) {
    out_size = 0;
    out_rgb.clear();
    if (!path || !path[0]) {
        return false;
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return false;
    }
    uint32_t size = 0;
    float dmin[3] = {0.0f, 0.0f, 0.0f};
    float dmax[3] = {1.0f, 1.0f, 1.0f};
    std::string line;
    std::vector<float> values;
    while (std::getline(ifs, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        const char* p = line.c_str();
        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (*p == '\0') {
            continue;
        }
        if (std::strncmp(p, "LUT_3D_SIZE", 11) == 0 && std::isspace(static_cast<unsigned char>(p[11]))) {
            const unsigned long sz = std::strtoul(p + 11, nullptr, 10);
            if (sz < 4UL || sz > 65UL) {
                return false;
            }
            size = static_cast<uint32_t>(sz);
            values.reserve(static_cast<size_t>(size) * size * size * 3U);
            continue;
        }
        if (std::strncmp(p, "LUT_1D_SIZE", 11) == 0) {
            return false;
        }
        if (std::strncmp(p, "DOMAIN_MIN", 10) == 0 && std::isspace(static_cast<unsigned char>(p[10]))) {
            char* e1 = nullptr;
            char* e2 = nullptr;
            char* e3 = nullptr;
            const float a = std::strtof(p + 10, &e1);
            const float b = std::strtof(e1, &e2);
            const float c = std::strtof(e2, &e3);
            if (e3 != e2) {
                dmin[0] = a;
                dmin[1] = b;
                dmin[2] = c;
            }
            continue;
        }
        if (std::strncmp(p, "DOMAIN_MAX", 10) == 0 && std::isspace(static_cast<unsigned char>(p[10]))) {
            char* e1 = nullptr;
            char* e2 = nullptr;
            char* e3 = nullptr;
            const float a = std::strtof(p + 10, &e1);
            const float b = std::strtof(e1, &e2);
            const float c = std::strtof(e2, &e3);
            if (e3 != e2) {
                dmax[0] = a;
                dmax[1] = b;
                dmax[2] = c;
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(*p))) {
            continue;
        }
        char* e1 = nullptr;
        char* e2 = nullptr;
        char* e3 = nullptr;
        const float r = std::strtof(p, &e1);
        if (e1 == p) continue;
        const float g = std::strtof(e1, &e2);
        if (e2 == e1) continue;
        const float b = std::strtof(e2, &e3);
        if (e3 == e2 || !std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) continue;
        values.push_back(r);
        values.push_back(g);
        values.push_back(b);
    }
    if (size < 4U) {
        return false;
    }
    const size_t expected = static_cast<size_t>(size) * size * size * 3U;
    if (values.size() != expected) {
        return false;
    }
    float span[3] = {
        (std::fabs(dmax[0] - dmin[0]) > 1e-4f) ? (dmax[0] - dmin[0]) : 1.0f,
        (std::fabs(dmax[1] - dmin[1]) > 1e-4f) ? (dmax[1] - dmin[1]) : 1.0f,
        (std::fabs(dmax[2] - dmin[2]) > 1e-4f) ? (dmax[2] - dmin[2]) : 1.0f,
    };
    for (size_t i = 0; i < expected; i += 3U) {
        for (size_t c = 0; c < 3U; ++c) {
            const float nval = (values[i + c] - dmin[c]) / span[c];
            values[i + c] = (std::max)(0.0f, (std::min)(1.0f, nval));
        }
    }
    out_size = size;
    out_rgb.swap(values);
    return true;
}

bool CreateLutTexture3D(
    ID3D11Device* dev,
    uint32_t size,
    const std::vector<float>& rgb_triplets,
    ID3D11Texture3D** out_tex,
    ID3D11ShaderResourceView** out_srv) {
    if (!dev || size < 4U || rgb_triplets.size() != static_cast<size_t>(size) * size * size * 3U) {
        return false;
    }
    const size_t voxel_count = static_cast<size_t>(size) * size * size;
    std::vector<uint16_t> rgba16(voxel_count * 4U, 0x3C00U);  // alpha = 1.0f in FP16
    for (size_t i = 0; i < voxel_count; ++i) {
        rgba16[i * 4U + 0U] = FloatToHalf(rgb_triplets[i * 3U + 0U]);
        rgba16[i * 4U + 1U] = FloatToHalf(rgb_triplets[i * 3U + 1U]);
        rgba16[i * 4U + 2U] = FloatToHalf(rgb_triplets[i * 3U + 2U]);
    }

    D3D11_TEXTURE3D_DESC td{};
    td.Width = size;
    td.Height = size;
    td.Depth = size;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba16.data();
    init.SysMemPitch = size * 8U;
    init.SysMemSlicePitch = size * size * 8U;

    if (FAILED(dev->CreateTexture3D(&td, &init, out_tex)) || !*out_tex) {
        SafeRelease(*out_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = td.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    srvd.Texture3D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(*out_tex, &srvd, out_srv)) || !*out_srv) {
        SafeRelease(*out_srv);
        SafeRelease(*out_tex);
        return false;
    }
    return true;
}

void EnsureCustomLutPathInitialized() {
    if (g_st.custom_lut_path[0] != L'\0') {
        return;
    }
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = nullptr;
        for (DWORD i = 0; i < n; ++i) {
            if (exe[i] == L'\\' || exe[i] == L'/') slash = &exe[i];
        }
        if (slash) {
            *slash = L'\0';
            swprintf_s(g_st.custom_lut_path, L"%ls\\TL2_CustomLUT.cube", exe);
            return;
        }
    }
    swprintf_s(g_st.custom_lut_path, L"TL2_CustomLUT.cube");
}

bool EnsureLutReady(ID3D11Device* dev, int preset, RouteCD3DLogFn logger) {
    if (preset < 1 || preset > 3) {
        return false;
    }
    if (preset <= 2) {
        if (g_st.lut_srv[preset]) {
            return true;
        }
        constexpr uint32_t kSize = 32U;
        std::vector<float> rgb;
        BuildBuiltinLutData(preset, kSize, rgb);
        if (CreateLutTexture3D(dev, kSize, rgb, &g_st.lut_tex[preset], &g_st.lut_srv[preset])) {
            g_st.lut_size[preset] = kSize;
            return true;
        }
        return false;
    }

    EnsureCustomLutPathInitialized();
    const uint32_t seq = g_st.custom_lut_probe_seq++;
    if (!g_st.lut_srv[3] || (seq % 60U) == 0U) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        const bool exists = ::GetFileAttributesExW(g_st.custom_lut_path, GetFileExInfoStandard, &fad) != 0;
        const bool mtime_changed =
            exists &&
            (!g_st.custom_lut_exists ||
             fad.ftLastWriteTime.dwLowDateTime != g_st.custom_lut_mtime.dwLowDateTime ||
             fad.ftLastWriteTime.dwHighDateTime != g_st.custom_lut_mtime.dwHighDateTime);
        if (mtime_changed) {
            uint32_t sz = 0;
            std::vector<float> rgb;
            if (ParseCubeLutFile(g_st.custom_lut_path, sz, rgb)) {
                SafeRelease(g_st.lut_srv[3]);
                SafeRelease(g_st.lut_tex[3]);
                if (CreateLutTexture3D(dev, sz, rgb, &g_st.lut_tex[3], &g_st.lut_srv[3])) {
                    g_st.lut_size[3] = sz;
                    g_st.custom_lut_mtime = fad.ftLastWriteTime;
                    g_st.custom_lut_exists = true;
                    g_custom_cube_lut_loaded.store(true, std::memory_order_relaxed);
                    if (logger) {
                        logger("[RouteC][LUT] Loaded custom TL2_CustomLUT.cube (size=%ux%ux%u).", sz, sz, sz);
                    }
                    return true;
                }
            } else if (logger && !g_st.custom_lut_exists) {
                logger("[RouteC][LUT] TL2_CustomLUT.cube found but invalid; using neutral fallback.");
            }
        } else if (!exists && g_st.custom_lut_exists) {
            SafeRelease(g_st.lut_srv[3]);
            SafeRelease(g_st.lut_tex[3]);
            g_st.custom_lut_exists = false;
            g_st.custom_lut_mtime = {};
            g_custom_cube_lut_loaded.store(false, std::memory_order_relaxed);
        }
    }
    if (!g_st.lut_srv[3]) {
        constexpr uint32_t kSize = 16U;
        std::vector<float> rgb;
        BuildBuiltinLutData(0, kSize, rgb);
        if (CreateLutTexture3D(dev, kSize, rgb, &g_st.lut_tex[3], &g_st.lut_srv[3])) {
            g_st.lut_size[3] = kSize;
            g_custom_cube_lut_loaded.store(false, std::memory_order_relaxed);
        }
    }
    return g_st.lut_srv[3] != nullptr;
}

}  // namespace

bool ExecuteRouteCAoPass(const RouteCAoRenderRequest& req, RouteCD3DLogFn logger) {
    const bool need_depth = req.ao_enabled || req.capture_particle_depth || req.aerial_enabled || req.ssr_enabled;
    const bool any_work = need_depth || req.bloom_pyramid_enabled || (req.lut_preset > 0);
    if (!req.ctx || !req.proj_4x4 || req.vp_width <= 0 || req.vp_height <= 0 || !any_work) {
        return false;
    }

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    struct LockGuard {
        ~LockGuard() { ::LeaveCriticalSection(&g_d3d_lock); }
    } lock_guard;

    g_st.particle_depth_valid = false;
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
        cb.ao_params[0] = cfg.radius_world;
        cb.ao_params[1] = cfg.bias_cos;
        cb.ao_params[2] = cfg.strength;
        cb.ao_params[3] = cfg.min_ao_floor;
        cb.blur_and_mode[0] = 1.0f / static_cast<float>(req.vp_width);
        cb.blur_and_mode[1] = 1.0f / static_cast<float>(req.vp_height);
        cb.blur_and_mode[2] = cfg.depth_edge_scale;
        cb.blur_and_mode[3] = static_cast<float>(req.debug_mode);
        cb.sscs_params[0] = req.sscs_enabled ? 1.0f : 0.0f;
        cb.sscs_params[1] = 1.15f;
        cb.sscs_params[2] = 0.90f;
        cb.sscs_params[3] = req.aerial_enabled ? ((req.filter_preset == 2) ? 2.0f : 1.0f) : 0.0f;
        PopulateDynamicAoAerialParams(req, cb);
        std::memcpy(mapped.pData, &cb, sizeof(cb));
        req.ctx->Unmap(g_st.cb_params, 0);

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

    // Item 5: Water / Wet-Floor Screen-Space Reflections BEFORE AO + Aerial so crevices & fog shade reflections
    if (need_depth && req.ssr_enabled && req.debug_mode == 0 &&
        ds->ps_ssr && g_st.blend_toast_alpha &&
        CopyRtvTextureToP2Copy(dev, req.ctx, src_rt_tex, rt_desc, rtv_desc)) {
        req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_params);
        req.ctx->RSSetViewports(1, &ao_vp);
        req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
        req.ctx->OMSetBlendState(g_st.blend_toast_alpha, nullptr, 0xFFFFFFFFU);
        req.ctx->PSSetShader(ds->ps_ssr, nullptr, 0);
        ID3D11ShaderResourceView* srvs_ssr[2] = {g_st.depth_copy_srv, g_st.p2_copy_srv};
        req.ctx->PSSetShaderResources(0, 2, srvs_ssr);
        req.ctx->Draw(3, 0);
        req.ctx->PSSetShaderResources(0, 2, null_srvs);
    }

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

            // Pass 4 (Full-Res Joint-Bilateral Upsample): single-pass Dual-Source AO+Aerial or RGB-only AO Composite
            const bool use_dual_composite =
                (req.debug_mode == 0) && req.aerial_enabled && ds->ps_comp_dual && g_st.blend_dual_ao;
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
    } else if (req.aerial_enabled && bench_slot) {
        RecordTimestamp(req.ctx, bench_slot, 2);
        RecordTimestamp(req.ctx, bench_slot, 3);
        RecordTimestamp(req.ctx, bench_slot, 4);
    }

    const bool ran_dual_composite =
        req.ao_enabled && (req.debug_mode == 0) && req.aerial_enabled && ds->ps_comp_dual && g_st.blend_dual_ao;
    if (need_depth && req.aerial_enabled && req.debug_mode == 0 && !ran_dual_composite && ds->ps_aerial && g_st.blend_toast_alpha) {
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

    // Item 6: Multi-Stage HDR FP16 Bloom Pyramid (1/2 -> 1/4 -> 1/8 downsample + tent upsample + soft Screen blend)
    if (req.bloom_pyramid_enabled && req.debug_mode == 0 &&
        g_st.ps_bloom_prefilter && g_st.ps_bloom_downsample &&
        g_st.ps_bloom_upsample && g_st.ps_bloom_composite &&
        g_st.cb_bloom_lut && g_st.blend_screen_rgb &&
        EnsureBloomTargets(dev, static_cast<uint32_t>(req.vp_width), static_cast<uint32_t>(req.vp_height), logger) &&
        CopyRtvTextureToP2Copy(dev, req.ctx, src_rt_tex, rt_desc, rtv_desc)) {
        const float rt_w = static_cast<float>(rt_desc.Width);
        const float rt_h = static_cast<float>(rt_desc.Height);
        const float src_u0 = static_cast<float>(req.vp_left) / rt_w;
        const float src_v0 = static_cast<float>(req.vp_top) / rt_h;
        const float src_uw = static_cast<float>(req.vp_width) / rt_w;
        const float src_vh = static_cast<float>(req.vp_height) / rt_h;
        const bool is_dark = (req.filter_preset == 2);
        const float threshold = is_dark ? 0.56f : 0.48f;
        const float knee = 0.28f;
        const float composite_strength = is_dark ? 0.34f : 0.42f;
        req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_bloom_lut);
        req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);

        auto set_mip_vp = [&](uint32_t w, uint32_t h) {
            D3D11_VIEWPORT vp{};
            vp.Width = static_cast<float>(w);
            vp.Height = static_cast<float>(h);
            vp.MaxDepth = 1.0f;
            req.ctx->RSSetViewports(1, &vp);
        };

        if (UploadBloomLutCb(req.ctx, 0.0f, 0.0f,
                             static_cast<float>(g_st.bloom_w[0]), static_cast<float>(g_st.bloom_h[0]),
                             src_u0, src_v0, src_uw, src_vh,
                             1.0f / rt_w, 1.0f / rt_h, threshold, knee, 0.0f, 32.0f, 0.0f)) {
            set_mip_vp(g_st.bloom_w[0], g_st.bloom_h[0]);
            req.ctx->OMSetRenderTargets(1, &g_st.bloom_down_rtv[0], nullptr);
            req.ctx->PSSetShader(g_st.ps_bloom_prefilter, nullptr, 0);
            ID3D11ShaderResourceView* s0[2] = {g_st.p2_copy_srv, nullptr};
            req.ctx->PSSetShaderResources(0, 2, s0);
            req.ctx->Draw(3, 0);
        }
        for (int m = 0; m < 2; ++m) {
            req.ctx->PSSetShaderResources(0, 2, null_srvs);
            if (UploadBloomLutCb(req.ctx, 0.0f, 0.0f,
                                 static_cast<float>(g_st.bloom_w[m + 1]), static_cast<float>(g_st.bloom_h[m + 1]),
                                 0.0f, 0.0f, 1.0f, 1.0f,
                                 1.0f / static_cast<float>(g_st.bloom_w[m]),
                                 1.0f / static_cast<float>(g_st.bloom_h[m]),
                                 0.0f, 0.0f, 0.0f, 32.0f, 0.0f)) {
                set_mip_vp(g_st.bloom_w[m + 1], g_st.bloom_h[m + 1]);
                req.ctx->OMSetRenderTargets(1, &g_st.bloom_down_rtv[m + 1], nullptr);
                req.ctx->PSSetShader(g_st.ps_bloom_downsample, nullptr, 0);
                ID3D11ShaderResourceView* s[2] = {g_st.bloom_down_srv[m], nullptr};
                req.ctx->PSSetShaderResources(0, 2, s);
                req.ctx->Draw(3, 0);
            }
        }
        for (int m = 1; m >= 0; --m) {
            req.ctx->PSSetShaderResources(0, 2, null_srvs);
            ID3D11ShaderResourceView* coarse_srv = (m == 1) ? g_st.bloom_down_srv[2] : g_st.bloom_up_srv[1];
            if (UploadBloomLutCb(req.ctx, 0.0f, 0.0f,
                                 static_cast<float>(g_st.bloom_w[m]), static_cast<float>(g_st.bloom_h[m]),
                                 0.0f, 0.0f, 1.0f, 1.0f,
                                 1.0f / static_cast<float>(g_st.bloom_w[m + 1]),
                                 1.0f / static_cast<float>(g_st.bloom_h[m + 1]),
                                 0.0f, 0.0f, 0.72f, 32.0f, 0.0f)) {
                set_mip_vp(g_st.bloom_w[m], g_st.bloom_h[m]);
                req.ctx->OMSetRenderTargets(1, &g_st.bloom_up_rtv[m], nullptr);
                req.ctx->PSSetShader(g_st.ps_bloom_upsample, nullptr, 0);
                ID3D11ShaderResourceView* s[2] = {coarse_srv, g_st.bloom_down_srv[m]};
                req.ctx->PSSetShaderResources(0, 2, s);
                req.ctx->Draw(3, 0);
            }
        }
        req.ctx->PSSetShaderResources(0, 2, null_srvs);
        if (UploadBloomLutCb(req.ctx,
                             static_cast<float>(req.vp_left), static_cast<float>(req.vp_top),
                             static_cast<float>(req.vp_width), static_cast<float>(req.vp_height),
                             0.0f, 0.0f, 1.0f, 1.0f,
                             1.0f / static_cast<float>(g_st.bloom_w[0]),
                             1.0f / static_cast<float>(g_st.bloom_h[0]),
                             0.0f, 0.0f, composite_strength, 32.0f, 0.0f)) {
            req.ctx->RSSetViewports(1, &ao_vp);
            req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
            req.ctx->OMSetBlendState(g_st.blend_screen_rgb, nullptr, 0xFFFFFFFFU);
            req.ctx->PSSetShader(g_st.ps_bloom_composite, nullptr, 0);
            ID3D11ShaderResourceView* s[2] = {g_st.bloom_up_srv[0], nullptr};
            req.ctx->PSSetShaderResources(0, 2, s);
            req.ctx->Draw(3, 0);
            req.ctx->PSSetShaderResources(0, 2, null_srvs);
        }
    }

    // Item 7: 3D LUT Color Grading (pre-UI world scene + Bloom)
    if (req.lut_preset > 0 && req.debug_mode == 0 &&
        g_st.ps_lut_grade && g_st.cb_bloom_lut && g_st.blend_debug_rgb &&
        EnsureLutReady(dev, req.lut_preset, logger) &&
        CopyRtvTextureToP2Copy(dev, req.ctx, src_rt_tex, rt_desc, rtv_desc)) {
        const float rt_w = static_cast<float>(rt_desc.Width);
        const float rt_h = static_cast<float>(rt_desc.Height);
        const float lut_sz = static_cast<float>(g_st.lut_size[req.lut_preset]);
        const float lut_mix = (req.lut_preset == 3) ? 1.0f : 0.88f;
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
        }
    }

    if (!req.ao_enabled) {
        return g_st.particle_depth_valid || req.aerial_enabled || req.ssr_enabled ||
               req.bloom_pyramid_enabled || (req.lut_preset > 0);
    }

    g_inject_ok.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ExecuteRouteCP2FilterPass(const RouteCP2FilterRequest& req, RouteCD3DLogFn logger) {
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
    cb->params[2] = req.vignette_enabled ? 1.0f : 0.0f;
    cb->params[3] = 0.0f;
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

bool ExecuteRouteCToastOverlay(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    bool dll_enhance_on,
    bool ao_on,
    int filter_preset,
    bool ap_on,
    float fade_alpha,
    RouteCD3DLogFn logger) {
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

bool EnsurePanelTexture(ID3D11Device* dev, uint32_t width, uint32_t height) {
    if (g_st.panel_tex && g_st.panel_srv && g_st.panel_tex_w == width && g_st.panel_tex_h == height) {
        return true;
    }
    SafeRelease(g_st.panel_srv);
    SafeRelease(g_st.panel_tex);
    g_st.panel_tex_w = 0;
    g_st.panel_tex_h = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_st.panel_tex)) || !g_st.panel_tex) {
        SafeRelease(g_st.panel_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_st.panel_tex, &srvd, &g_st.panel_srv)) || !g_st.panel_srv) {
        SafeRelease(g_st.panel_srv);
        SafeRelease(g_st.panel_tex);
        return false;
    }
    g_st.panel_tex_w = width;
    g_st.panel_tex_h = height;
    return true;
}

bool ExecuteRouteCPanelOverlay(const RouteCPanelOverlayRequest& req, RouteCD3DLogFn logger) {
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

bool VerifyRouteCShadersCompile(RouteCD3DLogFn logger) {
    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        return false;
    }
    const D3D_SHADER_MACRO macro_single[] = {{"MSAA_DEPTH", "0"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO macro_msaa[] = {{"MSAA_DEPTH", "1"}, {nullptr, nullptr}};

    struct Spec {
        const char* entry;
        const char* target;
        const D3D_SHADER_MACRO* macros;
    };
    const Spec specs[] = {
        {"VS_Fullscreen", "vs_4_0", macro_single},
        {"PS_AoComposite", "ps_4_0", macro_single},
        {"PS_AoComposite", "ps_4_1", macro_msaa},
        {"PS_AoCompositeDual", "ps_4_0", macro_single},
        {"PS_AoCompositeDual", "ps_4_1", macro_msaa},
        {"PS_AerialPerspective", "ps_4_0", macro_single},
        {"PS_AerialPerspective", "ps_4_1", macro_msaa},
        {"PS_WetFloorSsr", "ps_4_0", macro_single},
        {"PS_WetFloorSsr", "ps_4_1", macro_msaa},
        {"PS_C1DepthNormalDebug", "ps_4_0", macro_single},
        {"PS_C1DepthNormalDebug", "ps_4_1", macro_msaa},
        {"PS_AoRaw", "ps_4_0", macro_single},
        {"PS_AoRaw", "ps_4_1", macro_msaa},
        {"PS_BlurH", "ps_4_0", macro_single},
        {"PS_BlurH", "ps_4_1", macro_msaa},
        {"PS_BlurV", "ps_4_0", macro_single},
        {"PS_BlurV", "ps_4_1", macro_msaa},
        {"PS_ResolveDepthMsaa", "ps_4_1", macro_msaa},
    };
    for (const Spec& sp : specs) {
        ID3DBlob* blob = nullptr;
        const bool ok = CompileShaderBlob(compile_fn, sp.entry, sp.target, sp.macros, &blob, logger);
        SafeRelease(blob);
        if (!ok) {
            return false;
        }
    }
    ID3DBlob* toast_blob = nullptr;
    const bool toast_ok = CompileShaderBlobFromSource(
        compile_fn, kToastShaderSource, sizeof(kToastShaderSource) - 1, "route_c_toast.hlsl",
        "PS_ToastOverlay", "ps_4_0", macro_single, &toast_blob, logger);
    SafeRelease(toast_blob);

    ID3DBlob* p2_blob = nullptr;
    const bool p2_ok = CompileShaderBlobFromSource(
        compile_fn, kP2FilterShaderSource, sizeof(kP2FilterShaderSource) - 1, "route_b_p2_filter.hlsl",
        "PS_P2FilterFallback", "ps_4_0", macro_single, &p2_blob, logger);
    SafeRelease(p2_blob);

    ID3DBlob* panel_blob = nullptr;
    const bool panel_ok = CompileShaderBlobFromSource(
        compile_fn, kPanelShaderSource, sizeof(kPanelShaderSource) - 1, "route_c_panel.hlsl",
        "PS_PanelOverlay", "ps_4_0", macro_single, &panel_blob, logger);
    SafeRelease(panel_blob);

    ID3DBlob* cursor_blob = nullptr;
    const bool cursor_ok = CompileShaderBlobFromSource(
        compile_fn, kPanelShaderSource, sizeof(kPanelShaderSource) - 1, "route_c_panel.hlsl",
        "PS_CursorOverlay", "ps_4_0", macro_single, &cursor_blob, logger);
    SafeRelease(cursor_blob);

    const char* bloom_lut_entries[] = {
        "PS_BloomPrefilter",
        "PS_BloomDownsample",
        "PS_BloomUpsample",
        "PS_BloomComposite",
        "PS_LutGrade3D",
    };
    bool bloom_lut_ok = true;
    for (const char* entry : bloom_lut_entries) {
        ID3DBlob* b = nullptr;
        const bool ok_one = CompileShaderBlobFromSource(
            compile_fn, kBloomLutShaderSource, sizeof(kBloomLutShaderSource) - 1, "route_c_bloom_lut.hlsl",
            entry, "ps_4_0", macro_single, &b, logger);
        SafeRelease(b);
        bloom_lut_ok = bloom_lut_ok && ok_one;
    }

    return toast_ok && p2_ok && panel_ok && cursor_ok && bloom_lut_ok;
}

bool IsCustomCubeLutLoaded() {
    return g_custom_cube_lut_loaded.load(std::memory_order_relaxed);
}

void ShutdownRouteCD3D11Resources() {
    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    ReleaseAllDeviceObjects();
    ::LeaveCriticalSection(&g_d3d_lock);
}

RouteCD3DStats GetRouteCD3DStats() {
    RouteCD3DStats s{};
    s.inject_ok = g_inject_ok.load(std::memory_order_relaxed);
    s.inject_skip_no_om = g_inject_skip_no_om.load(std::memory_order_relaxed);
    s.inject_skip_fmt = g_inject_skip_fmt.load(std::memory_order_relaxed);
    s.inject_skip_fail = g_inject_skip_fail.load(std::memory_order_relaxed);
    s.depth_rebuilds = g_depth_rebuilds.load(std::memory_order_relaxed);
    s.last_tex_w = g_last_tex_w.load(std::memory_order_relaxed);
    s.last_tex_h = g_last_tex_h.load(std::memory_order_relaxed);
    s.last_rtv_fmt = g_last_rtv_fmt.load(std::memory_order_relaxed);
    s.last_dsv_tex_fmt = g_last_dsv_tex_fmt.load(std::memory_order_relaxed);
    s.last_sample_count = g_last_sample_count.load(std::memory_order_relaxed);
    s.shaders_ready = g_shaders_ready.load(std::memory_order_relaxed);

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    if (g_bench_samples > 0) {
        const double inv_n = 1.0 / static_cast<double>(g_bench_samples);
        s.bench_samples = g_bench_samples;
        s.avg_copy_ms = g_sum_copy_ms * inv_n;
        s.avg_ao_ms = g_sum_ao_ms * inv_n;
        s.avg_blur_h_ms = g_sum_blur_h_ms * inv_n;
        s.avg_blur_v_ms = g_sum_blur_v_ms * inv_n;
        s.avg_comp_ms = g_sum_comp_ms * inv_n;
        s.avg_total_ms = g_sum_total_ms * inv_n;
    }
    ::LeaveCriticalSection(&g_d3d_lock);
    return s;
}

bool GetRouteCParticleDepthSnapshot(RouteCParticleDepthSnapshot* out) {
    if (!out) {
        return false;
    }
    *out = {};
    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
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
    ::LeaveCriticalSection(&g_d3d_lock);
    return valid;
}

bool BindRouteCParticleState(
    ID3D11DeviceContext* ctx,
    int soft_mode,
    float inv_soft_range,
    const RouteCParticleDepthSnapshot* snap) {
    if (!ctx) {
        return false;
    }
    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    struct LockGuard {
        ~LockGuard() { ::LeaveCriticalSection(&g_d3d_lock); }
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

void ResetRouteCParticleDepthSnapshot() {
    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    g_st.particle_depth_valid = false;
    ::LeaveCriticalSection(&g_d3d_lock);
}

}  // namespace routeb
