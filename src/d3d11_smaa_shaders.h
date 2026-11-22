#pragma once

namespace routeb {

// SMAA 1x HLSL shaders for TL2 Route B/C
// Compiled with D3DCompile (ps_4_0)
const char kSmaaShadersSource[] = R"HLSL(
cbuffer CbSmaaParams : register(b0)
{
    float4 gSmaaRtMetrics;      // x=1/W, y=1/H, z=W, w=H
    float4 gProjZAndMode;       // x=proj_22, y=proj_23, z=mode(1=Color, 2=Depth+Color), w=unused
    int4   gSmaaVpClamp;        // x=minX, y=minY, z=maxX, w=maxY
};

struct VsOut
{
    float4 pos : SV_Position;
};

SamplerState LinearSampler : register(s0);
SamplerState PointSampler  : register(s1);

#define SMAA_MAX_SEARCH_STEPS 32
#define SMAA_MAX_SEARCH_STEPS_DIAG 16
#define SMAA_CORNER_ROUNDING 60
#define SMAA_CORNER_ROUNDING_NORM (float(SMAA_CORNER_ROUNDING) / 100.0)
#define SMAA_AREATEX_MAX_DISTANCE 16
#define SMAA_AREATEX_MAX_DISTANCE_DIAG 20
#define SMAA_AREATEX_PIXEL_SIZE (1.0 / float2(160.0, 560.0))
#define SMAA_AREATEX_SUBTEX_SIZE (1.0 / 7.0)
#define SMAA_SEARCHTEX_SIZE float2(66.0, 33.0)
#define SMAA_SEARCHTEX_PACKED_SIZE float2(64.0, 16.0)

// ----------------------------------------------------------------------------
// PASS 1: Edge Detection (Color + Optional Depth Guidance)
// ----------------------------------------------------------------------------
Texture2D<float4> gColorTex : register(t0);
Texture2D<float>  gDepthTex : register(t1);

float4 PS_SmaaEdgeDetection(VsOut input) : SV_Target0
{
    // Red line 1: input.pos.xy is already at pixel center (0.5, 1.5, ...).
    // Do NOT add + 0.5f!
    float2 texcoord = input.pos.xy * gSmaaRtMetrics.xy;

    float4 offset[3];
    offset[0] = mad(gSmaaRtMetrics.xyxy, float4(-1.0, 0.0, 0.0, -1.0), texcoord.xyxy);
    offset[1] = mad(gSmaaRtMetrics.xyxy, float4( 1.0, 0.0, 0.0,  1.0), texcoord.xyxy);
    offset[2] = mad(gSmaaRtMetrics.xyxy, float4(-2.0, 0.0, 0.0, -2.0), texcoord.xyxy);

    float threshold = 0.050f;
    float isGeoEdge = 0.0f;
    float2 geoEdges = float2(0.0f, 0.0f);

    // Mode 2: Depth+Color extreme tier with second-order linear depth guidance
    [branch]
    if (gProjZAndMode.z > 1.5f)
    {
        int2 pix = clamp(int2(input.pos.xy), gSmaaVpClamp.xy, gSmaaVpClamp.zw);
        float rawC = gDepthTex.Load(int3(pix, 0));
        [branch]
        if (rawC < 0.99998f)
        {
            float dC = gProjZAndMode.y / min(rawC + gProjZAndMode.x, -1e-6f);
            int2 pL = clamp(pix + int2(-1,  0), gSmaaVpClamp.xy, gSmaaVpClamp.zw);
            int2 pR = clamp(pix + int2( 1,  0), gSmaaVpClamp.xy, gSmaaVpClamp.zw);
            int2 pT = clamp(pix + int2( 0, -1), gSmaaVpClamp.xy, gSmaaVpClamp.zw);
            int2 pB = clamp(pix + int2( 0,  1), gSmaaVpClamp.xy, gSmaaVpClamp.zw);

            float dL = gProjZAndMode.y / min(gDepthTex.Load(int3(pL, 0)) + gProjZAndMode.x, -1e-6f);
            float dR = gProjZAndMode.y / min(gDepthTex.Load(int3(pR, 0)) + gProjZAndMode.x, -1e-6f);
            float dT = gProjZAndMode.y / min(gDepthTex.Load(int3(pT, 0)) + gProjZAndMode.x, -1e-6f);
            float dB = gProjZAndMode.y / min(gDepthTex.Load(int3(pB, 0)) + gProjZAndMode.x, -1e-6f);

            float invD = rcp(max(dC, 1.0f));
            float depthJumpH = abs(2.0f * dC - dL - dR) * invD;
            float depthJumpV = abs(2.0f * dC - dT - dB) * invD;
            isGeoEdge = saturate(max(depthJumpH, depthJumpV) * 50.0f);
            threshold = lerp(0.050f, 0.025f, isGeoEdge);

            geoEdges = float2(
                (abs(dC - dL) * invD > 0.020f && depthJumpH > 0.012f) ? 1.0f : 0.0f,
                (abs(dC - dT) * invD > 0.015f && depthJumpV > 0.012f) ? 1.0f : 0.0f);
        }
    }

    float4 delta;
    float3 C = gColorTex.SampleLevel(PointSampler, texcoord, 0).rgb;

    float3 Cleft = gColorTex.SampleLevel(PointSampler, offset[0].xy, 0).rgb;
    float3 tL = abs(C - Cleft);
    delta.x = max(max(tL.r, tL.g), tL.b);

    float3 Ctop = gColorTex.SampleLevel(PointSampler, offset[0].zw, 0).rgb;
    float3 tT = abs(C - Ctop);
    delta.y = max(max(tT.r, tT.g), tT.b);

    float2 edges = step(threshold, delta.xy);

    if (dot(max(edges, geoEdges), float2(1.0, 1.0)) == 0.0)
    {
        discard;
    }

    float3 Cright = gColorTex.SampleLevel(PointSampler, offset[1].xy, 0).rgb;
    float3 tR = abs(C - Cright);
    delta.z = max(max(tR.r, tR.g), tR.b);

    float3 Cbottom = gColorTex.SampleLevel(PointSampler, offset[1].zw, 0).rgb;
    float3 tB = abs(C - Cbottom);
    delta.w = max(max(tB.r, tB.g), tB.b);

    float2 maxDelta = max(delta.xy, delta.zw);

    float3 Cleftleft = gColorTex.SampleLevel(PointSampler, offset[2].xy, 0).rgb;
    float3 tLL = abs(C - Cleftleft);
    delta.z = max(max(tLL.r, tLL.g), tLL.b);

    float3 Ctoptop = gColorTex.SampleLevel(PointSampler, offset[2].zw, 0).rgb;
    float3 tTT = abs(C - Ctoptop);
    delta.w = max(max(tTT.r, tTT.g), tTT.b);

    maxDelta = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    // Color edge local contrast adaptation
    edges.xy *= step(finalDelta, 2.0f * delta.xy);
    // 3D geometric depth silhouettes must bypass color contrast suppression
    edges = max(edges, geoEdges);
    return float4(edges, 0.0f, 0.0f);
}

// ----------------------------------------------------------------------------
// PASS 2: Blending Weight Calculation
// ----------------------------------------------------------------------------
Texture2D<float2> gEdgesTex  : register(t0);
Texture2D<float2> gAreaTex   : register(t1);
Texture2D<float>  gSearchTex : register(t2);

float SMAASearchLength(Texture2D<float> searchTex, float2 e, float offset)
{
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5, -1.0);
    float2 bias = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0);
    scale += float2(-1.0, 1.0);
    bias  += float2( 0.5, -0.5);
    scale *= (1.0 / SMAA_SEARCHTEX_PACKED_SIZE);
    bias  *= (1.0 / SMAA_SEARCHTEX_PACKED_SIZE);
    return searchTex.SampleLevel(PointSampler, mad(scale, e, bias), 0);
}

