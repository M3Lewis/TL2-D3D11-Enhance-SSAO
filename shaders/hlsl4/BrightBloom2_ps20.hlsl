// Route B P2: Soft-knee Luminance Threshold Bloom (restores crisp midtones when F10 filter active)
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

Texture2D    RT        : register(t0);
SamplerState RTSampler : register(s0);

float  Bloom;
float4 p2FilterParams;  // x=preset (0=OFF, 1=HOB, 2=DRK), y=tonemap, z=vignette, w=bloomPyramidOverride

float4 postprocess_impl(float2 texCoord)
{
    if (p2FilterParams.w > 0.5)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }
    float4 tex = RT.Sample(RTSampler, texCoord);
    if (p2FilterParams.x > 0.5)
    {
        float3 col = max(tex.rgb, 0.0);
        float peak = max(col.r, max(col.g, col.b));
        float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
        float energy = lerp(luma, peak, 0.45);

        float threshold = (p2FilterParams.x > 1.5) ? 0.62 : 0.58;
        float knee = 0.22;
        float soft = clamp(energy - threshold + knee, 0.0, 2.0 * knee);
        soft = (soft * soft) / (4.0 * knee + 1e-5);
        float contrib = max(soft, energy - threshold) / max(energy, 1e-4);
        float boost = 1.45 + max(Bloom, 0.0) * 0.65;
        return float4(col * saturate(contrib) * boost, tex.a);
    }

    // Vanilla BrightBloom2 fallback when F10 filter is OFF
    tex -= 1;
    float4 bright4 = -2 * tex * tex + 1.05 + Bloom;
    float bright = dot(bright4, float4(0.333333, 0.333333, 0.333333, 0));
    tex += bright + 0.0 + Bloom * 0.5;
    return tex;
}

float4 main(VS_OUTPUT input) : SV_Target
{
    return postprocess_impl(input.Texcoord0.xy);
}
