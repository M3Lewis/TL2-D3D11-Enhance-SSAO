#include "d3d11_post_shaders.h"

namespace routeb {

const char kAoShaderSource[] = R"HLSL(
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
Texture2DMS<float4> gWaterDepthTex : register(t2);
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
Texture2D<float4> gDepthTex : register(t0);
Texture2D<float4> gWaterDepthTex : register(t2);
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
        float sumDirOcc = 0.0f;

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
                    float hPlane = dot(n, V);
                    float cosTheta = hPlane * invDist;
                    float angleOcc = saturate((cosTheta - biasCos) * invOneMinusBias);
                    float heightGate = smoothstep(0.055f, 0.14f, hPlane);
                    float falloff = saturate(1.0f - distSq * invR2);
                    float sampleOcc = angleOcc * heightGate * falloff;
                    maxDirOcc = max(maxDirOcc, sampleOcc);
                    sumDirOcc += sampleOcc;
                }
            }
        }
        occSum += lerp(sumDirOcc * 0.25f, maxDirOcc, 0.65f);
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

            // P0-2A: Full-res directional contact shadows (synced with DirectionalLight0).
            // Require meaningful perpendicular height (hPlane >= 3.2cm) and steeper horizon
            // angle (cosN > 0.28) with multi-step weighting, and avoid raw +/-2px depth probes
            // that outline modular terrain tile seams / chunk overlaps.
            float2 keyDir = gSscsKeyAndRange.xy;
            float contactMax = 0.0f;
            float contactSum = 0.0f;
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
                    float hPlane = dot(n, V);
                    float cosN = hPlane * invDist;
                    float dz = dC - (-S.z);
                    float rangeW = saturate(1.0f - distSq * 2.4f);
                    float thickW = smoothstep(0.055f, 0.13f, hPlane) *
                                   (1.0f - smoothstep(0.28f, 0.55f, hPlane)) *
                                   smoothstep(0.035f, 0.080f, dz) *
                                   (1.0f - smoothstep(0.32f, 0.60f, dz));
                    float angW = saturate((cosN - 0.30f) * 2.5f);
                    float sampleOcc = angW * rangeW * thickW * (1.10f - 0.14f * float(s));
                    if (sampleOcc > 0.01f)
                    {
                        float3 nS = ReconstructNormalVS(spix, S);
                        // Parallel flat ground planes (terrain chunk steps) are not upright occluders.
                        sampleOcc *= (1.0f - smoothstep(0.76f, 0.92f, dot(n, nS)));
                    }
                    contactMax = max(contactMax, sampleOcc);
                    contactSum += sampleOcc;
                }
            }

            if (contactMax > 0.01f)
            {
                int2 bpix = pix - int2(round(keyDir * 3.0f));
                float bRaw = 1.0f;
                float3 B = ReconstructPosVS(bpix, bRaw);
                if (bRaw < 0.99998f && abs(B.z - P.z) < 0.08f)
                {
                    float3 nB = ReconstructNormalVS(bpix, B);
                    float normStable = smoothstep(0.76f, 0.92f, dot(n, nB));
                    contactMax *= normStable;
                    contactSum *= normStable;
                }
            }

            float contactOcc = lerp(contactSum * 0.20f, contactMax, 0.45f);
            float microAtten = 1.0f - saturate(contactOcc * gSscsParams.y) * 0.36f * fade;
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
const size_t kAoShaderSourceLength = sizeof(kAoShaderSource) - 1;

