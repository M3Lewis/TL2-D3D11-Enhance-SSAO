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

Texture2D    Texture0 : register(t0);
SamplerState Sampler0 : register(s0);
Texture2D    Texture1 : register(t1);
SamplerState Sampler1 : register(s1);

float4 FogColor;
float  FogStart;
float  FogEnd;
float  FogDensity;

// p3Flags:
//   x = stage1 blend op (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2, 5=modulate_x4)
//   y = unused
//   z = fog mode (0=none, 1=exp, 2=exp2, 3=linear)
//   w = stage1 projective flag (0=planar .xy, 1=projective .xy/.w)
float4 p3Flags = float4(0.0, 1.0, 0.0, 0.0);

// p3HdrParams:
//   x = HDR tonemap mode (0=OFF/LDR clamp, 1=ACES Filmic, 2=AgX Punchy)
//   y = Exposure multiplier
float4 p3HdrParams = float4(0.0, 1.0, 0.0, 0.0);

float2 ResolveStageUv(float4 rawUv, bool isProjective)
{
    if (isProjective)
    {
        return (abs(rawUv.w) > 1e-5) ? (rawUv.xy / rawUv.w) : float2(-1.0, -1.0);
    }
    return rawUv.xy;
}

float3 ApplyStage1Op(float3 baseColor, float2 uv, int op)
{
    float4 t1 = Texture1.Sample(Sampler1, uv);
    bool hdr = (p3HdrParams.x > 0.5);
    float3 res = baseColor;
    if (op == 1)
        res = baseColor * t1.rgb;
    else if (op == 2)
        res = baseColor + t1.rgb;
    else if (op == 3)
        res = lerp(baseColor, t1.rgb, t1.a);
    else if (op == 4)
        res = baseColor * (t1.rgb * 2.0);
    else if (op == 5)
        res = baseColor * (t1.rgb * 4.0);
    return hdr ? max(res, 0.0) : saturate(res);
}

float3 ApplyOgreFog(float3 color, float rawFogDist, int fogMode)
{
    if (fogMode <= 0)
        return color;

    float d = abs(rawFogDist);
    float f = 1.0;
    if (fogMode == 1)
        f = saturate(exp(-FogDensity * d));
    else if (fogMode == 2)
    {
        float k = FogDensity * d;
        f = saturate(exp(-(k * k)));
    }
    else if (fogMode == 3)
    {
        f = saturate((FogEnd - d) / max(FogEnd - FogStart, 1e-5));
    }
    return lerp(FogColor.rgb, color, f);
}

float4 main(VS_OUTPUT input) : SV_TARGET
{
    float4 tex0 = Texture0.Sample(Sampler0, input.Texcoord0.xy);
    float3 color = tex0.rgb * input.Color.rgb;

    if (input.Texcoord6.w > 0.5)
    {
        float nLenSq = dot(input.Texcoord6.xyz, input.Texcoord6.xyz);
        float vLenSq = dot(input.Texcoord7.xyz, input.Texcoord7.xyz);
        if (nLenSq > 1e-6 && vLenSq > 1e-6)
        {
            float3 N = input.Texcoord6.xyz * rsqrt(nLenSq);
            float3 V = input.Texcoord7.xyz * rsqrt(vLenSq);
            float wrap = saturate(dot(N, V));
            color *= lerp(0.95, 1.05, wrap);
        }
    }

    int texOp = (int)(p3Flags.x + 0.5);
    if (texOp > 0)
    {
        float2 uv1 = ResolveStageUv(input.Texcoord1, p3Flags.w > 0.5);
        color = ApplyStage1Op(color, uv1, texOp);
    }

    if (p3HdrParams.x > 0.5)
    {
        color = max(color + input.ColorSpec.rgb, 0.0);
    }
    else
    {
        // Soft-knee shoulder compression to keep additive light passes saturated without flat clipping
        float lum = dot(color, float3(0.299, 0.587, 0.114));
        if (lum > 0.78)
        {
            float compressed = 0.78 + (lum - 0.78) / (1.0 + 0.65 * (lum - 0.78));
            color *= compressed / lum;
        }
        color = saturate(color + input.ColorSpec.rgb);
    }
    color = ApplyOgreFog(color, input.fogDist, (int)(p3Flags.z + 0.5));
    return float4(color, saturate(tex0.a * input.Color.a));
}
