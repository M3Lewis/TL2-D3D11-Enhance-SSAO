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
//   x = stage1 blend op (0=none, 1=modulate, 2=add, 3=alpha_blend, 4=modulate_x2)
//   y = alpha reject compare func (Ogre CompareFunction: 0=fail,1=pass,2=<,3<=,4==,5!=,6>=,7>)
//   z = fog mode (0=none, 1=exp, 2=exp2, 3=linear)
//   w = stage1 texcoord index (0 or 1)
float4 p3Flags = float4(0.0, 1.0, 0.0, 1.0);

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

float4 ShadeDiffPass(VS_OUTPUT input, bool doAlphaReject)
{
    float4 albedo = Texture0.Sample(Sampler0, input.Texcoord0.xy);
    float outAlpha = saturate(albedo.a * input.Color.a);

    if (doAlphaReject)
    {
        int cmpFunc = (int)(p3Flags.y + 0.5);
        if (!PassesAlphaReject(outAlpha, AlphaRejectValue, cmpFunc))
            discard;
    }

    float3 litEnv = input.Color.rgb;
    float3 specAdd = float3(0.0, 0.0, 0.0);

    if (input.Texcoord6.w > 0.5)
    {
        float nLenSq = dot(input.Texcoord6.xyz, input.Texcoord6.xyz);
        float vLenSq = dot(input.Texcoord7.xyz, input.Texcoord7.xyz);
        if (nLenSq > 1e-6 && vLenSq > 1e-6)
        {
            float3 N = input.Texcoord6.xyz * rsqrt(nLenSq);
            float3 V = input.Texcoord7.xyz * rsqrt(vLenSq);
            float NdotV = saturate(dot(N, V));

            // View-space up vector derived safely from active View matrix
            float3 rawUp = mul((float3x3)View, float3(0.0, 1.0, 0.0));
            float upLenSq = dot(rawUp, rawUp);
            float3 upV = (upLenSq > 1e-6) ? (rawUp * rsqrt(upLenSq)) : float3(0.0, 1.0, 0.0);

            float hemi = dot(N, upV) * 0.5 + 0.5;
            litEnv *= lerp(0.92, 1.08, hemi);

            float dirLum = dot(DirectionalLight0_Diffuse.rgb, float3(0.299, 0.587, 0.114));
            if (dirLum > 1e-4)
            {
                float3 rawL0 = mul((float3x3)View, -DirectionalLight0_Direction.xyz);
                float l0LenSq = dot(rawL0, rawL0);
                if (l0LenSq > 1e-6)
                {
                    float3 L0 = rawL0 * rsqrt(l0LenSq);
                    float ndlRaw = dot(N, L0);
                    float3 H = normalize(L0 + V);
                    float ndh = max(dot(N, H), 0.0);

                    float specPow = clamp((MaterialPower > 1.0) ? MaterialPower : 24.0, 12.0, 64.0);
                    float fresnel = 0.25 + 0.75 * pow(1.0 - NdotV, 3.0);
                    float termMask = saturate(ndlRaw * 2.5 + 0.15);
                    float specScalar = pow(ndh, specPow) * 0.14 * fresnel * termMask;

                    float3 surfTint = lerp(float3(1.0, 1.0, 1.0), albedo.rgb, 0.55);
                    specAdd = DirectionalLight0_Diffuse.rgb * MaterialDiffuse.rgb * surfTint * specScalar;
                }
            }
        }
    }

    float3 color = albedo.rgb * litEnv + specAdd;

    int texOp = (int)(p3Flags.x + 0.5);
    if (texOp > 0)
    {
        float2 uv1 = (p3Flags.w > 0.5) ? input.Texcoord1.xy : input.Texcoord0.xy;
        color = ApplyStage1Op(color, uv1, texOp);
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
