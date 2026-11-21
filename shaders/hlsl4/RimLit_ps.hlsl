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
TextureCube  TextureCube2 : register(t2);
SamplerState Sampler2     : register(s2);

float4x4 View;
float4 BaseLightAmbient;
float4 DirectionalLight0_Direction;
float4 DirectionalLight0_Diffuse;
float4 PointLight0_Diffuse;
float4 PointLight0_Specular;
float3 PointLight0_Position;
float  PointLight0_Range;
float3 PointLight0_Attenuation;
float4 PointLight1_Diffuse;
float4 PointLight1_Specular;
float3 PointLight1_Position;
float  PointLight1_Range;
float3 PointLight1_Attenuation;
float4 PointLight2_Diffuse;
float4 PointLight2_Specular;
float3 PointLight2_Position;
float  PointLight2_Range;
float3 PointLight2_Attenuation;
float4 MaterialDiffuse;
float  MaterialPower;

float  AlphaRejectValue;
float4 FogColor;
float  FogStart;
float  FogEnd;
float  FogDensity;

// p3Flags:
//   x = rim stage blend op (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2, 5=modulate_x4)
//   y = alpha reject compare func (Ogre CompareFunction: 0=fail,1=pass,2=<,3<=,4==,5!=,6>=,7>)
//   z = fog mode (0=none, 1=exp, 2=exp2, 3=linear)
//   w = unused
float4 p3Flags = float4(2.0, 1.0, 0.0, 1.0);

// p3Flags2:
//   x = stage1 blend op in 3-stage RimLit (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2, 5=modulate_x4)
//   y = enviro coord swap in 3-stage RimLit (0: tc1=uv2D, tc2=cubeDir; 1: tc1=cubeDir, tc2=uv2D)
//   z = unused
//   w = isSkinned VS (0=fixed-func VS using FFP light uniforms, 1=HardwareSkinning* using Texcoord3..5)
float4 p3Flags2 = float4(0.0, 0.0, 0.0, 0.0);

// p3EnhanceFlags:
//   x = F11 AF+PointLight scale (0..2)
//   y = P0-1 RimLit dynamic relief, metallic specular & 3-stage _GLO boost scale (0..2)
//   z = Phase 3 material clarity & micro-normal slope scale (0..2)
float4 p3EnhanceFlags = float4(1.0, 1.0, 1.0, 0.0);

float3 ApplyTextureStageOp(float3 baseColor, float4 tSample, int op)
{
    if (op == 1)
        return saturate(baseColor * tSample.rgb);
    if (op == 2)
        return saturate(baseColor + tSample.rgb);
    if (op == 3)
        return saturate(lerp(baseColor, tSample.rgb, tSample.a));
    if (op == 4)
        return saturate(baseColor * (tSample.rgb * 2.0));
    if (op == 5)
        return saturate(baseColor * (tSample.rgb * 4.0));
    return baseColor;
}

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

