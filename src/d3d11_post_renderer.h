#pragma once

#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

#include "d3d11_post_types.h"
#include "d3d11_ssao_pass.h"

namespace routeb {

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
inline void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

inline void SafeReleaseClassInstances(ID3D11ClassInstance** arr, UINT count) {
    if (!arr) return;
    const UINT n = (std::min)(count, static_cast<UINT>(D3D11_SHADER_MAX_INTERFACES));
    for (UINT i = 0; i < n; ++i) SafeRelease(arr[i]);
}

inline UINT ComputeDsvSubresource(
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
        if (!ctx_) return;
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
        ctx_->PSGetConstantBuffers(0, 4, ps_cbs_);
        ctx_->PSGetShaderResources(0, 8, ps_srvs_);
        ctx_->PSGetSamplers(0, 4, ps_samps_);
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
            ctx_->PSSetConstantBuffers(0, 4, ps_cbs_);
            ctx_->PSSetShaderResources(0, 8, ps_srvs_);
            ctx_->PSSetSamplers(0, 4, ps_samps_);
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
        for (UINT i = 0; i < 4; ++i) {
            SafeRelease(ps_cbs_[i]);
        }
        for (UINT i = 0; i < 8; ++i) {
            SafeRelease(ps_srvs_[i]);
        }
        for (UINT i = 0; i < 4; ++i) {
            SafeRelease(ps_samps_[i]);
        }
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
    ID3D11Buffer* ps_cbs_[4]{};
    ID3D11ShaderResourceView* ps_srvs_[8]{};
    ID3D11SamplerState* ps_samps_[4]{};
};

struct DepthVariantShaders {
    ID3D11PixelShader* ps_c1_debug = nullptr;
    ID3D11PixelShader* ps_ao_raw = nullptr;
    ID3D11PixelShader* ps_blur_h = nullptr;
    ID3D11PixelShader* ps_blur_v = nullptr;
    ID3D11PixelShader* ps_composite = nullptr;
    ID3D11PixelShader* ps_comp_dual = nullptr;
    ID3D11PixelShader* ps_aerial = nullptr;
    ID3D11PixelShader* ps_ssr = nullptr;
    ID3D11PixelShader* ps_ssr_resolve = nullptr;
};

struct BenchSlot {
    bool in_flight = false;
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* q[6]{};  // 0=start, 1=afterCopy, 2=afterAo, 3=afterBlurH, 4=afterBlurV, 5=afterComp
};

struct PostRenderContext {
    ID3D11Device* device = nullptr;
    ID3D11VertexShader* vs_fullscreen = nullptr;
    DepthVariantShaders shaders_single{};
    DepthVariantShaders shaders_msaa{};
    bool msaa_shaders_ready = false;

    ID3D11Buffer* cb_params = nullptr;
    ID3D11Buffer* cb_toast = nullptr;
    ID3D11PixelShader* ps_toast = nullptr;
    ID3D11Buffer* cb_p2_filter = nullptr;
    ID3D11PixelShader* ps_p2_filter = nullptr;
    ID3D11Buffer* cb_bloom_lut = nullptr;
    ID3D11PixelShader* ps_lut_grade = nullptr;
    ID3D11Buffer* cb_panel = nullptr;
    ID3D11PixelShader* ps_panel = nullptr;
    ID3D11PixelShader* ps_cursor = nullptr;
    ID3D11SamplerState* samp_linear = nullptr;
    ID3D11Texture2D* panel_tex = nullptr;
    ID3D11ShaderResourceView* panel_srv = nullptr;
    uint32_t panel_tex_w = 0;
    uint32_t panel_tex_h = 0;
    ID3D11BlendState* blend_debug_rgb = nullptr;
    ID3D11BlendState* blend_mul_rgb = nullptr;
    ID3D11BlendState* blend_dual_ao = nullptr;
    ID3D11BlendState* blend_toast_alpha = nullptr;
    ID3D11DepthStencilState* dss_disabled = nullptr;
    ID3D11RasterizerState* rs_fullscreen = nullptr;

    ID3D11Texture2D* p2_copy_tex = nullptr;
    ID3D11ShaderResourceView* p2_copy_srv = nullptr;
    uint32_t p2_copy_w = 0;
    uint32_t p2_copy_h = 0;
    DXGI_FORMAT p2_copy_fmt = DXGI_FORMAT_UNKNOWN;

