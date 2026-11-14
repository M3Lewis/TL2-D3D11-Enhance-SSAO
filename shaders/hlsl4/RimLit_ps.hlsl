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

Texture2D    Texture0     : register(t0);
SamplerState Sampler0     : register(s0);
Texture2D    Texture1     : register(t1);
TextureCube  TextureCube1 : register(t1);
SamplerState Sampler1     : register(s1);

float  AlphaRejectValue;
float4 FogColor;
float  FogStart;
float  FogEnd;
float  FogDensity;

// p3Flags:
//   x = stage1 blend op (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2)
//   y = alpha reject compare func (Ogre CompareFunction: 0=fail,1=pass,2=<,3<=,4==,5!=,6>=,7>)
//   z = fog mode (0=none, 1=exp, 2=exp2, 3=linear)
//   w = unused
float4 p3Flags = float4(2.0, 1.0, 0.0, 1.0);

float3 CombineRim(float3 baseColor, float4 rimSample, int op)
{
    if (op == 1)
        return baseColor * rimSample.rgb;
    if (op == 2)
        return baseColor + rimSample.rgb;
    if (op == 3)
        return lerp(baseColor, rimSample.rgb, rimSample.a);
    if (op == 4)
        return baseColor * (rimSample.rgb * 2.0);
    return baseColor + rimSample.rgb;
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

bool PassesAlphaReject(float alpha, float threshold, int func)
{
    if (func == 0) return false;
    if (func == 2) return alpha < threshold;
    if (func == 3) return alpha <= threshold;
    if (func == 4) return abs(alpha - threshold) < 1e-4;
    if (func == 5) return abs(alpha - threshold) >= 1e-4;
    if (func == 6) return alpha >= threshold;
    if (func == 7) return alpha > threshold;
    return true;
}

float4 main_2d(VS_OUTPUT input) : SV_TARGET
{
    float4 albedo = Texture0.Sample(Sampler0, input.Texcoord0.xy);
    float outAlpha = saturate(albedo.a * input.Color.a);

    int cmpFunc = (int)(p3Flags.y + 0.5);
    if (!PassesAlphaReject(outAlpha, AlphaRejectValue, cmpFunc))
        discard;

    float3 base = albedo.rgb * input.Color.rgb;

    float2 rimUv = input.Texcoord1.xy;
    float fresnelWeight = 1.0;
    if (input.Texcoord6.w > 0.5 && dot(input.Texcoord6.xyz, input.Texcoord6.xyz) > 1e-6)
    {
        float3 N = normalize(input.Texcoord6.xyz);
        rimUv = float2(N.x * 0.5 + 0.5, -N.y * 0.5 + 0.5);
        if (dot(input.Texcoord7.xyz, input.Texcoord7.xyz) > 1e-6)
        {
            float3 V = normalize(input.Texcoord7.xyz);
            fresnelWeight = lerp(0.85, 1.15, pow(1.0 - saturate(dot(N, V)), 1.6));
        }
    }

    float4 rimSample = Texture1.Sample(Sampler1, rimUv);
    int texOp = (int)(p3Flags.x + 0.5);
    float3 color = CombineRim(base, float4(rimSample.rgb * fresnelWeight, rimSample.a), texOp);

    color = saturate(color + input.ColorSpec.rgb);
    color = ApplyOgreFog(color, input.fogDist, (int)(p3Flags.z + 0.5));
    return float4(color, outAlpha);
}

float4 main_cube(VS_OUTPUT input) : SV_TARGET
{
    float4 albedo = Texture0.Sample(Sampler0, input.Texcoord0.xy);
    float outAlpha = saturate(albedo.a * input.Color.a);

    int cmpFunc = (int)(p3Flags.y + 0.5);
    if (!PassesAlphaReject(outAlpha, AlphaRejectValue, cmpFunc))
        discard;

    float3 base = albedo.rgb * input.Color.rgb;

    float3 cubeDir = input.Texcoord1.xyz;
    float fresnelWeight = 1.0;
    if (input.Texcoord6.w > 0.5 && dot(input.Texcoord6.xyz, input.Texcoord6.xyz) > 1e-6)
    {
        float3 N = normalize(input.Texcoord6.xyz);
        cubeDir = N;
        if (dot(input.Texcoord7.xyz, input.Texcoord7.xyz) > 1e-6)
        {
            float3 V = normalize(input.Texcoord7.xyz);
            fresnelWeight = lerp(0.85, 1.15, pow(1.0 - saturate(dot(N, V)), 1.6));
        }
    }

    float4 rimSample = TextureCube1.Sample(Sampler1, cubeDir);
    int texOp = (int)(p3Flags.x + 0.5);
    float3 color = CombineRim(base, float4(rimSample.rgb * fresnelWeight, rimSample.a), texOp);

    color = saturate(color + input.ColorSpec.rgb);
    color = ApplyOgreFog(color, input.fogDist, (int)(p3Flags.z + 0.5));
    return float4(color, outAlpha);
}
