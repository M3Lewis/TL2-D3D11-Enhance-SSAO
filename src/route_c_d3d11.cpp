#include "route_c_d3d11.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace routeb {

namespace {

constexpr size_t kBenchSlots = 4;

using D3DCompileFn = HRESULT(WINAPI*)(
    LPCVOID pSrcData,
    SIZE_T SrcDataSize,
    LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines,
    ID3DInclude* pInclude,
    LPCSTR pEntrypoint,
    LPCSTR pTarget,
    UINT Flags1,
    UINT Flags2,
    ID3DBlob** ppCode,
    ID3DBlob** ppErrorMsgs);

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

void SafeReleaseClassInstances(ID3D11ClassInstance** arr, UINT count) {
    if (!arr) {
        return;
    }
    const UINT n = (std::min)(count, static_cast<UINT>(D3D11_SHADER_MAX_INTERFACES));
    for (UINT i = 0; i < n; ++i) {
        SafeRelease(arr[i]);
    }
}

float ReadEnvFloat(const char* name, float fallback, float min_v, float max_v) {
    char buf[32] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0 || len >= sizeof(buf)) {
        return fallback;
    }
    char* end = nullptr;
    const float val = std::strtof(buf, &end);
    if (end == buf || !std::isfinite(val)) {
        return fallback;
    }
    return (std::max)(min_v, (std::min)(max_v, val));
}

bool EnvFlagIsOne(const char* name) {
    char buf[8] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return len > 0 && buf[0] == '1';
}

bool EnvFlagIsZero(const char* name) {
    char buf[8] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return len > 0 && buf[0] == '0';
}

struct AoTuneConfig {
    bool initialized = false;
    float radius_world = 0.95f;
    float bias_cos = 0.10f;
    float strength = 1.35f;
    float min_ao_floor = 0.55f;
    float depth_edge_scale = 55.0f;
    float fade_start = 42.0f;
    float fade_end_cap = 78.0f;
    int bench_mode = 1;  // 0=off, 1=sample every 32 frames, 2=every frame
};

const AoTuneConfig& GetAoTuneConfig(RouteCD3DLogFn logger) {
    static AoTuneConfig cfg{};
    if (!cfg.initialized) {
        cfg.initialized = true;
        cfg.radius_world = ReadEnvFloat("TL2_C_AO_RADIUS", 0.95f, 0.10f, 8.0f);
        cfg.bias_cos = ReadEnvFloat("TL2_C_AO_BIAS", 0.10f, 0.01f, 0.50f);
        cfg.strength = ReadEnvFloat("TL2_C_AO_STRENGTH", 1.35f, 0.0f, 4.0f);
        cfg.min_ao_floor = ReadEnvFloat("TL2_C_AO_FLOOR", 0.55f, 0.20f, 0.98f);
        cfg.depth_edge_scale = ReadEnvFloat("TL2_C_AO_EDGE", 55.0f, 5.0f, 300.0f);
        cfg.fade_start = ReadEnvFloat("TL2_C_AO_FADESTART", 42.0f, 5.0f, 150.0f);
        cfg.fade_end_cap = ReadEnvFloat("TL2_C_AO_FADEEND", 78.0f, 10.0f, 250.0f);
        if (EnvFlagIsZero("TL2_C_AO_BENCH")) {
            cfg.bench_mode = 0;
        } else if (EnvFlagIsOne("TL2_C_AO_BENCH")) {
            cfg.bench_mode = 2;
        } else {
            cfg.bench_mode = 1;
        }
        if (logger) {
            logger(
                "[RouteC][C3] Config halfRes=1 radius=%.2f bias=%.2f strength=%.2f floor=%.2f edge=%.1f fade=(%.1f,%.1f) bench=%d",
                cfg.radius_world,
                cfg.bias_cos,
                cfg.strength,
                cfg.min_ao_floor,
                cfg.depth_edge_scale,
                cfg.fade_start,
                cfg.fade_end_cap,
                cfg.bench_mode);
        }
    }
    return cfg;
}

D3DCompileFn ResolveD3DCompile(RouteCD3DLogFn logger) {
    static D3DCompileFn cached = nullptr;
    static bool attempted = false;
    if (attempted) {
        return cached;
    }
    attempted = true;

    const char* dlls[] = {"d3dcompiler_47.dll", "D3DCompiler_43.dll"};
    for (const char* dll_name : dlls) {
        HMODULE mod = ::GetModuleHandleA(dll_name);
        if (!mod) {
            mod = ::LoadLibraryA(dll_name);
        }
        if (mod) {
            auto fn = reinterpret_cast<D3DCompileFn>(::GetProcAddress(mod, "D3DCompile"));
            if (fn) {
                cached = fn;
                if (logger) {
                    logger("[RouteC][D3D11] Resolved D3DCompile from %s.", dll_name);
                }
                return cached;
            }
        }
    }
    if (logger) {
        logger("[RouteC][D3D11] FAILED to resolve D3DCompile from d3dcompiler_47/43.");
    }
    return nullptr;
}

constexpr const char kAoShaderSource[] = R"HLSL(
cbuffer CbAoParams : register(b0)
{
    float4 gProjXY;      // x=p00, y=p11, z=p02, w=p12
    float4 gProjZAndFade;// x=p22, y=p23, z=fadeEnd, w=invFadeRange
    float4 gVpRect;      // x=vpOriginX, y=vpOriginY, z=vpWidth, w=vpHeight
    int4   gVpClamp;     // x=minX, y=minY, z=maxX, w=maxY
    float4 gAoParams;    // x=radiusWorld, y=biasCos, z=strength, w=minAoFloor
    float4 gBlurAndMode; // x=invVpW, y=invVpH, z=depthEdgeScale, w=debugMode
};

#if MSAA_DEPTH
Texture2DMS<float4> gDepthTex : register(t0);
float LoadRawDepth(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gDepthTex.Load(c, 0).r;
}
#else
Texture2D<float4> gDepthTex : register(t0);
float LoadRawDepth(int2 pix)
{
    int2 c = clamp(pix, gVpClamp.xy, gVpClamp.zw);
    return gDepthTex.Load(int3(c, 0)).r;
}
#endif

Texture2D<float4> gAoInputTex : register(t1);

struct VsOut
{
    float4 pos : SV_Position;
};

