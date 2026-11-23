#include "d3d11_post_renderer.h"
#include "d3d11_post_shaders.h"
#include "d3d11_smaa_pass.h"
#include "d3d11_smaa_shaders.h"
#include "phase3_scene_pipeline.h"
#include "render_hook_manager.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <dxgi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace routeb {

PostRenderContext g_st{};

PostRenderContext& GetPostContext() {
    return g_st;
}

CRITICAL_SECTION g_d3d_lock;
INIT_ONCE g_lock_once = INIT_ONCE_STATIC_INIT;
BOOL CALLBACK InitLockOnce(PINIT_ONCE, PVOID, PVOID*) {
    ::InitializeCriticalSection(&g_d3d_lock);
    return TRUE;
}
void EnsurePostLockInit() {
    ::InitOnceExecuteOnce(&g_lock_once, &InitLockOnce, nullptr, nullptr);
}

PostLockScope::PostLockScope() {
    EnsurePostLockInit();
    ::EnterCriticalSection(&g_d3d_lock);
}

PostLockScope::~PostLockScope() {
    ::LeaveCriticalSection(&g_d3d_lock);
}

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
std::atomic<bool> g_custom_cube_lut_loaded{false};

uint64_t g_bench_samples = 0;
double g_sum_copy_ms = 0.0;
double g_sum_ao_ms = 0.0;
double g_sum_blur_h_ms = 0.0;
double g_sum_blur_v_ms = 0.0;
double g_sum_comp_ms = 0.0;
double g_sum_total_ms = 0.0;

