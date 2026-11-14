sampler2D sceneTexture : register(s0);

float4 main(float2 uv : TEXCOORD0) : COLOR
{
    return tex2D(sceneTexture, uv);
}