float SMAASearchXLeft(Texture2D<float2> edgesTex, Texture2D<float> searchTex, float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    while (texcoord.x > end && e.g > 0.8281 && e.r == 0.0)
    {
        e = edgesTex.SampleLevel(LinearSampler, texcoord, 0).rg;
        texcoord = mad(-float2(2.0, 0.0), gSmaaRtMetrics.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e, 0.0), 3.25);
    return mad(gSmaaRtMetrics.x, offset, texcoord.x);
}

float SMAASearchXRight(Texture2D<float2> edgesTex, Texture2D<float> searchTex, float2 texcoord, float end)
{
    float2 e = float2(0.0, 1.0);
    while (texcoord.x < end && e.g > 0.8281 && e.r == 0.0)
    {
        e = edgesTex.SampleLevel(LinearSampler, texcoord, 0).rg;
        texcoord = mad(float2(2.0, 0.0), gSmaaRtMetrics.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e, 0.5), 3.25);
    return mad(-gSmaaRtMetrics.x, offset, texcoord.x);
}

float SMAASearchYUp(Texture2D<float2> edgesTex, Texture2D<float> searchTex, float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    while (texcoord.y > end && e.r > 0.8281 && e.g == 0.0)
    {
        e = edgesTex.SampleLevel(LinearSampler, texcoord, 0).rg;
        texcoord = mad(-float2(0.0, 2.0), gSmaaRtMetrics.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e.gr, 0.0), 3.25);
    return mad(gSmaaRtMetrics.y, offset, texcoord.y);
}