void RecordFrameBenchmark(double copy_ms, double ao_ms, double blur_h_ms, double blur_v_ms, double comp_ms, double total_ms) {
    PostLockScope lock;
    ++g_bench_samples;
    g_sum_copy_ms += copy_ms;
    g_sum_ao_ms += ao_ms;
    g_sum_blur_h_ms += blur_h_ms;
    g_sum_blur_v_ms += blur_v_ms;
    g_sum_comp_ms += comp_ms;
    g_sum_total_ms += total_ms;
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

void ReleaseDepthVariantShaders(DepthVariantShaders& s) {
    SafeRelease(s.ps_ssr_resolve);
    SafeRelease(s.ps_ssr);
    SafeRelease(s.ps_aerial);
    SafeRelease(s.ps_comp_dual);
    SafeRelease(s.ps_composite);
    SafeRelease(s.ps_blur_v);
    SafeRelease(s.ps_blur_h);
    SafeRelease(s.ps_ao_raw);
    SafeRelease(s.ps_c1_debug);
}

void ReleaseLutResources() {
    for (int i = 0; i < 4; ++i) {
        SafeRelease(g_st.lut_srv[i]);
        SafeRelease(g_st.lut_tex[i]);
        g_st.lut_size[i] = 0;
    }
    g_st.custom_lut_mtime = {};
    g_st.custom_lut_exists = false;
    g_st.custom_lut_probe_seq = 0;
    g_custom_cube_lut_loaded.store(false, std::memory_order_relaxed);
}

void ReleaseTargets() {
    SafeRelease(g_st.panel_srv);
    SafeRelease(g_st.panel_tex);
    g_st.panel_tex_w = 0;
    g_st.panel_tex_h = 0;

    SafeRelease(g_st.p2_copy_srv);
    SafeRelease(g_st.p2_copy_tex);
    g_st.p2_copy_w = 0;
    g_st.p2_copy_h = 0;
    g_st.p2_copy_fmt = DXGI_FORMAT_UNKNOWN;

    SafeRelease(g_st.ao_blur_srv);
    SafeRelease(g_st.ao_blur_rtv);
    SafeRelease(g_st.ao_blur_tex);
    SafeRelease(g_st.ao_rt_srv);
    SafeRelease(g_st.ao_rt_rtv);
    SafeRelease(g_st.ao_rt_tex);
    SafeRelease(g_st.ssr_rt_srv);
    SafeRelease(g_st.ssr_rt_rtv);
    SafeRelease(g_st.ssr_rt_tex);
    g_st.ssr_rt_w = 0;
    g_st.ssr_rt_h = 0;
    SafeRelease(g_st.water_depth_srv);
    SafeRelease(g_st.water_depth_rtv);
    SafeRelease(g_st.water_depth_tex);
    g_st.water_depth_w = 0;
    g_st.water_depth_h = 0;
    g_st.water_depth_samples = 0;
    SafeRelease(g_st.depth_resolve_srv);
    SafeRelease(g_st.depth_resolve_rtv);
    SafeRelease(g_st.depth_resolve_tex);
    g_st.depth_resolve_w = 0;
    g_st.depth_resolve_h = 0;
    SafeRelease(g_st.depth_copy_srv);
    SafeRelease(g_st.depth_copy_tex);
    g_st.depth_w = 0;
    g_st.depth_h = 0;
    g_st.depth_tex_fmt = DXGI_FORMAT_UNKNOWN;
    g_st.depth_srv_fmt = DXGI_FORMAT_UNKNOWN;
    g_st.depth_sample_count = 0;
    g_st.depth_sample_quality = 0;
    g_st.particle_depth_valid = false;
    g_st.post_world_depth_valid = false;
    g_st.post_world_msaa = false;
    ReleaseSmaaTargets();
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
    ReleaseLutResources();
    ReleaseSmaaAllObjects();
    SafeRelease(g_st.rs_fullscreen);
    SafeRelease(g_st.dss_disabled);
    SafeRelease(g_st.blend_toast_alpha);
    SafeRelease(g_st.blend_dual_ao);
    SafeRelease(g_st.blend_mul_rgb);
    SafeRelease(g_st.blend_debug_rgb);
    SafeRelease(g_st.samp_linear);
    SafeRelease(g_st.ps_cursor);
    SafeRelease(g_st.ps_panel);
    SafeRelease(g_st.cb_panel);
    SafeRelease(g_st.ps_lut_grade);
    SafeRelease(g_st.cb_bloom_lut);
    SafeRelease(g_st.cb_toast);
    SafeRelease(g_st.ps_toast);
    SafeRelease(g_st.cb_p2_filter);
    SafeRelease(g_st.ps_p2_filter);
    SafeRelease(g_st.particle_params_srv);
    SafeRelease(g_st.particle_params_tex);
    SafeRelease(g_st.ps_resolve_depth_msaa);
    SafeRelease(g_st.cb_params);
    SafeRelease(g_st.ps_write_water_depth);
    SafeRelease(g_st.ds_water_depth_readonly);
    ReleaseDepthVariantShaders(g_st.shaders_msaa);
    ReleaseDepthVariantShaders(g_st.shaders_single);
    SafeRelease(g_st.vs_fullscreen);
    SafeRelease(g_st.device);
    g_st.last_cb_particle_valid = false;
    g_st.msaa_shaders_ready = false;
    g_st.init_failed = false;
    g_shaders_ready.store(false, std::memory_order_relaxed);
}
bool CompileShaderBlobFromSource(
    D3DCompileFn compile_fn,
    const char* source,
    size_t source_bytes,
    const char* source_name,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3DBlob** out_blob,
    RouteCD3DLogFn logger) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile_fn(
        source,
        source_bytes,
        source_name,
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


bool CompileShaderBlob(
    D3DCompileFn compile_fn,
    const char* entry,
    const char* target,
    const D3D_SHADER_MACRO* defines,
    ID3DBlob** out_blob,
    PostLogFn logger) {
    static const std::string kCombined =
        std::string(kAoShaderSource) + kAoAtmosSource + kAoSsrTailSource + kSsrResolveShaderSource;
    return CompileShaderBlobFromSource(
        compile_fn, kCombined.data(), kCombined.size(), "post_process.hlsl",
        entry, target, defines, out_blob, logger);
}
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
    RouteCD3DLogFn logger) {
    ID3DBlob* blob = nullptr;
    if (!CompileShaderBlobFromSource(compile_fn, source, source_bytes, source_name, entry, target, defines, &blob, logger)) {
        return false;
    }
    const HRESULT hr = dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out_ps);
    SafeRelease(blob);
    return SUCCEEDED(hr) && *out_ps != nullptr;
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
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AoComposite", ps_profile, defines, &out.ps_composite, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AoCompositeDual", ps_profile, defines, &out.ps_comp_dual, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_AerialPerspective", ps_profile, defines, &out.ps_aerial, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_WetFloorSsr", ps_profile, defines, &out.ps_ssr, logger) &&
        CompileAndCreatePixelShader(dev, compile_fn, "PS_SsrSpatialResolve", ps_profile, defines, &out.ps_ssr_resolve, logger);
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
        g_st.shaders_single.ps_comp_dual && g_st.shaders_single.ps_aerial && g_st.shaders_single.ps_ssr &&
        g_st.shaders_single.ps_ssr_resolve &&
        g_st.ps_lut_grade && g_st.cb_bloom_lut &&
        g_st.ps_toast && g_st.cb_toast && g_st.blend_toast_alpha &&
        g_st.ps_panel && g_st.ps_cursor && g_st.cb_panel && g_st.samp_linear &&
        g_st.cb_params && g_st.blend_debug_rgb && g_st.blend_mul_rgb && g_st.blend_dual_ao &&
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
              CreateDepthVariantShaders(dev, compile_fn, "ps_4_0", macro_single, g_st.shaders_single, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kToastShaderSource, kToastShaderSourceLength, "route_c_toast.hlsl",
                  "PS_ToastOverlay", "ps_4_0", macro_single, &g_st.ps_toast, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kP2FilterShaderSource, kP2FilterShaderSourceLength, "route_b_p2_filter.hlsl",
                  "PS_P2FilterFallback", "ps_4_0", macro_single, &g_st.ps_p2_filter, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kPanelShaderSource, kPanelShaderSourceLength, "route_c_panel.hlsl",
                  "PS_PanelOverlay", "ps_4_0", macro_single, &g_st.ps_panel, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kPanelShaderSource, kPanelShaderSourceLength, "route_c_panel.hlsl",
                  "PS_CursorOverlay", "ps_4_0", macro_single, &g_st.ps_cursor, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kBloomLutShaderSource, kBloomLutShaderSourceLength, "route_c_bloom_lut.hlsl",
                  "PS_LutGrade3D", "ps_4_0", macro_single, &g_st.ps_lut_grade, logger) &&
              CompileAndCreatePixelShaderFromSource(
                  dev, compile_fn, kWriteWaterDepthShaderSource, kWriteWaterDepthShaderSourceLength, "route_b_water_depth.hlsl",
                  "PS_WriteWaterDepth", "ps_4_0", macro_single, &g_st.ps_write_water_depth, logger);
    SafeRelease(vs_blob);

    if (ok && dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_10_1) {
        g_st.msaa_shaders_ready = CreateDepthVariantShaders(dev, compile_fn, "ps_4_1", macro_msaa, g_st.shaders_msaa, logger);
        if (g_st.msaa_shaders_ready) {
            if (!CompileAndCreatePixelShader(
                    dev, compile_fn, "PS_ResolveDepthMsaa", "ps_4_1", macro_msaa, &g_st.ps_resolve_depth_msaa, logger)) {
                SafeRelease(g_st.ps_resolve_depth_msaa);
                g_st.msaa_shaders_ready = false;
            }
        }
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

    D3D11_BLEND_DESC bd_dual{};
    bd_dual.RenderTarget[0].BlendEnable = TRUE;
    bd_dual.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd_dual.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
    bd_dual.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_dual.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_dual.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_dual.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_dual.RenderTarget[0].RenderTargetWriteMask =
        D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;

    D3D11_BUFFER_DESC cb_toast_desc{};
    cb_toast_desc.ByteWidth = sizeof(CbToastParams);
    cb_toast_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_toast_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_toast_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BUFFER_DESC cb_p2_desc{};
    cb_p2_desc.ByteWidth = sizeof(CbP2FilterParams);
    cb_p2_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_p2_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_p2_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BUFFER_DESC cb_bloom_desc{};
    cb_bloom_desc.ByteWidth = sizeof(CbBloomLutParams);
    cb_bloom_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_bloom_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_bloom_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_BLEND_DESC bd_toast{};
    bd_toast.RenderTarget[0].BlendEnable = TRUE;
    bd_toast.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd_toast.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd_toast.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd_toast.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd_toast.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd_toast.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd_toast.RenderTarget[0].RenderTargetWriteMask =
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

    D3D11_BUFFER_DESC cb_panel_desc{};
    cb_panel_desc.ByteWidth = sizeof(CbPanelParams);
    cb_panel_desc.Usage = D3D11_USAGE_DYNAMIC;
    cb_panel_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb_panel_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_TEXTURE2D_DESC particle_tex_desc{};
    particle_tex_desc.Width = 2;
    particle_tex_desc.Height = 1;
    particle_tex_desc.MipLevels = 1;
    particle_tex_desc.ArraySize = 1;
    particle_tex_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    particle_tex_desc.SampleDesc.Count = 1;
    particle_tex_desc.Usage = D3D11_USAGE_DYNAMIC;
    particle_tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    particle_tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SHADER_RESOURCE_VIEW_DESC particle_srv_desc{};
    particle_srv_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    particle_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    particle_srv_desc.Texture2D.MipLevels = 1;

    D3D11_SAMPLER_DESC samp_desc{};
    samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp_desc.MaxLOD = D3D11_FLOAT32_MAX;

    D3D11_DEPTH_STENCIL_DESC ds_water_desc{};
    ds_water_desc.DepthEnable = TRUE;
    ds_water_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    ds_water_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ds_water_desc.StencilEnable = FALSE;

    ok = SUCCEEDED(dev->CreateBuffer(&cb_desc, nullptr, &g_st.cb_params)) &&
         SUCCEEDED(dev->CreateBuffer(&cb_toast_desc, nullptr, &g_st.cb_toast)) &&
         SUCCEEDED(dev->CreateBuffer(&cb_p2_desc, nullptr, &g_st.cb_p2_filter)) &&
         SUCCEEDED(dev->CreateBuffer(&cb_bloom_desc, nullptr, &g_st.cb_bloom_lut)) &&
         SUCCEEDED(dev->CreateBuffer(&cb_panel_desc, nullptr, &g_st.cb_panel)) &&
         SUCCEEDED(dev->CreateTexture2D(&particle_tex_desc, nullptr, &g_st.particle_params_tex)) &&
         SUCCEEDED(dev->CreateShaderResourceView(g_st.particle_params_tex, &particle_srv_desc, &g_st.particle_params_srv)) &&
         SUCCEEDED(dev->CreateSamplerState(&samp_desc, &g_st.samp_linear)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_dbg, &g_st.blend_debug_rgb)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_mul, &g_st.blend_mul_rgb)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_dual, &g_st.blend_dual_ao)) &&
         SUCCEEDED(dev->CreateBlendState(&bd_toast, &g_st.blend_toast_alpha)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&dss_desc, &g_st.dss_disabled)) &&
         SUCCEEDED(dev->CreateDepthStencilState(&ds_water_desc, &g_st.ds_water_depth_readonly)) &&
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