SurfaceDetail AnalyzeSurfaceDetail(float2 uv, float2 duvX, float2 duvY, float3 centerAlbedo, float3 baseNormal, bool hasNormal, float p3Scale)
{
    SurfaceDetail sd;
    sd.albedo = centerAlbedo;
    sd.microNormal = baseNormal;
    sd.detailFade = 0.0;

    float lenX = length(duvX);
    float lenY = length(duvY);
    float uvFoot = max(lenX, lenY);
    float fade = 1.0 - smoothstep(0.004, 0.016, uvFoot);

    float3 avg4 = centerAlbedo;
    float gx = 0.0;
    float gy = 0.0;

    if (fade > 0.01 && uvFoot > 1e-6 && uvFoot < 0.04)
    {
        float maxStep = 0.0030;
        float2 stepX = (lenX > maxStep) ? (duvX * (maxStep / lenX)) : duvX;
        float2 stepY = (lenY > maxStep) ? (duvY * (maxStep / lenY)) : duvY;

        float3 sXp = Texture0.SampleGrad(Sampler0, uv + stepX * 0.85, duvX, duvY).rgb;
        float3 sXm = Texture0.SampleGrad(Sampler0, uv - stepX * 0.85, duvX, duvY).rgb;
        float3 sYp = Texture0.SampleGrad(Sampler0, uv + stepY * 0.85, duvX, duvY).rgb;
        float3 sYm = Texture0.SampleGrad(Sampler0, uv - stepY * 0.85, duvX, duvY).rgb;

        avg4 = (sXp + sXm + sYp + sYm) * 0.25;

        const float3 lumaW = float3(0.299, 0.587, 0.114);
        float lumC = dot(centerAlbedo, lumaW);
        float lumAvg = dot(avg4, lumaW);
        gx = dot(sXp - sXm, lumaW);
        gy = -dot(sYp - sYm, lumaW);

        // Zero-mean micro-detail clarity + subtle crevice etching
        float lumHeadroom = saturate(min(lumC, 1.0 - lumC) * 4.5);
        float3 highFreq = clamp(centerAlbedo - avg4, -0.07, 0.07);
        float3 enhanced = saturate(centerAlbedo + highFreq * (0.42 * fade * lumHeadroom * p3Scale));

        float dip = max(lumAvg - lumC, 0.0);
        float crevice = 1.0 - saturate(dip * 1.15) * saturate(0.16 * fade * p3Scale);
        sd.albedo = enhanced * crevice;
        sd.detailFade = fade;

        if (hasNormal)
        {
            float3 rawGrad = float3(gx * 1.65, gy * 1.65, 0.0);
            float3 tanGrad = rawGrad - baseNormal * dot(rawGrad, baseNormal);
            float gradLen = length(tanGrad);
            if (gradLen > 1e-5)
            {
                float shapedLen = (gradLen * 1.8) / (1.0 + gradLen * 2.4);
                tanGrad *= (shapedLen / gradLen);
            }
            sd.microNormal = normalize(baseNormal - tanGrad * (fade * p3Scale));
        }
    }

    float maxC = max(sd.albedo.r, max(sd.albedo.g, sd.albedo.b));
    float minC = min(sd.albedo.r, min(sd.albedo.g, sd.albedo.b));
    float sat = (maxC > 1e-4) ? ((maxC - minC) / maxC) : 0.0;
    float lum = dot(sd.albedo, float3(0.299, 0.587, 0.114));
    float lumAvg = dot(avg4, float3(0.299, 0.587, 0.114));
    float localNoise = saturate(abs(lum - lumAvg) * 3.5 + (abs(gx) + abs(gy)) * 1.5);

    sd.smoothness = saturate((1.0 - localNoise * 0.62) * smoothstep(0.08, 0.34, lum));
    sd.metalMask = (1.0 - smoothstep(0.16, 0.38, sat)) *
                   smoothstep(0.14, 0.28, lum) *
                   (1.0 - smoothstep(0.72, 0.88, lum)) *
                   saturate(1.0 - localNoise * 0.45);
    // Non-metals (skin, beard, fabric, wood, stone) MUST be matte; high lum != polished mirror
    sd.smoothness = min(sd.smoothness, lerp(0.18, 0.95, sd.metalMask));
    return sd;
}

void AccumulatePointLightRim(
    float3 lightWorldPos,
    float3 lightDiffuseRgb,
    float3 lightSpecularRgb,
    float  lightRange,
    float3 lightAtten,
    float3 Pview,
    float3 N,
    float3 Nmicro,
    float3 V,
    float  specPow,
    float  fresnel,
    float  matScale,
    float3 surfTint,
    float  reliefFade,
    inout float3 pointReliefSum,
    inout float3 specAdd)
{
    float lightLum = dot(lightDiffuseRgb, float3(0.299, 0.587, 0.114));
    if (lightLum <= 1e-4 || lightRange <= 0.01)
        return;

    float3 lightViewPos = mul(float4(lightWorldPos, 1.0), View).xyz;
    float3 deltaView = lightViewPos - Pview;
    float distSq = dot(deltaView, deltaView);
    if (distSq <= 1e-6 || distSq > lightRange * lightRange)
        return;

    float dist = sqrt(distSq);
    float3 L = deltaView / dist;
    float attenDenom = dot(lightAtten, float3(1.0, dist, distSq));
    float atten = min(1.0 / max(attenDenom, 1e-5), 2.0);
    float rangeT = dist / lightRange;
    float rangeSq = rangeT * rangeT;
    atten *= saturate(1.0 - rangeSq * rangeSq);

    float ndlBase = saturate(dot(N, L));
    float ndlMicro = saturate(dot(Nmicro, L));
    float reliefDelta = ndlMicro - ndlBase;
    pointReliefSum += lightDiffuseRgb * MaterialDiffuse.rgb * (reliefDelta * atten * 1.15 * reliefFade);

    float3 H = normalize(L + V);
    float ndh = max(dot(Nmicro, H), 0.0);
    float termMask = saturate(dot(Nmicro, L) * 2.5 + 0.15);
    float specScalar = pow(ndh, specPow) * matScale * fresnel * termMask * atten;
    float specLum = dot(lightSpecularRgb, float3(0.299, 0.587, 0.114));
    float3 effSpecCol = (specLum > 1e-4) ? lightSpecularRgb : (lightDiffuseRgb * 0.90);
    specAdd += effSpecCol * MaterialDiffuse.rgb * surfTint * specScalar;
}