    ID3D11Texture3D* lut_tex[4]{};
    ID3D11ShaderResourceView* lut_srv[4]{};
    uint32_t lut_size[4]{};
    FILETIME custom_lut_mtime{};
    bool custom_lut_exists = false;
    uint32_t custom_lut_probe_seq = 0;
    wchar_t custom_lut_path[MAX_PATH]{};

    ID3D11Texture2D* depth_copy_tex = nullptr;
    ID3D11ShaderResourceView* depth_copy_srv = nullptr;
    ID3D11Texture2D* depth_resolve_tex = nullptr;
    ID3D11RenderTargetView* depth_resolve_rtv = nullptr;
    ID3D11ShaderResourceView* depth_resolve_srv = nullptr;
    uint32_t depth_resolve_w = 0;
    uint32_t depth_resolve_h = 0;
    ID3D11Texture2D* particle_params_tex = nullptr;
    ID3D11ShaderResourceView* particle_params_srv = nullptr;
    ID3D11PixelShader* ps_resolve_depth_msaa = nullptr;
    ParticleParamsTexData last_cb_particle{};
    bool last_cb_particle_valid = false;
    bool particle_depth_valid = false;
    bool post_world_depth_valid = false;
    bool post_world_msaa = false;
    uint8_t post_world_cb_bytes[sizeof(CbAoParams)]{};
    float particle_proj_22 = 0.0f;
    float particle_proj_23 = 0.0f;
    int32_t particle_clamp[4]{};

    ID3D11Texture2D* ao_rt_tex = nullptr;
    ID3D11RenderTargetView* ao_rt_rtv = nullptr;
    ID3D11ShaderResourceView* ao_rt_srv = nullptr;
    ID3D11Texture2D* ao_blur_tex = nullptr;
    ID3D11RenderTargetView* ao_blur_rtv = nullptr;
    ID3D11ShaderResourceView* ao_blur_srv = nullptr;

    ID3D11Texture2D* ssr_rt_tex = nullptr;
    ID3D11RenderTargetView* ssr_rt_rtv = nullptr;
    ID3D11ShaderResourceView* ssr_rt_srv = nullptr;
    uint32_t ssr_rt_w = 0;
    uint32_t ssr_rt_h = 0;