VsOut VS_Fullscreen(uint vid : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

float RawToLinearDist(float zNdc)
{
    float denom = min(zNdc + gProjZAndFade.x, -1e-6f);
    return gProjZAndFade.y / denom; // positive view-space distance d = -z_vs
}

float3 ReconstructPosVS(int2 pix, out float rawDepth)
{
    rawDepth = LoadRawDepth(pix);
    float d = RawToLinearDist(rawDepth);
    float2 pixelCenter = float2(pix) + 0.5f;
    float2 uv = (pixelCenter - gVpRect.xy) / gVpRect.zw;
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float x = d * (ndc.x + gProjXY.z) / gProjXY.x;
    float y = d * (ndc.y + gProjXY.w) / gProjXY.y;
    return float3(x, y, -d);
}

float3 ReconstructNormalVS(int2 pix, float3 P)
{
    float dL, dR, dU, dD;
    float3 pL = ReconstructPosVS(pix + int2(-1,  0), dL);
    float3 pR = ReconstructPosVS(pix + int2( 1,  0), dR);
    float3 pU = ReconstructPosVS(pix + int2( 0, -1), dU);
    float3 pD = ReconstructPosVS(pix + int2( 0,  1), dD);

    float3 dxL = P - pL;
    float3 dxR = pR - P;
    float3 dPdx = (abs(dxL.z) < abs(dxR.z)) ? dxL : dxR;

    float3 dyU = P - pU;
    float3 dyD = pD - P;
    float3 dPdy = (abs(dyU.z) < abs(dyD.z)) ? dyU : dyD;

    float3 cp = cross(dPdy, dPdx);
    float lenSq = dot(cp, cp);
    float3 n = (lenSq > 1e-12f) ? (cp * rsqrt(lenSq)) : float3(0.0f, 0.0f, 1.0f);
    if (n.z < 0.0f)
    {
        n = -n;
    }
    return n;
}

float4 PS_C1DepthNormalDebug(VsOut input) : SV_Target0
{
    int2 pix = int2(input.pos.xy);
    float rawDepth = 1.0f;
    float3 P = ReconstructPosVS(pix, rawDepth);
    float2 uv = (input.pos.xy - gVpRect.xy) / gVpRect.zw;

    if (abs(uv.x - 0.5f) < gBlurAndMode.x * 1.5f)
    {
        return float4(0.95f, 0.80f, 0.15f, 1.0f);
    }
    if (rawDepth >= 0.99998f)
    {
        return float4(0.06f, 0.16f, 0.30f, 1.0f);
    }

    if (uv.x < 0.5f)
    {
        float d = -P.z;
        float normDist = saturate((d - 1.0f) / 48.0f);
        float phase = frac(d * 0.5f);
        float lineMask = 1.0f - smoothstep(0.0f, 0.09f, min(phase, 1.0f - phase));
        float baseShade = 1.0f - normDist * 0.85f;
        float3 col = lerp(float3(baseShade, baseShade * 0.98f, baseShade * 0.95f),
                          float3(0.08f, 0.85f, 0.95f),
                          lineMask * 0.8f);
        return float4(col, 1.0f);
    }
    else
    {
        float3 n = ReconstructNormalVS(pix, P);
        float3 vis = saturate(n * 0.5f + 0.5f);
        return float4(vis, 1.0f);
    }
}

float4 PS_AoRaw(VsOut input) : SV_Target0
{
    // Pass 1 executes at half resolution; map hpix -> exact even full-res depth pixel.
    int2 hpix = int2(input.pos.xy);
    int2 pix = clamp(hpix * 2, gVpClamp.xy, gVpClamp.zw);
    float rawDepth = 1.0f;
    float3 P = ReconstructPosVS(pix, rawDepth);
    if (rawDepth >= 0.99998f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float d = -P.z;
    float fade = saturate((gProjZAndFade.z - d) * gProjZAndFade.w);
    if (fade <= 0.001f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float3 n = ReconstructNormalVS(pix, P);
    float R = gAoParams.x;
    float pixPerUnit = 0.5f * gVpRect.w * gProjXY.y / max(d, 0.25f);
    float pixRadius = clamp(R * pixPerUnit, 5.0f, 48.0f);

    float ignA = frac(52.9829189f * frac(dot(float2(hpix), float2(0.06711056f, 0.00583715f))));
    float ignS = frac(52.9829189f * frac(dot(float2(hpix) + float2(19.0f, 37.0f), float2(0.06711056f, 0.00583715f))));

    float s0, c0;
    sincos(ignA * 1.5707963f, s0, c0);
    float2 dirs[4] = {
        float2( c0,  s0),
        float2(-s0,  c0),
        float2(-c0, -s0),
        float2( s0, -c0)
    };

    float occSum = 0.0f;
    float biasCos = gAoParams.y;
    float invOneMinusBias = 1.0f / max(1.0f - biasCos, 1e-3f);
    float invR2 = 1.0f / max(R * R, 1e-5f);

    [unroll]
    for (int k = 0; k < 4; ++k)
    {
        float2 dir = dirs[k];
        float maxDirOcc = 0.0f;

        [unroll]
        for (int s = 1; s <= 4; ++s)
        {
            float tFrac = (float(s) - 0.5f + 0.65f * (ignS - 0.5f)) * 0.25f;
            float stepPx = max(tFrac * pixRadius, 2.0f);
            int2 off = int2(round(dir * stepPx));
            if (off.x != 0 || off.y != 0)
            {
                float sRawD = 1.0f;
                float3 S = ReconstructPosVS(pix + off, sRawD);
                if (sRawD < 0.99998f)
                {
                    float3 V = S - P;
                    float distSq = dot(V, V);
                    float invDist = rsqrt(max(distSq, 1e-8f));
                    float cosTheta = dot(n, V) * invDist;
                    float angleOcc = saturate((cosTheta - biasCos) * invOneMinusBias);
                    float falloff = saturate(1.0f - distSq * invR2);
                    maxDirOcc = max(maxDirOcc, angleOcc * falloff);
                }
            }
        }
        occSum += maxDirOcc;
    }

    float occ = occSum * 0.25f;
    float darkened = saturate(occ * gAoParams.z) * (1.0f - gAoParams.w) * fade;
    float ao = 1.0f - darkened;
    return float4(ao, ao, ao, 1.0f);
}

float BilateralBlur1D(int2 hpix, int2 axisDir)
{
    int2 vpHalfMin = gVpClamp.xy >> 1;
    int2 vpHalfMax = gVpClamp.zw >> 1;
    int2 chpix = clamp(hpix, vpHalfMin, vpHalfMax);
    int2 cpix = clamp(chpix * 2, gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(cpix);
    if (rawC >= 0.99998f)
    {
        return 1.0f;
    }

    float dCenter = max(RawToLinearDist(rawC), 1e-3f);
    float sumAo = gAoInputTex.Load(int3(chpix, 0)).r;
    float sumW = 1.0f;
    float edgeScale = gBlurAndMode.z;

    [unroll]
    for (int i = -3; i <= 3; ++i)
    {
        if (i != 0)
        {
            int2 shpix = clamp(hpix + axisDir * i, vpHalfMin, vpHalfMax);
            int2 spix = clamp(shpix * 2, gVpClamp.xy, gVpClamp.zw);
            float rawS = LoadRawDepth(spix);
            if (rawS < 0.99998f)
            {
                float dS = RawToLinearDist(rawS);
                float relDiff = abs(dS - dCenter) / dCenter;
                float ed = relDiff * edgeScale;
                float w = exp(-0.22f * float(i * i) - ed * ed);
                sumAo += gAoInputTex.Load(int3(shpix, 0)).r * w;
                sumW += w;
            }
        }
    }
    return sumAo / max(sumW, 1e-5f);
}

float4 PS_BlurH(VsOut input) : SV_Target0
{
    float v = BilateralBlur1D(int2(input.pos.xy), int2(1, 0));
    return float4(v, v, v, 1.0f);
}

float4 PS_BlurV(VsOut input) : SV_Target0
{
    float v = BilateralBlur1D(int2(input.pos.xy), int2(0, 1));
    return float4(v, v, v, 1.0f);
}

float4 PS_AoComposite(VsOut input) : SV_Target0
{
    int2 pix = clamp(int2(input.pos.xy), gVpClamp.xy, gVpClamp.zw);
    float rawC = LoadRawDepth(pix);
    if (rawC >= 0.99998f)
    {
        return float4(1.0f, 1.0f, 1.0f, 1.0f);
    }

    float dC = max(RawToLinearDist(rawC), 1e-3f);
    float edgeScale = gBlurAndMode.z;
    int2 vpHalfMin = gVpClamp.xy >> 1;
    int2 vpHalfMax = gVpClamp.zw >> 1;
    int2 h0 = pix >> 1;
    float fx = (pix.x & 1) ? 0.5f : 0.0f;
    float fy = (pix.y & 1) ? 0.5f : 0.0f;
    float wx[2] = {1.0f - fx, fx};
    float wy[2] = {1.0f - fy, fy};

    float sumAo = 0.0f;
    float sumW = 0.0f;

    [unroll]
    for (int dy = 0; dy <= 1; ++dy)
    {
        [unroll]
        for (int dx = 0; dx <= 1; ++dx)
        {
            float wSpat = wx[dx] * wy[dy];
            if (wSpat > 0.0f)
            {
                int2 shpix = clamp(h0 + int2(dx, dy), vpHalfMin, vpHalfMax);
                int2 spix = clamp(shpix * 2, gVpClamp.xy, gVpClamp.zw);
                float rawS = LoadRawDepth(spix);
                float dS = max(RawToLinearDist(rawS), 1e-3f);
                float relDiff = abs(dS - dC) / dC;
                float ed = relDiff * edgeScale;
                float wDepth = (rawS < 0.99998f) ? exp(-(ed * ed)) : 0.0f;
                float w = wSpat * (wDepth + 1e-4f);
                sumAo += gAoInputTex.Load(int3(shpix, 0)).r * w;
                sumW += w;
            }
        }
    }

    float ao = (sumW > 1e-5f) ? (sumAo / sumW) : 1.0f;
    return float4(ao, ao, ao, 1.0f);
}
)HLSL";

struct DepthCopyFormats {
    bool supported = false;
    DXGI_FORMAT tex_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT srv_format = DXGI_FORMAT_UNKNOWN;
};

DepthCopyFormats MapDepthCopyFormats(DXGI_FORMAT tex_fmt, DXGI_FORMAT dsv_fmt) {
    if (tex_fmt == DXGI_FORMAT_R24G8_TYPELESS || dsv_fmt == DXGI_FORMAT_D24_UNORM_S8_UINT) {
        return {true, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS};
    }
    if (tex_fmt == DXGI_FORMAT_R32_TYPELESS || dsv_fmt == DXGI_FORMAT_D32_FLOAT) {
        return {true, DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT};
    }
    if (tex_fmt == DXGI_FORMAT_R16_TYPELESS || dsv_fmt == DXGI_FORMAT_D16_UNORM) {
        return {true, DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM};
    }
    if (tex_fmt == DXGI_FORMAT_R32G8X24_TYPELESS || dsv_fmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT) {
        return {true, DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS};
    }
    return {};
}

UINT ComputeDsvSubresource(
    const D3D11_DEPTH_STENCIL_VIEW_DESC& dsv_desc,
    UINT mip_levels,
    UINT* out_mip_slice) {
    UINT mip = 0;
    UINT slice = 0;
    switch (dsv_desc.ViewDimension) {
        case D3D11_DSV_DIMENSION_TEXTURE2D:
            mip = dsv_desc.Texture2D.MipSlice;
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
            mip = dsv_desc.Texture2DArray.MipSlice;
            slice = dsv_desc.Texture2DArray.FirstArraySlice;
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMS:
            break;
        case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
            slice = dsv_desc.Texture2DMSArray.FirstArraySlice;
            break;
        default:
            break;
    }
    if (out_mip_slice) {
        *out_mip_slice = mip;
    }
    return D3D11CalcSubresource(mip, slice, mip_levels > 0 ? mip_levels : 1);
}

class D3D11StateGuard {
public:
    explicit D3D11StateGuard(ID3D11DeviceContext* ctx) : ctx_(ctx) {
        if (!ctx_) {
            return;
        }
        ctx_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, &dsv_);
        ctx_->OMGetBlendState(&blend_, blend_factor_, &sample_mask_);
        ctx_->OMGetDepthStencilState(&dss_, &stencil_ref_);
        ctx_->RSGetState(&rs_);
        ctx_->RSGetViewports(&num_viewports_, viewports_);
        ctx_->RSGetScissorRects(&num_scissors_, scissors_);
        ctx_->IAGetInputLayout(&input_layout_);
        ctx_->IAGetPrimitiveTopology(&topology_);
        ctx_->VSGetShader(&vs_, vs_ci_, &vs_ci_cnt_);
        ctx_->GSGetShader(&gs_, gs_ci_, &gs_ci_cnt_);
        ctx_->PSGetShader(&ps_, ps_ci_, &ps_ci_cnt_);
        ctx_->PSGetConstantBuffers(0, 1, &ps_cb0_);
        ctx_->PSGetShaderResources(0, 2, ps_srvs_);
        ctx_->PSGetSamplers(0, 1, &ps_samp0_);
    }

    ~D3D11StateGuard() {
        if (ctx_) {
            ctx_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, dsv_);
            ctx_->OMSetBlendState(blend_, blend_factor_, sample_mask_);
            ctx_->OMSetDepthStencilState(dss_, stencil_ref_);
            ctx_->RSSetState(rs_);
            ctx_->RSSetViewports(num_viewports_, viewports_);
            ctx_->RSSetScissorRects(num_scissors_, scissors_);
            ctx_->IASetInputLayout(input_layout_);
            ctx_->IASetPrimitiveTopology(topology_);
            ctx_->VSSetShader(vs_, vs_ci_, vs_ci_cnt_);
            ctx_->GSSetShader(gs_, gs_ci_, gs_ci_cnt_);
            ctx_->PSSetShader(ps_, ps_ci_, ps_ci_cnt_);
            ctx_->PSSetConstantBuffers(0, 1, &ps_cb0_);
            ctx_->PSSetShaderResources(0, 2, ps_srvs_);
            ctx_->PSSetSamplers(0, 1, &ps_samp0_);
        }
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            SafeRelease(rtvs_[i]);
        }
        SafeRelease(dsv_);
        SafeRelease(blend_);
        SafeRelease(dss_);
        SafeRelease(rs_);
        SafeRelease(input_layout_);
        SafeRelease(vs_);
        SafeRelease(gs_);
        SafeRelease(ps_);
        SafeRelease(ps_cb0_);
        SafeRelease(ps_srvs_[0]);
        SafeRelease(ps_srvs_[1]);
        SafeRelease(ps_samp0_);
        SafeReleaseClassInstances(vs_ci_, vs_ci_cnt_);
        SafeReleaseClassInstances(gs_ci_, gs_ci_cnt_);
        SafeReleaseClassInstances(ps_ci_, ps_ci_cnt_);
    }

    D3D11StateGuard(const D3D11StateGuard&) = delete;
    D3D11StateGuard& operator=(const D3D11StateGuard&) = delete;

    ID3D11RenderTargetView* om_rtv0() const { return rtvs_[0]; }
    ID3D11DepthStencilView* om_dsv() const { return dsv_; }

private:
    ID3D11DeviceContext* ctx_ = nullptr;
    ID3D11RenderTargetView* rtvs_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv_ = nullptr;
    ID3D11BlendState* blend_ = nullptr;
    FLOAT blend_factor_[4]{1.0f, 1.0f, 1.0f, 1.0f};
    UINT sample_mask_ = 0xFFFFFFFFU;
    ID3D11DepthStencilState* dss_ = nullptr;
    UINT stencil_ref_ = 0;
    ID3D11RasterizerState* rs_ = nullptr;
    UINT num_viewports_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT num_scissors_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11InputLayout* input_layout_ = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vs_ = nullptr;
    ID3D11ClassInstance* vs_ci_[D3D11_SHADER_MAX_INTERFACES]{};
    UINT vs_ci_cnt_ = D3D11_SHADER_MAX_INTERFACES;
    ID3D11GeometryShader* gs_ = nullptr;
    ID3D11ClassInstance* gs_ci_[D3D11_SHADER_MAX_INTERFACES]{};
    UINT gs_ci_cnt_ = D3D11_SHADER_MAX_INTERFACES;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11ClassInstance* ps_ci_[D3D11_SHADER_MAX_INTERFACES]{};
    UINT ps_ci_cnt_ = D3D11_SHADER_MAX_INTERFACES;
    ID3D11Buffer* ps_cb0_ = nullptr;
    ID3D11ShaderResourceView* ps_srvs_[2]{};
    ID3D11SamplerState* ps_samp0_ = nullptr;
};