bool EnsureDepthResolveTarget(ID3D11Device* dev, uint32_t width, uint32_t height, RouteCD3DLogFn logger) {
    if (g_st.depth_resolve_tex && g_st.depth_resolve_rtv && g_st.depth_resolve_srv &&
        g_st.depth_resolve_w == width && g_st.depth_resolve_h == height) {
        return true;
    }
    SafeRelease(g_st.depth_resolve_srv);
    SafeRelease(g_st.depth_resolve_rtv);
    SafeRelease(g_st.depth_resolve_tex);
    g_st.depth_resolve_w = 0;
    g_st.depth_resolve_h = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_st.depth_resolve_tex)) || !g_st.depth_resolve_tex) {
        if (logger) {
            logger("[RouteC][D3D11] FAILED creating R32_FLOAT MSAA depth resolve target (%ux%u).", width, height);
        }
        SafeRelease(g_st.depth_resolve_tex);
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(g_st.depth_resolve_tex, nullptr, &g_st.depth_resolve_rtv)) || !g_st.depth_resolve_rtv) {
        SafeRelease(g_st.depth_resolve_rtv);
        SafeRelease(g_st.depth_resolve_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_st.depth_resolve_tex, &srvd, &g_st.depth_resolve_srv)) || !g_st.depth_resolve_srv) {
        SafeRelease(g_st.depth_resolve_srv);
        SafeRelease(g_st.depth_resolve_rtv);
        SafeRelease(g_st.depth_resolve_tex);
        return false;
    }
    g_st.depth_resolve_w = width;
    g_st.depth_resolve_h = height;
    return true;
}

bool EnsureSsrTarget(ID3D11Device* dev, uint32_t width, uint32_t height) {
    if (g_st.ssr_rt_tex && g_st.ssr_rt_rtv && g_st.ssr_rt_srv &&
        g_st.ssr_rt_w == width && g_st.ssr_rt_h == height) {
        return true;
    }
    SafeRelease(g_st.ssr_rt_srv);
    SafeRelease(g_st.ssr_rt_rtv);
    SafeRelease(g_st.ssr_rt_tex);
    g_st.ssr_rt_w = 0;
    g_st.ssr_rt_h = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_st.ssr_rt_tex)) || !g_st.ssr_rt_tex) {
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(g_st.ssr_rt_tex, nullptr, &g_st.ssr_rt_rtv)) || !g_st.ssr_rt_rtv) {
        SafeRelease(g_st.ssr_rt_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    srvd.Texture2D.MostDetailedMip = 0;
    if (FAILED(dev->CreateShaderResourceView(g_st.ssr_rt_tex, &srvd, &g_st.ssr_rt_srv)) || !g_st.ssr_rt_srv) {
        SafeRelease(g_st.ssr_rt_rtv);
        SafeRelease(g_st.ssr_rt_tex);
        return false;
    }
    g_st.ssr_rt_w = width;
    g_st.ssr_rt_h = height;
    return true;
}

ID3D11RenderTargetView* g_main_scene_rtv = nullptr;
void SetMainSceneRtv(ID3D11RenderTargetView* rtv) {
    g_main_scene_rtv = rtv;
}
ID3D11RenderTargetView* GetMainSceneRtv() {
    return g_main_scene_rtv;
}

std::atomic<bool> g_water_pass_active{false};
void SetWaterPassActive(bool active) {
    g_water_pass_active.store(active, std::memory_order_relaxed);
}
bool IsWaterPassActive() {
    return g_water_pass_active.load(std::memory_order_relaxed);
}

std::atomic<bool> g_world_water_capture_open{false};
void SetWorldWaterCaptureOpen(bool open) {
    g_world_water_capture_open.store(open, std::memory_order_relaxed);
}
bool IsWorldWaterCaptureOpen() {
    return g_world_water_capture_open.load(std::memory_order_relaxed);
}

std::atomic<uint64_t> g_water_funnel_set_pass{0};
std::atomic<uint64_t> g_water_funnel_draw_active{0};
std::atomic<uint64_t> g_water_funnel_gate_open{0};
std::atomic<uint64_t> g_water_funnel_can_replay{0};
std::atomic<uint64_t> g_water_funnel_bind_ok{0};

bool EnsureWaterDepthTarget(ID3D11Device* dev, uint32_t width, uint32_t height, uint32_t sample_count, uint32_t sample_quality) {
    if (g_st.water_depth_tex && g_st.water_depth_rtv && g_st.water_depth_srv &&
        g_st.water_depth_w == width && g_st.water_depth_h == height &&
        g_st.water_depth_samples == sample_count) {
        return true;
    }
    SafeRelease(g_st.water_depth_srv);
    SafeRelease(g_st.water_depth_rtv);
    SafeRelease(g_st.water_depth_tex);
    g_st.water_depth_w = 0;
    g_st.water_depth_h = 0;
    g_st.water_depth_samples = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_FLOAT;
    td.SampleDesc.Count = (sample_count > 0) ? sample_count : 1;
    td.SampleDesc.Quality = sample_quality;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_st.water_depth_tex)) || !g_st.water_depth_tex) {
        return false;
    }
    if (FAILED(dev->CreateRenderTargetView(g_st.water_depth_tex, nullptr, &g_st.water_depth_rtv)) || !g_st.water_depth_rtv) {
        SafeRelease(g_st.water_depth_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R32_FLOAT;
    if (td.SampleDesc.Count > 1) {
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
    } else {
        srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvd.Texture2D.MipLevels = 1;
        srvd.Texture2D.MostDetailedMip = 0;
    }
    if (FAILED(dev->CreateShaderResourceView(g_st.water_depth_tex, &srvd, &g_st.water_depth_srv)) || !g_st.water_depth_srv) {
        SafeRelease(g_st.water_depth_rtv);
        SafeRelease(g_st.water_depth_tex);
        return false;
    }
    g_st.water_depth_w = width;
    g_st.water_depth_h = height;
    g_st.water_depth_samples = td.SampleDesc.Count;
    return true;
}

void ClearWaterDepthTargetForFrame(ID3D11DeviceContext* ctx) {
    if (ctx && g_st.water_depth_rtv) {
        const float clear_val[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ctx->ClearRenderTargetView(g_st.water_depth_rtv, clear_val);
    }
}

namespace {

typedef void (STDMETHODCALLTYPE *DrawIndexedFn)(
    ID3D11DeviceContext* This,
    UINT IndexCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation);

typedef void (STDMETHODCALLTYPE *DrawFn)(
    ID3D11DeviceContext* This,
    UINT VertexCount,
    UINT StartVertexLocation);

void* g_shadow_context_vtable[256] = {};
void** g_orig_context_vtable = nullptr;
ID3D11DeviceContext* g_hooked_context = nullptr;
DrawIndexedFn g_orig_draw_indexed = nullptr;
DrawFn g_orig_draw = nullptr;
SRWLOCK g_draw_hook_lock = SRWLOCK_INIT;

thread_local bool t_replaying = false;

bool CanReplayWaterDraw(
    ID3D11RenderTargetView* rtv0,
    ID3D11DepthStencilView* dsv,
    ID3D11BlendState* blend,
    ID3D11DepthStencilState* dss)
{
    if (!IsP2SsrEnabled() || IsMainMenuActive() || !IsWorldWaterCaptureOpen()) {
        return false;
    }
    if (!g_st.water_depth_rtv || !g_st.ps_write_water_depth) {
        return false;
    }
    if (!rtv0 || !dsv || !blend || !dss) {
        return false;
    }
    if (rtv0 == g_st.water_depth_rtv) {
        return false;
    }

    // 1. D3D11 Pipeline State Gate (Expert Advice 19):
    // Real water surface MUST be alpha-blended and depth-tested (LESS or LESS_EQUAL).
    // dw=1 (DepthWriteMask == ALL) is explicitly allowed because TL2 water writes depth.
    // Solid props (shipwreck, walls, statues, terrain) have BlendEnable == FALSE and are rejected.
    D3D11_BLEND_DESC bdesc{};
    blend->GetDesc(&bdesc);
    if (!bdesc.RenderTarget[0].BlendEnable) {
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dsdesc{};
    dss->GetDesc(&dsdesc);
    if (!dsdesc.DepthEnable) {
        return false;
    }
    // CEGUI buttons use DepthFunc == ALWAYS; real 3D water uses LESS or LESS_EQUAL
    if (dsdesc.DepthFunc != D3D11_COMPARISON_LESS && dsdesc.DepthFunc != D3D11_COMPARISON_LESS_EQUAL) {
        return false;
    }

    // 2. RTV Dimension & SampleDesc Gate (Expert Advice 19):
    // Rejects 512x512 ProjectorRTT or off-size buffers; ensures rtv0 matches water_depth buffer.
    // (Pointers are not compared because TL2 draws scene to compositor RTT rather than swapchain RTV)
    ID3D11Resource* rtv_res = nullptr;
    rtv0->GetResource(&rtv_res);
    if (!rtv_res) return false;
    ID3D11Texture2D* rtv_tex = nullptr;
    const HRESULT hr_rtv = rtv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&rtv_tex));
    SafeRelease(rtv_res);
    if (FAILED(hr_rtv) || !rtv_tex) return false;
    D3D11_TEXTURE2D_DESC rtv_desc{};
    rtv_tex->GetDesc(&rtv_desc);
    SafeRelease(rtv_tex);
    if (rtv_desc.Width != g_st.water_depth_w || rtv_desc.Height != g_st.water_depth_h ||
        rtv_desc.SampleDesc.Count != g_st.water_depth_samples) {
        return false;
    }

    // 3. DSV Dimension & SampleDesc Gate (Expert Advice 19):
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv->GetDesc(&dsv_desc);
    if (dsv_desc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2D &&
        dsv_desc.ViewDimension != D3D11_DSV_DIMENSION_TEXTURE2DMS) {
        return false;
    }
    ID3D11Resource* dsv_res = nullptr;
    dsv->GetResource(&dsv_res);
    if (!dsv_res) return false;
    ID3D11Texture2D* dsv_tex = nullptr;
    const HRESULT hr_dsv = dsv_res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&dsv_tex));
    SafeRelease(dsv_res);
    if (FAILED(hr_dsv) || !dsv_tex) return false;
    D3D11_TEXTURE2D_DESC dsv_tex_desc{};
    dsv_tex->GetDesc(&dsv_tex_desc);
    SafeRelease(dsv_tex);
    if (dsv_tex_desc.Width != g_st.water_depth_w || dsv_tex_desc.Height != g_st.water_depth_h ||
        dsv_tex_desc.SampleDesc.Count != g_st.water_depth_samples) {
        return false;
    }

    return true;
}

