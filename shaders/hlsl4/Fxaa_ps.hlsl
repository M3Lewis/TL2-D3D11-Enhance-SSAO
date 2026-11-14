// Route B Phase 4: Single-pass Resolve FXAA + edge-masked CAS micro-contrast + highlight shoulder roll-off
struct VS_OUTPUT
{
    float4 Pos       : SV_POSITION;
    float4 Color     : COLOR0;
    float4 ColorSpec : COLOR1;
    float  fogDist   : FOGDISTANCE;
    float4 Texcoord0 : TEXCOORD0;
    float4 Texcoord1 : TEXCOORD1;
    float4 Texcoord2 : TEXCOORD2;
    float4 Texcoord3 : TEXCOORD3;
    float4 Texcoord4 : TEXCOORD4;
    float4 Texcoord5 : TEXCOORD5;
    float4 Texcoord6 : TEXCOORD6;
    float4 Texcoord7 : TEXCOORD7;
};

Texture2D    sceneTexture        : register(t0);
SamplerState sceneTextureSampler : register(s0);
Texture2D    transmissionTexture        : register(t1);
SamplerState transmissionTextureSampler : register(s1);

float4 texelSize;
#ifdef PREVIEW_FXAA
static const float filterAlpha = 1;
float outputTransmission;
#else
static const float filterAlpha = 0;
#endif

static const float3 kLumaRgb = float3(0.299, 0.587, 0.114);
static const float  kCasMaxSharpness = 0.18;
static const float  kShoulderThreshold = 0.92;
static const float  kShoulderHeadroom = 0.08;

float4 sceneSample(float2 uv)
{
    float4 value = sceneTexture.Sample(sceneTextureSampler, uv);
#ifdef PREVIEW_FXAA
    value.a = 1.0 - dot(transmissionTexture.Sample(transmissionTextureSampler, uv).rgb, kLumaRgb);
#endif
    return value;
}

float3 ApplyCasMicroContrast(float2 uv, float2 texel, float3 baseRgb, float3 centerRgb, float edgeDelta)
{
    float3 n = sceneTexture.Sample(sceneTextureSampler, uv + float2( 0.0, -texel.y)).rgb;
    float3 s = sceneTexture.Sample(sceneTextureSampler, uv + float2( 0.0,  texel.y)).rgb;
    float3 w = sceneTexture.Sample(sceneTextureSampler, uv + float2(-texel.x,  0.0)).rgb;
    float3 e = sceneTexture.Sample(sceneTextureSampler, uv + float2( texel.x,  0.0)).rgb;

    float3 minRgb = min(centerRgb, min(min(n, s), min(w, e)));
    float3 maxRgb = max(centerRgb, max(max(n, s), max(w, e)));

    float3 amp = saturate(min(minRgb, 1.0 - maxRgb) / max(maxRgb, float3(1e-4, 1e-4, 1e-4)));
    float  weight = saturate(min(amp.r, min(amp.g, amp.b))) * kCasMaxSharpness;

    // Preserve FXAA anti-aliasing on high-contrast jagged silhouette edges.
    float  edgeMask = 1.0 - saturate(edgeDelta * 5.0);
    weight *= edgeMask;

    float3 ringAvg = 0.25 * (n + s + w + e);
    float3 sharpened = baseRgb + (baseRgb - ringAvg) * weight;
    return clamp(sharpened, minRgb, maxRgb);
}

float3 ApplySoftHighlightShoulder(float3 rgb)
{
    rgb = max(rgb, 0.0);
    float peak = max(rgb.r, max(rgb.g, rgb.b));
    if (peak > kShoulderThreshold)
    {
        float excess = peak - kShoulderThreshold;
        float compressedPeak = kShoulderThreshold + kShoulderHeadroom * (excess / (excess + kShoulderHeadroom));
        rgb *= (compressedPeak / peak);
    }
    return saturate(rgb);
}

float4 postprocess_impl(float2 uv)
{
    float2 texel = texelSize.xy;
    float4 luma = float4(kLumaRgb, filterAlpha);
    float4 centre = sceneSample(uv);
    float lumaM  = dot(centre, luma);
    float lumaNW = dot(sceneSample(uv + float2(-1, -1) * texel), luma);
    float lumaNE = dot(sceneSample(uv + float2( 1, -1) * texel), luma);
    float lumaSW = dot(sceneSample(uv + float2(-1,  1) * texel), luma);
    float lumaSE = dot(sceneSample(uv + float2( 1,  1) * texel), luma);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 dir = float2(-((lumaNW + lumaNE) - (lumaSW + lumaSE)),
                        (lumaNW + lumaSW) - (lumaNE + lumaSE));
    float reduce = max((lumaNW + lumaNE + lumaSW + lumaSE) / 32.0, 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * texel;

    float4 a = 0.5 * (sceneSample(uv + dir * (1.0 / 3.0 - 0.5)) +
                      sceneSample(uv + dir * (2.0 / 3.0 - 0.5)));
    float4 b = a * 0.5 + 0.25 * (sceneSample(uv - dir * 0.5) +
                                 sceneSample(uv + dir * 0.5));
    float lumaB = dot(b, luma);

#ifdef PREVIEW_FXAA
    [flatten] if (outputTransmission > 0)
    {
        a = 0.5 * (transmissionTexture.Sample(transmissionTextureSampler, uv + dir * (1.0 / 3.0 - 0.5)) +
                   transmissionTexture.Sample(transmissionTextureSampler, uv + dir * (2.0 / 3.0 - 0.5)));
        b = a * 0.5 + 0.25 * (transmissionTexture.Sample(transmissionTextureSampler, uv - dir * 0.5) +
                              transmissionTexture.Sample(transmissionTextureSampler, uv + dir * 0.5));
    }
    return (lumaB < lumaMin || lumaB > lumaMax) ? a : b;
#else
    float4 fxaaColor = (lumaB < lumaMin || lumaB > lumaMax) ? a : b;
    float edgeDelta = length(fxaaColor.rgb - centre.rgb);
    float3 casRgb = ApplyCasMicroContrast(uv, texel, fxaaColor.rgb, centre.rgb, edgeDelta);
    float3 resolvedRgb = ApplySoftHighlightShoulder(casRgb);
    return float4(resolvedRgb, centre.a);
#endif
}

float4 main(VS_OUTPUT input) : SV_Target
{
    return postprocess_impl(input.Texcoord0.xy);
}
