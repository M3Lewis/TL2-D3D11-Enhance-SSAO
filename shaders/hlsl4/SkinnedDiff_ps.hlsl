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
Texture2D    Texture3 : register(t3); // Optional _tl2n.dds (DXT5nm: A = nx, G = ny)
Texture2D    Texture4 : register(t4); // Optional _tl2s.dds (DXT1: R = smoothness, G = metal, B = cavityAO)

float  AlphaRejectValue;
float4 FogColor;
float  FogStart;
float  FogEnd;
float  FogDensity;

// p3Flags:
//   x = stage1 blend op (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2, 5=modulate_x4)
//   y = alpha reject compare func (Ogre CompareFunction: 0=fail,1=pass,2=<,3<=,4==,5!=,6>=,7>)
//   z = fog mode (0=none, 1=exp, 2=exp2, 3=linear)
//   w = stage1 texcoord selector (0=tc0, 1=tc1, 2=tc2)
float4 p3Flags = float4(0.0, 1.0, 0.0, 1.0);

// p3Flags2:
//   z = companion _tl2n normal map bound in t3 (0/1)
//   w = companion _tl2s spec/cavity map bound in t4 (0/1)
float4 p3Flags2 = float4(0.0, 0.0, 0.0, 0.0);

// p3EnhanceFlags:
//   x = F11 dynamic PointLight micro-normal relief + Blinn-Phong specular scale (0..2)
//   z = Phase 3 material clarity, micro-normal relief & directional specular scale (0..2)
float4 p3EnhanceFlags = float4(0.0, 0.0, 1.0, 0.0);

