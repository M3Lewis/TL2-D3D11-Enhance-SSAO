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
Texture2D    Texture2 : register(t2);
SamplerState Sampler2 : register(s2);
Texture2D    Texture3 : register(t3); // Optional _tl2n.dds (DXT5nm: A = nx, G = ny)
Texture2D    Texture4 : register(t4); // Optional _tl2s.dds (DXT1: R = smoothness, G = metal, B = cavityAO)

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
float4 MaterialAmbient;
float4 MaterialDiffuse;
float4 MaterialEmissive;
float  MaterialPower;
float  AlphaRejectValue;
float4 FogColor;
float  FogStart;
float  FogEnd;
float  FogDensity;

// p3Flags:
//   x = stage1 blend op (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2, 5=modulate_x4)
//   y = alpha reject compare func (Ogre CompareFunction: 0=fail,1=pass,2=<,3<=,4==,5!=,6>=,7>)
//   z = fog mode (0=none, 1=exp, 2=exp2, 3=linear)
//   w = stage1 projective flag (0=planar .xy, 1=projective .xy/.w)
float4 p3Flags = float4(0.0, 1.0, 0.0, 0.0);

// p3Flags2:
//   x = stage2 blend op (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2, 5=modulate_x4)
//   y = stage2 projective flag (0=planar .xy, 1=projective .xy/.w)
//   z = companion _tl2n normal map bound in t3 (0/1)
//   w = companion _tl2s spec/cavity map bound in t4 (0/1)
float4 p3Flags2 = float4(0.0, 0.0, 0.0, 0.0);

// p3EnhanceFlags:
//   x = F11 dynamic PointLight micro-normal relief + GGX specular scale (0..2)
//   z = Phase 3 material clarity, micro-normal relief & directional specular scale (0..2)
//   w = Dynamic weather rain wetness scale (0..1)
float4 p3EnhanceFlags = float4(0.0, 0.0, 1.0, 0.0);

// p3ExtraFlags:
//   x = ProjectorRTT 8-tap rotated Poisson PCF radius in texels (0 disables PCF)
//   y = Cool sky shadow tint strength (0 disables tint)
//   z = Steep POM + micro-self-shadow strength (0..2, 0 disables POM)
//   w = World-space anti-tiling + near-camera micro-grain strength (0..2, 0 disables)
float4 p3ExtraFlags = float4(2.2, 0.75, 1.0, 1.0);

float2 ResolveFixedFuncUv(float4 rawUv, bool isProjective)
{
    if (isProjective)
    {
        return (abs(rawUv.w) > 1e-5) ? (rawUv.xy / rawUv.w) : float2(-1.0, -1.0);
    }
    return rawUv.xy;
}

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

float ShadowIgnHash(float2 pos)
{
    return frac(52.9829189 * frac(dot(pos, float2(0.06711056, 0.00583715))));
}

float4 SampleProjectiveShadowPcf(Texture2D tex, SamplerState samp, float2 uv, float2 screenPos, float radiusTexels)
{
    uint texW = 1, texH = 1;
    tex.GetDimensions(texW, texH);
    float4 result = float4(1.0, 1.0, 1.0, 1.0);
    if (radiusTexels <= 0.01 || texW < 16 || texH < 16)
    {
        result = tex.Sample(samp, uv);
    }
    else
    {
        float2 texelScale = radiusTexels / float2((float)texW, (float)texH);
        float ang = ShadowIgnHash(screenPos) * 6.2831853;
        float s, c;
        sincos(ang, s, c);
        float2x2 rot = float2x2(c, -s, s, c);

        static const float2 kPoisson[8] = {
            float2(-0.7071,  0.7071),
            float2(-0.0000,  0.8750),
            float2( 0.5303,  0.5303),
            float2(-0.9239, -0.3827),
            float2( 0.8750, -0.0000),
            float2(-0.3827, -0.9239),
            float2( 0.3536, -0.3536),
            float2(-0.3536,  0.1464)
        };

        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        [unroll]
        for (int i = 0; i < 8; ++i)
        {
            float2 off = mul(kPoisson[i], rot) * texelScale;
            sum += tex.Sample(samp, uv + off);
        }
        result = sum * 0.125;
    }
    return result;
}

float4 ApplyShadowCoolTint(float4 stageSample, int op, float tintStrength)
{
    if ((op != 1 && op != 4) || tintStrength <= 0.005)
        return stageSample;

    float3 effRgb = (op == 4) ? (stageSample.rgb * 2.0) : stageSample.rgb;
    float lum = saturate(dot(effRgb, float3(0.299, 0.587, 0.114)));
    float shadowMask = saturate((1.0 - lum) * 1.15);
    float3 coolBias = lerp(float3(1.0, 1.0, 1.0), float3(0.88, 0.96, 1.12), clamp(shadowMask * tintStrength, 0.0, 1.0));
    stageSample.rgb = saturate(stageSample.rgb * coolBias);
    return stageSample;
}

