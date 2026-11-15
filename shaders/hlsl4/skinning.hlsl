// Upgraded for TL2 Route B Phase 3 (preserves all 24 entry points & SM4.0 12-register VS_OUTPUT)
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

float4 ambient;
float4 diffuse;
float4 emissive;
float4 lightDiffuseColour[2];
float4 lightPos[2];
float4x4 texViewProj;
float4x4 viewMatrix;
float4x4 viewProjectionMatrix;
float3x4 worldMatrix3x4Array[64];

void ComputeSkinnedCore(
    int boneCount,
    float4 position,
    float3 normal,
    uint4 blendIdx,
    float4 blendWgt,
    out float4 blendPos,
    out float3 worldNorm,
    out float3 lightDir0,
    out float4 oPosition,
    out float4 colour)
{
    blendPos = float4(0.0, 0.0, 0.0, 0.0);
    float3 norm = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < boneCount; ++i)
    {
        float3x4 boneMat = worldMatrix3x4Array[blendIdx[i]];
        blendPos += float4(mul(boneMat, position).xyz, 1.0) * blendWgt[i];
        norm += mul((float3x3)boneMat, normal) * blendWgt[i];
    }
    worldNorm = normalize(norm);
    oPosition = mul(viewProjectionMatrix, blendPos);

    lightDir0 = normalize(lightPos[0].xyz - (blendPos.xyz * lightPos[0].w));
    float3 lightDir1 = normalize(lightPos[1].xyz - (blendPos.xyz * lightPos[1].w));

    colour = ambient * diffuse +
             diffuse * (saturate(dot(lightDir0, worldNorm)) * lightDiffuseColour[0]) +
             diffuse * (saturate(dot(lightDir1, worldNorm)) * lightDiffuseColour[1]) +
             emissive;
    colour = saturate(colour);
    colour.a = 1.0;
}

void PopulateViewSpaceExtras(
    float4 blendPos,
    float3 worldNorm,
    float3 lightDir0,
    inout VS_OUTPUT result)
{
    float3 viewNormal = normalize(mul((float3x3)viewMatrix, worldNorm));
    float3 viewPos = mul(viewMatrix, float4(blendPos.xyz, 1.0)).xyz;
    float3 viewLightDir0 = normalize(mul((float3x3)viewMatrix, lightDir0));

    result.Texcoord4 = float4(saturate(diffuse.rgb * lightDiffuseColour[0].rgb), 1.0);
    result.Texcoord5 = float4(viewLightDir0, saturate(dot(worldNorm, lightDir0)));
    result.Texcoord6 = float4(viewNormal, 1.0);
    // Keep unnormalized -viewPos so PS derives exact view-space dPdx/dPdy and normalized V.
    result.Texcoord7 = float4(-viewPos, 1.0);
}

VS_OUTPUT BuildStandardSkin(int boneCount, float4 position, float3 normal, float2 uv, uint4 blendIdx, float4 blendWgt, bool isSecondary, bool isEnviro)
{
    VS_OUTPUT result = (VS_OUTPUT)0;
    float4 blendPos = 0;
    float3 worldNorm = 0;
    float3 lightDir0 = 0;
    float4 oPosition = 0;
    float4 colour = 0;

    ComputeSkinnedCore(boneCount, position, normal, blendIdx, blendWgt, blendPos, worldNorm, lightDir0, oPosition, colour);

    result.Pos = oPosition;
    result.Color = colour;
    result.Texcoord0 = float4(uv, 0.0, 1.0);

    if (isEnviro)
    {
        float3 envProj = mul(texViewProj, blendPos).xyz;
        result.Texcoord1 = float4(envProj, 1.0);
        if (isSecondary)
            result.Texcoord2 = float4(uv, 0.0, 1.0);
    }
    else
    {
        float3 normProj = mul((float3x3)texViewProj, mul((float3x3)viewMatrix, worldNorm));
        if (isSecondary)
        {
            result.Texcoord1 = float4(uv, 0.0, 1.0);
            result.Texcoord2 = float4(normProj, 1.0);
        }
        else
        {
            result.Texcoord1 = float4(normProj, 1.0);
        }
    }

    PopulateViewSpaceExtras(blendPos, worldNorm, lightDir0, result);
    result.fogDist = result.Pos.w;
    return result;
}

VS_OUTPUT BuildBehindSkin(int boneCount, float4 position, float3 normal, uint4 blendIdx, float4 blendWgt)
{
    VS_OUTPUT result = (VS_OUTPUT)0;
    float4 blendPos = float4(0.0, 0.0, 0.0, 0.0);
    float3 norm = float3(0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < boneCount; ++i)
    {
        float3x4 boneMat = worldMatrix3x4Array[blendIdx[i]];
        blendPos += float4(mul(boneMat, position).xyz, 1.0) * blendWgt[i];
        norm += mul((float3x3)boneMat, normal) * blendWgt[i];
    }
    norm = normalize(norm);
    float4 oPosition = mul(viewProjectionMatrix, blendPos);
    float3 lightDir0 = normalize(lightPos[0].xyz - (blendPos.xyz * lightPos[0].w));

    float4 colour = ambient * diffuse +
                    diffuse * (saturate(dot(lightDir0, norm)) * lightDiffuseColour[0]) +
                    emissive;
    colour = saturate(colour);
    colour.a = 1.0;

    norm = mul((float3x3)viewMatrix, norm);
    norm.xy *= 0.5;
    norm.xy += 0.5;
    norm.y *= -1.0;

    result.Pos = oPosition;
    result.Texcoord0 = float4(norm.xy, 0.0, 1.0);
    result.Color = colour;
    result.fogDist = result.Pos.w;
    return result;
}