struct DepthVariantShaders {
    ID3D11PixelShader* ps_c1_debug = nullptr;
    ID3D11PixelShader* ps_ao_raw = nullptr;
    ID3D11PixelShader* ps_blur_h = nullptr;
    ID3D11PixelShader* ps_blur_v = nullptr;
    ID3D11PixelShader* ps_composite = nullptr;
};

struct BenchSlot {
    bool in_flight = false;
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* q[6]{};  // 0=start, 1=afterCopy, 2=afterAo, 3=afterBlurH, 4=afterBlurV, 5=afterComp
};

struct RouteCD3DState {
    ID3D11Device* device = nullptr;
    ID3D11VertexShader* vs_fullscreen = nullptr;
    DepthVariantShaders shaders_single{};
    DepthVariantShaders shaders_msaa{};
    bool msaa_shaders_ready = false;

    ID3D11Buffer* cb_params = nullptr;
    ID3D11BlendState* blend_debug_rgb = nullptr;
    ID3D11BlendState* blend_mul_rgb = nullptr;
    ID3D11DepthStencilState* dss_disabled = nullptr;
    ID3D11RasterizerState* rs_fullscreen = nullptr;

    ID3D11Texture2D* depth_copy_tex = nullptr;
    ID3D11ShaderResourceView* depth_copy_srv = nullptr;
    ID3D11Texture2D* ao_rt_tex = nullptr;
    ID3D11RenderTargetView* ao_rt_rtv = nullptr;
    ID3D11ShaderResourceView* ao_rt_srv = nullptr;
    ID3D11Texture2D* ao_blur_tex = nullptr;
    ID3D11RenderTargetView* ao_blur_rtv = nullptr;
    ID3D11ShaderResourceView* ao_blur_srv = nullptr;