void ComputeRimLitLighting(
    VS_OUTPUT input,
    SurfaceDetail sd,
    float3 N,
    bool hasNormal,
    out float3 litBase,
    out float fresnelWeight)
{
    litBase = sd.albedo * input.Color.rgb;
    fresnelWeight = 1.0;

    if (!hasNormal)
        return;

    float vLenSq = dot(input.Texcoord7.xyz, input.Texcoord7.xyz);
    if (vLenSq <= 1e-6)
        return;

    float3 V = input.Texcoord7.xyz * rsqrt(vLenSq);
    float3 Nmicro = sd.microNormal;

    float rimScale = max(p3EnhanceFlags.y, 0.0);
    if (rimScale <= 0.01)
    {
        float bevel = dot(Nmicro, V) - dot(N, V);
        litBase *= saturate(1.0 + bevel * (0.14 * sd.detailFade));
        fresnelWeight = lerp(0.85, 1.15, pow(1.0 - saturate(dot(N, V)), 1.6));
        return;
    }

    // P0-1 Enhanced RimLit: micro-normal dynamic relief + metallic/chitin specular
    float3 litEnv = input.Color.rgb;
    float3 specAdd = float3(0.0, 0.0, 0.0);

    float NdotV = saturate(dot(Nmicro, V));
    float bevel = dot(Nmicro, V) - dot(N, V);
    litEnv *= saturate(1.0 + bevel * (0.24 * sd.detailFade * rimScale));
    // Rim light should be a crisp silhouette contour, not a flat frontal wash
    fresnelWeight = lerp(0.12, 1.15, pow(1.0 - NdotV, 2.6)) * (0.70 + 0.35 * rimScale);

    float basePow = clamp((MaterialPower > 1.0) ? MaterialPower : 24.0, 14.0, 64.0);
    float specPow = lerp(36.0, 96.0, max(sd.smoothness, sd.metalMask));
    float fresnelSpec = lerp(0.20, 0.85, pow(1.0 - NdotV, 2.5));
    float matScale = lerp(0.015, 0.05, sd.smoothness * sd.smoothness) + sd.metalMask * 0.22;
    float3 surfTint = lerp(sd.albedo, float3(1.0, 1.0, 1.0), sd.metalMask * 0.40);
    float afPointScale = max(p3EnhanceFlags.x, 0.0);

    if (p3Flags2.w > 0.5)
    {
        // HardwareSkinning* path (monsters, bosses, characters, armor)
        float l0LenSq = dot(input.Texcoord5.xyz, input.Texcoord5.xyz);
        if (l0LenSq > 1e-6 && dot(input.Texcoord4.rgb, float3(0.299, 0.587, 0.114)) > 1e-4)
        {
            float3 L0 = input.Texcoord5.xyz * rsqrt(l0LenSq);
            float relief0 = dot(Nmicro, L0) - dot(N, L0);
            litEnv *= saturate(1.0 + relief0 * (0.45 * sd.detailFade * rimScale));

            // Soft organic wrapped diffuse for warm flesh / skin tones (only when saturated, not on white hair/beard)
            float ndlRaw0 = dot(Nmicro, L0);
            float maxC = max(sd.albedo.r, max(sd.albedo.g, sd.albedo.b));
            float minC = min(sd.albedo.r, min(sd.albedo.g, sd.albedo.b));
            float sat = (maxC > 1e-4) ? ((maxC - minC) / maxC) : 0.0;
            float isFlesh = saturate(1.0 - sd.metalMask * 1.8) * smoothstep(0.12, 0.35, sat);
            if (isFlesh > 0.05)
            {
                float wrapNdl0 = saturate((ndlRaw0 + 0.30) / 1.30);
                float scatterTerm0 = pow(1.0 - abs(ndlRaw0), 2.2) * wrapNdl0;
                float3 sssColor0 = float3(0.85, 0.35, 0.15) * (scatterTerm0 * 0.12 * isFlesh * rimScale);
                litEnv += sssColor0;
            }

            float3 H0 = normalize(L0 + V);
            float ndh0 = max(dot(Nmicro, H0), 0.0);
            float ndlMask0 = saturate(ndlRaw0 * 2.5 + 0.18);
            float specScalar0 = pow(ndh0, specPow) * matScale * fresnelSpec * ndlMask0;
            // No redundant rimGlance! CombineRim already adds the material's rim texture.
            specAdd += input.Texcoord4.rgb * surfTint * (specScalar0 * rimScale);
        }

        float l1LenSq = dot(input.Texcoord3.xyz, input.Texcoord3.xyz);
        if (afPointScale > 0.01 && input.Texcoord3.w > 1e-4 && l1LenSq > 1e-6)
        {
            float3 L1 = input.Texcoord3.xyz * rsqrt(l1LenSq);
            float relief1 = saturate(dot(Nmicro, L1)) - saturate(dot(N, L1));
            float l1Strength = saturate(input.Texcoord3.w) * afPointScale;
            float3 envTint = input.Color.rgb / max(max(input.Color.r, max(input.Color.g, input.Color.b)), 0.05);
            float3 l1Tint = lerp(float3(1.0, 0.92, 0.82), saturate(envTint), 0.65);

            litEnv = max(litEnv + l1Tint * (relief1 * l1Strength * 0.65 * sd.detailFade * rimScale), float3(0.0, 0.0, 0.0));

            float3 H1 = normalize(L1 + V);
            float ndh1 = max(dot(Nmicro, H1), 0.0);
            float ndlMask1 = saturate(dot(Nmicro, L1) * 2.5 + 0.15);
            float specScalar1 = pow(ndh1, specPow) * matScale * fresnelSpec * ndlMask1 * l1Strength;
            specAdd += l1Tint * surfTint * (specScalar1 * rimScale);
        }
    }
    else
    {
        // Fixed-function VS path (unskeletonized weapons/cannons/shields/props)
        float dirLum = dot(DirectionalLight0_Diffuse.rgb, float3(0.299, 0.587, 0.114));
        if (dirLum > 1e-4)
        {
            float3 rawL0 = mul(-DirectionalLight0_Direction.xyz, (float3x3)View);
            float l0LenSq = dot(rawL0, rawL0);
            if (l0LenSq > 1e-6)
            {
                float3 L0 = rawL0 * rsqrt(l0LenSq);
                float relief = dot(Nmicro, L0) - dot(N, L0);
                litEnv *= saturate(1.0 + relief * (0.50 * sd.detailFade * rimScale));

                float ndlRaw = dot(Nmicro, L0);
                float maxC = max(sd.albedo.r, max(sd.albedo.g, sd.albedo.b));
                float minC = min(sd.albedo.r, min(sd.albedo.g, sd.albedo.b));
                float sat = (maxC > 1e-4) ? ((maxC - minC) / maxC) : 0.0;
                float isFlesh = saturate(1.0 - sd.metalMask * 1.8) * smoothstep(0.12, 0.35, sat);
                if (isFlesh > 0.05)
                {
                    float wrapNdl = saturate((ndlRaw + 0.30) / 1.30);
                    float scatterTerm = pow(1.0 - abs(ndlRaw), 2.2) * wrapNdl;
                    float3 sssColor = float3(0.85, 0.35, 0.15) * (scatterTerm * 0.12 * isFlesh * rimScale);
                    litEnv += sssColor;
                }

                float3 H = normalize(L0 + V);
                float ndh = max(dot(Nmicro, H), 0.0);
                float termMask = saturate(ndlRaw * 2.5 + 0.15);
                float specScalar = pow(ndh, specPow) * matScale * fresnelSpec * termMask;
                specAdd += DirectionalLight0_Diffuse.rgb * MaterialDiffuse.rgb * surfTint * (specScalar * rimScale);
            }
        }

        if (afPointScale > 0.01)
        {
            float3 Pview = -input.Texcoord7.xyz;
            float3 pointReliefSum = float3(0.0, 0.0, 0.0);
            float3 pointSpecSum = float3(0.0, 0.0, 0.0);
            AccumulatePointLightRim(
                PointLight0_Position, PointLight0_Diffuse.rgb, PointLight0_Specular.rgb,
                PointLight0_Range, PointLight0_Attenuation,
                Pview, N, Nmicro, V, specPow, fresnelSpec, matScale, surfTint, sd.detailFade,
                pointReliefSum, pointSpecSum);
            AccumulatePointLightRim(
                PointLight1_Position, PointLight1_Diffuse.rgb, PointLight1_Specular.rgb,
                PointLight1_Range, PointLight1_Attenuation,
                Pview, N, Nmicro, V, specPow, fresnelSpec, matScale, surfTint, sd.detailFade,
                pointReliefSum, pointSpecSum);
            AccumulatePointLightRim(
                PointLight2_Position, PointLight2_Diffuse.rgb, PointLight2_Specular.rgb,
                PointLight2_Range, PointLight2_Attenuation,
                Pview, N, Nmicro, V, specPow, fresnelSpec, matScale, surfTint, sd.detailFade,
                pointReliefSum, pointSpecSum);
            litEnv = max(litEnv + pointReliefSum * (afPointScale * rimScale), float3(0.0, 0.0, 0.0));
            specAdd += pointSpecSum * (afPointScale * rimScale);
        }
    }

    litBase = saturate(sd.albedo * litEnv + specAdd);
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
    SurfaceDetail sd = AnalyzeSurfaceDetail(input.Texcoord0.xy, duvX, duvY, albedo.rgb, N, hasNormal, max(p3EnhanceFlags.z, 0.0));

    float3 base = 0.0;
    float fresnelWeight = 1.0;
    ComputeRimLitLighting(input, sd, N, hasNormal, base, fresnelWeight);

    float2 rimUv = input.Texcoord1.xy;
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
    SurfaceDetail sd = AnalyzeSurfaceDetail(input.Texcoord0.xy, duvX, duvY, albedo.rgb, N, hasNormal, max(p3EnhanceFlags.z, 0.0));

    float3 base = 0.0;
    float fresnelWeight = 1.0;
    ComputeRimLitLighting(input, sd, N, hasNormal, base, fresnelWeight);

    float3 cubeDir = input.Texcoord1.xyz;
    float rimScale = max(p3EnhanceFlags.y, 0.0);
    if (hasNormal)
    {
        if (dot(cubeDir, cubeDir) <= 1e-6)
            cubeDir = N;
        if (rimScale > 0.01 && sd.detailFade > 0.01)
            cubeDir = normalize(cubeDir + (sd.microNormal - N) * (0.45 * sd.detailFade * rimScale));
    }

    float4 rimSample = TextureCube1.Sample(Sampler1, cubeDir);
    int texOp = (int)(p3Flags.x + 0.5);
    float3 color = CombineRim(base, float4(rimSample.rgb * fresnelWeight, rimSample.a), texOp);

    color = saturate(color + input.ColorSpec.rgb);
    color = ApplyOgreFog(color, input.fogDist, (int)(p3Flags.z + 0.5));
    return float4(color, outAlpha);
}