    ID3D11Texture2D* water_depth_tex = nullptr;
    ID3D11RenderTargetView* water_depth_rtv = nullptr;
    ID3D11ShaderResourceView* water_depth_srv = nullptr;
    uint32_t water_depth_w = 0;
    uint32_t water_depth_h = 0;
    uint32_t water_depth_samples = 0;
    ID3D11DepthStencilState* ds_water_depth_readonly = nullptr;
    ID3D11PixelShader* ps_write_water_depth = nullptr;

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

// Global singletons and atomic counters
extern PostRenderContext g_st;
extern CRITICAL_SECTION g_d3d_lock;
extern std::atomic<uint64_t> g_inject_ok;
extern std::atomic<uint64_t> g_inject_skip_no_om;
extern std::atomic<uint64_t> g_inject_skip_fmt;
extern std::atomic<uint64_t> g_inject_skip_fail;
extern std::atomic<uint32_t> g_depth_rebuilds;
extern std::atomic<uint32_t> g_last_tex_w;
extern std::atomic<uint32_t> g_last_tex_h;
extern std::atomic<uint32_t> g_last_rtv_fmt;
extern std::atomic<uint32_t> g_last_dsv_tex_fmt;
extern std::atomic<uint32_t> g_last_sample_count;
extern std::atomic<bool> g_shaders_ready;
extern std::atomic<bool> g_custom_cube_lut_loaded;

void EnsurePostLockInit();
inline void EnsureLockInit() { EnsurePostLockInit(); }
class PostLockScope {
public:
    PostLockScope();
    ~PostLockScope();
};

PostRenderContext& GetPostContext();

DepthCopyFormats MapDepthCopyFormats(DXGI_FORMAT tex_fmt, DXGI_FORMAT dsv_fmt);
D3DCompileFn ResolveD3DCompile(PostLogFn logger);
bool CompileShaderBlobFromSource(
    D3DCompileFn compile_fn,
    const char* source,
    size_t source_bytes,
    const char* source_name,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3DBlob** out_blob,
    PostLogFn logger);
bool CompileShaderBlob(
    D3DCompileFn compile_fn,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3DBlob** out_blob,
    PostLogFn logger);
bool CompileAndCreatePixelShaderFromSource(
    ID3D11Device* dev,
    D3DCompileFn compile_fn,
    const char* source,
    size_t source_bytes,
    const char* source_name,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3D11PixelShader** out_ps,
    PostLogFn logger);
bool CompileAndCreatePixelShader(
    ID3D11Device* dev,
    D3DCompileFn compile_fn,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3D11PixelShader** out_ps,
    PostLogFn logger);

void ReleaseDepthVariantShaders(DepthVariantShaders& s);
void ReleaseLutResources();
void ReleaseTargets();
void ReleaseBenchQueries();
void ReleaseAllDeviceObjects();

bool EnsurePipelineObjects(ID3D11Device* dev, PostLogFn logger);
bool EnsureAoTargets(
    ID3D11Device* dev,
    const D3D11_TEXTURE2D_DESC& src_desc,
    const DepthCopyFormats& fmts,
    PostLogFn logger);
bool EnsureDepthResolveTarget(ID3D11Device* dev, uint32_t width, uint32_t height, PostLogFn logger);
bool EnsureSsrTarget(ID3D11Device* dev, uint32_t width, uint32_t height);
bool EnsureWaterDepthTarget(ID3D11Device* dev, uint32_t width, uint32_t height, uint32_t sample_count = 1, uint32_t sample_quality = 0);
void ClearWaterDepthTargetForFrame(ID3D11DeviceContext* ctx);
void SetMainSceneRtv(ID3D11RenderTargetView* rtv);
ID3D11RenderTargetView* GetMainSceneRtv();
void SetWaterPassActive(bool active);
bool IsWaterPassActive();
void SetWorldWaterCaptureOpen(bool open);
bool IsWorldWaterCaptureOpen();
extern std::atomic<uint64_t> g_water_funnel_set_pass;
extern std::atomic<uint64_t> g_water_funnel_draw_active;
extern std::atomic<uint64_t> g_water_funnel_gate_open;
extern std::atomic<uint64_t> g_water_funnel_can_replay;
extern std::atomic<uint64_t> g_water_funnel_bind_ok;
void EnsureContextDrawHooked(ID3D11DeviceContext* ctx);
void RemoveContextDrawHook();
bool EnsureP2CopyTarget(ID3D11Device* dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt);
bool CopyRtvTextureToP2Copy(
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* src_tex,
    const D3D11_TEXTURE2D_DESC& rt_desc,
    const D3D11_RENDER_TARGET_VIEW_DESC& rtv_desc);
bool EnsurePanelTexture(ID3D11Device* dev, uint32_t width, uint32_t height);
bool EnsureLutReady(ID3D11Device* dev, int preset, PostLogFn logger);
bool UploadBloomLutCb(
    ID3D11DeviceContext* ctx,
    float dst_x,
    float dst_y,
    float dst_w,
    float dst_h,
    float src_u0,
    float src_v0,
    float src_uw,
    float src_vh,
    float inv_src_w,
    float inv_src_h,
    float threshold,
    float knee,
    float mix_or_strength,
    float lut_size,
    float lut_mix);

BenchSlot* BeginBenchSlot(ID3D11Device* dev, ID3D11DeviceContext* ctx, const AoTuneConfig& cfg);
void RecordTimestamp(ID3D11DeviceContext* ctx, BenchSlot* slot, size_t idx);
void HarvestBenchSlot(ID3D11DeviceContext* ctx, BenchSlot& b);
void RecordFrameBenchmark(double copy_ms, double ao_ms, double blur_h_ms, double blur_v_ms, double comp_ms, double total_ms);

bool VerifyPostShadersCompile(PostLogFn logger);
void ShutdownPostRendererResources();
PostRenderStats GetPostRenderStats();
bool IsCustomCubeLutLoaded();

// Legacy shims
inline bool VerifyRouteCShadersCompile(PostLogFn logger) { return VerifyPostShadersCompile(logger); }
inline void ShutdownRouteCD3D11Resources() { ShutdownPostRendererResources(); }
inline PostRenderStats GetRouteCD3DStats() { return GetPostRenderStats(); }

}  // namespace routeb