    uint32_t depth_w = 0;
    uint32_t depth_h = 0;
    DXGI_FORMAT depth_tex_fmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depth_srv_fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t depth_sample_count = 0;
    uint32_t depth_sample_quality = 0;

    BenchSlot bench[kBenchSlots]{};
    uint32_t bench_frame_seq = 0;
    bool bench_queries_failed = false;

    bool init_failed = false;
    bool logged_unsupported_fmt = false;
};

CRITICAL_SECTION g_d3d_lock;
INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK InitLockOnce(PINIT_ONCE, PVOID, PVOID*) {
    ::InitializeCriticalSection(&g_d3d_lock);
    return TRUE;
}
void EnsureLockInit() {
    ::InitOnceExecuteOnce(&g_lock_once, &InitLockOnce, nullptr, nullptr);
}

RouteCD3DState g_st{};

std::atomic<uint64_t> g_inject_ok{0};
std::atomic<uint64_t> g_inject_skip_no_om{0};
std::atomic<uint64_t> g_inject_skip_fmt{0};
std::atomic<uint64_t> g_inject_skip_fail{0};
std::atomic<uint32_t> g_depth_rebuilds{0};
std::atomic<uint32_t> g_last_tex_w{0};
std::atomic<uint32_t> g_last_tex_h{0};
std::atomic<uint32_t> g_last_rtv_fmt{0};
std::atomic<uint32_t> g_last_dsv_tex_fmt{0};
std::atomic<uint32_t> g_last_sample_count{0};
std::atomic<bool> g_shaders_ready{false};

uint64_t g_bench_samples = 0;
double g_sum_copy_ms = 0.0;
double g_sum_ao_ms = 0.0;
double g_sum_blur_h_ms = 0.0;
double g_sum_blur_v_ms = 0.0;
double g_sum_comp_ms = 0.0;
double g_sum_total_ms = 0.0;

void ReleaseDepthVariantShaders(DepthVariantShaders& s) {
    SafeRelease(s.ps_composite);
    SafeRelease(s.ps_blur_v);
    SafeRelease(s.ps_blur_h);
    SafeRelease(s.ps_ao_raw);
    SafeRelease(s.ps_c1_debug);
}

void ReleaseTargets() {
    SafeRelease(g_st.ao_blur_srv);
    SafeRelease(g_st.ao_blur_rtv);
    SafeRelease(g_st.ao_blur_tex);
    SafeRelease(g_st.ao_rt_srv);
    SafeRelease(g_st.ao_rt_rtv);
    SafeRelease(g_st.ao_rt_tex);
    SafeRelease(g_st.depth_copy_srv);
    SafeRelease(g_st.depth_copy_tex);
    g_st.depth_w = 0;
    g_st.depth_h = 0;
    g_st.depth_tex_fmt = DXGI_FORMAT_UNKNOWN;
    g_st.depth_srv_fmt = DXGI_FORMAT_UNKNOWN;
    g_st.depth_sample_count = 0;
    g_st.depth_sample_quality = 0;
}

void ReleaseBenchQueries() {
    for (BenchSlot& b : g_st.bench) {
        b.in_flight = false;
        SafeRelease(b.disjoint);
        for (ID3D11Query*& q : b.q) {
            SafeRelease(q);
        }
    }
    g_st.bench_queries_failed = false;
}

void ReleaseAllDeviceObjects() {
    ReleaseBenchQueries();
    ReleaseTargets();
    SafeRelease(g_st.rs_fullscreen);
    SafeRelease(g_st.dss_disabled);
    SafeRelease(g_st.blend_mul_rgb);
    SafeRelease(g_st.blend_debug_rgb);
    SafeRelease(g_st.cb_params);
    ReleaseDepthVariantShaders(g_st.shaders_msaa);
    ReleaseDepthVariantShaders(g_st.shaders_single);
    SafeRelease(g_st.vs_fullscreen);
    SafeRelease(g_st.device);
    g_st.msaa_shaders_ready = false;
    g_st.init_failed = false;
    g_shaders_ready.store(false, std::memory_order_relaxed);
}

