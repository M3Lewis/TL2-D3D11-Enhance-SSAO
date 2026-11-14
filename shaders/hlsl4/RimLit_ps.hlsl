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
//   x = stage1 blend op (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2, 5=modulate_x4)
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
    if (op == 5)
        return saturate(baseColor * (rimSample.rgb * 4.0));
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

struct SurfaceDetail
{
    float3 albedo;
    float3 microNormal;
    float  detailFade;
    float  smoothness;
    float  metalMask;
};

SurfaceDetail AnalyzeSurfaceDetail(float2 uv, float2 duvX, float2 duvY, float3 centerAlbedo, float3 baseNormal, bool hasNormal)
{
    SurfaceDetail sd;
    sd.albedo = centerAlbedo;
    sd.microNormal = baseNormal;
    sd.detailFade = 0.0;

    float lenX = length(duvX);
    float lenY = length(duvY);
    float uvFoot = max(lenX, lenY);
    float fade = 1.0 - smoothstep(0.003, 0.012, uvFoot);

    float3 avg4 = centerAlbedo;
    float gx = 0.0;
    float gy = 0.0;

    if (fade > 0.01 && uvFoot > 1e-6 && uvFoot < 0.03)
    {
        float maxStep = 0.0025;
        float2 stepX = (lenX > maxStep) ? (duvX * (maxStep / lenX)) : duvX;
        float2 stepY = (lenY > maxStep) ? (duvY * (maxStep / lenY)) : duvY;

        float3 sXp = Texture0.SampleGrad(Sampler0, uv + stepX * 0.75, duvX, duvY).rgb;
        float3 sXm = Texture0.SampleGrad(Sampler0, uv - stepX * 0.75, duvX, duvY).rgb;
        float3 sYp = Texture0.SampleGrad(Sampler0, uv + stepY * 0.75, duvX, duvY).rgb;
        float3 sYm = Texture0.SampleGrad(Sampler0, uv - stepY * 0.75, duvX, duvY).rgb;

        avg4 = (sXp + sXm + sYp + sYm) * 0.25;

        const float3 lumaW = float3(0.299, 0.587, 0.114);
        float lumC = dot(centerAlbedo, lumaW);
        float lumAvg = dot(avg4, lumaW);
        gx = dot(sXp - sXm, lumaW);
        gy = -dot(sYp - sYm, lumaW);

        // Zero-mean 2x micro-detail clarity + subtle crevice etching (without perturbing stylized rim cubemap)
        float lumHeadroom = saturate(min(lumC, 1.0 - lumC) * 4.5);
        float3 highFreq = clamp(centerAlbedo - avg4, -0.06, 0.06);
        float3 enhanced = saturate(centerAlbedo + highFreq * (0.34 * fade * lumHeadroom));

        float dip = max(lumAvg - lumC, 0.0);
        float crevice = 1.0 - saturate(dip * 1.05) * (0.12 * fade);
        sd.albedo = enhanced * crevice;
        sd.detailFade = fade;

        if (hasNormal)
        {
            float3 rawGrad = float3(gx, gy, 0.0);
            float3 tanGrad = rawGrad - baseNormal * dot(rawGrad, baseNormal);
            float gradLen = length(tanGrad);
            if (gradLen > 0.28)
                tanGrad *= (0.28 / gradLen);
            sd.microNormal = normalize(baseNormal - tanGrad * (0.75 * fade));
        }
    }

    float maxC = max(sd.albedo.r, max(sd.albedo.g, sd.albedo.b));
    float minC = min(sd.albedo.r, min(sd.albedo.g, sd.albedo.b));
    float sat = (maxC > 1e-4) ? ((maxC - minC) / maxC) : 0.0;
    float lum = dot(sd.albedo, float3(0.299, 0.587, 0.114));
    float lumAvg = dot(avg4, float3(0.299, 0.587, 0.114));
    float localNoise = saturate(abs(lum - lumAvg) * 3.5 + (abs(gx) + abs(gy)) * 1.5);

    sd.smoothness = saturate((1.0 - localNoise * 0.65) * smoothstep(0.10, 0.35, lum));
    sd.metalMask = (1.0 - smoothstep(0.14, 0.34, sat)) *
                   smoothstep(0.14, 0.28, lum) *
                   (1.0 - smoothstep(0.68, 0.85, lum)) *
                   saturate(1.0 - localNoise * 0.5);
    return sd;
}

float4 main_2d(VS_OUTPUT input) : SV_TARGET
{
    float2 duvX = ddx(input.Texcoord0.xy);
    float2 duvY = ddy(input.Texcoord0.xy);
    float4 albedo = Texture0.Sample(Sampler0, input.Texcoord0.xy);
    float outAlpha = saturate(albedo.a * input.Color.a);

    int cmpFunc = (int)(p3Flags.y + 0.5);
    if (!PassesAlphaReject(outAlpha, AlphaRejectValue, cmpFunc))
        discard;

    bool hasNormal = (input.Texcoord6.w > 0.5 && dot(input.Texcoord6.xyz, input.Texcoord6.xyz) > 1e-6);
    float3 N = hasNormal ? normalize(input.Texcoord6.xyz) : float3(0.0, 0.0, 1.0);
    SurfaceDetail sd = AnalyzeSurfaceDetail(input.Texcoord0.xy, duvX, duvY, albedo.rgb, N, hasNormal);

    float3 base = sd.albedo * input.Color.rgb;
    float2 rimUv = input.Texcoord1.xy;
    float fresnelWeight = 1.0;

    if (hasNormal)
    {
        if (dot(input.Texcoord7.xyz, input.Texcoord7.xyz) > 1e-6)
        {
            float3 V = normalize(input.Texcoord7.xyz);
            float bevel = dot(sd.microNormal, V) - dot(N, V);
            base *= saturate(1.0 + bevel * (0.14 * sd.detailFade));
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
    float2 duvX = ddx(input.Texcoord0.xy);
    float2 duvY = ddy(input.Texcoord0.xy);
    float4 albedo = Texture0.Sample(Sampler0, input.Texcoord0.xy);
    float outAlpha = saturate(albedo.a * input.Color.a);

    int cmpFunc = (int)(p3Flags.y + 0.5);
    if (!PassesAlphaReject(outAlpha, AlphaRejectValue, cmpFunc))
        discard;

    bool hasNormal = (input.Texcoord6.w > 0.5 && dot(input.Texcoord6.xyz, input.Texcoord6.xyz) > 1e-6);
    float3 N = hasNormal ? normalize(input.Texcoord6.xyz) : float3(0.0, 0.0, 1.0);
    SurfaceDetail sd = AnalyzeSurfaceDetail(input.Texcoord0.xy, duvX, duvY, albedo.rgb, N, hasNormal);

    float3 base = sd.albedo * input.Color.rgb;
    float3 cubeDir = input.Texcoord1.xyz;
    float fresnelWeight = 1.0;

    if (hasNormal)
    {
        if (dot(cubeDir, cubeDir) <= 1e-6)
            cubeDir = N;
        if (dot(input.Texcoord7.xyz, input.Texcoord7.xyz) > 1e-6)
        {
            float3 V = normalize(input.Texcoord7.xyz);
            float bevel = dot(sd.microNormal, V) - dot(N, V);
            base *= saturate(1.0 + bevel * (0.14 * sd.detailFade));
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