float SMAASearchYDown(Texture2D<float2> edgesTex, Texture2D<float> searchTex, float2 texcoord, float end)
{
    float2 e = float2(1.0, 0.0);
    while (texcoord.y < end && e.r > 0.8281 && e.g == 0.0)
    {
        e = edgesTex.SampleLevel(LinearSampler, texcoord, 0).rg;
        texcoord = mad(float2(0.0, 2.0), gSmaaRtMetrics.xy, texcoord);
    }
    float offset = mad(-(255.0 / 127.0), SMAASearchLength(searchTex, e.gr, 0.5), 3.25);
    return mad(-gSmaaRtMetrics.y, offset, texcoord.y);
}
)HLSL"
R"HLSL(
float2 SMAAArea(Texture2D<float2> areaTex, float2 dist, float e1, float e2, float offset)
{
    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE), round(4.0 * float2(e1, e2)), dist);
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    texcoord.y = mad(SMAA_AREATEX_SUBTEX_SIZE, offset, texcoord.y);
    return areaTex.SampleLevel(LinearSampler, texcoord, 0).rg;
}

void SMAADetectHorizontalCornerPattern(Texture2D<float2> edgesTex, inout float2 weights, float4 texcoord, float2 d)
{
    float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * edgesTex.SampleLevel(PointSampler, texcoord.xy, 0, int2(0,  1)).r;
    factor.x -= rounding.y * edgesTex.SampleLevel(PointSampler, texcoord.xy, 0, int2(0, -2)).r;
    factor.y -= rounding.x * edgesTex.SampleLevel(PointSampler, texcoord.zw, 0, int2(0,  1)).r;
    factor.y -= rounding.y * edgesTex.SampleLevel(PointSampler, texcoord.zw, 0, int2(0, -2)).r;
    weights *= saturate(factor);
}

void SMAADetectVerticalCornerPattern(Texture2D<float2> edgesTex, inout float2 weights, float4 texcoord, float2 d)
{
    float2 leftRight = step(d.xy, d.yx);
    float2 rounding = (1.0 - SMAA_CORNER_ROUNDING_NORM) * leftRight;
    rounding /= leftRight.x + leftRight.y;
    float2 factor = float2(1.0, 1.0);
    factor.x -= rounding.x * edgesTex.SampleLevel(PointSampler, texcoord.xy, 0, int2( 1, 0)).g;
    factor.x -= rounding.y * edgesTex.SampleLevel(PointSampler, texcoord.xy, 0, int2(-2, 0)).g;
    factor.y -= rounding.x * edgesTex.SampleLevel(PointSampler, texcoord.zw, 0, int2( 1, 0)).g;
    factor.y -= rounding.y * edgesTex.SampleLevel(PointSampler, texcoord.zw, 0, int2(-2, 0)).g;
    weights *= saturate(factor);
}

float2 SMAASearchDiag1(Texture2D<float2> edgesTex, float2 texcoord, float2 dir, out float2 e)
{
    float4 coord = float4(texcoord, -1.0, 1.0);
    float3 t = float3(gSmaaRtMetrics.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)
    {
        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);
        e = edgesTex.SampleLevel(LinearSampler, coord.xy, 0).rg;
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAASearchDiag2(Texture2D<float2> edgesTex, float2 texcoord, float2 dir, out float2 e)
{
    float4 coord = float4(texcoord, -1.0, 1.0);
    float3 t = float3(gSmaaRtMetrics.xy, 1.0);
    while (coord.z < float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) && coord.w > 0.9)
    {
        coord.xyz = mad(t, float3(dir, 1.0), coord.xyz);
        e.g = edgesTex.SampleLevel(PointSampler, coord.xy, 0).g;
        e.r = edgesTex.SampleLevel(PointSampler, coord.xy, 0, int2(1, 0)).r;
        coord.w = dot(e, float2(0.5, 0.5));
    }
    return coord.zw;
}

float2 SMAAAreaDiag(Texture2D<float2> areaTex, float2 dist, float2 e, float offset)
{
    float2 texcoord = mad(float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG), e, dist);
    texcoord = mad(SMAA_AREATEX_PIXEL_SIZE, texcoord, 0.5 * SMAA_AREATEX_PIXEL_SIZE);
    texcoord.x += 0.5;
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return areaTex.SampleLevel(LinearSampler, texcoord, 0).rg;
}

