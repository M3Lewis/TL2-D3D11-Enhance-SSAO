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

float4x4 View;
float4 BaseLightAmbient;
float4 DirectionalLight0_Direction;
float4 DirectionalLight0_Diffuse;
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
float4 p3Flags2 = float4(0.0, 0.0, 0.0, 0.0);

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
    float detailWeight)
{
    SurfaceDetail sd;
    sd.albedo = centerAlbedo;
    sd.microNormal = baseNormal;
    sd.detailFade = 0.0;
    sd.reliefFade = 0.0;

    float lenX = length(duvX);
    float lenY = length(duvY);
    float uvFoot = max(lenX, lenY);

    // Isometric camera keeps walls/floors around uvFoot 0.004..0.025; keep relief active without horizon shimmer.
    float fadeFine  = (1.0 - smoothstep(0.007, 0.024, uvFoot)) * detailWeight;
    float fadeMacro = (1.0 - smoothstep(0.009, 0.030, uvFoot)) * detailWeight;

    float3 avgFine = centerAlbedo;
    float3 avgMacro = centerAlbedo;
    float gxFine = 0.0;
    float gyFine = 0.0;
    float gxMacro = 0.0;
    float gyMacro = 0.0;

    if (fadeMacro > 0.01 && uvFoot > 1e-6 && uvFoot < 0.040)
    {
        float maxFineStep = 0.0045;
        float2 fineX = (lenX > maxFineStep) ? (duvX * (maxFineStep / lenX)) : duvX;
        float2 fineY = (lenY > maxFineStep) ? (duvY * (maxFineStep / lenY)) : duvY;

        float maxMacroStep = 0.0065;
        float2 macroX = (lenX > maxMacroStep) ? (duvX * (maxMacroStep / lenX)) : duvX;
        float2 macroY = (lenY > maxMacroStep) ? (duvY * (maxMacroStep / lenY)) : duvY;

        float3 sXp = Texture0.SampleGrad(Sampler0, uv + fineX * 0.80, duvX, duvY).rgb;
        float3 sXm = Texture0.SampleGrad(Sampler0, uv - fineX * 0.80, duvX, duvY).rgb;
        float3 sYp = Texture0.SampleGrad(Sampler0, uv + fineY * 0.80, duvX, duvY).rgb;
        float3 sYm = Texture0.SampleGrad(Sampler0, uv - fineY * 0.80, duvX, duvY).rgb;
        avgFine = (sXp + sXm + sYp + sYm) * 0.25;

        float3 dPP = Texture0.SampleGrad(Sampler0, uv + (macroX + macroY) * 1.25, duvX, duvY).rgb;
        float3 dMP = Texture0.SampleGrad(Sampler0, uv + (-macroX + macroY) * 1.25, duvX, duvY).rgb;
        float3 dPM = Texture0.SampleGrad(Sampler0, uv + (macroX - macroY) * 1.25, duvX, duvY).rgb;
        float3 dMM = Texture0.SampleGrad(Sampler0, uv + (-macroX - macroY) * 1.25, duvX, duvY).rgb;
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
        float dxtGate = smoothstep(0.006, 0.025, fineSignal);

        // Zero-mean two-scale detail clarity + brick/stone mortar crevice depth (no global brightness drift).
        float lumHeadroom = saturate(min(lumC, 1.0 - lumC) * 4.5);
        float3 fineHF = clamp(centerAlbedo - avgFine, -0.075, 0.075);
        float3 macroHF = clamp(centerAlbedo - avgMacro, -0.065, 0.065);
        float3 enhanced = saturate(
            centerAlbedo +
            (fineHF * (0.40 * fadeFine * dxtGate) + macroHF * (0.28 * fadeMacro)) * lumHeadroom);

        float dipFine = max(lumFine - lumC, 0.0);
        float dipMacro = max(lumMacro - lumC, 0.0);
        float dipBlend = dipFine * 0.45 + dipMacro * 0.75;
        float crevice = 1.0 - saturate(dipBlend * 1.35) * (0.20 * fadeMacro * smoothstep(0.02, 0.12, lumMacro));

        sd.albedo = enhanced * crevice;
        sd.detailFade = fadeFine;
        sd.reliefFade = fadeMacro;

        if (hasNormal)
        {
            float gx = gxFine * (0.55 * fadeFine * dxtGate) + gxMacro * (0.80 * fadeMacro);
            float gy = gyFine * (0.55 * fadeFine * dxtGate) + gyMacro * (0.80 * fadeMacro);
            float3 rawGrad = float3(gx, gy, 0.0);
            float3 tanGrad = rawGrad - baseNormal * dot(rawGrad, baseNormal);
            float gradLen = length(tanGrad);
            if (gradLen > 0.42)
                tanGrad *= (0.42 / gradLen);
            sd.microNormal = normalize(baseNormal - tanGrad * 1.30);
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
    sd.metalMask = (1.0 - smoothstep(0.14, 0.34, sat)) *
                   smoothstep(0.16, 0.30, lum) *
                   (1.0 - smoothstep(0.66, 0.82, lum)) *
                   (1.0 - smoothstep(0.15, 0.45, fineRough));
    return sd;
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

    SurfaceDetail sd = AnalyzeSurfaceDetail(input.Texcoord0.xy, duvX, duvY, albedo.rgb, N, hasNormal, detailWeight);

    float3 litEnv = input.Color.rgb;
    float3 specAdd = float3(0.0, 0.0, 0.0);

    if (hasNormal)
    {
        float3 Nmicro = sd.microNormal;
        float NdotV = saturate(dot(Nmicro, V));

        // Preserve original lighting feel while carving multi-scale stone/brick bevels
        float bevel = dot(Nmicro, V) - dot(N, V);
        litEnv *= saturate(1.0 + bevel * (0.26 * sd.reliefFade));

        float dirLum = dot(DirectionalLight0_Diffuse.rgb, float3(0.299, 0.587, 0.114));
        if (dirLum > 1e-4)
        {
            float3 rawL0 = mul(-DirectionalLight0_Direction.xyz, (float3x3)View);
            float l0LenSq = dot(rawL0, rawL0);
            if (l0LenSq > 1e-6)
            {
                float3 L0 = rawL0 * rsqrt(l0LenSq);
                float relief = dot(Nmicro, L0) - dot(N, L0);
                litEnv *= saturate(1.0 + relief * (0.52 * sd.reliefFade));

                float ndlRaw = dot(Nmicro, L0);
                float3 H = normalize(L0 + V);
                float ndh = max(dot(Nmicro, H), 0.0);

                float basePow = clamp((MaterialPower > 1.0) ? MaterialPower : 24.0, 14.0, 64.0);
                float specPow = lerp(basePow * 0.85, max(basePow, 48.0), max(sd.smoothness, sd.metalMask));
                float fresnel = lerp(0.28, 0.90, pow(1.0 - NdotV, 2.8));
                float termMask = saturate(ndlRaw * 2.5 + 0.15);
                float matScale = lerp(0.035, 0.16, sd.smoothness) + sd.metalMask * 0.09;
                float specScalar = pow(ndh, specPow) * matScale * fresnel * termMask * detailWeight;

                float tintWeight = lerp(0.45, 0.80, sd.metalMask);
                float3 surfTint = lerp(float3(1.0, 1.0, 1.0), sd.albedo, tintWeight);
                specAdd = DirectionalLight0_Diffuse.rgb * MaterialDiffuse.rgb * surfTint * specScalar;
            }
        }
    }

    float3 color = saturate(sd.albedo * litEnv + specAdd);

    int op1 = (int)(p3Flags.x + 0.5);
    if (op1 > 0)
    {
        float2 uv1 = ResolveFixedFuncUv(input.Texcoord1, p3Flags.w > 0.5);
        color = ApplyTextureStageOp(color, Texture1.Sample(Sampler1, uv1), op1);
    }

    int op2 = (int)(p3Flags2.x + 0.5);
    if (op2 > 0)
    {
        float2 uv2 = ResolveFixedFuncUv(input.Texcoord2, p3Flags2.y > 0.5);
        color = ApplyTextureStageOp(color, Texture2.Sample(Sampler2, uv2), op2);
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
