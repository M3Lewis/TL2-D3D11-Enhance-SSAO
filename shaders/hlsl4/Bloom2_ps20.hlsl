// Route B P2: Bloom Composite + Uncharted2/Hable Filmic Tonemap & Hob Color Grading
struct VS_OUTPUT {
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

Texture2D    RT           : register(t0);
SamplerState RTSampler    : register(s0);
Texture2D    Blur1        : register(t1);
SamplerState Blur1Sampler : register(s1);

float  OriginalImageWeight;
float  BlurWeight;
float4 p2FilterParams;  // x=preset (0=OFF, 1=HOB, 2=DRK), y=applyTonemap (1=yes)

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

float4 postprocess_impl(float2 texCoord)
{
    float4 sharp = RT.Sample(RTSampler, texCoord);
    float4 blur  = Blur1.Sample(Blur1Sampler, texCoord);
    float4 combined = blur * BlurWeight + sharp * OriginalImageWeight;

    if (p2FilterParams.x > 0.5 && p2FilterParams.y > 0.5)
    {
        return float4(ApplyP2HobFilter(combined.rgb, p2FilterParams.x), combined.a);
    }
    return combined;
}

float4 main(VS_OUTPUT input) : SV_Target
{
    return postprocess_impl(input.Texcoord0.xy);
}