float2 SMAACalculateDiagWeights(Texture2D<float2> edgesTex, Texture2D<float2> areaTex, float2 texcoord, float2 e)
{
    float2 weights = float2(0.0, 0.0);

    // Search for the line ends (first diagonal direction):
    float4 d;
    float2 end;
    if (e.r > 0.0)
    {
        d.xz = SMAASearchDiag1(edgesTex, texcoord, float2(-1.0,  1.0), end);
        d.x += float(end.y > 0.9);
    }
    else
    {
        d.xz = float2(0.0, 0.0);
    }
    d.yw = SMAASearchDiag1(edgesTex, texcoord, float2( 1.0, -1.0), end);

    [branch]
    if (d.x + d.y > 2.0)
    {
        float4 coords = mad(float4(-d.x, d.x, d.y, -d.y), gSmaaRtMetrics.xyxy, texcoord.xyxy);
        float4 c;
        c.x = edgesTex.SampleLevel(PointSampler, coords.xy, 0, int2(-1,  0)).g;
        c.y = edgesTex.SampleLevel(PointSampler, coords.xy, 0, int2( 0,  0)).r;
        c.z = edgesTex.SampleLevel(PointSampler, coords.zw, 0, int2( 1,  0)).g;
        c.w = edgesTex.SampleLevel(PointSampler, coords.zw, 0, int2( 1, -1)).r;

        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);

        if (d.z > 0.9) cc.x = 0.0;
        if (d.w > 0.9) cc.y = 0.0;

        weights += SMAAAreaDiag(areaTex, d.xy, cc, 0.0);
    }

    // Search for the line ends (second diagonal direction):
    d.xz = SMAASearchDiag2(edgesTex, texcoord, float2(-1.0, -1.0), end);
    if (edgesTex.SampleLevel(PointSampler, texcoord, 0, int2(1, 0)).r > 0.0)
    {
        d.yw = SMAASearchDiag2(edgesTex, texcoord, float2( 1.0,  1.0), end);
        d.y += float(end.y > 0.9);
    }
    else
    {
        d.yw = float2(0.0, 0.0);
    }

    [branch]
    if (d.x + d.y > 2.0)
    {
        float4 coords = mad(float4(-d.x, -d.x, d.y, d.y), gSmaaRtMetrics.xyxy, texcoord.xyxy);
        float4 c;
        c.x  = edgesTex.SampleLevel(PointSampler, coords.xy, 0, int2(-1,  0)).g;
        c.y  = edgesTex.SampleLevel(PointSampler, coords.xy, 0, int2( 0, -1)).r;
        c.zw = edgesTex.SampleLevel(PointSampler, coords.zw, 0, int2( 1,  0)).gr;

        float2 cc = mad(float2(2.0, 2.0), c.xz, c.yw);

        if (d.z > 0.9) cc.x = 0.0;
        if (d.w > 0.9) cc.y = 0.0;

        weights += SMAAAreaDiag(areaTex, d.xy, cc, 0.0).gr;
    }

    return weights;
}