bool CompileShaderBlob(
    D3DCompileFn compile_fn,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3DBlob** out_blob,
    RouteCD3DLogFn logger) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile_fn(
        kAoShaderSource,
        sizeof(kAoShaderSource) - 1,
        "route_c_ao.hlsl",
        defines,
        nullptr,
        entry,
        target,
        0,
        0,
        out_blob,
        &err);
    if (FAILED(hr) || !out_blob || !*out_blob) {
        if (logger) {
            const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "<no error msg>";
            logger("[RouteC][D3D11] HLSL compile failed (%s %s, hr=0x%08X): %s",
                   entry, target, static_cast<unsigned>(hr), msg);
        }
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);
    return true;
}

bool CompileAndCreatePixelShader(
    ID3D11Device* dev,
    D3DCompileFn compile_fn,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3D11PixelShader** out_ps,
    RouteCD3DLogFn logger) {
    ID3DBlob* blob = nullptr;
    if (!CompileShaderBlob(compile_fn, entry, target, defines, &blob, logger)) {
        return false;
    }
    const HRESULT hr = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out_ps);
    SafeRelease(blob);
    return SUCCEEDED(hr) && *out_ps != nullptr;
}

bool CreateDepthVariantShaders(
    ID3D11Device* dev,
    D3DCompileFn compile_fn,
    const char* ps_profile,
    const D3D_SHADER_MACRO* defines,
    DepthVariantShaders& out,
    RouteCD3DLogFn logger) {
    const bool ok =
        CompileAndCreatePixelShader(dev, compile_fn, "PS_C1DepthNormalDebug", ps_profile, defines, &out.ps_c1_debug, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AoRaw", ps_profile, defines, &out.ps_ao_raw, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_BlurH", ps_profile, defines, &out.ps_blur_h, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_BlurV", ps_profile, defines, &out.ps_blur_v, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AoComposite", ps_profile, defines, &out.ps_composite, logger);
    if (!ok) {
        ReleaseDepthVariantShaders(out);
    }
    return ok;
}

bool EnsurePipelineObjects(ID3D11Device* dev, RouteCD3DLogFn logger) {
    if (g_st.device && g_st.device != dev) {
        if (logger) {
            logger("[RouteC][D3D11] ID3D11Device changed; rebuilding Route C D3D11 objects.");
        }
        ReleaseAllDeviceObjects();
    }
    if (g_st.init_failed) {
        return false;
    }
    if (g_st.vs_fullscreen && g_st.shaders_single.ps_ao_raw && g_st.shaders_single.ps_composite &&
        g_st.cb_params && g_st.blend_debug_rgb && g_st.blend_mul_rgb &&
        g_st.dss_disabled && g_st.rs_fullscreen) {
        return true;
    }

    if (!g_st.device) {
        g_st.device = dev;
        g_st.device->AddRef();
    }

    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        g_st.init_failed = true;
        return false;
    }

    const D3D_SHADER_MACRO macro_single[] = {{"MSAA_DEPTH", "0"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO macro_msaa[] = {{"MSAA_DEPTH", "1"}, {nullptr, nullptr}};

    ID3DBlob* vs_blob = nullptr;
    bool ok = CompileShaderBlob(compile_fn, "VS_Fullscreen", "vs_4_0", macro_single, &vs_blob, logger) &&
              SUCCEEDED(dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_st.vs_fullscreen)) &&
              CreateDepthVariantShaders(dev, compile_fn, "ps_4_0", macro_single, g_st.shaders_single, logger);
    SafeRelease(vs_blob);

    if (ok && dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1) {
        g_st.msaa_shaders_ready = CreateDepthVariantShaders(dev, compile_fn, "ps_4_1", macro_msaa, g_st.shaders_msaa, logger);
    }

    if (!ok) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED creating Route C D3D11 shaders.");
        }
        ReleaseAllDeviceObjects();
        g_st.init_failed = true;
        return false;
    }

    D3D11_BUFFER_DESC cb_desc{};
    cb_desc.ByteWidth = sizeof(CbAoParams);
    cb_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BLEND_DESC bd_dbg{};
    bd_dbg.RenderTarget[0].BlendEnable = FALSE;
    bd_dbg.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_BLEND_DESC bd_mul{};
    bd_mul.RenderTarget[0].BlendEnable = TRUE;
    bd_mul.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bd_mul.RenderTarget[0].DestBlend = D3D11_BLEND_SRC_COLOR;
    bd_mul.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_mul.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_mul.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_mul.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_mul.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_DEPTH_STENCIL_DESC dss_desc{};
    dss_desc.DepthEnable = FALSE;
    dss_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dss_desc.StencilEnable = FALSE;

    D3D11_RASTERIZER_DESC rs_desc{};
    rs_desc.FillMode = D3D11_FILL_SOLID;
    rs_desc.CullMode = D3D11_CULL_NONE;
    rs_desc.DepthClipEnable = FALSE;
    rs_desc.ScissorEnable = FALSE;
    rs_desc.MultisampleEnable = FALSE;

    ok = SUCCEEDED(dev->CreateBuffer(&cb_desc, nullptr, &g_st.cb_params)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_dbg, &g_st.blend_debug_rgb)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_mul, &g_st.blend_mul_rgb)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dss_desc, &g_st.dss_disabled)) &&
         SUCCEEDED(dev->CreateRasterizerState(&rs_desc, &g_st.rs_fullscreen));

    if (!ok) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED creating Route C D3D11 state/CB objects.");
        }
        ReleaseAllDeviceObjects();
        g_st.init_failed = true;
        return false;
    }

    g_shaders_ready.store(true, std::memory_order_relaxed);
    if (logger) {
        logger("[RouteC][D3D11] Compiled C2 AO+Blur+Composite shaders (msaa=%d, WriteMask=RGB).",
               g_st.msaa_shaders_ready ? 1 : 0);
    }
    return true;
}

bool CreateR8AoTarget(
    ID3D11Device* dev,
    uint32_t w,
    uint32_t h,
    ID3D11Texture2D** out_tex,
    ID3D11RenderTargetView** out_rtv,
    ID3D11ShaderResourceView** out_srv) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(dev->CreateTexture2D(&td, nullptr, out_tex)) || !*out_tex) {
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(*out_tex, nullptr, out_rtv)) || !*out_rtv) {
        return false;
    }
    if (FAILED(dev->CreateShaderResourceView(*out_tex, nullptr, out_srv)) || !*out_srv) {
        return false;
    }
    return true;
}