void ReplayWaterDrawIndexed(
    ID3D11DeviceContext* ctx,
    UINT IndexCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    if (t_replaying || !g_orig_draw_indexed) return;
    if (!IsP2SsrEnabled() || IsMainMenuActive() || !g_world_water_capture_open.load(std::memory_order_relaxed)) {
        return;
    }
    t_replaying = true;

    if (!g_st.water_depth_rtv || !g_st.ds_water_depth_readonly || !g_st.ps_write_water_depth ||
        g_st.water_depth_w == 0 || g_st.water_depth_h == 0) {
        t_replaying = false;
        return;
    }

    ID3D11RenderTargetView* prev_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* prev_dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, &prev_dsv);

    ID3D11BlendState* prev_blend = nullptr;
    FLOAT prev_blend_factor[4] = {};
    UINT prev_sample_mask = 0xFFFFFFFF;
    ctx->OMGetBlendState(&prev_blend, prev_blend_factor, &prev_sample_mask);

    ID3D11DepthStencilState* prev_dss = nullptr;
    UINT prev_stencil_ref = 0;
    ctx->OMGetDepthStencilState(&prev_dss, &prev_stencil_ref);

    if (!CanReplayWaterDraw(prev_rtvs[0], prev_dsv, prev_blend, prev_dss)) {
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            SafeRelease(prev_rtvs[i]);
        }
        SafeRelease(prev_dsv);
        SafeRelease(prev_blend);
        SafeRelease(prev_dss);
        t_replaying = false;
        return;
    }
    g_water_funnel_can_replay.fetch_add(1, std::memory_order_relaxed);

    ID3D11PixelShader* prev_ps = nullptr;
    ID3D11ClassInstance* prev_class_inst[256] = {};
    UINT prev_num_instances = 256;
    ctx->PSGetShader(&prev_ps, prev_class_inst, &prev_num_instances);

    ctx->OMSetRenderTargets(1, &g_st.water_depth_rtv, prev_dsv);

    // Lock 5: Verify that OMSetRenderTargets actually bound water_depth_rtv
    ID3D11RenderTargetView* verify_rtv = nullptr;
    ctx->OMGetRenderTargets(1, &verify_rtv, nullptr);
    if (verify_rtv != g_st.water_depth_rtv) {
        SafeRelease(verify_rtv);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, prev_dsv);
        ctx->OMSetBlendState(prev_blend, prev_blend_factor, prev_sample_mask);
        ctx->OMSetDepthStencilState(prev_dss, prev_stencil_ref);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            SafeRelease(prev_rtvs[i]);
        }
        SafeRelease(prev_dsv);
        SafeRelease(prev_blend);
        SafeRelease(prev_dss);
        SafeRelease(prev_ps);
        for (UINT i = 0; i < prev_num_instances; ++i) {
            SafeRelease(prev_class_inst[i]);
        }
        t_replaying = false;
        return;
    }
    SafeRelease(verify_rtv);
    g_water_funnel_bind_ok.fetch_add(1, std::memory_order_relaxed);

    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_st.ds_water_depth_readonly, 0);
    ctx->PSSetShader(g_st.ps_write_water_depth, nullptr, 0);

    g_orig_draw_indexed(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, prev_dsv);
    ctx->OMSetBlendState(prev_blend, prev_blend_factor, prev_sample_mask);
    ctx->OMSetDepthStencilState(prev_dss, prev_stencil_ref);
    ctx->PSSetShader(prev_ps, prev_class_inst, prev_num_instances);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        SafeRelease(prev_rtvs[i]);
    }
    SafeRelease(prev_dsv);
    SafeRelease(prev_blend);
    SafeRelease(prev_dss);
    SafeRelease(prev_ps);
    for (UINT i = 0; i < prev_num_instances; ++i) {
        SafeRelease(prev_class_inst[i]);
    }

    t_replaying = false;
}

