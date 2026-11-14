float4x4 Projection;

struct VS_INPUT
{
    float3 Position : POSITION0;
    uint   Diffuse  : COLOR0;
    float2 Uv       : TEXCOORD0;
};

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

VS_OUTPUT main(VS_INPUT input)
{
    VS_OUTPUT output = (VS_OUTPUT)0;
    // Matches CEGUI::OgreCEGUIRenderer::initRenderStates (World=I, View=I, Projection row-major).
    output.Pos = mul(Projection, float4(input.Position, 1.0f));
    output.Color = float4(
        (input.Diffuse >> 16) & 255u,
        (input.Diffuse >> 8)  & 255u,
        (input.Diffuse)       & 255u,
        (input.Diffuse >> 24) & 255u
    ) / 255.0f;
    output.ColorSpec = float4(0.0f, 0.0f, 0.0f, 0.0f);
    output.fogDist   = 0.0f;
    output.Texcoord0 = float4(input.Uv, 0.0f, 1.0f);
    return output;
}