VS_OUTPUT BuildCasterSkin(int boneCount, float4 position, uint4 blendIdx, float4 blendWgt)
{
    VS_OUTPUT result = (VS_OUTPUT)0;
    float4 blendPos = float4(0.0, 0.0, 0.0, 0.0);
    [unroll]
    for (int i = 0; i < boneCount; ++i)
    {
        blendPos += float4(mul(worldMatrix3x4Array[blendIdx[i]], position).xyz, 1.0) * blendWgt[i];
    }
    float4 colour = float4(0.0, 0.0, 0.0, 1.0) + emissive;
    colour.a = 1.0;

    result.Pos = mul(viewProjectionMatrix, blendPos);
    result.Color = colour;
    result.fogDist = result.Pos.w;
    return result;
}

// Primary Diffuse Skinning (4/3/2/1 bones)
VS_OUTPUT hardwareSkinning_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(4, position, normal, uv, blendIdx, blendWgt, false, false);
}
VS_OUTPUT hardwareSkinningThree_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(3, position, normal, uv, blendIdx, blendWgt, false, false);
}
VS_OUTPUT hardwareSkinningTwo_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(2, position, normal, uv, blendIdx, blendWgt, false, false);
}
VS_OUTPUT hardwareSkinningOne_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(1, position, normal, uv, blendIdx, blendWgt, false, false);
}

// Secondary Diffuse Skinning (4/3/2/1 bones)
VS_OUTPUT hardwareSkinningSecondary_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(4, position, normal, uv, blendIdx, blendWgt, true, false);
}
VS_OUTPUT hardwareSkinningThreeSecondary_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(3, position, normal, uv, blendIdx, blendWgt, true, false);
}
VS_OUTPUT hardwareSkinningTwoSecondary_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(2, position, normal, uv, blendIdx, blendWgt, true, false);
}
VS_OUTPUT hardwareSkinningOneSecondary_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(1, position, normal, uv, blendIdx, blendWgt, true, false);
}

// Enviro Skinning (4/3/2/1 bones)
VS_OUTPUT hardwareSkinningEnviro_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(4, position, normal, uv, blendIdx, blendWgt, false, true);
}
VS_OUTPUT hardwareSkinningEnviroThree_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(3, position, normal, uv, blendIdx, blendWgt, false, true);
}
VS_OUTPUT hardwareSkinningEnviroTwo_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(2, position, normal, uv, blendIdx, blendWgt, false, true);
}
VS_OUTPUT hardwareSkinningEnviroOne_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(1, position, normal, uv, blendIdx, blendWgt, false, true);
}

// Enviro Secondary Skinning (4/3/2/1 bones)
VS_OUTPUT hardwareSkinningEnviroSecondary_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(4, position, normal, uv, blendIdx, blendWgt, true, true);
}
VS_OUTPUT hardwareSkinningEnviroThreeSecondary_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(3, position, normal, uv, blendIdx, blendWgt, true, true);
}
VS_OUTPUT hardwareSkinningEnviroTwoSecondary_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(2, position, normal, uv, blendIdx, blendWgt, true, true);
}
VS_OUTPUT hardwareSkinningEnviroOneSecondary_vp(float4 position:POSITION, float3 normal:NORMAL, float2 uv:TEXCOORD0, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildStandardSkin(1, position, normal, uv, blendIdx, blendWgt, true, true);
}

// RenderBehind (vanilla untouched logic)
VS_OUTPUT hardwareSkinningBehind_vp(float4 position:POSITION, float3 normal:NORMAL, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildBehindSkin(4, position, normal, blendIdx, blendWgt);
}
VS_OUTPUT hardwareSkinningBehindThree_vp(float4 position:POSITION, float3 normal:NORMAL, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildBehindSkin(3, position, normal, blendIdx, blendWgt);
}
VS_OUTPUT hardwareSkinningBehindTwo_vp(float4 position:POSITION, float3 normal:NORMAL, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildBehindSkin(2, position, normal, blendIdx, blendWgt);
}
VS_OUTPUT hardwareSkinningBehindOne_vp(float4 position:POSITION, float3 normal:NORMAL, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildBehindSkin(1, position, normal, blendIdx, blendWgt);
}

// Shadow Caster (vanilla untouched logic)
VS_OUTPUT hardwareSkinningCaster_vp(float4 position:POSITION, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildCasterSkin(4, position, blendIdx, blendWgt);
}
VS_OUTPUT hardwareSkinningCasterThree_vp(float4 position:POSITION, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildCasterSkin(3, position, blendIdx, blendWgt);
}
VS_OUTPUT hardwareSkinningCasterTwo_vp(float4 position:POSITION, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildCasterSkin(2, position, blendIdx, blendWgt);
}
VS_OUTPUT hardwareSkinningCasterOne_vp(float4 position:POSITION, uint4 blendIdx:BLENDINDICES, float4 blendWgt:BLENDWEIGHT)
{
    return BuildCasterSkin(1, position, blendIdx, blendWgt);
}