void ReplayWaterDraw(
    ID3D11DeviceContext* ctx,
    UINT VertexCount,
    UINT StartVertexLocation)
{
    if (t_replaying || !g_orig_draw) return;
    if (!IsP2SsrEnabled() || IsMainMenuActive() || !g_world_water_capture_open.load(std::memory_order_relaxed)) {
        return;
    }
    t_replaying = true;

    if (!g_st.water_depth_rtv || !g_st.ds_water_depth_readonly || !g_st.ps_write_water_depth ||
        g_st.water_depth_w == 0 || g_st.water_depth_h == 0) {
        t_replaying = false;
        return;
    }

    ID3D11RenderTargetView* prev_rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* prev_dsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, &prev_dsv);

    ID3D11BlendState* prev_blend = nullptr;
    FLOAT prev_blend_factor[4] = {};
    UINT prev_sample_mask = 0xFFFFFFFF;
    ctx->OMGetBlendState(&prev_blend, prev_blend_factor, &prev_sample_mask);

    ID3D11DepthStencilState* prev_dss = nullptr;
    UINT prev_stencil_ref = 0;
    ctx->OMGetDepthStencilState(&prev_dss, &prev_stencil_ref);

    if (!CanReplayWaterDraw(prev_rtvs[0], prev_dsv, prev_blend, prev_dss)) {
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            SafeRelease(prev_rtvs[i]);
        }
        SafeRelease(prev_dsv);
        SafeRelease(prev_blend);
        SafeRelease(prev_dss);
        t_replaying = false;
        return;
    }
    g_water_funnel_can_replay.fetch_add(1, std::memory_order_relaxed);

    ID3D11PixelShader* prev_ps = nullptr;
    ID3D11ClassInstance* prev_class_inst[256] = {};
    UINT prev_num_instances = 256;
    ctx->PSGetShader(&prev_ps, prev_class_inst, &prev_num_instances);

    ctx->OMSetRenderTargets(1, &g_st.water_depth_rtv, prev_dsv);

    // Lock 5: Verify that OMSetRenderTargets actually bound water_depth_rtv
    ID3D11RenderTargetView* verify_rtv = nullptr;
    ctx->OMGetRenderTargets(1, &verify_rtv, nullptr);
    if (verify_rtv != g_st.water_depth_rtv) {
        SafeRelease(verify_rtv);
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, prev_dsv);
        ctx->OMSetBlendState(prev_blend, prev_blend_factor, prev_sample_mask);
        ctx->OMSetDepthStencilState(prev_dss, prev_stencil_ref);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            SafeRelease(prev_rtvs[i]);
        }
        SafeRelease(prev_dsv);
        SafeRelease(prev_blend);
        SafeRelease(prev_dss);
        SafeRelease(prev_ps);
        for (UINT i = 0; i < prev_num_instances; ++i) {
            SafeRelease(prev_class_inst[i]);
        }
        t_replaying = false;
        return;
    }
    SafeRelease(verify_rtv);
    g_water_funnel_bind_ok.fetch_add(1, std::memory_order_relaxed);

    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_st.ds_water_depth_readonly, 0);
    ctx->PSSetShader(g_st.ps_write_water_depth, nullptr, 0);

    g_orig_draw(ctx, VertexCount, StartVertexLocation);

    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prev_rtvs, prev_dsv);
    ctx->OMSetBlendState(prev_blend, prev_blend_factor, prev_sample_mask);
    ctx->OMSetDepthStencilState(prev_dss, prev_stencil_ref);
    ctx->PSSetShader(prev_ps, prev_class_inst, prev_num_instances);

    for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
        SafeRelease(prev_rtvs[i]);
    }
    SafeRelease(prev_dsv);
    SafeRelease(prev_blend);
    SafeRelease(prev_dss);
    SafeRelease(prev_ps);
    for (UINT i = 0; i < prev_num_instances; ++i) {
        SafeRelease(prev_class_inst[i]);
    }

    t_replaying = false;
}

void STDMETHODCALLTYPE HookedDrawIndexed(
    ID3D11DeviceContext* ctx,
    UINT IndexCount,
    UINT StartIndexLocation,
    INT BaseVertexLocation)
{
    if (g_orig_draw_indexed) {
        g_orig_draw_indexed(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);
    }
    if (!g_water_pass_active.load(std::memory_order_relaxed)) {
        return;
    }
    g_water_funnel_draw_active.fetch_add(1, std::memory_order_relaxed);
    if (!IsP2SsrEnabled() ||
        !g_world_water_capture_open.load(std::memory_order_relaxed) ||
        IsMainMenuActive()) {
        return;
    }
    g_water_funnel_gate_open.fetch_add(1, std::memory_order_relaxed);
    ReplayWaterDrawIndexed(ctx, IndexCount, StartIndexLocation, BaseVertexLocation);
}

void STDMETHODCALLTYPE HookedDraw(
    ID3D11DeviceContext* ctx,
    UINT VertexCount,
    UINT StartVertexLocation)
{
    if (g_orig_draw) {
        g_orig_draw(ctx, VertexCount, StartVertexLocation);
    }
    if (!g_water_pass_active.load(std::memory_order_relaxed)) {
        return;
    }
    g_water_funnel_draw_active.fetch_add(1, std::memory_order_relaxed);
    if (!IsP2SsrEnabled() ||
        !g_world_water_capture_open.load(std::memory_order_relaxed) ||
        IsMainMenuActive()) {
        return;
    }
    g_water_funnel_gate_open.fetch_add(1, std::memory_order_relaxed);
    ReplayWaterDraw(ctx, VertexCount, StartVertexLocation);
}

}  // namespace

void EnsureContextDrawHooked(ID3D11DeviceContext* ctx) {
    if (!ctx) return;
    if (IsMainMenuActive() || !IsP2SsrEnabled()) {
        RemoveContextDrawHook();
        return;
    }
    ::AcquireSRWLockExclusive(&g_draw_hook_lock);
    if (g_hooked_context == ctx) {
        ::ReleaseSRWLockExclusive(&g_draw_hook_lock);
        return;
    }
    if (g_hooked_context && g_hooked_context != ctx) {
        if (g_orig_context_vtable) {
            *reinterpret_cast<void***>(g_hooked_context) = g_orig_context_vtable;
        }
        g_hooked_context = nullptr;
        g_orig_context_vtable = nullptr;
        g_orig_draw_indexed = nullptr;
        g_orig_draw = nullptr;
    }

    void** vtbl = *reinterpret_cast<void***>(ctx);
    if (!vtbl || vtbl == g_shadow_context_vtable) {
        ::ReleaseSRWLockExclusive(&g_draw_hook_lock);
        return;
    }

    size_t copy_count = 115;
    MEMORY_BASIC_INFORMATION mbi{};
    if (::VirtualQuery(vtbl, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        const uintptr_t page_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        const uintptr_t max_bytes = page_end - reinterpret_cast<uintptr_t>(vtbl);
        const size_t max_ptrs = max_bytes / sizeof(void*);
        copy_count = (std::min)(static_cast<size_t>(256), (std::max)(static_cast<size_t>(115), max_ptrs));
    }

    std::memcpy(g_shadow_context_vtable, vtbl, copy_count * sizeof(void*));
    g_orig_context_vtable = vtbl;
    g_orig_draw_indexed = reinterpret_cast<DrawIndexedFn>(vtbl[12]);
    g_orig_draw = reinterpret_cast<DrawFn>(vtbl[13]);

    g_shadow_context_vtable[12] = reinterpret_cast<void*>(&HookedDrawIndexed);
    g_shadow_context_vtable[13] = reinterpret_cast<void*>(&HookedDraw);

    *reinterpret_cast<void***>(ctx) = g_shadow_context_vtable;
    g_hooked_context = ctx;

    ::ReleaseSRWLockExclusive(&g_draw_hook_lock);
}

void RemoveContextDrawHook() {
    ::AcquireSRWLockExclusive(&g_draw_hook_lock);
    if (g_hooked_context && g_orig_context_vtable) {
        *reinterpret_cast<void***>(g_hooked_context) = g_orig_context_vtable;
    }
    g_hooked_context = nullptr;
    g_orig_context_vtable = nullptr;
    g_orig_draw_indexed = nullptr;
    g_orig_draw = nullptr;
    ::ReleaseSRWLockExclusive(&g_draw_hook_lock);
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

bool EnsureP2CopyTarget(ID3D11Device* dev, uint32_t width, uint32_t height, DXGI_FORMAT fmt) {
    if (g_st.p2_copy_tex && g_st.p2_copy_srv &&
        g_st.p2_copy_w == width && g_st.p2_copy_h == height && g_st.p2_copy_fmt == fmt) {
        return true;
    }
    SafeRelease(g_st.p2_copy_srv);
    SafeRelease(g_st.p2_copy_tex);
    g_st.p2_copy_w = 0;
    g_st.p2_copy_h = 0;
    g_st.p2_copy_fmt = DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_st.p2_copy_tex)) || !g_st.p2_copy_tex) {
        SafeRelease(g_st.p2_copy_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = fmt;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_st.p2_copy_tex, &srvd, &g_st.p2_copy_srv)) || !g_st.p2_copy_srv) {
        SafeRelease(g_st.p2_copy_srv);
        SafeRelease(g_st.p2_copy_tex);
        return false;
    }
    g_st.p2_copy_w = width;
    g_st.p2_copy_h = height;
    g_st.p2_copy_fmt = fmt;
    return true;
}