float4 main_cube3(VS_OUTPUT input) : SV_TARGET
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
    SurfaceDetail sd = AnalyzeSurfaceDetail(input.Texcoord0.xy, duvX, duvY, albedo.rgb, N, hasNormal, max(p3EnhanceFlags.z, 0.0));

    float3 base = 0.0;
    float fresnelWeight = 1.0;
    ComputeRimLitLighting(input, sd, N, hasNormal, base, fresnelWeight);

    bool enviroSwap = (p3Flags2.y > 0.5);
    float2 stage1Uv = enviroSwap ? input.Texcoord2.xy : input.Texcoord1.xy;
    float3 cubeDir  = enviroSwap ? input.Texcoord1.xyz : input.Texcoord2.xyz;

    float rimScale = max(p3EnhanceFlags.y, 0.0);
    int stage1Op = (int)(p3Flags2.x + 0.5);
    if (stage1Op > 0)
    {
        float4 stage1Sample = Texture1.Sample(Sampler1, stage1Uv);
        if (stage1Op == 2 && rimScale > 0.01)
        {
            // Accentuate _GLO emissive runes/backpack cores cleanly
            stage1Sample.rgb *= (1.0 + 0.24 * rimScale);
        }
        base = ApplyTextureStageOp(base, stage1Sample, stage1Op);
    }

    if (hasNormal)
    {
        if (dot(cubeDir, cubeDir) <= 1e-6)
            cubeDir = N;
        if (rimScale > 0.01 && sd.detailFade > 0.01)
            cubeDir = normalize(cubeDir + (sd.microNormal - N) * (0.45 * sd.detailFade * rimScale));
    }

    float4 rimSample = TextureCube2.Sample(Sampler2, cubeDir);
    int texOp = (int)(p3Flags.x + 0.5);
    float3 color = CombineRim(base, float4(rimSample.rgb * fresnelWeight, rimSample.a), texOp);

    color = saturate(color + input.ColorSpec.rgb);
    color = ApplyOgreFog(color, input.fogDist, (int)(p3Flags.z + 0.5));
    return float4(color, outAlpha);
}