float Hash21At(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float ValueNoise2DAt(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = Hash21At(i);
    float b = Hash21At(i + float2(1.0, 0.0));
    float c = Hash21At(i + float2(0.0, 1.0));
    float d = Hash21At(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
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

float3 CombineDiffuseAndSpec(float3 diff, float3 spec)
{
    float3 baseDiff = max(diff, 0.0);
    float3 raw = baseDiff + max(spec, 0.0);

    const float knee = 0.90;
    const float headroom = 1.0 - knee; // 0.10
    float3 over = max(raw - knee, 0.0);
    // Asymptotic compression: as over -> inf, compressed -> knee + headroom = 1.0
    float3 compressed = min(raw, knee) + (over * headroom) / (headroom + over);

    // AgX-style highlight desaturation towards bright white core on extreme highlights (> 0.90)
    float rawLum = dot(raw, float3(0.299, 0.587, 0.114));
    float desat = saturate((rawLum - knee) * 2.8);
    compressed = lerp(compressed, float3(1.0, 1.0, 1.0), desat * 0.65);

    // Critical protection: lantern cores, white hair, bright diffuse retain full brightness to trigger Bloom
    return saturate(max(compressed, min(baseDiff, 1.0)));
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
    float  reliefFade;
    float  smoothness;
    float  metalMask;
};

SurfaceDetail AnalyzeSurfaceDetail(
    float2 uv,
    float2 duvX,
    float2 duvY,
    float3 centerAlbedo,
    float3 baseNormal,
    bool hasNormal,
    float detailWeight,
    bool hasCompanionMaps,
    float p3Scale)
{
    SurfaceDetail sd;
    sd.albedo = centerAlbedo;
    sd.microNormal = baseNormal;
    sd.detailFade = 0.0;
    sd.reliefFade = 0.0;
    sd.smoothness = 0.0;
    sd.metalMask = 0.0;

    // Early-exit when companion normal & spec maps are present:
    // avoids 8 redundant texture fetches and eliminates double crevice AO darkening.
    if (hasCompanionMaps)
    {
        return sd;
    }

    float lenX = length(duvX);
    float lenY = length(duvY);
    float uvFoot = max(lenX, lenY);

    // Isometric camera keeps walls/floors around uvFoot 0.004..0.025; keep relief active without horizon shimmer.
    float fadeFine  = (1.0 - smoothstep(0.008, 0.026, uvFoot)) * detailWeight;
    float fadeMacro = (1.0 - smoothstep(0.010, 0.034, uvFoot)) * detailWeight;

    float3 avgFine = centerAlbedo;
    float3 avgMacro = centerAlbedo;
    float gxFine = 0.0;
    float gyFine = 0.0;
    float gxMacro = 0.0;
    float gyMacro = 0.0;

    if (fadeMacro > 0.01 && uvFoot > 1e-6 && uvFoot < 0.045)
    {
        float maxFineStep = 0.0055;
        float2 fineX = (lenX > maxFineStep) ? (duvX * (maxFineStep / lenX)) : duvX;
        float2 fineY = (lenY > maxFineStep) ? (duvY * (maxFineStep / lenY)) : duvY;

        float maxMacroStep = 0.0080;
        float2 macroX = (lenX > maxMacroStep) ? (duvX * (maxMacroStep / lenX)) : duvX;
        float2 macroY = (lenY > maxMacroStep) ? (duvY * (maxMacroStep / lenY)) : duvY;

        float3 sXp = Texture0.SampleGrad(Sampler0, uv + fineX * 0.85, duvX, duvY).rgb;
        float3 sXm = Texture0.SampleGrad(Sampler0, uv - fineX * 0.85, duvX, duvY).rgb;
        float3 sYp = Texture0.SampleGrad(Sampler0, uv + fineY * 0.85, duvX, duvY).rgb;
        float3 sYm = Texture0.SampleGrad(Sampler0, uv - fineY * 0.85, duvX, duvY).rgb;
        avgFine = (sXp + sXm + sYp + sYm) * 0.25;

        float3 dPP = Texture0.SampleGrad(Sampler0, uv + (macroX + macroY) * 1.45, duvX, duvY).rgb;
        float3 dMP = Texture0.SampleGrad(Sampler0, uv + (-macroX + macroY) * 1.45, duvX, duvY).rgb;
        float3 dPM = Texture0.SampleGrad(Sampler0, uv + (macroX - macroY) * 1.45, duvX, duvY).rgb;
        float3 dMM = Texture0.SampleGrad(Sampler0, uv + (-macroX - macroY) * 1.45, duvX, duvY).rgb;
        avgMacro = (dPP + dMP + dPM + dMM) * 0.25;

        const float3 lumaW = float3(0.299, 0.587, 0.114);
        float lumC = dot(centerAlbedo, lumaW);
        float lumFine = dot(avgFine, lumaW);
        float lumMacro = dot(avgMacro, lumaW);

        gxFine = dot(sXp - sXm, lumaW);
        gyFine = -dot(sYp - sYm, lumaW);
        gxMacro = (dot(dPP + dPM, lumaW) - dot(dMP + dMM, lumaW)) * 0.5;
        gyMacro = -(dot(dPP + dMP, lumaW) - dot(dPM + dMM, lumaW)) * 0.5;

        // Avoid exaggerating faint 8-bit/DXT block noise on ultra-flat gradients while keeping real edges sharp.
        float fineSignal = abs(lumC - lumFine) + max(abs(gxFine), abs(gyFine)) * 0.5;
        float dxtGate = smoothstep(0.005, 0.022, fineSignal);

        // Zero-mean two-scale detail clarity + relative-contrast mortar/crack depth (no global brightness drift).
        float lumHeadroom = saturate(min(lumC, 1.0 - lumC * 0.92) * 5.0);
        float3 fineHF = clamp(centerAlbedo - avgFine, -0.11, 0.11);
        float3 macroHF = clamp(centerAlbedo - avgMacro, -0.10, 0.10);
        float3 enhanced = saturate(
            centerAlbedo +
            (fineHF * (0.62 * fadeFine * dxtGate) + macroHF * (0.48 * fadeMacro)) * lumHeadroom * p3Scale);

        float dip = max(lumFine - lumC, 0.0) * 0.40 + max(lumMacro - lumC, 0.0) * 0.85;
        float relDip = dip / max(lumMacro, 0.12);
        float crevice = 1.0 - saturate(relDip * 0.85) * saturate(0.28 * fadeMacro * smoothstep(0.02, 0.10, lumMacro) * p3Scale);

        sd.albedo = enhanced * crevice;
        sd.detailFade = fadeFine;
        sd.reliefFade = fadeMacro;

        if (hasNormal)
        {
            float gx = gxFine * (1.35 * fadeFine * dxtGate) + gxMacro * (2.20 * fadeMacro);
            float gy = gyFine * (1.35 * fadeFine * dxtGate) + gyMacro * (2.20 * fadeMacro);
            float3 rawGrad = float3(gx, gy, 0.0);
            float3 tanGrad = rawGrad - baseNormal * dot(rawGrad, baseNormal);
            float gradLen = length(tanGrad);
            if (gradLen > 1e-5)
            {
                // Soft-saturation curve: boosts subtle brick/stone bevel slopes while bounding sharp edges cleanly.
                float shapedLen = (gradLen * 2.6) / (1.0 + gradLen * 2.8);
                tanGrad *= (shapedLen / gradLen);
            }
            sd.microNormal = normalize(baseNormal - tanGrad * p3Scale);
        }
    }

    const float3 lumaW2 = float3(0.299, 0.587, 0.114);
    float maxC = max(sd.albedo.r, max(sd.albedo.g, sd.albedo.b));
    float minC = min(sd.albedo.r, min(sd.albedo.g, sd.albedo.b));
    float sat = (maxC > 1e-4) ? ((maxC - minC) / maxC) : 0.0;
    float lum = dot(sd.albedo, lumaW2);
    float lumFine2 = dot(avgFine, lumaW2);
    float lumMacro2 = dot(avgMacro, lumaW2);

    float fineRough = saturate(abs(lum - lumFine2) * 4.2 + (abs(gxFine) + abs(gyFine)) * 1.4);
    float macroVar = saturate(abs(lum - lumMacro2) * 3.0 + (abs(gxMacro) + abs(gyMacro)) * 1.1);
    float roughness = saturate(fineRough * 0.65 + macroVar * 0.35);

    sd.smoothness = saturate((1.0 - roughness * 0.72) * smoothstep(0.08, 0.34, lum));
    // Heavily damp procedural metalness on environment (stone/dirt/wood is not chrome)
    sd.metalMask = (1.0 - smoothstep(0.14, 0.34, sat)) *
                   smoothstep(0.16, 0.30, lum) *
                   (1.0 - smoothstep(0.66, 0.82, lum)) *
                   (1.0 - smoothstep(0.15, 0.45, fineRough)) * 0.35;
    // Non-metals (ground, rock, wood, plaster) MUST be matte; high lum != mirror
    sd.smoothness = min(sd.smoothness, lerp(0.18, 0.95, sd.metalMask));
    return sd;
}

bool BuildCotangentFrame(float3 N, float3 V_unnorm, float2 duv1, float2 duv2, out float3 outT, out float3 outB)
{
    float3 dp1 = ddx(-V_unnorm);
    float3 dp2 = ddy(-V_unnorm);
    float3 dp2perp = cross(N, dp2);
    float3 dp1perp = cross(dp1, N);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float maxSq = max(dot(T, T), dot(B, B));
    if (maxSq < 1e-10)
    {
        outT = float3(1.0, 0.0, 0.0);
        outB = float3(0.0, 1.0, 0.0);
        return false;
    }

    float invmax = rsqrt(maxSq);
    outT = T * invmax;
    outB = B * invmax;
    return true;
}

float SamplePomHeight(float2 sampleUv, float2 duvX, float2 duvY, bool hasSpecMap)
{
    float h = 0.5;
    if (hasSpecMap)
    {
        float4 sp = Texture4.SampleGrad(Sampler0, sampleUv, duvX, duvY);
        h = (sp.a < 0.995) ? sp.a : saturate((sp.b - 0.18) / 0.82);
    }
    else
    {
        float3 alb = Texture0.SampleGrad(Sampler0, sampleUv, duvX, duvY).rgb;
        h = saturate(dot(alb, float3(0.299, 0.587, 0.114)) * 1.15 - 0.10);
    }
    return h;
}

float3 PerturbNormalFromMap(float3 N, float3 V_unnorm, float2 duv1, float2 duv2, float3 tsNormal, float bumpScale)
{
    float3 T, B;
    if (!BuildCotangentFrame(N, V_unnorm, duv1, duv2, T, B))
        return N;

    float3x3 tbn = float3x3(T, B, N);
    float3 scaled = float3(tsNormal.x * bumpScale, tsNormal.y * bumpScale, max(tsNormal.z, 0.05));
    return normalize(mul(scaled, tbn));
}

// Stylized microfacet GGX distribution with normalized peak and physical heavy tail
float EvaluateGgxLobe(float ndh, float smoothness)
{
    float alpha = lerp(0.48, 0.10, smoothness);
    float alpha2 = alpha * alpha;
    float denom = ndh * ndh * (alpha2 - 1.0) + 1.0;
    float ratio = alpha2 / max(denom, 1e-4);
    return ratio * ratio;
}

void AccumulatePointLightRelief(
    float3 lightWorldPos,
    float3 lightDiffuseRgb,
    float3 lightSpecularRgb,
    float  lightRange,
    float3 lightAtten,
    float3 Pview,
    float3 N,
    float3 Nmicro,
    float3 V,
    float  smoothness,
    float  F0,
    float  matScale,
    float3 surfTint,
    float  reliefFade,
    float  detailWeight,
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
    pointReliefSum += lightDiffuseRgb * MaterialDiffuse.rgb * (reliefDelta * atten * 1.25 * reliefFade);

    float3 H = normalize(L + V);
    float HdotV = saturate(dot(H, V));
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);
    float ndh = max(dot(Nmicro, H), 0.0);
    float ggx = EvaluateGgxLobe(ndh, smoothness);
    float termMask = saturate(dot(Nmicro, L) * 2.5 + 0.12);
    float specScalar = ggx * matScale * fresnel * termMask * detailWeight * atten;
    float specLum = dot(lightSpecularRgb, float3(0.299, 0.587, 0.114));
    float3 effSpecCol = (specLum > 1e-4) ? lightSpecularRgb : (lightDiffuseRgb * 0.90);
    specAdd += effSpecCol * MaterialDiffuse.rgb * surfTint * specScalar;
}

float4 ShadeDiffPass(VS_OUTPUT input, bool doAlphaReject)
{
    float2 duvX = ddx(input.Texcoord0.xy);
    float2 duvY = ddy(input.Texcoord0.xy);
    float4 albedo = Texture0.Sample(Sampler0, input.Texcoord0.xy);
    float outAlpha = saturate(albedo.a * input.Color.a);

    float detailWeight = 1.0;
    if (doAlphaReject)
    {
        int cmpFunc = (int)(p3Flags.y + 0.5);
        if (!PassesAlphaReject(outAlpha, AlphaRejectValue, cmpFunc))
            discard;
        if (cmpFunc >= 6 && AlphaRejectValue > 0.01)
        {
            float alphaInterior = smoothstep(AlphaRejectValue, min(AlphaRejectValue + 0.25, 0.96), outAlpha);
            detailWeight = lerp(0.20, 1.0, alphaInterior);
        }
    }

    bool hasNormal = false;
    float3 N = float3(0.0, 0.0, 1.0);
    float3 V = float3(0.0, 0.0, 1.0);
    if (input.Texcoord6.w > 0.5)
    {
        float nLenSq = dot(input.Texcoord6.xyz, input.Texcoord6.xyz);
        float vLenSq = dot(input.Texcoord7.xyz, input.Texcoord7.xyz);
        if (nLenSq > 1e-6 && vLenSq > 1e-6)
        {
            N = input.Texcoord6.xyz * rsqrt(nLenSq);
            V = input.Texcoord7.xyz * rsqrt(vLenSq);
            hasNormal = true;
        }
    }

    float p3Scale = max(p3EnhanceFlags.z, 0.0);
    bool hasNormMap = (p3Flags2.z > 0.5);
    bool hasSpecMap = (p3Flags2.w > 0.5);
    bool hasCompanionMaps = (hasNormMap && hasSpecMap);

    float2 sampleUv = input.Texcoord0.xy;
    float pomHeight = 1.0;
    float pomFade = 0.0;
    float pomScale = max(p3ExtraFlags.z, 0.0);
    float3 tbnT = float3(1.0, 0.0, 0.0);
    float3 tbnB = float3(0.0, 1.0, 0.0);
    bool hasTbn = false;

    if (hasNormal && !doAlphaReject && pomScale > 0.001)
    {
        float uvFootPom = max(length(duvX), length(duvY));
        pomFade = (1.0 - smoothstep(0.012, 0.048, uvFootPom)) * smoothstep(0.22, 0.55, dot(N, V));
        if (pomFade > 0.03)
        {
            hasTbn = BuildCotangentFrame(N, input.Texcoord7.xyz, duvX, duvY, tbnT, tbnB);
            if (hasTbn)
            {
                float2 pDir = float2(dot(V, tbnT), dot(V, tbnB)) / max(dot(V, N), 0.28);
                float2 maxOffset = pDir * (0.015 * min(pomScale, 1.8) * pomFade);
                static const int kPomSteps = 6;
                float layerStep = 1.0 / (float)kPomSteps;
                float2 deltaUv = maxOffset * layerStep;

                float2 curUv = sampleUv;
                float curLayerDepth = 0.0;
                float curHeight = SamplePomHeight(curUv, duvX, duvY, hasSpecMap);

                [unroll]
                for (int ps = 0; ps < kPomSteps; ++ps)
                {
                    if (curLayerDepth < (1.0 - curHeight))
                    {
                        curUv -= deltaUv;
                        curLayerDepth += layerStep;
                        curHeight = SamplePomHeight(curUv, duvX, duvY, hasSpecMap);
                    }
                }

                float2 prevUv = curUv + deltaUv;
                float afterDepth = (1.0 - curHeight) - curLayerDepth;
                float beforeDepth = (1.0 - SamplePomHeight(prevUv, duvX, duvY, hasSpecMap)) - (curLayerDepth - layerStep);
                float denom = afterDepth - beforeDepth;
                float wSec = (abs(denom) > 1e-4) ? saturate(afterDepth / denom) : 0.5;
                sampleUv = lerp(curUv, prevUv, wSec);
                pomHeight = saturate(1.0 - lerp(curLayerDepth, curLayerDepth - layerStep, wSec));
                albedo = Texture0.SampleGrad(Sampler0, sampleUv, duvX, duvY);
            }
        }
    }

    SurfaceDetail sd = AnalyzeSurfaceDetail(sampleUv, duvX, duvY, albedo.rgb, N, hasNormal, detailWeight, hasCompanionMaps, p3Scale);

    if (hasNormMap || hasSpecMap)
    {
        float uvFoot2 = max(length(duvX), length(duvY));
        sd.detailFade = (1.0 - smoothstep(0.014, 0.052, uvFoot2)) * detailWeight;
        sd.reliefFade = (1.0 - smoothstep(0.018, 0.068, uvFoot2)) * detailWeight;

        if (hasSpecMap)
        {
            float3 spSample = Texture4.SampleGrad(Sampler0, sampleUv, duvX, duvY).rgb;
            sd.smoothness = saturate(spSample.r);
            sd.metalMask = saturate(spSample.g);
            float cavityAO = saturate(spSample.b);
            // Single cavity AO without double-multiplying procedural crevice dip
            float aoScale = hasCompanionMaps ? 1.0 : 0.65;
            sd.albedo = saturate(sd.albedo * lerp(1.0, lerp(0.72, 1.00, cavityAO), saturate(sd.detailFade * p3Scale * aoScale)));
        }

        if (hasNormMap && hasNormal && sd.reliefFade > 0.01 && p3Scale > 0.001)
        {
            float4 nSample = Texture3.SampleGrad(Sampler0, sampleUv, duvX, duvY);
            float2 xy = float2(nSample.a, nSample.g) * 2.0 - 1.0;
            float nz = sqrt(saturate(1.0 - dot(xy, xy)));
            sd.microNormal = PerturbNormalFromMap(N, input.Texcoord7.xyz, duvX, duvY, float3(xy, nz), 1.45 * sd.reliefFade * p3Scale);
        }
    }

    float3 litEnv = input.Color.rgb;
    float3 specAdd = float3(0.0, 0.0, 0.0);

    // Pre-sample Texture1 and Texture2 to compute shadow awareness & sky ambient tint
    int op1 = (int)(p3Flags.x + 0.5);
    bool isProj1 = (p3Flags.w > 0.5);
    float4 tSample1 = float4(1.0, 1.0, 1.0, 1.0);
    if (op1 > 0)
    {
        float2 uv1 = ResolveFixedFuncUv(input.Texcoord1, isProj1);
        if (isProj1 && (op1 == 1 || op1 == 4 || op1 == 2) && p3ExtraFlags.x > 0.01)
        {
            float rad1 = (op1 == 2) ? (p3ExtraFlags.x * 0.55) : p3ExtraFlags.x;
            tSample1 = SampleProjectiveShadowPcf(Texture1, Sampler1, uv1, input.Pos.xy, rad1);
            tSample1 = ApplyShadowCoolTint(tSample1, op1, p3ExtraFlags.y);
        }
        else
        {
            tSample1 = Texture1.Sample(Sampler1, uv1);
        }
    }

    int op2 = (int)(p3Flags2.x + 0.5);
    bool isProj2 = (p3Flags2.y > 0.5);
    float4 tSample2 = float4(1.0, 1.0, 1.0, 1.0);
    if (op2 > 0)
    {
        float2 uv2 = ResolveFixedFuncUv(input.Texcoord2, isProj2);
        if (isProj2 && (op2 == 1 || op2 == 4 || op2 == 2) && p3ExtraFlags.x > 0.01)
        {
            float rad2 = (op2 == 2) ? (p3ExtraFlags.x * 0.55) : p3ExtraFlags.x;
            tSample2 = SampleProjectiveShadowPcf(Texture2, Sampler2, uv2, input.Pos.xy, rad2);
            tSample2 = ApplyShadowCoolTint(tSample2, op2, p3ExtraFlags.y);
        }
        else
        {
            tSample2 = Texture2.Sample(Sampler2, uv2);
        }
    }

    // Directional shadow attenuation from projective shadow maps (ProjectorRTT_104 / ProjectorRTTLight_105)
    float dirShadowAtten = 1.0;
    if (isProj1 && op1 == 1)
    {
        dirShadowAtten *= saturate(dot(tSample1.rgb, float3(0.299, 0.587, 0.114)));
    }
    else if (isProj1 && op1 == 4)
    {
        dirShadowAtten *= saturate(dot(tSample1.rgb, float3(0.299, 0.587, 0.114)) * 2.0);
    }

    if (isProj2 && op2 == 1)
    {
        dirShadowAtten *= saturate(dot(tSample2.rgb, float3(0.299, 0.587, 0.114)));
    }
    else if (isProj2 && op2 == 4)
    {
        dirShadowAtten *= saturate(dot(tSample2.rgb, float3(0.299, 0.587, 0.114)) * 2.0);
    }

    if (hasNormal)
    {
        float3 Nmicro = sd.microNormal;
        float NdotV = saturate(dot(Nmicro, V));

        // View-up hemisphere micro-relief replaces camera-headlight bevel
        float3 viewUp = normalize(mul(float3(0.0, 1.0, 0.0), (float3x3)View));
        float worldUpY = dot(Nmicro, viewUp);

        // Module 6: World-space anti-tiling macro variation (8m & 2.8m) + near-camera micro-grain (<14m)
        float antiTileScale = max(p3ExtraFlags.w, 0.0);
        if (antiTileScale > 0.001)
        {
            float3 PviewGeo = -input.Texcoord7.xyz;
            float camDist = length(PviewGeo);
            float groundMask = smoothstep(0.55, 0.85, worldUpY) * detailWeight * (1.0 - sd.metalMask);
            if (groundMask > 0.01)
            {
                float3 Pworld = mul((float3x3)View, PviewGeo - float3(View[3].x, View[3].y, View[3].z));
                float n8 = ValueNoise2DAt(Pworld.xz * 0.125) - 0.5;
                float n28 = ValueNoise2DAt(Pworld.xz * 0.357 + 19.4) - 0.5;
                float macroShift = n8 * 0.65 + n28 * 0.35;
                float str = min(antiTileScale, 2.0) * groundMask;

                float3 macroTint = lerp(float3(0.955, 0.980, 1.035), float3(1.045, 1.015, 0.955), saturate(macroShift + 0.5));
                sd.albedo = saturate(sd.albedo * lerp(float3(1.0, 1.0, 1.0), macroTint, str * 0.85) * (1.0 + macroShift * 0.09 * str));
                sd.smoothness = saturate(sd.smoothness + macroShift * 0.08 * str);

                float nearFade = (1.0 - smoothstep(4.5, 13.5, camDist)) * str;
                if (nearFade > 0.01)
                {
                    float mgA = ValueNoise2DAt(Pworld.xz * 9.5) - 0.5;
                    float mgB = ValueNoise2DAt(Pworld.xz * 9.5 + float2(7.3, 13.7)) - 0.5;
                    sd.albedo = saturate(sd.albedo * (1.0 + mgA * 0.075 * nearFade));
                    if (hasTbn)
                    {
                        Nmicro = normalize(Nmicro + (tbnT * mgA + tbnB * mgB) * (0.18 * nearFade));
                        worldUpY = dot(Nmicro, viewUp);
                    }
                }
            }
        }

        float skyRelief = worldUpY - dot(N, viewUp);
        litEnv *= saturate(1.0 + skyRelief * (0.22 * sd.reliefFade * p3Scale));

        // Rain weather global surface wetness linkage:
        // Rain falls from the sky, wetting upward-facing surfaces (worldUpY > 0.20).
        // 1. Porous dielectric darkening (stone/cloth/ground becomes richer and darker).
        // 2. Liquid water film glossiness: elevates smoothness to produce wet GGX sheen under lights.
        float rainMask = saturate(p3EnhanceFlags.w) * smoothstep(0.20, 0.75, worldUpY);
        sd.albedo *= lerp(1.0, 0.82, rainMask * (1.0 - sd.metalMask));
        sd.smoothness = lerp(sd.smoothness, max(sd.smoothness, 0.80), rainMask);

        // Dual-hemisphere ambient modulation (luminance neutral ~ 1.0):
        // Cool sky (0.96, 1.00, 1.06) on skyward surfaces, warm earth bounce (1.06, 0.99, 0.93) on groundward surfaces
        float hemiT = worldUpY * 0.5 + 0.5;
        float3 hemiTint = lerp(float3(1.06, 0.99, 0.93), float3(0.96, 1.00, 1.06), hemiT);
        float shadowWeight = 1.0;

        float effSmoothness = max(sd.smoothness, sd.metalMask);
        float matScale = lerp(0.22, 0.88, sd.smoothness * sd.smoothness);
        float F0 = lerp(0.04, 0.78, sd.metalMask);
        float3 surfTint = lerp(lerp(float3(1.0, 1.0, 1.0), sd.albedo, 0.15), sd.albedo, sd.metalMask);

        float dirLum = dot(DirectionalLight0_Diffuse.rgb, float3(0.299, 0.587, 0.114));
        if (dirLum > 1e-4)
        {
            float3 rawL0 = mul(-DirectionalLight0_Direction.xyz, (float3x3)View);
            float l0LenSq = dot(rawL0, rawL0);
            if (l0LenSq > 1e-6)
            {
                float3 L0 = rawL0 * rsqrt(l0LenSq);
                float ndl0 = saturate(dot(Nmicro, L0));
                shadowWeight = saturate(1.0 - ndl0 * (0.35 + 0.65 * dirShadowAtten));

                float pomSelfShadow = 1.0;
                if (hasTbn && pomFade > 0.05 && pomHeight < 0.96 && ndl0 > 0.05)
                {
                    float ldotN = max(dot(N, L0), 0.22);
                    float2 lStepUv = float2(dot(L0, tbnT), dot(L0, tbnB)) * (0.012 * min(pomScale, 1.8) / (ldotN * 4.0));
                    float rayH = pomHeight;
                    float hStep = (1.0 - pomHeight) * 0.25;
                    float maxOcc = 0.0;
                    [unroll]
                    for (int ss = 1; ss <= 4; ++ss)
                    {
                        rayH += hStep;
                        float hS = SamplePomHeight(sampleUv + lStepUv * (float)ss, duvX, duvY, hasSpecMap);
                        maxOcc = max(maxOcc, (hS - rayH) * (1.0 - (float)(ss - 1) * 0.18));
                    }
                    pomSelfShadow = lerp(1.0, saturate(1.0 - maxOcc * 2.8), saturate(pomScale) * pomFade);
                }

                // Saturated back-face truncation eliminates ghost relief in shadow
                float relief = ndl0 - saturate(dot(N, L0));
                float effectiveRelief = relief * (0.35 + 0.65 * dirShadowAtten);
                litEnv *= saturate(1.0 + effectiveRelief * (0.60 * sd.reliefFade * p3Scale)) * lerp(0.76, 1.0, pomSelfShadow);

                float ndlRaw = dot(Nmicro, L0);
                float3 H = normalize(L0 + V);
                float HdotV = saturate(dot(H, V));
                float fresnel = F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);
                float ndh = max(dot(Nmicro, H), 0.0);
                float ggx = EvaluateGgxLobe(ndh, effSmoothness);
                float termMask = saturate(ndlRaw * 2.5 + 0.15);
                float specShadowMask = smoothstep(0.25, 0.85, dirShadowAtten) * pomSelfShadow;
                float specScalar = ggx * matScale * fresnel * termMask * detailWeight * p3Scale * specShadowMask;

                specAdd = DirectionalLight0_Diffuse.rgb * MaterialDiffuse.rgb * surfTint * specScalar;

                // Foliage Translucency with normal perturbation, back-face masking & saturation gate
                if (doAlphaReject)
                {
                    float3 backDir = normalize(-L0 + Nmicro * 0.35);
                    float vDotBack = saturate(dot(V, backDir));
                    float backlight = pow(vDotBack, 4.0);
                    float maxC = max(sd.albedo.r, max(sd.albedo.g, sd.albedo.b));
                    float minC = min(sd.albedo.r, min(sd.albedo.g, sd.albedo.b));
                    float sat = (maxC > 1e-4) ? ((maxC - minC) / maxC) : 0.0;
                    float leafSat = smoothstep(0.10, 0.30, sat);
                    float backSide = saturate(1.0 - max(dot(N, L0), 0.0) * 0.7);
                    float transMask = (1.0 - sd.metalMask) * leafSat * backSide * saturate(sd.reliefFade * 1.5);
                    float3 transColor = DirectionalLight0_Diffuse.rgb * sd.albedo * (backlight * 0.24 * transMask * dirShadowAtten);
                    specAdd += transColor;
                }
            }
        }

        // Dual-hemisphere ambient weighting: higher in shadow/ambient crevices, subtler on direct sunlit surfaces
        float hemiWeight = lerp(0.50, 0.95, shadowWeight) * saturate(p3Scale);
        litEnv *= lerp(float3(1.0, 1.0, 1.0), hemiTint, hemiWeight * 0.70);

        float afPointScale = max(p3EnhanceFlags.x, 0.0);
        if (afPointScale > 0.01)
        {
            float3 Pview = -input.Texcoord7.xyz;
            float3 pointReliefSum = float3(0.0, 0.0, 0.0);
            float3 pointSpecSum = float3(0.0, 0.0, 0.0);
            AccumulatePointLightRelief(
                PointLight0_Position, PointLight0_Diffuse.rgb, PointLight0_Specular.rgb,
                PointLight0_Range, PointLight0_Attenuation,
                Pview, N, Nmicro, V, effSmoothness, F0, matScale, surfTint,
                sd.reliefFade, detailWeight, pointReliefSum, pointSpecSum);
            AccumulatePointLightRelief(
                PointLight1_Position, PointLight1_Diffuse.rgb, PointLight1_Specular.rgb,
                PointLight1_Range, PointLight1_Attenuation,
                Pview, N, Nmicro, V, effSmoothness, F0, matScale, surfTint,
                sd.reliefFade, detailWeight, pointReliefSum, pointSpecSum);
            AccumulatePointLightRelief(
                PointLight2_Position, PointLight2_Diffuse.rgb, PointLight2_Specular.rgb,
                PointLight2_Range, PointLight2_Attenuation,
                Pview, N, Nmicro, V, effSmoothness, F0, matScale, surfTint,
                sd.reliefFade, detailWeight, pointReliefSum, pointSpecSum);
            litEnv = max(litEnv + pointReliefSum * (afPointScale * 0.75), float3(0.0, 0.0, 0.0));
            specAdd += pointSpecSum * afPointScale;
        }
    }

    float3 color = CombineDiffuseAndSpec(sd.albedo * litEnv, specAdd);

    // Apply texture stages cleanly without washing out deep shadows
    if (op1 > 0)
    {
        color = ApplyTextureStageOp(color, tSample1, op1);
    }

    if (op2 > 0)
    {
        color = ApplyTextureStageOp(color, tSample2, op2);
    }

    color = saturate(color + input.ColorSpec.rgb);
    color = ApplyOgreFog(color, input.fogDist, (int)(p3Flags.z + 0.5));
    return float4(color, outAlpha);
}

float4 main_opaque(VS_OUTPUT input) : SV_TARGET
{
    return ShadeDiffPass(input, false);
}

float4 main_alpha(VS_OUTPUT input) : SV_TARGET
{
    return ShadeDiffPass(input, true);
}