bool CopyRtvTextureToP2Copy(
    ID3D11Device* dev,
    ID3D11DeviceContext* ctx,
    ID3D11Texture2D* src_tex,
    const D3D11_TEXTURE2D_DESC& rt_desc,
    const D3D11_RENDER_TARGET_VIEW_DESC& rtv_desc) {
    if (!dev || !ctx || !src_tex || rt_desc.Width == 0 || rt_desc.Height == 0 ||
        rtv_desc.Format == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    if (!EnsureP2CopyTarget(dev, rt_desc.Width, rt_desc.Height, rtv_desc.Format)) {
        return false;
    }
    ID3D11ShaderResourceView* null_srvs[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, null_srvs);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    if (rt_desc.SampleDesc.Count > 1) {
        ctx->ResolveSubresource(g_st.p2_copy_tex, 0, src_tex, 0, rtv_desc.Format);
    } else {
        ctx->CopySubresourceRegion(g_st.p2_copy_tex, 0, 0, 0, 0, src_tex, 0, nullptr);
    }
    return true;
}

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
    float lut_mix) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(g_st.cb_bloom_lut, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        return false;
    }
    auto* cb = static_cast<CbBloomLutParams*>(mapped.pData);
    cb->dst_vp_rect[0] = dst_x;
    cb->dst_vp_rect[1] = dst_y;
    cb->dst_vp_rect[2] = dst_w;
    cb->dst_vp_rect[3] = dst_h;
    cb->src_uv_rect[0] = src_u0;
    cb->src_uv_rect[1] = src_v0;
    cb->src_uv_rect[2] = src_uw;
    cb->src_uv_rect[3] = src_vh;
    cb->texel_and_bias[0] = inv_src_w;
    cb->texel_and_bias[1] = inv_src_h;
    cb->texel_and_bias[2] = threshold;
    cb->texel_and_bias[3] = knee;
    cb->mix_and_lut[0] = mix_or_strength;
    cb->mix_and_lut[1] = lut_size;
    cb->mix_and_lut[2] = (lut_size > 1.0f) ? (1.0f / lut_size) : 0.03125f;
    cb->mix_and_lut[3] = lut_mix;
    ctx->Unmap(g_st.cb_bloom_lut, 0);
    return true;
}

uint16_t FloatToHalf(float value) {
    const float clamped = (std::max)(0.0f, (std::min)(65504.0f, std::isfinite(value) ? value : 0.0f));
    uint32_t fbits = 0;
    std::memcpy(&fbits, &clamped, sizeof(fbits));
    const uint32_t exp = (fbits >> 23) & 0xFFU;
    const uint32_t mant = fbits & 0x7FFFFFU;
    if (exp == 0) {
        return 0;
    }
    const int half_exp = static_cast<int>(exp) - 127 + 15;
    if (half_exp <= 0) {
        if (half_exp < -10) {
            return 0;
        }
        const uint32_t m = mant | 0x800000U;
        const uint32_t shift = static_cast<uint32_t>(14 - half_exp);
        return static_cast<uint16_t>((m + (1U << (shift - 1U))) >> shift);
    }
    if (half_exp >= 31) {
        return 0x7BFFU;
    }
    const uint32_t rounded_mant = mant + 0x1000U;
    if (rounded_mant >= 0x800000U) {
        return static_cast<uint16_t>((static_cast<uint32_t>(half_exp + 1) << 10) & 0x7FFFU);
    }
    return static_cast<uint16_t>((static_cast<uint32_t>(half_exp) << 10) | (rounded_mant >> 13));
}

float SmoothStep(float e0, float e1, float x) {
    const float t = (std::max)(0.0f, (std::min)(1.0f, (x - e0) / (std::max)(e1 - e0, 1e-5f)));
    return t * t * (3.0f - 2.0f * t);
}

void BuildBuiltinLutData(int preset, uint32_t size, std::vector<float>& out_rgb) {
    const size_t n = static_cast<size_t>(size);
    out_rgb.resize(n * n * n * 3U);
    const float inv_max = 1.0f / static_cast<float>(size - 1U);
    size_t idx = 0;
    for (uint32_t bi = 0; bi < size; ++bi) {
        const float b0 = static_cast<float>(bi) * inv_max;
        for (uint32_t gi = 0; gi < size; ++gi) {
            const float g0 = static_cast<float>(gi) * inv_max;
            for (uint32_t ri = 0; ri < size; ++ri) {
                const float r0 = static_cast<float>(ri) * inv_max;
                float r = r0, g = g0, b = b0;
                if (preset == 1) {
                    // Teal & Orange Cinema: gentle S-curve + teal shadow + golden highlight warmth
                    auto scurve = [](float x) {
                        const float s = x * x * (3.0f - 2.0f * x);
                        return x * 0.72f + s * 0.28f;
                    };
                    r = scurve(r);
                    g = scurve(g);
                    b = scurve(b);
                    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    const float sw = std::pow((std::max)(0.0f, 1.0f - luma * 1.35f), 1.6f);
                    const float hw = SmoothStep(0.32f, 0.92f, luma);
                    r += (-0.042f * sw) + (0.055f * hw);
                    g += (0.012f * sw) + (0.018f * hw);
                    b += (0.062f * sw) - (0.048f * hw);
                    const float nl = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = nl + (r - nl) * 1.12f;
                    g = nl + (g - nl) * 1.12f;
                    b = nl + (b - nl) * 1.12f;
                } else if (preset == 2) {
                    // Cold Nordic Slate: restrained saturation, cool steel shadows, crisp highlights
                    auto scurve = [](float x) {
                        const float s = x * x * (3.0f - 2.0f * x);
                        return x * 0.65f + s * 0.35f;
                    };
                    r = scurve(r);
                    g = scurve(g);
                    b = scurve(b);
                    const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    const float sw = std::pow((std::max)(0.0f, 1.0f - luma * 1.15f), 1.5f);
                    const float hw = SmoothStep(0.45f, 0.98f, luma);
                    r *= (1.0f - 0.14f * sw);
                    g *= (1.0f - 0.04f * sw);
                    b *= (1.0f + 0.09f * sw);
                    r += 0.025f * hw;
                    g += 0.032f * hw;
                    b += 0.045f * hw;
                    const float nl = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    r = nl + (r - nl) * 0.86f;
                    g = nl + (g - nl) * 0.86f;
                    b = nl + (b - nl) * 0.86f;
                }
                out_rgb[idx++] = (std::max)(0.0f, (std::min)(1.0f, r));
                out_rgb[idx++] = (std::max)(0.0f, (std::min)(1.0f, g));
                out_rgb[idx++] = (std::max)(0.0f, (std::min)(1.0f, b));
            }
        }
    }
}