bool EnsureAoTargets(
    ID3D11Device* dev,
    const D3D11_TEXTURE2D_DESC& src_desc,
    const DepthCopyFormats& fmts,
    RouteCD3DLogFn logger) {
    if (g_st.depth_copy_tex && g_st.depth_copy_srv &&
        g_st.ao_rt_tex && g_st.ao_rt_rtv && g_st.ao_rt_srv &&
        g_st.ao_blur_tex && g_st.ao_blur_rtv && g_st.ao_blur_srv &&
        g_st.depth_w == src_desc.Width &&
        g_st.depth_h == src_desc.Height &&
        g_st.depth_tex_fmt == fmts.tex_format &&
        g_st.depth_srv_fmt == fmts.srv_format &&
        g_st.depth_sample_count == src_desc.SampleDesc.Count &&
        g_st.depth_sample_quality == src_desc.SampleDesc.Quality) {
        return true;
    }

    ReleaseTargets();

    D3D11_TEXTURE2D_DESC copy_desc{};
    copy_desc.Width = src_desc.Width;
    copy_desc.Height = src_desc.Height;
    copy_desc.MipLevels = 1;
    copy_desc.ArraySize = 1;
    copy_desc.Format = fmts.tex_format;
    copy_desc.SampleDesc = src_desc.SampleDesc;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(dev->CreateTexture2D(&copy_desc, nullptr, &g_st.depth_copy_tex)) || !g_st.depth_copy_tex) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED CreateTexture2D for depth copy (%ux%u).", src_desc.Width, src_desc.Height);
        }
        ReleaseTargets();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
    srv_desc.Format = fmts.srv_format;
    if (src_desc.SampleDesc.Count > 1) {
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = 1;
    }
    if (FAILED(dev->CreateShaderResourceView(g_st.depth_copy_tex, &srv_desc, &g_st.depth_copy_srv)) || !g_st.depth_copy_srv) {
        ReleaseTargets();
        return false;
    }

    const uint32_t ao_w = (std::max)(1U, (src_desc.Width + 1U) / 2U);
    const uint32_t ao_h = (std::max)(1U, (src_desc.Height + 1U) / 2U);
    if (!CreateR8AoTarget(dev, ao_w, ao_h, &g_st.ao_rt_tex, &g_st.ao_rt_rtv, &g_st.ao_rt_srv) ||
        !CreateR8AoTarget(dev, ao_w, ao_h, &g_st.ao_blur_tex, &g_st.ao_blur_rtv, &g_st.ao_blur_srv)) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED creating R8_UNORM half-res AO targets (%ux%u).", ao_w, ao_h);
        }
        ReleaseTargets();
        return false;
    }

    g_st.depth_w = src_desc.Width;
    g_st.depth_h = src_desc.Height;
    g_st.depth_tex_fmt = fmts.tex_format;
    g_st.depth_srv_fmt = fmts.srv_format;
    g_st.depth_sample_count = src_desc.SampleDesc.Count;
    g_st.depth_sample_quality = src_desc.SampleDesc.Quality;

    const uint32_t rebuilds = g_depth_rebuilds.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logger && rebuilds <= 8) {
        logger("[RouteC][D3D11] Built Route C targets #%u: %ux%u texFmt=%u srvFmt=%u msaa=%ux%u.",
               rebuilds,
               g_st.depth_w,
               g_st.depth_h,
               static_cast<unsigned>(g_st.depth_tex_fmt),
               static_cast<unsigned>(g_st.depth_srv_fmt),
               g_st.depth_sample_count,
               g_st.depth_sample_quality);
    }
    return true;
}

bool EnsureBenchQueries(ID3D11Device* dev) {
    if (g_st.bench_queries_failed) {
        return false;
    }
    if (g_st.bench[0].disjoint) {
        return true;
    }
    D3D11_QUERY_DESC dq{};
    dq.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC tq{};
    tq.Query = D3D11_QUERY_TIMESTAMP;
    for (BenchSlot& b : g_st.bench) {
        if (FAILED(dev->CreateQuery(&dq, &b.disjoint)) || !b.disjoint) {
            ReleaseBenchQueries();
            g_st.bench_queries_failed = true;
            return false;
        }
        for (ID3D11Query*& q : b.q) {
            if (FAILED(dev->CreateQuery(&tq, &q)) || !q) {
                ReleaseBenchQueries();
                g_st.bench_queries_failed = true;
                return false;
            }
        }
    }
    return true;
}