const char kAoSsrTailSource[] = R"HLSL(
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
    float waterZ = LoadWaterDepth(pix);
    float rainStrength = saturate(gAerialFogParams.y);
    bool isWaterMesh = (waterZ > 1e-5f && waterZ < 0.99998f);

    // 既不是真实水体网格，也不是雨天积水，直接物理级全关 SSR！
    // 荒原沙地、石板、雪地在晴天或室内全是 waterZ == 0.0，瞬间退出，彻底根除任何误判！
    if (!isWaterMesh && rainStrength <= 0.02f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // 若是真实水体网格，直接用精确水面深度 waterZ 重建水面发射点 P；
    // 若是下雨天地面湿润，则沿用原始地表深度 LoadRawDepth(pix)
    float rawC = isWaterMesh ? waterZ : LoadRawDepth(pix);
    float3 P = ReconstructPosVS(pix, rawC);
    float dC = -P.z;
    if (rawC >= 0.99998f || dC >= gProjZAndFade.z)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float3 baseCol = gAoInputTex.Load(int3(pix, 0)).rgb;
    float baseLuma = dot(baseCol, float3(0.299f, 0.587f, 0.114f));
    float maxCh = max(max(baseCol.r, baseCol.g), baseCol.b);

    float gTime = gAerialFogParams.x;
    float timeHours = gAerialFogParams.z;

    // 水深与浅滩岸边羽化（暗坑2防护）：TL2水体材质dw=1，场景深度LoadRawDepth已被水面写入覆盖(dBed == dC)；
    // 真实水面网格保持shoreFade = 1.0f全开，仅在水底明显深于水面(dw=0情况)时才做平滑羽化，彻底杜绝倒影被0值误杀。
    float shoreFade = 1.0f;
    if (isWaterMesh)
    {
        float bedRaw = LoadRawDepth(pix);
        float dBed = (bedRaw < 0.99998f) ? RawToLinearDist(bedRaw) : (dC + 5.0f);
        float waterDepthDiff = dBed - dC;
        shoreFade = (waterDepthDiff > 0.015f) ? smoothstep(0.015f, 0.14f, waterDepthDiff) : 1.0f;
    }

    float3 N = ReconstructNormalVS(pix, P);
    float3 upVS = normalize(gViewUpAndWater.xyz);
    float upDot = dot(N, upVS);
    float horizMask = smoothstep(0.72f, 0.88f, upDot);
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
    float flatMask = 1.0f - smoothstep(0.12f, 0.36f, curv);
    if (flatMask <= 0.02f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float wx = dot(P, gInvViewRow0.xyz) + gInvViewRow0.w;
    float wz = dot(P, gInvViewRow2.xyz) + gInvViewRow2.w;

    // 雨天地面湿润与自然积水洼模拟
    float wetGroundMask = 0.0f;
    if (rainStrength > 0.02f)
    {
        float pNoise = sin(wx * 0.82f + sin(wz * 0.68f) * 1.3f) *
                       cos(wz * 0.92f + cos(wx * 0.78f) * 1.2f);
        float puddle = smoothstep(0.18f - rainStrength * 0.38f, 0.68f - rainStrength * 0.22f, pNoise);
        float wetFilm = 0.22f * rainStrength;
        wetGroundMask = saturate(lerp(wetFilm, 1.0f, puddle) * rainStrength);
    }

    float effectiveWaterMask = isWaterMesh ? shoreFade : wetGroundMask;
    if (effectiveWaterMask <= 0.02f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    // 【自发光/半透明水晶防穿帮保护】
    // 防止 depth_write off 的青色水晶、传送门、技能光效把背后平地的湿地/水面倒影画到自己脸上
    float minC = min(baseCol.r, min(baseCol.g, baseCol.b));
    float baseSat = (maxCh > 1e-4f) ? ((maxCh - minC) / maxCh) : 0.0f;
    float baseLuma0 = baseLuma;

    // 1) 同时满足高饱和 (>0.42) + 中高亮度 (>0.38)：精准排除青水晶(含暗棱面)、传送门、魔法特效
    //    真实河水 maxCh < 0.38、湿石板 baseSat < 0.38，两者均不受影响
    float emissiveNeon = smoothstep(0.42f, 0.68f, baseSat) * smoothstep(0.38f, 0.58f, maxCh);
    // 2) 极高亮自发光核心（如水晶白青高光棱面、传送门中心）同样禁止覆盖暗倒影
    float emissiveCore = smoothstep(0.76f, 0.94f, baseLuma0);
    float nonEmissiveMask = 1.0f - saturate(max(emissiveNeon, emissiveCore));

    float distFade = saturate((gProjZAndFade.z - dC) * gProjZAndFade.w);
    float wetMask = saturate(effectiveWaterMask) * horizMask * flatMask * distFade * nonEmissiveMask;
    if (wetMask <= 0.025f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    uint texW = 1, texH = 1;
    gAoInputTex.GetDimensions(texW, texH);
    float2 invTex = rcp(float2(max(texW, 1u), max(texH, 1u)));

    // 适配火炬2 默认俯视角视距 (14~32m)，避免正常游玩时波纹与焦散提前衰减归零
    float waveFade = 1.0f - smoothstep(18.0f, 48.0f, dC);

    float3 refAxis = (abs(upVS.z) < 0.9f) ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
    float3 tX = normalize(cross(upVS, refAxis));
    float3 tZ = cross(tX, upVS);

    float2 wuv1 = float2(wx * 2.2f + gTime * 0.55f, wz * 1.7f - gTime * 0.45f);
    float2 wuv2 = float2(wz * 3.1f - gTime * 0.75f, wx * 2.6f + gTime * 0.60f);
    float2 ripRaw = float2(sin(wuv1.x + wuv1.y), cos(wuv1.y - wuv1.x)) * 0.6f +
                    float2(sin(wuv2.x - wuv2.y), cos(wuv2.x + wuv2.y)) * 0.4f;

    // 雨天模式叠加高频跳动雨滴涟漪
    if (rainStrength > 0.02f)
    {
        float2 rdrop1 = float2(wx * 9.8f + gTime * 4.4f, wz * 9.8f + gTime * 3.3f);
        float2 rdrop2 = float2(wz * 13.4f - gTime * 5.0f, wx * 13.4f - gTime * 3.9f);
        float2 rainRip = float2(sin(rdrop1.x * 2.1f) * cos(rdrop1.y * 1.8f),
                                sin(rdrop2.x * 1.9f) * cos(rdrop2.y * 2.2f));
        ripRaw += rainRip * (0.42f * rainStrength);
    }

    float3 Nrefl = normalize(upVS + (tX * ripRaw.x + tZ * ripRaw.y) * (0.0015f * waveFade));
    float3 V = normalize(P);
    float3 R = reflect(V, Nrefl);

    float3 rayOrigin = P + upVS * 0.06f;

    // 1. 视距自适应步长：无论拉近特写还是拉远俯视，屏幕步距恒定在细腻的 ~6-12px
    float stepScale = clamp(dC * 0.05f, 0.25f, 1.5f);
    float baseStep = 0.085f * stepScale;
    // 有序梯度噪点 (IGN) 抖动关掉，使用固定步进初值，彻底杜绝相邻像素由于抖动导致一边命中、一边穿透
    float t = baseStep * 0.50f;
    float prevT = max(t - baseStep, 0.02f);

    // 真实交点允许的最大精修残差（特写 ~0.30m，远景 ~0.45m），拦截“背后穿过”的重复鬼影
    float maxAcceptDiff = clamp(0.28f + dC * 0.007f, 0.30f, 0.45f);

    float occAtten = 1.0f;
    bool hit = false;
    float2 hitTexUv = float2(0.0f, 0.0f);
    float2 hitVpUv = float2(0.5f, 0.5f);
    float hitT = 0.0f;
    float hitDiff = 0.0f;

    [loop]
    for (int i = 0; i < 32; ++i)
    {
        float stepDist = baseStep * (1.0f + float(i) * 0.14f);
        float3 sampleVS = rayOrigin + R * t;
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
            // 粗步进容差仅略大于当前步长，既防薄墙漏检，又不轻易误吸 1m 外的前景物体
            float thickLimit = clamp(0.28f + stepDist * 1.4f, 0.35f, 0.90f);
            if (diff > 0.03f && diff < thickLimit)
            {
                float lo = prevT;
                float hi = t;
                float2 bestVpUv = vpUv;
                float bestT = t;
                float bestDiff = diff;
                [unroll]
                for (int rStep = 0; rStep < 5; ++rStep)
                {
                    float midT = 0.5f * (lo + hi);
                    float3 midVS = rayOrigin + R * midT;
                    float2 midVpUv = ProjectViewToVpUv(midVS);
                    int2 mpix = clamp(int2(gVpRect.xy + midVpUv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
                    float mRaw = LoadRawDepth(mpix);
                    if (mRaw < 0.99995f)
                    {
                        float mSceneD = RawToLinearDist(mRaw);
                        float midDiff = (-midVS.z) - mSceneD;
                        if (midDiff > 0.01f && midDiff < thickLimit)
                        {
                            hi = midT;
                            bestVpUv = midVpUv;
                            bestT = midT;
                            bestDiff = midDiff;
                        }
                        else
                        {
                            lo = midT;
                        }
                    }
                    else
                    {
                        lo = midT;
                    }
                }

                // 【关键防重影校验】：只有精修后真正贴近表面 (bestDiff 小) 才确认命中；
                // 若 bestDiff 依然很大，说明是悬空前景的假命中，记录遮挡穿透衰减，继续追踪后方真实景物！
                if (bestDiff < maxAcceptDiff)
                {
                    hitT = bestT;
                    hitDiff = bestDiff;
                    hitVpUv = bestVpUv;
                    hitTexUv = (gVpRect.xy + bestVpUv * gVpRect.zw) * invTex;
                    hit = true;
                    break;
                }
                else
                {
                    // 光线穿透了悬空前景物体（如紫炮），压制后方背景反射，防止刺眼白石台碎块钻入倒影
                    occAtten *= 0.25f;
                }
            }
        }
        prevT = t;
        t += stepDist;
    }

    float ssrStr = max(gAoParams.z, 0.0f);

    // 1. 先算 SSR 反射采样（接触硬化清晰边缘 + 防跨深度串色水波）
    float reflAlpha = 0.0f;
    float3 reflCol = float3(0.0f, 0.0f, 0.0f);
    if (hit)
    {
        int2 hitPix = clamp(int2(gVpRect.xy + hitVpUv * gVpRect.zw), gVpClamp.xy, gVpClamp.zw);
        float hitRaw = 0.0f;
        float3 hitSurfP = ReconstructPosVS(hitPix, hitRaw);
        float hitHeight = dot(hitSurfP - P, upVS);
        float heightFade = smoothstep(0.03f, 0.20f, hitHeight);

        float contactFade = 1.0f - smoothstep(maxAcceptDiff * 0.40f, maxAcceptDiff, hitDiff);

        // 【平滑深度阻尼水波】：用连续 smoothstep 替代硬 if 切换，彻底消灭炮管/人物轮廓撕裂锯齿
        float2 wavePxOfs = ripRaw * (2.4f * smoothstep(0.15f, 2.2f, hitT) * waveFade);
        int2 wavePix = clamp(hitPix + int2(round(wavePxOfs)), gVpClamp.xy, gVpClamp.zw);
        float waveRaw = LoadRawDepth(wavePix);
        float waveDepthDiff = (waveRaw < 0.99995f) ? abs(RawToLinearDist(waveRaw) - (-hitSurfP.z)) : 10.0f;
        float waveValid = 1.0f - smoothstep(0.12f, 0.42f, waveDepthDiff);
        float2 sampleUv = hitTexUv + wavePxOfs * (waveValid * invTex);

        float2 edgeDist = min(hitVpUv, 1.0f - hitVpUv);
        float edgeFade = smoothstep(0.0f, 0.08f, min(edgeDist.x, edgeDist.y));

        float viewGraze = 1.0f - saturate(dot(-V, Nrefl));
        float fresnel = saturate(0.50f + 0.50f * pow(viewGraze, 1.4f));

        // 【旋转网格抗锯齿采样 (RGSS 4-Tap AA)】：
        // 旋转 22.5° 采样专门抚平 1x 深度图的斜边锯齿，清晰又不糊
        float blurPx = lerp(0.65f, 1.35f, smoothstep(0.5f, 3.0f, hitT));
        float2 o1 = float2( 0.38f,  0.92f) * invTex * blurPx;
        float2 o2 = float2(-0.92f,  0.38f) * invTex * blurPx;
        reflCol =
            gAoInputTex.SampleLevel(gAoSamp, sampleUv + o1, 0).rgb * 0.25f +
            gAoInputTex.SampleLevel(gAoSamp, sampleUv - o1, 0).rgb * 0.25f +
            gAoInputTex.SampleLevel(gAoSamp, sampleUv + o2, 0).rgb * 0.25f +
            gAoInputTex.SampleLevel(gAoSamp, sampleUv - o2, 0).rgb * 0.25f;

        float reflLuma = dot(reflCol, float3(0.299f, 0.587f, 0.114f));
        // 夜晚与弱光环境自适应门禁：夜晚基底暗时自适应降低门禁，让月光下的人物衣着与环境倒影清晰可见
        float minGate = lerp(0.015f, 0.038f, smoothstep(0.02f, 0.12f, baseLuma));
        float shadowGate = smoothstep(minGate, minGate + 0.09f, reflLuma);

        // 【真实水体吸收 + 柔和高光压缩】：
        // 去掉 1.12x 过曝增亮，柔和压制炮管白反光条/阳光石台边/小鹿光环，消除水里刺眼的怪异白块
        reflCol = reflCol / (1.0f + max(reflCol - 0.62f, 0.0f) * 0.90f);
        reflCol *= float3(0.84f, 0.90f, 0.96f);

        // 当前像素本身很亮且明显亮于暗倒影时，平滑压低暗倒影覆盖，防止在发光边缘留下脏黑印
        float darkOverBright = 1.0f - smoothstep(0.20f, 0.55f, baseLuma0 - reflLuma) * smoothstep(0.45f, 0.75f, baseLuma0);
        reflAlpha = saturate(wetMask * fresnel * edgeFade * heightFade * contactFade * shadowGate * darkOverBright * 0.72f * ssrStr) * occAtten;
    }

    // 2. 再算水底流动焦散（强倒影区用 2.5 次方彻底让位给倒影，杜绝焦散白丝混在炮身上）
    float cw1 = 1.0f - abs(sin(wx * 2.4f + gTime * 0.95f + sin(wz * 1.9f + gTime * 0.65f) * 0.8f));
    float cw2 = 1.0f - abs(sin(wz * 2.7f - gTime * 0.85f + cos(wx * 2.1f - gTime * 0.70f) * 0.8f));
    float caustics = pow(saturate(cw1 * cw2), 2.4f) * 0.16f * waveFade *
                     smoothstep(0.05f, 0.26f, baseLuma) * smoothstep(0.30f, 0.75f, effectiveWaterMask);
    // 强倒影区用 2.5 次方彻底让位给倒影，杜绝焦散白丝混在炮身上
    float caustAlpha = saturate(caustics * wetMask * saturate(ssrStr) * pow(1.0f - reflAlpha, 2.5f));
    float3 caustCol = float3(0.86f, 0.96f, 1.0f);

    // 3. 无暗化精确合成
    float weightSum = reflAlpha + caustAlpha;
    if (weightSum <= 1e-4f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }

    float outA = saturate(weightSum);
    float3 outRgb = (reflCol * reflAlpha + caustCol * caustAlpha) / weightSum;
    return float4(saturate(outRgb), outA);
}
)HLSL";
const size_t kAoSsrTailSourceLength = sizeof(kAoSsrTailSource) - 1;

const char kSsrResolveShaderSource[] = R"HLSL(
// 【SSR 后置跨像素双边空间滤波 Pass】
// 解决 1x 深度图硬边与相邻光线穿透跳变造成的硬锯齿，以预乘 Alpha 还原滤波消除黑边，双边深度限制防岸上溢出
float4 PS_SsrSpatialResolve(VsOut input) : SV_Target0
{
    int2 pix = clamp(int2(input.pos.xy), gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(pix);
    if (rawC >= 0.99998f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float dC = RawToLinearDist(rawC);

    float3 sumPremulRgb = float3(0.0f, 0.0f, 0.0f);
    float sumAlpha = 0.0f;
    float sumWeight = 0.0f;

    // 3x3 空间与双边深度滤波核（对角 45° 权重 0.75，中心权重 1.40，严格抚平 1x 深度图斜边锯齿）
    [unroll]
    for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 spix = clamp(pix + int2(dx, dy), gVpClamp.xy, gVpClamp.zw);
            float rawN = LoadRawDepth(spix);
            if (rawN >= 0.99998f)
            {
                continue;
            }
            float dN = RawToLinearDist(rawN);
            float depthDiff = abs(dN - dC);

            // 深度双边阈值收紧在 ~0.14m，严防倒影溢到石台岸上
            float bilateralW = exp(-depthDiff * 14.0f);
            float spatialW = (dx == 0 && dy == 0) ? 1.40f : ((dx != 0 && dy != 0) ? 0.75f : 1.0f);
            float w = spatialW * bilateralW;

            float4 s = gAoInputTex.Load(int3(spix, 0));
            sumPremulRgb += s.rgb * (s.a * w);
            sumAlpha += s.a * w;
            sumWeight += w;
        }
    }

    float outA = sumAlpha / max(sumWeight, 1e-4f);
    if (outA <= 1e-4f)
    {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float3 outRgb = sumPremulRgb / max(sumAlpha, 1e-4f);
    return float4(saturate(outRgb), saturate(outA));
}
)HLSL";
const size_t kSsrResolveShaderSourceLength = sizeof(kSsrResolveShaderSource) - 1;

const char kToastShaderSource[] = R"HLSL(
cbuffer CbToastParams : register(b0) {
    float4 gToastRect;    // x=left, y=top, z=width, w=height
    float4 gToastParams;  // x=dllOn, y=aoOn, z=fadeAlpha, w=scale
    float4 gToastExtra;   // x=filterMode (0=OFF, 1=HOB, 2=DRK), y=apOn (F11: 16xAF + PointLight)
};

struct VS_OUT {
    float4 pos : SV_Position;
};

uint GetToastChar(int idx, bool dllOn) {
    // Format (45 chars): "[F8] GRAPHICS PANEL: READY - PRESS F8 TO OPEN"
    if (idx == 0) return 91u;                                          // '['
    if (idx == 3) return 93u;                                          // ']'
    if (idx == 1 || idx == 35) return 70u;                             // 'F'
    if (idx == 2 || idx == 36) return 56u;                             // '8'
    if (idx == 5) return 71u;                                          // 'G'
    if (idx == 6 || idx == 30) return 82u;                             // 'R'
    if (idx == 7 || idx == 15) return 65u;                             // 'A'
    if (idx == 8 || idx == 14 || idx == 29 || idx == 42) return 80u;   // 'P'
    if (idx == 9) return 72u;                                          // 'H'
    if (idx == 10) return 73u;                                         // 'I'
    if (idx == 11) return 67u;                                         // 'C'
    if (idx == 12 || idx == 32 || idx == 33) return 83u;               // 'S'
    if (idx == 16 || idx == 44) return 78u;                            // 'N'
    if (idx == 17 || idx == 31 || idx == 43) return 69u;               // 'E'
    if (idx == 18) return 76u;                                         // 'L'
    if (idx == 19) return 58u;                                         // ':'
    if (idx == 21) return dllOn ? 82u : 79u;                           // 'R' : 'O'
    if (idx == 22) return dllOn ? 69u : 70u;                           // 'E' : 'F'
    if (idx == 23) return dllOn ? 65u : 70u;                           // 'A' : 'F'
    if (idx == 24) return dllOn ? 68u : 32u;                           // 'D' : ' '
    if (idx == 25) return dllOn ? 89u : 32u;                           // 'Y' : ' '
    if (idx == 27) return 45u;                                         // '-'
    if (idx == 38) return 84u;                                         // 'T'
    if (idx == 39 || idx == 41) return 79u;                            // 'O'
    return 32u;                                                        // ' '
}

uint GetGlyph5x7Row(uint ch, uint row) {
    if (ch == 91u) { return (row == 0u || row == 6u) ? 14u : 8u; }                          // '['
    if (ch == 93u) { return (row == 0u || row == 6u) ? 14u : 2u; }                          // ']'
    if (ch == 58u) { return (row == 1u || row == 2u || row == 4u || row == 5u) ? 4u : 0u; } // ':'
    if (ch == 45u) { return (row == 3u) ? 14u : 0u; }                                       // '-'
    if (ch == 48u) {                                                                        // '0'
        if (row == 0u || row == 6u) return 14u;
        if (row == 2u || row == 3u) return 21u;
        return 17u;
    }
    if (ch == 49u) {                                                                        // '1'
        if (row == 1u) return 12u;
        if (row == 6u) return 14u;
        return 4u;
    }
    if (ch == 56u) { return (row == 0u || row == 3u || row == 6u) ? 14u : 17u; }            // '8'
    if (ch == 57u) {                                                                        // '9'
        if (row == 0u) return 14u;
        if (row == 1u || row == 2u) return 17u;
        if (row == 3u) return 15u;
        if (row == 4u) return 1u;
        if (row == 5u) return 2u;
        return 12u;
    }
    if (ch == 65u) { return (row == 0u) ? 14u : ((row == 3u) ? 31u : 17u); }                // 'A'
    if (ch == 67u) { return (row == 0u || row == 6u) ? 14u : ((row == 1u || row == 5u) ? 17u : 16u); } // 'C'
    if (ch == 68u) { return (row == 0u || row == 6u) ? 30u : 17u; }                         // 'D'
    if (ch == 69u) { return (row == 0u || row == 6u) ? 31u : ((row == 3u) ? 30u : 16u); } // 'E'
    if (ch == 70u) { return (row == 0u) ? 31u : ((row == 3u) ? 30u : 16u); }                // 'F'
    if (ch == 71u) {                                                                        // 'G'
        if (row == 0u || row == 6u) return 14u;
        if (row == 3u) return 23u;
        if (row == 2u) return 16u;
        return 17u;
    }
    if (ch == 72u) { return (row == 3u) ? 31u : 17u; }                                      // 'H'
    if (ch == 73u) { return (row == 0u || row == 6u) ? 14u : 4u; }                          // 'I'
    if (ch == 76u) { return (row == 6u) ? 31u : 16u; }                                      // 'L'
    if (ch == 78u) {                                                                        // 'N'
        if (row == 1u) return 25u;
        if (row == 2u) return 21u;
        if (row == 3u) return 19u;
        return 17u;
    }
    if (ch == 79u) { return (row == 0u || row == 6u) ? 14u : 17u; }                         // 'O'
    if (ch == 80u) {                                                                        // 'P'
        if (row == 0u || row == 3u) return 30u;
        if (row <= 2u) return 17u;
        return 16u;
    }
    if (ch == 82u) {                                                                        // 'R'
        if (row == 0u || row == 3u) return 30u;
        if (row == 4u) return 20u;
        if (row == 5u) return 18u;
        return 17u;
    }
    if (ch == 83u) {                                                                        // 'S'
        if (row == 0u || row == 3u || row == 6u) return 14u;
        if (row == 2u) return 16u;
        if (row == 4u) return 1u;
        return 17u;
    }
    if (ch == 84u) { return (row == 0u) ? 31u : 4u; }                                       // 'T'
    if (ch == 89u) {                                                                        // 'Y'
        if (row <= 1u) return 17u;
        if (row == 2u) return 10u;
        return 4u;
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

    bool hit = false;
    int charIdx = -1;
    if (cell.y >= 0 && cell.y < 7 && cell.x >= 0) {
        charIdx = cell.x / 6;
        int gx = cell.x - charIdx * 6;
        if (charIdx >= 0 && charIdx < 45 && gx >= 0 && gx < 5) {
            uint ch = GetToastChar(charIdx, gToastParams.x > 0.5f);
            uint bits = GetGlyph5x7Row(ch, (uint)cell.y);
            hit = ((bits >> (4u - (uint)gx)) & 1u) != 0u;
        }
    }

    float fade = saturate(gToastParams.z);
    if (hit) {
        bool dllOn = (gToastParams.x > 0.5f);
        float3 fg = float3(0.96f, 0.97f, 1.00f);
        if (charIdx <= 3 || (charIdx >= 35 && charIdx <= 36)) {
            // "[F8]" or "F8" in amber gold
            fg = float3(1.00f, 0.82f, 0.32f);
        } else if (charIdx >= 21 && charIdx <= 25) {
            // "READY" in emerald green / "OFF" in warm red
            fg = dllOn ? float3(0.38f, 0.96f, 0.58f) : float3(1.00f, 0.44f, 0.42f);
        } else if (charIdx == 27) {
            // "-" in muted slate
            fg = float3(0.55f, 0.62f, 0.72f);
        } else if (charIdx >= 29) {
            // "PRESS F8 TO OPEN" secondary text in soft silver
            fg = float3(0.82f, 0.86f, 0.92f);
        }
        return float4(fg, fade);
    }

    float3 bg = lerp(float3(0.06f, 0.08f, 0.12f), float3(0.45f, 0.52f, 0.62f), border);
    float bgA = lerp(0.84f, 0.92f, border) * fade;
    return float4(bg, bgA);
}
)HLSL";
const size_t kToastShaderSourceLength = sizeof(kToastShaderSource) - 1;

const char kP2FilterShaderSource[] = R"HLSL(
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
    float filterStrength = clamp(p2FilterParams.w, 0.0f, 1.65f);
    float3 hobRgb = ApplyP2HobFilter(src.rgb, p2FilterParams.x);
    float3 outRgb = (p2FilterParams.x > 0.5f) ? saturate(lerp(src.rgb, hobRgb, filterStrength)) : src.rgb;
    if (p2FilterParams.z > 0.001f) {
        float2 uv = saturate((input.pos.xy - p2VpRect.xy) / max(p2VpRect.zw, 1.0f));
        float vig = ComputeP2OpticalVignette(uv);
        outRgb *= max(1.0f - (1.0f - vig) * p2FilterParams.z, 0.35f);
    }
    return float4(outRgb, 1.0f);
}
)HLSL";
const size_t kP2FilterShaderSourceLength = sizeof(kP2FilterShaderSource) - 1;

const char kPanelShaderSource[] = R"HLSL(
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
const size_t kPanelShaderSourceLength = sizeof(kPanelShaderSource) - 1;

const char kBloomLutShaderSource[] = R"HLSL(
cbuffer CbBloomLutParams : register(b0)
{
    float4 gDstVpRect;     // xy=dstOriginPx, zw=dstSizePx
    float4 gSrcUvRect;     // xy=srcUvOrigin, zw=srcUvSize
    float4 gTexelAndBias;  // xy=invSrcTexSize, z=threshold, w=knee
    float4 gMixAndLut;     // x=upsampleMixOrBloomStrength, y=lutSize, z=lutInvSize, w=lutMix
};

Texture2D<float4> gTex0 : register(t0);
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

float4 PS_LutGrade3D(VS_OUT input) : SV_Target0
{
    float2 uv = ComputeSrcUv(input.pos.xy);
    float3 src = saturate(gTex0.SampleLevel(gSamp, ClampSrcUv(uv), 0).rgb);
    float3 lutUv = src * ((gMixAndLut.y - 1.0f) * gMixAndLut.z) + (0.5f * gMixAndLut.z);
    float3 graded = saturate(gLutTex.SampleLevel(gSamp, lutUv, 0).rgb);
    return float4(saturate(lerp(src, graded, gMixAndLut.w)), 1.0f);
}
)HLSL";
const size_t kBloomLutShaderSourceLength = sizeof(kBloomLutShaderSource) - 1;

const char kWriteWaterDepthShaderSource[] = R"HLSL(
float PS_WriteWaterDepth(float4 pos : SV_POSITION) : SV_Target0
{
    return pos.z;
}
)HLSL";
const size_t kWriteWaterDepthShaderSourceLength = sizeof(kWriteWaterDepthShaderSource) - 1;

}  // namespace routeb