float3 ApplyStage1Op(float3 baseColor, float2 uv, int op)
{
    float4 t1 = Texture1.Sample(Sampler1, uv);
    if (op == 1)
        return baseColor * t1.rgb;
    if (op == 2)
        return baseColor + t1.rgb;
    if (op == 3)
        return lerp(baseColor, t1.rgb, t1.a);
    if (op == 4)
        return baseColor * (t1.rgb * 2.0);
    if (op == 5)
        return saturate(baseColor * (t1.rgb * 4.0));
    return baseColor;
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

float3 SoftHighlightClamp(float3 c)
{
    // Below 0.80: 100% linear pass-through to preserve native contrast.
    // Above 0.80: smooth C1 roll-off asymptotically approaching 1.0, preventing blown-out white clipping.
    const float knee = 0.80;
    const float maxOver = 1.0 - knee; // 0.20
    float3 over = max(c - knee, 0.0);
    return min(c, knee) + over / (1.0 + over / maxOver);
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

SurfaceDetail AnalyzeSurfaceDetail(
    float2 uv,
    float2 duvX,
    float2 duvY,
    float3 centerAlbedo,
    float3 baseNormal,
    bool hasNormal,
    bool hasCompanionMaps,
    float p3Scale)
{
    SurfaceDetail sd;
    sd.albedo = centerAlbedo;
    sd.microNormal = baseNormal;
    sd.detailFade = 0.0;
    sd.smoothness = 0.0;
    sd.metalMask = 0.0;

    // Early-exit when companion normal & spec maps are present:
    // avoids 4 redundant texture fetches and eliminates double crevice AO darkening.
    if (hasCompanionMaps)
    {
        return sd;
    }

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

        // Filter out DXT1 block artifacts on smooth skin/clothing
        float fineSignal = abs(lumC - lumAvg) + max(abs(gx), abs(gy)) * 0.5;
        float dxtGate = smoothstep(0.005, 0.022, fineSignal);

        float lumHeadroom = saturate(min(lumC, 1.0 - lumC) * 4.5);
        float3 highFreq = clamp(centerAlbedo - avg4, -0.06, 0.06);
        float3 enhanced = saturate(centerAlbedo + highFreq * (0.34 * fade * lumHeadroom * dxtGate * p3Scale));

        float dip = max(lumAvg - lumC, 0.0);
        float crevice = 1.0 - saturate(dip * 1.05) * saturate(0.12 * fade * p3Scale);
        sd.albedo = enhanced * crevice;
        sd.detailFade = fade;

        if (hasNormal)
        {
            float3 rawGrad = float3(gx, gy, 0.0) * dxtGate;
            float3 tanGrad = rawGrad - baseNormal * dot(rawGrad, baseNormal);
            float gradLen = length(tanGrad);
            if (gradLen > 0.28)
                tanGrad *= (0.28 / gradLen);
            sd.microNormal = normalize(baseNormal - tanGrad * (0.75 * fade * p3Scale));
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
    // Non-metals (skin, beard, fabric, wood, stone) MUST be matte; high lum != mirror!
    sd.smoothness = min(sd.smoothness, lerp(0.18, 0.95, sd.metalMask));
    return sd;
}

float3 PerturbNormalFromMap(float3 N, float3 V_unnorm, float2 uv, float3 tsNormal, float bumpScale)
{
    float3 dp1 = ddx(-V_unnorm);
    float3 dp2 = ddy(-V_unnorm);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);

    float3 dp2perp = cross(N, dp2);
    float3 dp1perp = cross(dp1, N);
    float3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    float3 B = dp2perp * duv1.y + dp1perp * duv2.y;

    float maxSq = max(dot(T, T), dot(B, B));
    if (maxSq < 1e-10)
        return N;

    float invmax = rsqrt(maxSq);
    float3x3 tbn = float3x3(T * invmax, B * invmax, N);
    float3 scaled = float3(tsNormal.x * bumpScale, tsNormal.y * bumpScale, max(tsNormal.z, 0.05));
    return normalize(mul(scaled, tbn));
}

float4 ShadeSkinnedDiff(VS_OUTPUT input, bool doAlphaReject)
{
    float2 duvX = ddx(input.Texcoord0.xy);
    float2 duvY = ddy(input.Texcoord0.xy);
    float4 albedo = Texture0.Sample(Sampler0, input.Texcoord0.xy);
    float outAlpha = saturate(albedo.a * input.Color.a);

    if (doAlphaReject)
    {
        int cmpFunc = (int)(p3Flags.y + 0.5);
        if (!PassesAlphaReject(outAlpha, AlphaRejectValue, cmpFunc))
            discard;
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

    SurfaceDetail sd = AnalyzeSurfaceDetail(input.Texcoord0.xy, duvX, duvY, albedo.rgb, N, hasNormal, hasCompanionMaps, p3Scale);

    if (hasNormMap || hasSpecMap)
    {
        float uvFoot2 = max(length(duvX), length(duvY));
        sd.detailFade = 1.0 - smoothstep(0.012, 0.048, uvFoot2);

        if (hasSpecMap)
        {
            float3 spSample = Texture4.SampleGrad(Sampler0, input.Texcoord0.xy, duvX, duvY).rgb;
            sd.smoothness = saturate(spSample.r);
            sd.metalMask = saturate(spSample.g);
            float cavityAO = saturate(spSample.b);
            // Single cavity AO without double-multiplying procedural crevice dip
            float aoScale = hasCompanionMaps ? 1.0 : 0.65;
            sd.albedo = saturate(sd.albedo * lerp(1.0, lerp(0.75, 1.00, cavityAO), saturate(sd.detailFade * p3Scale * aoScale)));
        }

        if (hasNormMap && hasNormal && sd.detailFade > 0.01 && p3Scale > 0.001)
        {
            float4 nSample = Texture3.SampleGrad(Sampler0, input.Texcoord0.xy, duvX, duvY);
            float2 xy = float2(nSample.a, nSample.g) * 2.0 - 1.0;
            float nz = sqrt(saturate(1.0 - dot(xy, xy)));
            sd.microNormal = PerturbNormalFromMap(N, input.Texcoord7.xyz, input.Texcoord0.xy, float3(xy, nz), 1.35 * sd.detailFade * p3Scale);
        }
    }

    float3 litEnv = input.Color.rgb;
    float3 specAdd = float3(0.0, 0.0, 0.0);

    // skinning.hlsl exports:
    //   Texcoord6.xyz = ViewNormal (.w > 0.5)
    //   Texcoord7.xyz = ViewDir
    //   Texcoord5.xyz = ViewLightDir0, .w = NdotL0
    //   Texcoord4.rgb = diffuse.rgb * lightDiffuseColour[0].rgb
    //   Texcoord3.xyz = ViewLightDir1, .w = lum(diffuse * lightDiffuseColour[1])
    if (hasNormal)
    {
        float3 Nmicro = sd.microNormal;
        float NdotV = saturate(dot(Nmicro, V));

        // View-up hemisphere micro-relief replaces camera-headlight bevel
        float3 viewUp = normalize(float3(0.0, 0.80, 0.60));
        float skyRelief = dot(Nmicro, viewUp) - dot(N, viewUp);
        litEnv *= saturate(1.0 + skyRelief * (0.20 * sd.detailFade * p3Scale));

        float specPow = lerp(36.0, 96.0, max(sd.smoothness, sd.metalMask));
        float normFactor = (specPow + 8.0) / 32.0;
        float matSpec = lerp(0.04, 0.18, sd.smoothness * sd.smoothness) * normFactor;
        float F0 = lerp(0.04, 0.78, sd.metalMask);
        float3 sheenTint = lerp(lerp(float3(1.0, 1.0, 1.0), sd.albedo, 0.15), sd.albedo, sd.metalMask);

        float l0LenSq = dot(input.Texcoord5.xyz, input.Texcoord5.xyz);
        if (l0LenSq > 1e-6 && dot(input.Texcoord4.rgb, float3(0.299, 0.587, 0.114)) > 1e-4)
        {
            float3 L0 = input.Texcoord5.xyz * rsqrt(l0LenSq);
            // Saturated back-face truncation eliminates ghost relief in shadow
            float relief = saturate(dot(Nmicro, L0)) - saturate(dot(N, L0));
            litEnv *= saturate(1.0 + relief * (0.45 * sd.detailFade * p3Scale));

            // Soft organic wrapped diffuse for flesh / skin tones with warm hue gate
            float ndlRaw = dot(Nmicro, L0);
            float maxC = max(sd.albedo.r, max(sd.albedo.g, sd.albedo.b));
            float minC = min(sd.albedo.r, min(sd.albedo.g, sd.albedo.b));
            float sat = (maxC > 1e-4) ? ((maxC - minC) / maxC) : 0.0;
            float warmHue = smoothstep(0.03, 0.14, sd.albedo.r - sd.albedo.b) *
                            smoothstep(-0.02, 0.08, sd.albedo.r - sd.albedo.g);
            float isFlesh = saturate(1.0 - sd.metalMask * 1.8) * smoothstep(0.12, 0.35, sat) * warmHue;
            if (isFlesh > 0.05)
            {
                float wrapNdl = saturate((ndlRaw + 0.30) / 1.30);
                float scatterTerm = pow(1.0 - abs(ndlRaw), 2.2) * wrapNdl;
                float3 sssColor = input.Texcoord4.rgb * float3(0.90, 0.38, 0.16) * (scatterTerm * 0.16 * isFlesh * p3Scale);
                litEnv += sssColor;
            }

            float3 H = normalize(L0 + V);
            float HdotV = saturate(dot(H, V));
            float fresnel = F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);
            float ndh = max(dot(Nmicro, H), 0.0);
            float ndlMask = saturate(ndlRaw * 2.5 + 0.18);

            float specScalar = pow(ndh, specPow) * matSpec * fresnel * ndlMask;
            float sunGate = saturate(dot(N, L0) * 0.5 + 0.5);
            float rim = pow(1.0 - NdotV, 3.5) * lerp(0.02, 0.06, max(sd.smoothness, sd.metalMask)) * sunGate;

            specAdd = input.Texcoord4.rgb * sheenTint * ((specScalar + rim) * p3Scale);
        }

        float l1LenSq = dot(input.Texcoord3.xyz, input.Texcoord3.xyz);
        float afPointScale = max(p3EnhanceFlags.x, 0.0);
        if (afPointScale > 0.01 && input.Texcoord3.w > 1e-4 && l1LenSq > 1e-6)
        {
            float3 L1 = input.Texcoord3.xyz * rsqrt(l1LenSq);
            float relief1 = saturate(dot(Nmicro, L1)) - saturate(dot(N, L1));
            float l1Strength = saturate(input.Texcoord3.w) * afPointScale;
            float3 envTint = input.Color.rgb / max(max(input.Color.r, max(input.Color.g, input.Color.b)), 0.05);
            float3 l1Tint = lerp(float3(1.0, 0.92, 0.82), saturate(envTint), 0.65);

            litEnv = max(litEnv + l1Tint * (relief1 * l1Strength * 0.65 * sd.detailFade), float3(0.0, 0.0, 0.0));

            float3 H1 = normalize(L1 + V);
            float HdotV1 = saturate(dot(H1, V));
            float fresnel1 = F0 + (1.0 - F0) * pow(1.0 - HdotV1, 5.0);
            float ndh1 = max(dot(Nmicro, H1), 0.0);
            float ndlMask1 = saturate(dot(Nmicro, L1) * 2.5 + 0.15);
            float specScalar1 = pow(ndh1, specPow) * matSpec * fresnel1 * ndlMask1 * l1Strength;
            specAdd += l1Tint * sheenTint * specScalar1;
        }
    }

    float3 color = saturate(sd.albedo * litEnv + specAdd);

    int texOp = (int)(p3Flags.x + 0.5);
    if (texOp > 0)
    {
        int uvSel = (int)(p3Flags.w + 0.5);
        float2 uv1 = input.Texcoord0.xy;
        if (uvSel == 1)
            uv1 = input.Texcoord1.xy;
        else if (uvSel == 2)
            uv1 = input.Texcoord2.xy;
        color = ApplyStage1Op(color, uv1, texOp);
    }

    color = saturate(color + input.ColorSpec.rgb);
    color = SoftHighlightClamp(color);
    color = ApplyOgreFog(color, input.fogDist, (int)(p3Flags.z + 0.5));
    return float4(color, outAlpha);
}

float4 main_opaque(VS_OUTPUT input) : SV_TARGET
{
    return ShadeSkinnedDiff(input, false);
}

float4 main_alpha(VS_OUTPUT input) : SV_TARGET
{
    return ShadeSkinnedDiff(input, true);
}