bool ParseCubeLutFile(const wchar_t* path, uint32_t& out_size, std::vector<float>& out_rgb) {
    out_size = 0;
    out_rgb.clear();
    if (!path || !path[0]) {
        return false;
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return false;
    }
    uint32_t size = 0;
    float dmin[3] = {0.0f, 0.0f, 0.0f};
    float dmax[3] = {1.0f, 1.0f, 1.0f};
    std::string line;
    std::vector<float> values;
    while (std::getline(ifs, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) {
            line.resize(hash);
        }
        const char* p = line.c_str();
        while (*p && std::isspace(static_cast<unsigned char>(*p))) {
            ++p;
        }
        if (*p == '\0') {
            continue;
        }
        if (std::strncmp(p, "LUT_3D_SIZE", 11) == 0 && std::isspace(static_cast<unsigned char>(p[11]))) {
            const unsigned long sz = std::strtoul(p + 11, nullptr, 10);
            if (sz < 4UL || sz > 65UL) {
                return false;
            }
            size = static_cast<uint32_t>(sz);
            values.reserve(static_cast<size_t>(size) * size * size * 3U);
            continue;
        }
        if (std::strncmp(p, "LUT_1D_SIZE", 11) == 0) {
            return false;
        }
        if (std::strncmp(p, "DOMAIN_MIN", 10) == 0 && std::isspace(static_cast<unsigned char>(p[10]))) {
            char* e1 = nullptr;
            char* e2 = nullptr;
            char* e3 = nullptr;
            const float a = std::strtof(p + 10, &e1);
            const float b = std::strtof(e1, &e2);
            const float c = std::strtof(e2, &e3);
            if (e3 != e2) {
                dmin[0] = a;
                dmin[1] = b;
                dmin[2] = c;
            }
            continue;
        }
        if (std::strncmp(p, "DOMAIN_MAX", 10) == 0 && std::isspace(static_cast<unsigned char>(p[10]))) {
            char* e1 = nullptr;
            char* e2 = nullptr;
            char* e3 = nullptr;
            const float a = std::strtof(p + 10, &e1);
            const float b = std::strtof(e1, &e2);
            const float c = std::strtof(e2, &e3);
            if (e3 != e2) {
                dmax[0] = a;
                dmax[1] = b;
                dmax[2] = c;
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(*p))) {
            continue;
        }
        char* e1 = nullptr;
        char* e2 = nullptr;
        char* e3 = nullptr;
        const float r = std::strtof(p, &e1);
        if (e1 == p) continue;
        const float g = std::strtof(e1, &e2);
        if (e2 == e1) continue;
        const float b = std::strtof(e2, &e3);
        if (e3 == e2 || !std::isfinite(r) || !std::isfinite(g) || !std::isfinite(b)) continue;
        values.push_back(r);
        values.push_back(g);
        values.push_back(b);
    }
    if (size < 4U) {
        return false;
    }
    const size_t expected = static_cast<size_t>(size) * size * size * 3U;
    if (values.size() != expected) {
        return false;
    }
    float span[3] = {
        (std::fabs(dmax[0] - dmin[0]) > 1e-4f) ? (dmax[0] - dmin[0]) : 1.0f,
        (std::fabs(dmax[1] - dmin[1]) > 1e-4f) ? (dmax[1] - dmin[1]) : 1.0f,
        (std::fabs(dmax[2] - dmin[2]) > 1e-4f) ? (dmax[2] - dmin[2]) : 1.0f,
    };
    for (size_t i = 0; i < expected; i += 3U) {
        for (size_t c = 0; c < 3U; ++c) {
            const float nval = (values[i + c] - dmin[c]) / span[c];
            values[i + c] = (std::max)(0.0f, (std::min)(1.0f, nval));
        }
    }
    out_size = size;
    out_rgb.swap(values);
    return true;
}

bool CreateLutTexture3D(
    ID3D11Device* dev,
    uint32_t size,
    const std::vector<float>& rgb_triplets,
    ID3D11Texture3D** out_tex,
    ID3D11ShaderResourceView** out_srv) {
    if (!dev || size < 4U || rgb_triplets.size() != static_cast<size_t>(size) * size * size * 3U) {
        return false;
    }
    const size_t voxel_count = static_cast<size_t>(size) * size * size;
    std::vector<uint16_t> rgba16(voxel_count * 4U, 0x3C00U);  // alpha = 1.0f in FP16
    for (size_t i = 0; i < voxel_count; ++i) {
        rgba16[i * 4U + 0U] = FloatToHalf(rgb_triplets[i * 3U + 0U]);
        rgba16[i * 4U + 1U] = FloatToHalf(rgb_triplets[i * 3U + 1U]);
        rgba16[i * 4U + 2U] = FloatToHalf(rgb_triplets[i * 3U + 2U]);
    }

    D3D11_TEXTURE3D_DESC td{};
    td.Width = size;
    td.Height = size;
    td.Depth = size;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = rgba16.data();
    init.SysMemPitch = size * 8U;
    init.SysMemSlicePitch = size * size * 8U;

    if (FAILED(dev->CreateTexture3D(&td, &init, out_tex)) || !*out_tex) {
        SafeRelease(*out_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = td.Format;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
    srvd.Texture3D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(*out_tex, &srvd, out_srv)) || !*out_srv) {
        SafeRelease(*out_srv);
        SafeRelease(*out_tex);
        return false;
    }
    return true;
}

void EnsureCustomLutPathInitialized() {
    if (g_st.custom_lut_path[0] != L'\0') {
        return;
    }
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        wchar_t* slash = nullptr;
        for (DWORD i = 0; i < n; ++i) {
            if (exe[i] == L'\\' || exe[i] == L'/') slash = &exe[i];
        }
        if (slash) {
            *slash = L'\0';
            swprintf_s(g_st.custom_lut_path, L"%ls\\TL2_CustomLUT.cube", exe);
            return;
        }
    }
    swprintf_s(g_st.custom_lut_path, L"TL2_CustomLUT.cube");
}

bool EnsureLutReady(ID3D11Device* dev, int preset, RouteCD3DLogFn logger) {
    if (preset < 1 || preset > 3) {
        return false;
    }
    if (preset <= 2) {
        if (g_st.lut_srv[preset]) {
            return true;
        }
        constexpr uint32_t kSize = 32U;
        std::vector<float> rgb;
        BuildBuiltinLutData(preset, kSize, rgb);
        if (CreateLutTexture3D(dev, kSize, rgb, &g_st.lut_tex[preset], &g_st.lut_srv[preset])) {
            g_st.lut_size[preset] = kSize;
            return true;
        }
        return false;
    }

    EnsureCustomLutPathInitialized();
    const uint32_t seq = g_st.custom_lut_probe_seq++;
    if (!g_st.lut_srv[3] || (seq % 60U) == 0U) {
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        const bool exists = ::GetFileAttributesExW(g_st.custom_lut_path, GetFileExInfoStandard, &fad) != 0;
        const bool mtime_changed =
            exists &&
            (!g_st.custom_lut_exists ||
             fad.ftLastWriteTime.dwLowDateTime != g_st.custom_lut_mtime.dwLowDateTime ||
             fad.ftLastWriteTime.dwHighDateTime != g_st.custom_lut_mtime.dwHighDateTime);
        if (mtime_changed) {
            uint32_t sz = 0;
            std::vector<float> rgb;
            if (ParseCubeLutFile(g_st.custom_lut_path, sz, rgb)) {
                SafeRelease(g_st.lut_srv[3]);
                SafeRelease(g_st.lut_tex[3]);
                if (CreateLutTexture3D(dev, sz, rgb, &g_st.lut_tex[3], &g_st.lut_srv[3])) {
                    g_st.lut_size[3] = sz;
                    g_st.custom_lut_mtime = fad.ftLastWriteTime;
                    g_st.custom_lut_exists = true;
                    g_custom_cube_lut_loaded.store(true, std::memory_order_relaxed);
                    if (logger) {
                        logger("[RouteC][LUT] Loaded custom TL2_CustomLUT.cube (size=%ux%ux%u).", sz, sz, sz);
                    }
                    return true;
                }
            } else if (logger && !g_st.custom_lut_exists) {
                logger("[RouteC][LUT] TL2_CustomLUT.cube found but invalid; using neutral fallback.");
            }
        } else if (!exists && g_st.custom_lut_exists) {
            SafeRelease(g_st.lut_srv[3]);
            SafeRelease(g_st.lut_tex[3]);
            g_st.custom_lut_exists = false;
            g_st.custom_lut_mtime = {};
            g_custom_cube_lut_loaded.store(false, std::memory_order_relaxed);
        }
    }
    if (!g_st.lut_srv[3]) {
        constexpr uint32_t kSize = 16U;
        std::vector<float> rgb;
        BuildBuiltinLutData(0, kSize, rgb);
        if (CreateLutTexture3D(dev, kSize, rgb, &g_st.lut_tex[3], &g_st.lut_srv[3])) {
            g_st.lut_size[3] = kSize;
            g_custom_cube_lut_loaded.store(false, std::memory_order_relaxed);
        }
    }
    return g_st.lut_srv[3] != nullptr;
}


