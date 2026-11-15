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
#ifndef PREVIEW_FXAA
float4 p2FilterParams;  // x=preset (0=OFF, 1=HOB, 2=DRK), y=applyTonemap (1 when Bloom OFF), z=vignetteEnable
#endif

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

#ifndef PREVIEW_FXAA
float3 HableFilmicCurve(float3 x)
{
    static const float A = 0.24;
    static const float B = 0.34;
    static const float C = 0.10;
    static const float D = 0.22;
    static const float E = 0.018;
    static const float F = 0.32;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - (E / F);
}

float3 ApplyP2HobFilter(float3 rgb, float mode)
{
    float3 c = max(rgb, 0.0);
    float3 lin = pow(c, 2.2);
    float exposure = (mode < 1.5) ? 1.45 : 1.15;
    float whiteScale = rcp(max(HableFilmicCurve(float3(1.60, 1.60, 1.60)).x, 1e-4));
    float3 mapped = saturate(HableFilmicCurve(lin * exposure) * whiteScale);
    float3 graded = pow(mapped, 1.0 / 2.2);

    float luma = dot(graded, float3(0.2126, 0.7152, 0.0722));
    float shadowW = pow(saturate(1.0 - luma * 1.45), 1.75);
    float hiW = smoothstep(0.38, 0.96, luma);

    if (mode < 1.5)
    {
        float3 tint = lerp(float3(1.0, 1.0, 1.0), float3(0.90, 0.98, 1.12), shadowW * 0.65);
        tint *= lerp(float3(1.0, 1.0, 1.0), float3(1.05, 1.01, 0.93), hiW * 0.55);
        graded *= tint;

        float maxC = max(graded.r, max(graded.g, graded.b));
        float minC = min(graded.r, min(graded.g, graded.b));
        float sat = maxC - minC;
        float vib = (1.0 - saturate(sat * 1.35)) * 0.20;
        float gLuma = dot(graded, float3(0.2126, 0.7152, 0.0722));
        graded = lerp(float3(gLuma, gLuma, gLuma), graded, 1.0 + vib);
    }
    else
    {
        float3 tint = lerp(float3(1.0, 1.0, 1.0), float3(0.84, 0.93, 1.14), shadowW * 0.75);
        tint *= lerp(float3(1.0, 1.0, 1.0), float3(1.07, 0.98, 0.87), hiW * 0.60);
        graded *= tint;

        float maxC = max(graded.r, max(graded.g, graded.b));
        float minC = min(graded.r, min(graded.g, graded.b));
        float sat = maxC - minC;
        float vib = (1.0 - saturate(sat * 1.30)) * 0.14;
        float gLuma = dot(graded, float3(0.2126, 0.7152, 0.0722));
        graded = lerp(float3(gLuma, gLuma, gLuma), graded, 1.0 + vib);
    }
    return saturate(graded);
}
float ComputeP2OpticalVignette(float2 uv)
{
    float2 d = (uv - 0.5) * float2(1.0, 0.90);
    float r2 = dot(d, d);
    return 1.0 - 0.16 * smoothstep(0.14, 0.50, r2);
}
#endif

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
    float3 resolvedRgb = (p2FilterParams.x > 0.5 && p2FilterParams.y > 0.5)
        ? ApplyP2HobFilter(casRgb, p2FilterParams.x)
        : ApplySoftHighlightShoulder(casRgb);
    if (p2FilterParams.z > 0.5)
    {
        resolvedRgb *= ComputeP2OpticalVignette(uv);
    }
    return float4(resolvedRgb, centre.a);
#endif
}

float4 main(VS_OUTPUT input) : SV_Target
{
    return postprocess_impl(input.Texcoord0.xy);
}