void HarvestBenchSlot(ID3D11DeviceContext* ctx, BenchSlot& b) {
    if (!b.in_flight || !b.disjoint) {
        return;
    }
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
    if (ctx->GetData(b.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
        return;
    }
    uint64_t ts[6]{};
    for (size_t i = 0; i < 6; ++i) {
        if (ctx->GetData(b.q[i], &ts[i], sizeof(uint64_t), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            return;
        }
    }
    b.in_flight = false;
    if (dj.Disjoint || dj.Frequency == 0) {
        return;
    }
    const double to_ms = 1000.0 / static_cast<double>(dj.Frequency);
    const auto delta_ms = [to_ms](uint64_t a, uint64_t c) -> double {
        return (c >= a) ? static_cast<double>(c - a) * to_ms : 0.0;
    };
    const double copy_ms = delta_ms(ts[0], ts[1]);
    const double ao_ms = delta_ms(ts[1], ts[2]);
    const double bh_ms = delta_ms(ts[2], ts[3]);
    const double bv_ms = delta_ms(ts[3], ts[4]);
    const double comp_ms = delta_ms(ts[4], ts[5]);
    const double total_ms = delta_ms(ts[0], ts[5]);

    ++g_bench_samples;
    g_sum_copy_ms += copy_ms;
    g_sum_ao_ms += ao_ms;
    g_sum_blur_h_ms += bh_ms;
    g_sum_blur_v_ms += bv_ms;
    g_sum_comp_ms += comp_ms;
    g_sum_total_ms += total_ms;
}

BenchSlot* BeginBenchSlot(ID3D11Device* dev, ID3D11DeviceContext* ctx, const AoTuneConfig& cfg) {
    if (cfg.bench_mode <= 0) {
        return nullptr;
    }
    const uint32_t seq = g_st.bench_frame_seq++;
    if (!EnsureBenchQueries(dev)) {
        return nullptr;
    }
    for (BenchSlot& b : g_st.bench) {
        HarvestBenchSlot(ctx, b);
    }
    if (cfg.bench_mode == 1 && (seq & 31U) != 0U) {
        return nullptr;
    }
    for (BenchSlot& b : g_st.bench) {
        if (!b.in_flight) {
            b.in_flight = true;
            ctx->Begin(b.disjoint);
            ctx->End(b.q[0]);
            return &b;
        }
    }
    return nullptr;
}

void RecordTimestamp(ID3D11DeviceContext* ctx, BenchSlot* slot, size_t idx) {
    if (slot && idx < 6) {
        ctx->End(slot->q[idx]);
    }
}

}  // namespace

bool ExecuteRouteCAoPass(const RouteCAoRenderRequest& req, RouteCD3DLogFn logger) {
    if (!req.ctx || !req.proj_4x4 || req.vp_width <= 0 || req.vp_height <= 0) {
        return false;
    }

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    struct LockGuard {
        ~LockGuard() { ::LeaveCriticalSection(&g_d3d_lock); }
    } lock_guard;

    D3D11StateGuard state_guard(req.ctx);

    ID3D11RenderTargetView* target_rtv = state_guard.om_rtv0() ? state_guard.om_rtv0() : req.fallback_rtv;
    ID3D11DepthStencilView* target_dsv = state_guard.om_dsv() ? state_guard.om_dsv() : req.fallback_dsv;
    if (!target_rtv || !target_dsv) {
        g_inject_skip_no_om.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ID3D11Resource* depth_res = nullptr;
    target_dsv->GetResource(&depth_res);
    ID3D11Texture2D* src_depth_tex = nullptr;
    if (!depth_res || FAILED(depth_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&src_depth_tex))) || !src_depth_tex) {
        SafeRelease(src_depth_tex);
        SafeRelease(depth_res);
        g_inject_skip_no_om.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    SafeRelease(depth_res);

    D3D11_TEXTURE2D_DESC depth_desc{};
    src_depth_tex->GetDesc(&depth_desc);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    target_dsv->GetDesc(&dsv_desc);
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    target_rtv->GetDesc(&rtv_desc);

    const DepthCopyFormats fmts = MapDepthCopyFormats(depth_desc.Format, dsv_desc.Format);
    if (!fmts.supported) {
        SafeRelease(src_depth_tex);
        g_inject_skip_fmt.fetch_add(1, std::memory_order_relaxed);
        if (!g_st.logged_unsupported_fmt && logger) {
            g_st.logged_unsupported_fmt = true;
            logger("[RouteC][D3D11] Unsupported depth format texFmt=%u dsvFmt=%u; skipping AO.",
                   static_cast<unsigned>(depth_desc.Format),
                   static_cast<unsigned>(dsv_desc.Format));
        }
        return false;
    }

    UINT src_mip = 0;
    const UINT src_subres = ComputeDsvSubresource(dsv_desc, depth_desc.MipLevels, &src_mip);
    if (src_mip != 0) {
        SafeRelease(src_depth_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    ID3D11Device* dev = nullptr;
    req.ctx->GetDevice(&dev);
    if (!dev) {
        SafeRelease(src_depth_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (!EnsurePipelineObjects(dev, logger) || !EnsureAoTargets(dev, depth_desc, fmts, logger)) {
        SafeRelease(dev);
        SafeRelease(src_depth_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const bool use_msaa = (depth_desc.SampleDesc.Count > 1);
    if (use_msaa && !g_st.msaa_shaders_ready) {
        SafeRelease(dev);
        SafeRelease(src_depth_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const DepthVariantShaders& ds = use_msaa ? g_st.shaders_msaa : g_st.shaders_single;

    const int min_x = (std::max)(0, req.vp_left);
    const int min_y = (std::max)(0, req.vp_top);
    const int max_x = (std::min)(static_cast<int>(depth_desc.Width) - 1, req.vp_left + req.vp_width - 1);
    const int max_y = (std::min)(static_cast<int>(depth_desc.Height) - 1, req.vp_top + req.vp_height - 1);
    if (min_x > max_x || min_y > max_y) {
        SafeRelease(dev);
        SafeRelease(src_depth_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const AoTuneConfig& cfg = GetAoTuneConfig(logger);
    BenchSlot* bench_slot = BeginBenchSlot(dev, req.ctx, cfg);
    SafeRelease(dev);

    // Pass 0: Unbind DSV/SRVs and copy depth cleanly.
    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    req.ctx->PSSetShaderResources(0, 2, null_srvs);
    req.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    req.ctx->CopySubresourceRegion(g_st.depth_copy_tex, 0, 0, 0, 0, src_depth_tex, src_subres, nullptr);
    SafeRelease(src_depth_tex);
    RecordTimestamp(req.ctx, bench_slot, 1);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(req.ctx->Map(g_st.cb_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        if (bench_slot) {
            req.ctx->End(bench_slot->disjoint);
            bench_slot->in_flight = false;
        }
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const float* P = req.proj_4x4;
    const float fade_start = cfg.fade_start;
    const float fade_end = (std::max)(fade_start + 5.0f, (std::min)(req.far_clip * 0.92f, cfg.fade_end_cap));
    CbAoParams cb{};
    cb.proj_xy[0] = P[0];   // m[0][0]
    cb.proj_xy[1] = P[5];   // m[1][1]
    cb.proj_xy[2] = P[2];   // m[0][2]
    cb.proj_xy[3] = P[6];   // m[1][2]
    cb.proj_z_fade[0] = P[10];  // m[2][2]
    cb.proj_z_fade[1] = P[11];  // m[2][3]
    cb.proj_z_fade[2] = fade_end;
    cb.proj_z_fade[3] = 1.0f / (std::max)(fade_end - fade_start, 1e-3f);
    cb.vp_rect[0] = static_cast<float>(req.vp_left);
    cb.vp_rect[1] = static_cast<float>(req.vp_top);
    cb.vp_rect[2] = static_cast<float>(req.vp_width);
    cb.vp_rect[3] = static_cast<float>(req.vp_height);
    cb.vp_clamp[0] = min_x;
    cb.vp_clamp[1] = min_y;
    cb.vp_clamp[2] = max_x;
    cb.vp_clamp[3] = max_y;
    cb.ao_params[0] = cfg.radius_world;
    cb.ao_params[1] = cfg.bias_cos;
    cb.ao_params[2] = cfg.strength;
    cb.ao_params[3] = cfg.min_ao_floor;
    cb.blur_and_mode[0] = 1.0f / static_cast<float>(req.vp_width);
    cb.blur_and_mode[1] = 1.0f / static_cast<float>(req.vp_height);
    cb.blur_and_mode[2] = cfg.depth_edge_scale;
    cb.blur_and_mode[3] = static_cast<float>(req.debug_mode);
    std::memcpy(mapped.pData, &cb, sizeof(cb));
    req.ctx->Unmap(g_st.cb_params, 0);

    D3D11_VIEWPORT ao_vp{};
    ao_vp.TopLeftX = static_cast<float>(req.vp_left);
    ao_vp.TopLeftY = static_cast<float>(req.vp_top);
    ao_vp.Width = static_cast<float>(req.vp_width);
    ao_vp.Height = static_cast<float>(req.vp_height);
    ao_vp.MinDepth = 0.0f;
    ao_vp.MaxDepth = 1.0f;

    const int half_w = (std::max)(1, ((req.vp_left + req.vp_width - 1) >> 1) - (req.vp_left >> 1) + 1);
    const int half_h = (std::max)(1, ((req.vp_top + req.vp_height - 1) >> 1) - (req.vp_top >> 1) + 1);
    D3D11_VIEWPORT ao_vp_half{};
    ao_vp_half.TopLeftX = static_cast<float>(req.vp_left >> 1);
    ao_vp_half.TopLeftY = static_cast<float>(req.vp_top >> 1);
    ao_vp_half.Width = static_cast<float>(half_w);
    ao_vp_half.Height = static_cast<float>(half_h);
    ao_vp_half.MinDepth = 0.0f;
    ao_vp_half.MaxDepth = 1.0f;

    req.ctx->OMSetDepthStencilState(g_st.dss_disabled, 0);
    req.ctx->RSSetState(g_st.rs_fullscreen);
    req.ctx->RSSetScissorRects(0, nullptr);
    req.ctx->IASetInputLayout(nullptr);
    req.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    req.ctx->VSSetShader(g_st.vs_fullscreen, nullptr, 0);
    req.ctx->GSSetShader(nullptr, nullptr, 0);
    req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_params);

    if (req.debug_mode == 3) {
        req.ctx->RSSetViewports(1, &ao_vp);
        req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
        req.ctx->OMSetBlendState(g_st.blend_debug_rgb, nullptr, 0xFFFFFFFFU);
        req.ctx->PSSetShader(ds.ps_c1_debug, nullptr, 0);
        ID3D11ShaderResourceView* srvs_c1[2] = {g_st.depth_copy_srv, nullptr};
        req.ctx->PSSetShaderResources(0, 2, srvs_c1);
        req.ctx->Draw(3, 0);
        RecordTimestamp(req.ctx, bench_slot, 2);
        RecordTimestamp(req.ctx, bench_slot, 3);
        RecordTimestamp(req.ctx, bench_slot, 4);
        RecordTimestamp(req.ctx, bench_slot, 5);
    } else {
        // Pass 1 (Half-Res): HBAO/GTAO-Lite Raw -> ao_rt_rtv
        req.ctx->RSSetViewports(1, &ao_vp_half);
        req.ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFU);
        req.ctx->OMSetRenderTargets(1, &g_st.ao_rt_rtv, nullptr);
        req.ctx->PSSetShader(ds.ps_ao_raw, nullptr, 0);
        ID3D11ShaderResourceView* srvs_p1[2] = {g_st.depth_copy_srv, nullptr};
        req.ctx->PSSetShaderResources(0, 2, srvs_p1);
        req.ctx->Draw(3, 0);
        RecordTimestamp(req.ctx, bench_slot, 2);

        if (req.debug_mode != 1) {
            // Pass 2 (Half-Res): Bilateral Blur Horizontal -> ao_blur_rtv
            req.ctx->OMSetRenderTargets(1, &g_st.ao_blur_rtv, nullptr);
            req.ctx->PSSetShader(ds.ps_blur_h, nullptr, 0);
            ID3D11ShaderResourceView* srvs_p2[2] = {g_st.depth_copy_srv, g_st.ao_rt_srv};
            req.ctx->PSSetShaderResources(0, 2, srvs_p2);
            req.ctx->Draw(3, 0);
            RecordTimestamp(req.ctx, bench_slot, 3);

            // Pass 3 (Half-Res): Bilateral Blur Vertical -> ao_rt_rtv
            ID3D11ShaderResourceView* unbind_t1[2] = {g_st.depth_copy_srv, nullptr};
            req.ctx->PSSetShaderResources(0, 2, unbind_t1);
            req.ctx->OMSetRenderTargets(1, &g_st.ao_rt_rtv, nullptr);
            req.ctx->PSSetShader(ds.ps_blur_v, nullptr, 0);
            ID3D11ShaderResourceView* srvs_p3[2] = {g_st.depth_copy_srv, g_st.ao_blur_srv};
            req.ctx->PSSetShaderResources(0, 2, srvs_p3);
            req.ctx->Draw(3, 0);
            RecordTimestamp(req.ctx, bench_slot, 4);
        } else {
            RecordTimestamp(req.ctx, bench_slot, 3);
            RecordTimestamp(req.ctx, bench_slot, 4);
        }

        // Pass 4 (Full-Res Joint-Bilateral Upsample): RGB-only Multiplicative Composite or Grayscale Debug -> target_rtv
        req.ctx->PSSetShaderResources(0, 2, null_srvs);
        req.ctx->RSSetViewports(1, &ao_vp);
        req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
        req.ctx->OMSetBlendState(
            (req.debug_mode == 0) ? g_st.blend_mul_rgb : g_st.blend_debug_rgb,
            nullptr,
            0xFFFFFFFFU);
        req.ctx->PSSetShader(ds.ps_composite, nullptr, 0);
        ID3D11ShaderResourceView* srvs_p4[2] = {g_st.depth_copy_srv, g_st.ao_rt_srv};
        req.ctx->PSSetShaderResources(0, 2, srvs_p4);
        req.ctx->Draw(3, 0);
        RecordTimestamp(req.ctx, bench_slot, 5);
    }

    if (bench_slot) {
        req.ctx->End(bench_slot->disjoint);
    }

    g_last_tex_w.store(depth_desc.Width, std::memory_order_relaxed);
    g_last_tex_h.store(depth_desc.Height, std::memory_order_relaxed);
    g_last_rtv_fmt.store(static_cast<uint32_t>(rtv_desc.Format), std::memory_order_relaxed);
    g_last_dsv_tex_fmt.store(static_cast<uint32_t>(depth_desc.Format), std::memory_order_relaxed);
    g_last_sample_count.store(depth_desc.SampleDesc.Count, std::memory_order_relaxed);
    g_inject_ok.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool VerifyRouteCShadersCompile(RouteCD3DLogFn logger) {
    D3DCompileFn compile_fn = ResolveD3DCompile(logger);
    if (!compile_fn) {
        return false;
    }
    const D3D_SHADER_MACRO macro_single[] = {{"MSAA_DEPTH", "0"}, {nullptr, nullptr}};
    const D3D_SHADER_MACRO macro_msaa[] = {{"MSAA_DEPTH", "1"}, {nullptr, nullptr}};

    struct Spec {
        const char* entry;
        const char* target;
        const D3D_SHADER_MACRO* macros;
    };
    const Spec specs[] = {
        {"VS_Fullscreen", "vs_4_0", macro_single},
        {"PS_AoComposite", "ps_4_0", macro_single},
        {"PS_AoComposite", "ps_4_1", macro_msaa},
        {"PS_C1DepthNormalDebug", "ps_4_0", macro_single},
        {"PS_C1DepthNormalDebug", "ps_4_1", macro_msaa},
        {"PS_AoRaw", "ps_4_0", macro_single},
        {"PS_AoRaw", "ps_4_1", macro_msaa},
        {"PS_BlurH", "ps_4_0", macro_single},
        {"PS_BlurH", "ps_4_1", macro_msaa},
        {"PS_BlurV", "ps_4_0", macro_single},
        {"PS_BlurV", "ps_4_1", macro_msaa},
    };
    for (const Spec& sp : specs) {
        ID3DBlob* blob = nullptr;
        const bool ok = CompileShaderBlob(compile_fn, sp.entry, sp.target, sp.macros, &blob, logger);
        SafeRelease(blob);
        if (!ok) {
            return false;
        }
    }
    return true;
}

void ShutdownRouteCD3D11Resources() {
    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    ReleaseAllDeviceObjects();
    ::LeaveCriticalSection(&g_d3d_lock);
}

RouteCD3DStats GetRouteCD3DStats() {
    RouteCD3DStats s{};
    s.inject_ok = g_inject_ok.load(std::memory_order_relaxed);
    s.inject_skip_no_om = g_inject_skip_no_om.load(std::memory_order_relaxed);
    s.inject_skip_fmt = g_inject_skip_fmt.load(std::memory_order_relaxed);
    s.inject_skip_fail = g_inject_skip_fail.load(std::memory_order_relaxed);
    s.depth_rebuilds = g_depth_rebuilds.load(std::memory_order_relaxed);
    s.last_tex_w = g_last_tex_w.load(std::memory_order_relaxed);
    s.last_tex_h = g_last_tex_h.load(std::memory_order_relaxed);
    s.last_rtv_fmt = g_last_rtv_fmt.load(std::memory_order_relaxed);
    s.last_dsv_tex_fmt = g_last_dsv_tex_fmt.load(std::memory_order_relaxed);
    s.last_sample_count = g_last_sample_count.load(std::memory_order_relaxed);
    s.shaders_ready = g_shaders_ready.load(std::memory_order_relaxed);

    EnsureLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
    if (g_bench_samples > 0) {
        const double inv_n = 1.0 / static_cast<double>(g_bench_samples);
        s.bench_samples = g_bench_samples;
        s.avg_copy_ms = g_sum_copy_ms * inv_n;
        s.avg_ao_ms = g_sum_ao_ms * inv_n;
        s.avg_blur_h_ms = g_sum_blur_h_ms * inv_n;
        s.avg_blur_v_ms = g_sum_blur_v_ms * inv_n;
        s.avg_comp_ms = g_sum_comp_ms * inv_n;
        s.avg_total_ms = g_sum_total_ms * inv_n;
    }
    ::LeaveCriticalSection(&g_d3d_lock);
    return s;
}

}  // namespace routeb