bool EnsurePanelTexture(ID3D11Device* dev, uint32_t width, uint32_t height) {
    if (g_st.panel_tex && g_st.panel_srv && g_st.panel_tex_w == width && g_st.panel_tex_h == height) {
        return true;
    }
    SafeRelease(g_st.panel_srv);
    SafeRelease(g_st.panel_tex);
    g_st.panel_tex_w = 0;
    g_st.panel_tex_h = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &g_st.panel_tex)) || !g_st.panel_tex) {
        SafeRelease(g_st.panel_tex);
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    if (FAILED(dev->CreateShaderResourceView(g_st.panel_tex, &srvd, &g_st.panel_srv)) || !g_st.panel_srv) {
        SafeRelease(g_st.panel_srv);
        SafeRelease(g_st.panel_tex);
        return false;
    }
    g_st.panel_tex_w = width;
    g_st.panel_tex_h = height;
    return true;
}

bool VerifyPostShadersCompile(PostLogFn logger) {
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
        {"PS_AoCompositeDual", "ps_4_0", macro_single},
        {"PS_AoCompositeDual", "ps_4_1", macro_msaa},
        {"PS_AerialPerspective", "ps_4_0", macro_single},
        {"PS_AerialPerspective", "ps_4_1", macro_msaa},
        {"PS_WetFloorSsr", "ps_4_0", macro_single},
        {"PS_WetFloorSsr", "ps_4_1", macro_msaa},
        {"PS_SsrSpatialResolve", "ps_4_0", macro_single},
        {"PS_SsrSpatialResolve", "ps_4_1", macro_msaa},
        {"PS_C1DepthNormalDebug", "ps_4_0", macro_single},
        {"PS_C1DepthNormalDebug", "ps_4_1", macro_msaa},
        {"PS_AoRaw", "ps_4_0", macro_single},
        {"PS_AoRaw", "ps_4_1", macro_msaa},
        {"PS_BlurH", "ps_4_0", macro_single},
        {"PS_BlurH", "ps_4_1", macro_msaa},
        {"PS_BlurV", "ps_4_0", macro_single},
        {"PS_BlurV", "ps_4_1", macro_msaa},
        {"PS_ResolveDepthMsaa", "ps_4_1", macro_msaa},
    };
    for (const Spec& sp : specs) {
        ID3DBlob* blob = nullptr;
        const bool ok = CompileShaderBlob(compile_fn, sp.entry, sp.target, sp.macros, &blob, logger);
        SafeRelease(blob);
        if (!ok) {
            return false;
        }
    }
    ID3DBlob* toast_blob = nullptr;
    const bool toast_ok = CompileShaderBlobFromSource(
        compile_fn, kToastShaderSource, kToastShaderSourceLength, "route_c_toast.hlsl",
        "PS_ToastOverlay", "ps_4_0", macro_single, &toast_blob, logger);
    SafeRelease(toast_blob);

    ID3DBlob* p2_blob = nullptr;
    const bool p2_ok = CompileShaderBlobFromSource(
        compile_fn, kP2FilterShaderSource, kP2FilterShaderSourceLength, "route_b_p2_filter.hlsl",
        "PS_P2FilterFallback", "ps_4_0", macro_single, &p2_blob, logger);
    SafeRelease(p2_blob);

    ID3DBlob* panel_blob = nullptr;
    const bool panel_ok = CompileShaderBlobFromSource(
        compile_fn, kPanelShaderSource, kPanelShaderSourceLength, "route_c_panel.hlsl",
        "PS_PanelOverlay", "ps_4_0", macro_single, &panel_blob, logger);
    SafeRelease(panel_blob);

    ID3DBlob* cursor_blob = nullptr;
    const bool cursor_ok = CompileShaderBlobFromSource(
        compile_fn, kPanelShaderSource, kPanelShaderSourceLength, "route_c_panel.hlsl",
        "PS_CursorOverlay", "ps_4_0", macro_single, &cursor_blob, logger);
    SafeRelease(cursor_blob);

    const char* bloom_lut_entries[] = {
        "PS_LutGrade3D",
    };
    bool bloom_lut_ok = true;
    for (const char* entry : bloom_lut_entries) {
        ID3DBlob* b = nullptr;
        const bool ok_one = CompileShaderBlobFromSource(
            compile_fn, kBloomLutShaderSource, kBloomLutShaderSourceLength, "route_c_bloom_lut.hlsl",
            entry, "ps_4_0", macro_single, &b, logger);
        SafeRelease(b);
        bloom_lut_ok = bloom_lut_ok && ok_one;
    }

    ID3DBlob* water_blob = nullptr;
    const bool water_ok = CompileShaderBlobFromSource(
        compile_fn, kWriteWaterDepthShaderSource, kWriteWaterDepthShaderSourceLength, "route_b_water_depth.hlsl",
        "PS_WriteWaterDepth", "ps_4_0", macro_single, &water_blob, logger);
    SafeRelease(water_blob);

    const char* smaa_entries[] = {
        "PS_SmaaEdgeDetection",
        "PS_SmaaBlendingWeight",
        "PS_SmaaNeighborhoodBlend",
    };
    bool smaa_ok = true;
    for (const char* entry : smaa_entries) {
        ID3DBlob* b = nullptr;
        const bool ok_one = CompileShaderBlobFromSource(
            compile_fn, kSmaaShadersSource, kSmaaShadersSourceLength, "route_c_smaa.hlsl",
            entry, "ps_4_0", macro_single, &b, logger);
        SafeRelease(b);
        smaa_ok = smaa_ok && ok_one;
    }

    return toast_ok && p2_ok && panel_ok && cursor_ok && bloom_lut_ok && water_ok && smaa_ok;
}

bool IsCustomCubeLutLoaded() {
    return g_custom_cube_lut_loaded.load(std::memory_order_relaxed);
}

void ShutdownPostRendererResources() {
    PostLockScope lock;
    ReleaseAllDeviceObjects();
    
}

PostRenderStats GetPostRenderStats() {
    PostRenderStats s{};
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

    PostLockScope lock;
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
    
    return s;
}


}  // namespace routeb