float4 PS_SmaaBlendingWeight(VsOut input) : SV_Target0
{
    float2 texcoord = input.pos.xy * gSmaaRtMetrics.xy;
    float2 pixcoord = input.pos.xy;

    float4 offset[3];
    offset[0] = mad(gSmaaRtMetrics.xyxy, float4(-0.25, -0.125,  1.25, -0.125), texcoord.xyxy);
    offset[1] = mad(gSmaaRtMetrics.xyxy, float4(-0.125, -0.25, -0.125,  1.25), texcoord.xyxy);
    offset[2] = mad(gSmaaRtMetrics.xxyy,
                    float4(-2.0, 2.0, -2.0, 2.0) * float(SMAA_MAX_SEARCH_STEPS),
                    float4(offset[0].xz, offset[1].yw));

    float4 weights = float4(0.0, 0.0, 0.0, 0.0);
    float2 e = gEdgesTex.SampleLevel(PointSampler, texcoord, 0).rg;

    [branch]
    if (e.g > 0.0) // Edge at north
    {
        weights.rg = SMAACalculateDiagWeights(gEdgesTex, gAreaTex, texcoord, e);
        [branch]
        if (weights.r == -weights.g)
        {
            float2 d;
            float3 coords;
            coords.x = SMAASearchXLeft(gEdgesTex, gSearchTex, offset[0].xy, offset[2].x);
            coords.y = offset[1].y;
            d.x = coords.x;

            float e1 = gEdgesTex.SampleLevel(LinearSampler, coords.xy, 0).r;
            coords.z = SMAASearchXRight(gEdgesTex, gSearchTex, offset[0].zw, offset[2].y);
            d.y = coords.z;

            d = abs(round(mad(gSmaaRtMetrics.zz, d, -pixcoord.xx)));
            float2 sqrt_d = sqrt(d);

            float e2 = gEdgesTex.SampleLevel(PointSampler, coords.zy, 0, int2(1, 0)).r;
            weights.rg = SMAAArea(gAreaTex, sqrt_d, e1, e2, 0.0);

            SMAADetectHorizontalCornerPattern(gEdgesTex, weights.rg, coords.xyzy, d);
        }
        else
        {
            e.r = 0.0;
        }
    }

    [branch]
    if (e.r > 0.0) // Edge at west
    {
        float2 d;
        float3 coords;
        coords.y = SMAASearchYUp(gEdgesTex, gSearchTex, offset[1].xy, offset[2].z);
        coords.x = offset[0].x;
        d.x = coords.y;

        float e1 = gEdgesTex.SampleLevel(LinearSampler, coords.xy, 0).g;
        coords.z = SMAASearchYDown(gEdgesTex, gSearchTex, offset[1].zw, offset[2].w);
        d.y = coords.z;

        d = abs(round(mad(gSmaaRtMetrics.ww, d, -pixcoord.yy)));
        float2 sqrt_d = sqrt(d);

        float e2 = gEdgesTex.SampleLevel(PointSampler, coords.xz, 0, int2(0, 1)).g;
        weights.ba = SMAAArea(gAreaTex, sqrt_d, e1, e2, 0.0);

        SMAADetectVerticalCornerPattern(gEdgesTex, weights.ba, coords.xyxz, d);
    }

    return weights;
}

// ----------------------------------------------------------------------------
// PASS 3: Neighborhood Blending
// ----------------------------------------------------------------------------
Texture2D<float4> gBlendTex      : register(t1);
Texture2D<float2> gDebugEdgesTex : register(t2);

float4 PS_SmaaNeighborhoodBlend(VsOut input) : SV_Target0
{
    float2 texcoord = input.pos.xy * gSmaaRtMetrics.xy;
    float4 offset = mad(gSmaaRtMetrics.xyxy, float4(1.0, 0.0, 0.0, 1.0), texcoord.xyxy);

    // Fetch the blending weights for current pixel using PointSampler to avoid bleed:
    float4 a;
    a.x = gBlendTex.SampleLevel(PointSampler, offset.xy, 0).a;  // Right
    a.y = gBlendTex.SampleLevel(PointSampler, offset.zw, 0).g;  // Top
    a.wz = gBlendTex.SampleLevel(PointSampler, texcoord, 0).xz; // Bottom / Left

    float4 origColor = gColorTex.SampleLevel(PointSampler, texcoord, 0);

    // Is there any blending weight with a value greater than 0.0?
    if (dot(a, float4(1.0, 1.0, 1.0, 1.0)) < 1e-5f)
    {
        // Mode 3: 10x difference heatmap (zero diff = pure black)
        if (gProjZAndMode.z > 2.5f)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
        return origColor;
    }

    bool h = max(a.x, a.z) > max(a.y, a.w);

    float4 blendingOffset = float4(0.0, a.y, 0.0, a.w);
    float2 blendingWeight = a.yw;
    if (h)
    {
        blendingOffset = float4(a.x, 0.0, a.z, 0.0);
        blendingWeight = a.xz;
    }
    blendingWeight /= max(dot(blendingWeight, float2(1.0, 1.0)), 1e-5f);

    float4 blendingCoord = mad(blendingOffset, float4(gSmaaRtMetrics.xy, -gSmaaRtMetrics.xy), texcoord.xyxy);

    float4 color = blendingWeight.x * gColorTex.SampleLevel(LinearSampler, blendingCoord.xy, 0);
    color += blendingWeight.y * gColorTex.SampleLevel(LinearSampler, blendingCoord.zw, 0);

    // Mode 3: 10x difference heatmap diagnostic
    [branch]
    if (gProjZAndMode.z > 2.5f)
    {
        return float4(abs(color.rgb - origColor.rgb) * 10.0f, 1.0f);
    }

    return color;
}
)HLSL";

const size_t kSmaaShadersSourceLength = sizeof(kSmaaShadersSource) - 1;

} // namespace routeb
