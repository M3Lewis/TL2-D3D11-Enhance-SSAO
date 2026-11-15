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
#include <cstring>

namespace routeb {

namespace {

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

    // Clean vertical divider between Left (Linear Depth Contours) and Right (VS Normals).
    if (abs(uv.x - 0.5f) < gBlurAndMode.x * 1.5f)
    {
        return float4(0.95f, 0.80f, 0.15f, 1.0f);
    }

    // Sky / far-cleared pixels remain clean without NaN.
    if (rawDepth >= 0.99998f)
    {
        return float4(0.06f, 0.16f, 0.30f, 1.0f);
    }

    if (uv.x < 0.5f)
    {
        float d = -P.z;
        float normDist = saturate((d - 1.0f) / 48.0f);
        float phase = frac(d * 0.5f); // 1 band per 2.0 world units
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

struct RouteCD3DState {
    ID3D11Device* device = nullptr;
    ID3D11VertexShader* vs_fullscreen = nullptr;
    ID3D11PixelShader* ps_c1_debug_single = nullptr;
    ID3D11PixelShader* ps_c1_debug_msaa = nullptr;
    ID3D11Buffer* cb_params = nullptr;
    ID3D11BlendState* blend_debug_rgb = nullptr;
    ID3D11BlendState* blend_mul_rgb = nullptr;
    ID3D11DepthStencilState* dss_disabled = nullptr;
    ID3D11RasterizerState* rs_fullscreen = nullptr;

    ID3D11Texture2D* depth_copy_tex = nullptr;
    ID3D11ShaderResourceView* depth_copy_srv = nullptr;
    uint32_t depth_w = 0;
    uint32_t depth_h = 0;
    DXGI_FORMAT depth_tex_fmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT depth_srv_fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t depth_sample_count = 0;
    uint32_t depth_sample_quality = 0;

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

void ReleaseDepthTargets() {
    SafeRelease(g_st.depth_copy_srv);
    SafeRelease(g_st.depth_copy_tex);
    g_st.depth_w = 0;
    g_st.depth_h = 0;
    g_st.depth_tex_fmt = DXGI_FORMAT_UNKNOWN;
    g_st.depth_srv_fmt = DXGI_FORMAT_UNKNOWN;
    g_st.depth_sample_count = 0;
    g_st.depth_sample_quality = 0;
}

void ReleaseAllDeviceObjects() {
    ReleaseDepthTargets();
    SafeRelease(g_st.rs_fullscreen);
    SafeRelease(g_st.dss_disabled);
    SafeRelease(g_st.blend_mul_rgb);
    SafeRelease(g_st.blend_debug_rgb);
    SafeRelease(g_st.cb_params);
    SafeRelease(g_st.ps_c1_debug_msaa);
    SafeRelease(g_st.ps_c1_debug_single);
    SafeRelease(g_st.vs_fullscreen);
    SafeRelease(g_st.device);
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
    if (g_st.vs_fullscreen && g_st.ps_c1_debug_single &&
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
    const bool supports_sm41 = (dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1);

    ID3DBlob* vs_blob = nullptr;
    ID3DBlob* ps_single_blob = nullptr;
    ID3DBlob* ps_msaa_blob = nullptr;

    bool ok = CompileShaderBlob(compile_fn, "VS_Fullscreen", "vs_4_0", macro_single, &vs_blob, logger) &&
              CompileShaderBlob(compile_fn, "PS_C1DepthNormalDebug", "ps_4_0", macro_single, &ps_single_blob, logger);

    if (ok) {
        ok = SUCCEEDED(dev->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &g_st.vs_fullscreen)) &&
             SUCCEEDED(dev->CreatePixelShader(ps_single_blob->GetBufferPointer(), ps_single_blob->GetBufferSize(), nullptr, &g_st.ps_c1_debug_single));
    }
    if (ok && supports_sm41) {
        if (CompileShaderBlob(compile_fn, "PS_C1DepthNormalDebug", "ps_4_1", macro_msaa, &ps_msaa_blob, logger)) {
            if (FAILED(dev->CreatePixelShader(ps_msaa_blob->GetBufferPointer(), ps_msaa_blob->GetBufferSize(), nullptr, &g_st.ps_c1_debug_msaa))) {
                SafeRelease(g_st.ps_c1_debug_msaa);
            }
        }
    }
    SafeRelease(vs_blob);
    SafeRelease(ps_single_blob);
    SafeRelease(ps_msaa_blob);

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
        logger("[RouteC][D3D11] Compiled C1 shaders and created shared D3D11 states (WriteMask=RGB).");
    }
    return true;
}

bool EnsureDepthCopyTargets(
    ID3D11Device* dev,
    const D3D11_TEXTURE2D_DESC& src_desc,
    const DepthCopyFormats& fmts,
    RouteCD3DLogFn logger) {
    if (g_st.depth_copy_tex && g_st.depth_copy_srv &&
        g_st.depth_w == src_desc.Width &&
        g_st.depth_h == src_desc.Height &&
        g_st.depth_tex_fmt == fmts.tex_format &&
        g_st.depth_srv_fmt == fmts.srv_format &&
        g_st.depth_sample_count == src_desc.SampleDesc.Count &&
        g_st.depth_sample_quality == src_desc.SampleDesc.Quality) {
        return true;
    }

    ReleaseDepthTargets();

    D3D11_TEXTURE2D_DESC copy_desc{};
    copy_desc.Width = src_desc.Width;
    copy_desc.Height = src_desc.Height;
    copy_desc.MipLevels = 1;
    copy_desc.ArraySize = 1;
    copy_desc.Format = fmts.tex_format;
    copy_desc.SampleDesc = src_desc.SampleDesc;
    copy_desc.Usage = D3D11_USAGE_DEFAULT;
    copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&copy_desc, nullptr, &tex)) || !tex) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED CreateTexture2D for depth copy (%ux%u fmt=%u msaa=%ux%u).",
                   src_desc.Width,
                   src_desc.Height,
                   static_cast<unsigned>(fmts.tex_format),
                   src_desc.SampleDesc.Count,
                   src_desc.SampleDesc.Quality);
        }
        SafeRelease(tex);
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

    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(dev->CreateShaderResourceView(tex, &srv_desc, &srv)) || !srv) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED CreateShaderResourceView for depth copy (srvFmt=%u msaa=%ux%u).",
                   static_cast<unsigned>(fmts.srv_format),
                   src_desc.SampleDesc.Count,
                   src_desc.SampleDesc.Quality);
        }
        SafeRelease(srv);
        SafeRelease(tex);
        return false;
    }

    g_st.depth_copy_tex = tex;
    g_st.depth_copy_srv = srv;
    g_st.depth_w = src_desc.Width;
    g_st.depth_h = src_desc.Height;
    g_st.depth_tex_fmt = fmts.tex_format;
    g_st.depth_srv_fmt = fmts.srv_format;
    g_st.depth_sample_count = src_desc.SampleDesc.Count;
    g_st.depth_sample_quality = src_desc.SampleDesc.Quality;

    const uint32_t rebuilds = g_depth_rebuilds.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logger && rebuilds <= 8) {
        logger("[RouteC][D3D11] Built gDepthCopyTex #%u: %ux%u texFmt=%u srvFmt=%u msaa=%ux%u.",
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

    if (!EnsurePipelineObjects(dev, logger) || !EnsureDepthCopyTargets(dev, depth_desc, fmts, logger)) {
        SafeRelease(dev);
        SafeRelease(src_depth_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    SafeRelease(dev);

    ID3D11PixelShader* ps = (depth_desc.SampleDesc.Count > 1) ? g_st.ps_c1_debug_msaa : g_st.ps_c1_debug_single;
    if (!ps) {
        SafeRelease(src_depth_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const int min_x = (std::max)(0, req.vp_left);
    const int min_y = (std::max)(0, req.vp_top);
    const int max_x = (std::min)(static_cast<int>(depth_desc.Width) - 1, req.vp_left + req.vp_width - 1);
    const int max_y = (std::min)(static_cast<int>(depth_desc.Height) - 1, req.vp_top + req.vp_height - 1);
    if (min_x > max_x || min_y > max_y) {
        SafeRelease(src_depth_tex);
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Detach DSV before copying depth and before reading the depth copy SRV in PS.
    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    req.ctx->PSSetShaderResources(0, 2, null_srvs);
    req.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    req.ctx->CopySubresourceRegion(g_st.depth_copy_tex, 0, 0, 0, 0, src_depth_tex, src_subres, nullptr);
    SafeRelease(src_depth_tex);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(req.ctx->Map(g_st.cb_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        g_inject_skip_fail.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const float* P = req.proj_4x4;
    const float fade_start = 40.0f;
    const float fade_end = (std::max)(fade_start + 5.0f, (std::min)(req.far_clip * 0.9f, 75.0f));
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
    cb.ao_params[0] = 0.75f;  // radiusWorld placeholder before C2 calibration
    cb.ao_params[1] = 0.10f;  // biasCos
    cb.ao_params[2] = 0.85f;  // strength
    cb.ao_params[3] = 0.80f;  // minAoFloor
    cb.blur_and_mode[0] = 1.0f / static_cast<float>(req.vp_width);
    cb.blur_and_mode[1] = 1.0f / static_cast<float>(req.vp_height);
    cb.blur_and_mode[2] = 40.0f;  // relative depth edge scale
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

    req.ctx->OMSetRenderTargets(1, &target_rtv, nullptr);
    req.ctx->OMSetBlendState(g_st.blend_debug_rgb, nullptr, 0xFFFFFFFFU);
    req.ctx->OMSetDepthStencilState(g_st.dss_disabled, 0);
    req.ctx->RSSetState(g_st.rs_fullscreen);
    req.ctx->RSSetViewports(1, &ao_vp);
    req.ctx->RSSetScissorRects(0, nullptr);
    req.ctx->IASetInputLayout(nullptr);
    req.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    req.ctx->VSSetShader(g_st.vs_fullscreen, nullptr, 0);
    req.ctx->GSSetShader(nullptr, nullptr, 0);

    req.ctx->PSSetShader(ps, nullptr, 0);
    req.ctx->PSSetConstantBuffers(0, 1, &g_st.cb_params);
    req.ctx->PSSetShaderResources(0, 1, &g_st.depth_copy_srv);
    req.ctx->Draw(3, 0);

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
    ID3DBlob* vs = nullptr;
    ID3DBlob* ps_single = nullptr;
    ID3DBlob* ps_msaa = nullptr;
    const bool ok =
        CompileShaderBlob(compile_fn, "VS_Fullscreen", "vs_4_0", macro_single, &vs, logger) &&
        CompileShaderBlob(compile_fn, "PS_C1DepthNormalDebug", "ps_4_0", macro_single, &ps_single, logger) &&
        CompileShaderBlob(compile_fn, "PS_C1DepthNormalDebug", "ps_4_1", macro_msaa, &ps_msaa, logger);
    SafeRelease(vs);
    SafeRelease(ps_single);
    SafeRelease(ps_msaa);
    return ok;
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
    return s;
}

}  // namespace routeb
