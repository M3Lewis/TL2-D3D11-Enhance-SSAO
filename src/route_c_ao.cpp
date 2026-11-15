#include "route_c_ao.h"
#include "route_c_d3d11.h"
#include "phase3_scene_pipeline.h"
#include "config_panel.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#pragma comment(lib, "user32.lib")

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hook_utils.h"

namespace routeb {

namespace {

// Verified PE export prologues in OgreMain.dll (TimeDateStamp 0x6af5faa0).
constexpr uint8_t kExpectedRenderScene[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18};
constexpr uint8_t kExpectedFireQueue[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};

constexpr size_t kRenderTargetVtblSwapBuffers = 80 / sizeof(uintptr_t);    // slot 10
constexpr size_t kRenderTargetVtblGetCustomAttr = 216 / sizeof(uintptr_t);  // slot 27
constexpr int kMaxSceneStack = 8;
constexpr size_t kMaxTargetRecords = 64;
constexpr size_t kMaxQueueRecords = 256;

struct OgreMatrix4Raw {
    float m[4][4] = {};
};
static_assert(sizeof(OgreMatrix4Raw) == 64, "Ogre::Matrix4 must be 64 bytes");

using RenderSceneFn = void(__fastcall*)(void* sm, void* cam, void* vp, uint8_t show_overlays);
using FireQueueFn = uint8_t(__fastcall*)(void* sm, uint8_t queue_group_id, const std::string& invocation);
using SwapBuffersFn = void(__fastcall*)(void* rt, uint8_t wait_for_vsync);

using VpGetCameraFn = void*(__fastcall*)(const void* vp);
using VpGetTargetFn = void*(__fastcall*)(const void* vp);
using VpGetActualDimsFn = void(__fastcall*)(const void* vp, int& left, int& top, int& width, int& height);
using VpGetBoolFn = bool(__fastcall*)(const void* vp);
using FrustumGetMatFn = const OgreMatrix4Raw&(__fastcall*)(const void* frustum);
using CamGetFloatFn = float(__fastcall*)(const void* cam);
using ObjGetNameFn = const std::string&(__fastcall*)(const void* obj);
using RtGetCustomAttrFn = void(__fastcall*)(void* rt, const std::string& name, void* out_data);

struct OgreC0Api {
    VpGetCameraFn vp_get_camera = nullptr;
    VpGetTargetFn vp_get_target = nullptr;
    VpGetActualDimsFn vp_get_dims = nullptr;
    VpGetBoolFn vp_get_clear = nullptr;
    VpGetBoolFn vp_get_overlays = nullptr;
    VpGetBoolFn vp_get_shadows = nullptr;
    FrustumGetMatFn frustum_get_proj_rs = nullptr;
    CamGetFloatFn cam_get_near = nullptr;
    CamGetFloatFn cam_get_far = nullptr;
    ObjGetNameFn rt_get_name = nullptr;
    ObjGetNameFn sm_get_name = nullptr;
    ObjGetNameFn cam_get_name = nullptr;
};

struct D3DResourceInfo {
    bool valid = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t tex_format = 0;
    uint32_t view_format = 0;
    uint32_t view_dim = 0;
    uint32_t sample_count = 1;
    uint32_t sample_quality = 0;
    uint32_t bind_flags = 0;
};

struct ActiveQueueCounters {
    bool active = false;
    uint8_t queue_id = 0;
    char invocation[40]{};
    uint32_t setpass_calls = 0;
    uint32_t solid_dw = 0;
    uint32_t cutout_dw = 0;
    uint32_t trans_nodw = 0;
    uint32_t trans_dw = 0;
    uint32_t unlit_nodw = 0;
    uint32_t has_vp = 0;
    uint32_t has_fp = 0;
    char sample_passes[128]{};
};

struct ActiveSceneFrame {
    void* sm = nullptr;
    void* cam = nullptr;
    void* vp = nullptr;
    void* rt = nullptr;
    uint64_t target_key = 0;
    bool is_perspective = false;
    bool world_candidate = false;
    bool ao_eligible = false;
    bool ao_injected = false;
    bool particle_depth_ready = false;
    bool filter_eligible = false;
    bool filter_injected = false;
    uint32_t world_geom_draws = 0;
    int vp_l = 0;
    int vp_t = 0;
    int vp_w = 0;
    int vp_h = 0;
    float near_clip = 1.0f;
    float far_clip = 100.0f;
    OgreMatrix4Raw proj{};
    char rt_name[48]{};
    ActiveQueueCounters queue{};
};

struct TargetSurveyRecord {
    uint64_t key = 0;
    uint64_t hits = 0;
    bool is_perspective = false;
    bool world_candidate = false;
    char summary[384]{};
};

struct QueueSurveyRecord {
    uint64_t key = 0;
    uint64_t hits = 0;
    uint64_t total_setpass = 0;
    uint64_t total_solid_dw = 0;
    uint64_t total_cutout_dw = 0;
    uint64_t total_trans_nodw = 0;
    uint64_t total_trans_dw = 0;
    uint64_t total_unlit_nodw = 0;
    uint64_t total_fp = 0;
    uint64_t total_vp = 0;
    uint8_t queue_id = 0;
    int vp_w = 0;
    int vp_h = 0;
    bool is_perspective = false;
    uint32_t om_rtv_count = 0;
    uint32_t om_rtv0_fmt = 0;
    uint32_t om_rtv0_msaa = 0;
    uint32_t om_dsv_tex_fmt = 0;
    uint32_t om_dsv_fmt = 0;
    uint32_t om_dsv_bind = 0;
    char rt_name[48]{};
    char invocation[40]{};
    char sample_passes[128]{};
};

OgreC0Api g_api{};
RouteCLogFn g_log = nullptr;

Detour64 g_detour_render_scene;
Detour64 g_detour_queue_started;
Detour64 g_detour_queue_ended;

RenderSceneFn g_orig_render_scene = nullptr;
FireQueueFn g_orig_queue_started = nullptr;
FireQueueFn g_orig_queue_ended = nullptr;

SRWLOCK g_swap_hook_lock = SRWLOCK_INIT;
void** g_swap_vtable = nullptr;
SwapBuffersFn g_orig_swap_buffers = nullptr;
std::atomic<bool> g_swap_hooked{false};

std::atomic<bool> g_hooked{false};
std::atomic<bool> g_c0_survey_active{false};
std::atomic<bool> g_ao_runtime_enabled{true};
std::atomic<bool> g_sscs_runtime_enabled{true};
std::atomic<bool> g_p1_soft_particles_enabled{true};
std::atomic<bool> g_world_depth_ready_for_particles{false};
std::atomic<uint64_t> g_toast_expire_tick_ms{0};
std::atomic<int> g_ao_debug_mode{0};  // Phase C2 default: DebugMode=0 (Live AO Composite)
std::atomic<uint64_t> g_render_scene_calls{0};
std::atomic<uint64_t> g_world_candidates{0};
std::atomic<uint64_t> g_queue_started_calls{0};
std::atomic<uint64_t> g_queue_ended_calls{0};

SRWLOCK g_survey_lock = SRWLOCK_INIT;
TargetSurveyRecord g_target_records[kMaxTargetRecords]{};
size_t g_target_record_count = 0;
QueueSurveyRecord g_queue_records[kMaxQueueRecords]{};
size_t g_queue_record_count = 0;

thread_local ActiveSceneFrame g_scene_stack[kMaxSceneStack]{};
thread_local int g_scene_depth = 0;

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
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

int ResolveAoDebugMode(int default_mode) {
    char buf[16] = {};
    const DWORD len = ::GetEnvironmentVariableA("TL2_C_AO_DEBUG", buf, static_cast<DWORD>(sizeof(buf)));
    if (len == 0) {
        return default_mode;
    }
    if (buf[0] == '0') {
        return 0;
    }
    if (buf[0] == '1') {
        return 1;
    }
    if (buf[0] == '2') {
        return 2;
    }
    if (buf[0] == '3') {
        return 3;
    }
    if (std::strcmp(buf, "off") == 0 || std::strcmp(buf, "none") == 0 || std::strcmp(buf, "-1") == 0) {
        return -1;
    }
    return default_mode;
}

uint64_t Fnv1aHashBytes(uint64_t seed, const void* data, size_t len) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(bytes[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t Fnv1aHashCStr(uint64_t seed, const char* s) {
    if (!s) {
        return seed;
    }
    return Fnv1aHashBytes(seed, s, std::strlen(s));
}

void CopyTruncatedStr(char* dst, size_t dst_size, const std::string& src) {
    if (!dst || dst_size == 0) {
        return;
    }
    const size_t n = (std::min)(src.size(), dst_size - 1);
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
    dst[n] = '\0';
}

void AppendUniqueSamplePass(char* buf, size_t buf_size, const char* pass_name) {
    if (!buf || buf_size == 0) {
        return;
    }
    const char* name = (pass_name && pass_name[0] != '\0') ? pass_name : "<unnamed>";
    if (std::strstr(buf, name) != nullptr) {
        return;
    }
    const size_t cur = std::strlen(buf);
    if (cur + 2 >= buf_size) {
        return;
    }
    if (cur > 0) {
        std::snprintf(buf + cur, buf_size - cur, ",%s", name);
    } else {
        std::snprintf(buf, buf_size, "%s", name);
    }
}

template <typename Fn>
bool ResolveOgreProc(HMODULE ogre_mod, const char* export_name, Fn* out_fn) {
    if (!ogre_mod || !export_name || !out_fn) {
        return false;
    }
    FARPROC proc = ::GetProcAddress(ogre_mod, export_name);
    if (!proc) {
        if (g_log) {
            g_log("[RouteC][C0] Missing OgreMain export: %s", export_name);
        }
        return false;
    }
    *out_fn = reinterpret_cast<Fn>(proc);
    return true;
}

bool InitOgreC0Api(HMODULE ogre_mod) {
    g_api = {};
    bool ok = true;
    ok &= ResolveOgreProc(ogre_mod, "?getCamera@Viewport@Ogre@@QEBAPEAVCamera@2@XZ", &g_api.vp_get_camera);
    ok &= ResolveOgreProc(ogre_mod, "?getTarget@Viewport@Ogre@@QEBAPEAVRenderTarget@2@XZ", &g_api.vp_get_target);
    ok &= ResolveOgreProc(ogre_mod, "?getActualDimensions@Viewport@Ogre@@QEBAXAEAH000@Z", &g_api.vp_get_dims);
    ok &= ResolveOgreProc(ogre_mod, "?getClearEveryFrame@Viewport@Ogre@@QEBA_NXZ", &g_api.vp_get_clear);
    ok &= ResolveOgreProc(ogre_mod, "?getOverlaysEnabled@Viewport@Ogre@@QEBA_NXZ", &g_api.vp_get_overlays);
    ok &= ResolveOgreProc(ogre_mod, "?getShadowsEnabled@Viewport@Ogre@@QEBA_NXZ", &g_api.vp_get_shadows);
    ok &= ResolveOgreProc(ogre_mod, "?getProjectionMatrixRS@Frustum@Ogre@@UEBAAEBVMatrix4@2@XZ", &g_api.frustum_get_proj_rs);
    ok &= ResolveOgreProc(ogre_mod, "?getNearClipDistance@Camera@Ogre@@UEBAMXZ", &g_api.cam_get_near);
    ok &= ResolveOgreProc(ogre_mod, "?getFarClipDistance@Camera@Ogre@@UEBAMXZ", &g_api.cam_get_far);
    ok &= ResolveOgreProc(ogre_mod, "?getName@RenderTarget@Ogre@@UEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ", &g_api.rt_get_name);
    ok &= ResolveOgreProc(ogre_mod, "?getName@SceneManager@Ogre@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ", &g_api.sm_get_name);
    ok &= ResolveOgreProc(ogre_mod, "?getName@Camera@Ogre@@UEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ", &g_api.cam_get_name);
    return ok;
}

bool TryGetBorrowedRtView(void* render_target, const char* attr_name, void** out_view) {
    *out_view = nullptr;
    if (!render_target || !attr_name) {
        return false;
    }
    auto* vtbl = *reinterpret_cast<uintptr_t* const*>(render_target);
    if (!vtbl) {
        return false;
    }
    auto fn = reinterpret_cast<RtGetCustomAttrFn>(vtbl[kRenderTargetVtblGetCustomAttr]);
    if (!fn) {
        return false;
    }
    const std::string key(attr_name);
    fn(render_target, key, out_view);
    return (*out_view != nullptr);
}

D3DResourceInfo InspectRtv(ID3D11RenderTargetView* rtv) {
    D3DResourceInfo info{};
    if (!rtv) {
        return info;
    }
    D3D11_RENDER_TARGET_VIEW_DESC rdesc{};
    rtv->GetDesc(&rdesc);
    info.view_format = static_cast<uint32_t>(rdesc.Format);
    info.view_dim = static_cast<uint32_t>(rdesc.ViewDimension);

    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (res) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC tdesc{};
            tex->GetDesc(&tdesc);
            info.valid = true;
            info.width = tdesc.Width;
            info.height = tdesc.Height;
            info.tex_format = static_cast<uint32_t>(tdesc.Format);
            info.sample_count = tdesc.SampleDesc.Count;
            info.sample_quality = tdesc.SampleDesc.Quality;
            info.bind_flags = tdesc.BindFlags;
            SafeRelease(tex);
        }
        SafeRelease(res);
    }
    return info;
}

D3DResourceInfo InspectDsv(ID3D11DepthStencilView* dsv) {
    D3DResourceInfo info{};
    if (!dsv) {
        return info;
    }
    D3D11_DEPTH_STENCIL_VIEW_DESC ddesc{};
    dsv->GetDesc(&ddesc);
    info.view_format = static_cast<uint32_t>(ddesc.Format);
    info.view_dim = static_cast<uint32_t>(ddesc.ViewDimension);

    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (res) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC tdesc{};
            tex->GetDesc(&tdesc);
            info.valid = true;
            info.width = tdesc.Width;
            info.height = tdesc.Height;
            info.tex_format = static_cast<uint32_t>(tdesc.Format);
            info.sample_count = tdesc.SampleDesc.Count;
            info.sample_quality = tdesc.SampleDesc.Quality;
            info.bind_flags = tdesc.BindFlags;
            SafeRelease(tex);
        }
        SafeRelease(res);
    }
    return info;
}

bool TryGetContextFromRenderTarget(void* render_target, ID3D11DeviceContext** out_ctx, ID3D11RenderTargetView** out_rt_rtv, ID3D11DepthStencilView** out_rt_dsv) {
    *out_ctx = nullptr;
    *out_rt_rtv = nullptr;
    *out_rt_dsv = nullptr;

    void* raw_rtv = nullptr;
    void* raw_dsv = nullptr;
    (void)TryGetBorrowedRtView(render_target, "ID3D11RenderTargetView", &raw_rtv);
    (void)TryGetBorrowedRtView(render_target, "ID3D11DepthStencilView", &raw_dsv);

    auto* rtv = static_cast<ID3D11RenderTargetView*>(raw_rtv);
    auto* dsv = static_cast<ID3D11DepthStencilView*>(raw_dsv);
    *out_rt_rtv = rtv;
    *out_rt_dsv = dsv;

    ID3D11Device* dev = nullptr;
    if (rtv) {
        rtv->GetDevice(&dev);
    } else if (dsv) {
        dsv->GetDevice(&dev);
    }
    if (!dev) {
        return false;
    }
    dev->GetImmediateContext(out_ctx);
    SafeRelease(dev);
    return (*out_ctx != nullptr);
}

void QueryActiveOmViews(
    ID3D11DeviceContext* ctx,
    uint32_t* out_rtv_count,
    D3DResourceInfo* out_rtv0,
    D3DResourceInfo* out_dsv) {
    *out_rtv_count = 0;
    *out_rtv0 = {};
    *out_dsv = {};
    if (!ctx) {
        return;
    }
    ID3D11RenderTargetView* rtvs[8]{};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(8, rtvs, &dsv);
    for (size_t i = 0; i < 8; ++i) {
        if (rtvs[i]) {
            ++(*out_rtv_count);
            if (i == 0) {
                *out_rtv0 = InspectRtv(rtvs[i]);
            }
            SafeRelease(rtvs[i]);
        }
    }
    if (dsv) {
        *out_dsv = InspectDsv(dsv);
        SafeRelease(dsv);
    }
}

void RecordTargetSurvey(
    const ActiveSceneFrame& frame,
    const char* sm_name,
    const char* cam_name,
    int vp_l,
    int vp_t,
    bool clear_every,
    bool overlays_en,
    bool shadows_en,
    uint8_t show_overlays,
    float near_clip,
    float far_clip,
    const OgreMatrix4Raw& proj,
    const D3DResourceInfo& rtv_info,
    const D3DResourceInfo& dsv_info,
    const char* dsv_source) {
    ::AcquireSRWLockExclusive(&g_survey_lock);
    for (size_t i = 0; i < g_target_record_count; ++i) {
        if (g_target_records[i].key == frame.target_key) {
            ++g_target_records[i].hits;
            ::ReleaseSRWLockExclusive(&g_survey_lock);
            return;
        }
    }
    if (g_target_record_count < kMaxTargetRecords) {
        TargetSurveyRecord& rec = g_target_records[g_target_record_count++];
        rec.key = frame.target_key;
        rec.hits = 1;
        rec.is_perspective = frame.is_perspective;
        rec.world_candidate = frame.world_candidate;
        std::snprintf(
            rec.summary,
            sizeof(rec.summary),
            "rt=\"%s\" sm=\"%s\" cam=\"%s\" vp=(%d,%d,%dx%d) persp=%d cand=%d near=%.4f far=%.2f "
            "p=[p00=%.4f p11=%.4f p02=%.4f p12=%.4f p22=%.6f p23=%.4f p32=%.1f p33=%.1f] "
            "clr=%d ovl=%d shd=%d showOvl=%u "
            "rtv=(%ux%u fmt=%u dim=%u msaa=%ux%u bind=0x%x) "
            "dsv=(%ux%u texFmt=%u dsvFmt=%u dim=%u msaa=%ux%u bind=0x%x src=%s)",
            frame.rt_name,
            sm_name ? sm_name : "",
            cam_name ? cam_name : "",
            vp_l,
            vp_t,
            frame.vp_w,
            frame.vp_h,
            frame.is_perspective ? 1 : 0,
            frame.world_candidate ? 1 : 0,
            near_clip,
            far_clip,
            proj.m[0][0],
            proj.m[1][1],
            proj.m[0][2],
            proj.m[1][2],
            proj.m[2][2],
            proj.m[2][3],
            proj.m[3][2],
            proj.m[3][3],
            clear_every ? 1 : 0,
            overlays_en ? 1 : 0,
            shadows_en ? 1 : 0,
            static_cast<unsigned>(show_overlays),
            rtv_info.width,
            rtv_info.height,
            rtv_info.tex_format,
            rtv_info.view_dim,
            rtv_info.sample_count,
            rtv_info.sample_quality,
            rtv_info.bind_flags,
            dsv_info.width,
            dsv_info.height,
            dsv_info.tex_format,
            dsv_info.view_format,
            dsv_info.view_dim,
            dsv_info.sample_count,
            dsv_info.sample_quality,
            dsv_info.bind_flags,
            dsv_source);
        if (g_log) {
            g_log("[RouteC][C0][Target] %s", rec.summary);
        }
    }
    ::ReleaseSRWLockExclusive(&g_survey_lock);
}

void RecordQueueSurvey(
    const ActiveSceneFrame& frame,
    const ActiveQueueCounters& q,
    uint32_t om_rtv_count,
    const D3DResourceInfo& om_rtv0,
    const D3DResourceInfo& om_dsv) {
    uint64_t qkey = frame.target_key;
    qkey = Fnv1aHashBytes(qkey, &q.queue_id, sizeof(q.queue_id));
    qkey = Fnv1aHashBytes(qkey, &om_rtv0.tex_format, sizeof(om_rtv0.tex_format));
    qkey = Fnv1aHashBytes(qkey, &om_dsv.tex_format, sizeof(om_dsv.tex_format));

    ::AcquireSRWLockExclusive(&g_survey_lock);
    for (size_t i = 0; i < g_queue_record_count; ++i) {
        if (g_queue_records[i].key == qkey) {
            QueueSurveyRecord& rec = g_queue_records[i];
            ++rec.hits;
            rec.total_setpass += q.setpass_calls;
            rec.total_solid_dw += q.solid_dw;
            rec.total_cutout_dw += q.cutout_dw;
            rec.total_trans_nodw += q.trans_nodw;
            rec.total_trans_dw += q.trans_dw;
            rec.total_unlit_nodw += q.unlit_nodw;
            rec.total_fp += q.has_fp;
            rec.total_vp += q.has_vp;
            if (rec.sample_passes[0] == '\0' && q.sample_passes[0] != '\0') {
                std::snprintf(rec.sample_passes, sizeof(rec.sample_passes), "%s", q.sample_passes);
            }
            ::ReleaseSRWLockExclusive(&g_survey_lock);
            return;
        }
    }
    if (g_queue_record_count < kMaxQueueRecords) {
        QueueSurveyRecord& rec = g_queue_records[g_queue_record_count++];
        rec.key = qkey;
        rec.hits = 1;
        rec.total_setpass = q.setpass_calls;
        rec.total_solid_dw = q.solid_dw;
        rec.total_cutout_dw = q.cutout_dw;
        rec.total_trans_nodw = q.trans_nodw;
        rec.total_trans_dw = q.trans_dw;
        rec.total_unlit_nodw = q.unlit_nodw;
        rec.total_fp = q.has_fp;
        rec.total_vp = q.has_vp;
        rec.queue_id = q.queue_id;
        rec.vp_w = frame.vp_w;
        rec.vp_h = frame.vp_h;
        rec.is_perspective = frame.is_perspective;
        rec.om_rtv_count = om_rtv_count;
        rec.om_rtv0_fmt = om_rtv0.tex_format;
        rec.om_rtv0_msaa = om_rtv0.sample_count;
        rec.om_dsv_tex_fmt = om_dsv.tex_format;
        rec.om_dsv_fmt = om_dsv.view_format;
        rec.om_dsv_bind = om_dsv.bind_flags;
        std::snprintf(rec.rt_name, sizeof(rec.rt_name), "%s", frame.rt_name);
        std::snprintf(rec.invocation, sizeof(rec.invocation), "%s", q.invocation);
        std::snprintf(rec.sample_passes, sizeof(rec.sample_passes), "%s", q.sample_passes);

        if (g_log) {
            g_log(
                "[RouteC][C0][Queue] rt=\"%s\"(%dx%d,persp=%d) q=%u inv=\"%s\" "
                "setPass=%u solidDW=%u cutout=%u transNoDW=%u transDW=%u unlitNoDW=%u vp=%u fp=%u "
                "omRtvs=%u omRtv0=(fmt=%u,msaa=%u) omDsv=(texFmt=%u,dsvFmt=%u,bind=0x%x) passes=[%s]",
                rec.rt_name,
                rec.vp_w,
                rec.vp_h,
                rec.is_perspective ? 1 : 0,
                static_cast<unsigned>(rec.queue_id),
                rec.invocation,
                q.setpass_calls,
                q.solid_dw,
                q.cutout_dw,
                q.trans_nodw,
                q.trans_dw,
                q.unlit_nodw,
                q.has_vp,
                q.has_fp,
                rec.om_rtv_count,
                rec.om_rtv0_fmt,
                rec.om_rtv0_msaa,
                rec.om_dsv_tex_fmt,
                rec.om_dsv_fmt,
                rec.om_dsv_bind,
                rec.sample_passes);
        }
    }
    ::ReleaseSRWLockExclusive(&g_survey_lock);
}

void TryDispatchAoForFrame(ActiveSceneFrame& frame, const char* trigger_src, uint8_t qid) {
    if (frame.ao_injected || !frame.ao_eligible || frame.world_geom_draws == 0) {
        return;
    }
    const bool ao_on = g_ao_runtime_enabled.load(std::memory_order_relaxed);
    const int dbg_mode = ao_on ? g_ao_debug_mode.load(std::memory_order_relaxed) : -1;
    const bool sp_on = g_p1_soft_particles_enabled.load(std::memory_order_relaxed);
    if (dbg_mode < 0 && !sp_on) {
        return;
    }
    frame.ao_injected = true;

    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rt_rtv = nullptr;
    ID3D11DepthStencilView* rt_dsv = nullptr;
    if (!TryGetContextFromRenderTarget(frame.rt, &ctx, &rt_rtv, &rt_dsv) || !ctx) {
        SafeRelease(ctx);
        return;
    }

    RouteCAoRenderRequest req{};
    req.ctx = ctx;
    req.fallback_rtv = rt_rtv;
    req.fallback_dsv = rt_dsv;
    req.proj_4x4 = &frame.proj.m[0][0];
    req.near_clip = frame.near_clip;
    req.far_clip = frame.far_clip;
    req.vp_left = frame.vp_l;
    req.vp_top = frame.vp_t;
    req.vp_width = frame.vp_w;
    req.vp_height = frame.vp_h;
    req.debug_mode = (dbg_mode >= 0) ? dbg_mode : 0;
    req.ao_enabled = (dbg_mode >= 0);
    req.sscs_enabled = IsRouteCSscsRuntimeEnabled();
    req.capture_particle_depth = sp_on;
    const bool ok = ExecuteRouteCAoPass(req, g_log);
    frame.particle_depth_ready = ok && sp_on;
    g_world_depth_ready_for_particles.store(frame.particle_depth_ready, std::memory_order_release);

    static std::atomic<uint32_t> s_dispatch_logs{0};
    const uint32_t log_idx = s_dispatch_logs.fetch_add(1, std::memory_order_relaxed);
    if (g_log && log_idx < 8) {
        const auto d3d_st = GetRouteCD3DStats();
        g_log(
            "[RouteC][C1][Dispatch #%u] ok=%d rt=\"%s\" trig=%s q=%u vp=(%d,%d,%dx%d) draws=%u tex=%ux%u rtvFmt=%u dsvTexFmt=%u",
            log_idx + 1,
            ok ? 1 : 0,
            frame.rt_name,
            trigger_src ? trigger_src : "?",
            static_cast<unsigned>(qid),
            frame.vp_l,
            frame.vp_t,
            frame.vp_w,
            frame.vp_h,
            frame.world_geom_draws,
            d3d_st.last_tex_w,
            d3d_st.last_tex_h,
            d3d_st.last_rtv_fmt,
            d3d_st.last_dsv_tex_fmt);
    }

    SafeRelease(ctx);
}

bool IsMainWorldRenderTargetName(const std::string& rt_name) {
    // Compositor scratch RTTs ("rtt/.../rt0", "rt1", "ProjectorRTT_*") are excluded;
    // keep either "rtt/.../rt_output/..." or non-RTT main RenderWindow.
    if (rt_name.rfind("rtt/", 0) == 0) {
        return rt_name.find("/rt_output/") != std::string::npos;
    }
    return true;
}

void TryDispatchP2FilterFallbackForFrame(ActiveSceneFrame& frame, uint8_t qid) {
    if (frame.filter_injected || !frame.filter_eligible || WasP2PostShaderSeenRecently()) {
        return;
    }
    const int preset = GetP2FilterPreset();
    if (preset <= 0) {
        return;
    }
    frame.filter_injected = true;

    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rt_rtv = nullptr;
    ID3D11DepthStencilView* rt_dsv = nullptr;
    if (!TryGetContextFromRenderTarget(frame.rt, &ctx, &rt_rtv, &rt_dsv) || !ctx) {
        SafeRelease(ctx);
        return;
    }

    RouteCP2FilterRequest req{};
    req.ctx = ctx;
    req.fallback_rtv = rt_rtv;
    req.vp_left = frame.vp_l;
    req.vp_top = frame.vp_t;
    req.vp_width = frame.vp_w;
    req.vp_height = frame.vp_h;
    req.filter_preset = preset;
    const bool ok = ExecuteRouteCP2FilterPass(req, g_log);

    static std::atomic<bool> s_logged_fb{false};
    if (!s_logged_fb.exchange(true, std::memory_order_relaxed) && g_log) {
        g_log("[RouteB][P2][Fallback] Applied scene filter preset=%s on main RT=\"%s\" q=%u ok=%d",
              P2FilterPresetName(preset), frame.rt_name, static_cast<unsigned>(qid), ok ? 1 : 0);
    }
    SafeRelease(ctx);
}

bool IsGameProcessForeground() {
    const HWND fg = ::GetForegroundWindow();
    if (!fg) {
        return false;
    }
    DWORD fg_pid = 0;
    ::GetWindowThreadProcessId(fg, &fg_pid);
    return fg_pid == ::GetCurrentProcessId();
}

void PollRuntimeToggleHotkeys() {
    if (!IsGameProcessForeground()) {
        return;
    }
    if (PollFallbackConfigPanelHotkey() && g_log) {
        g_log("[RouteB][Hotkey] F8 fallback -> Config panel: %s",
              IsConfigPanelOpen() ? "OPEN" : "CLOSED");
    }
}

void DrawToastOverlayForRenderTarget(void* rw) {
    if (IsConfigPanelOpen()) {
        return;
    }
    const uint64_t expire_ms = g_toast_expire_tick_ms.load(std::memory_order_relaxed);
    if (expire_ms == 0 || !rw) {
        return;
    }
    const uint64_t now_ms = ::GetTickCount64();
    if (now_ms >= expire_ms) {
        g_toast_expire_tick_ms.store(0, std::memory_order_relaxed);
        return;
    }
    const uint64_t rem_ms = expire_ms - now_ms;
    const float fade = (rem_ms < 350ULL) ? (static_cast<float>(rem_ms) / 350.0f) : 1.0f;

    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    const bool got_ctx = TryGetContextFromRenderTarget(rw, &ctx, &rtv, &dsv);
    bool drawn = false;
    if (got_ctx && ctx && rtv) {
        drawn = ExecuteRouteCToastOverlay(
            ctx,
            rtv,
            IsPhase3RuntimeEnhanceEnabled(),
            IsRouteCAoRuntimeEnabled(),
            GetP2FilterPreset(),
            IsF11VisualEnhanceEnabled(),
            fade,
            g_log);
    }
    static bool logged_toast_diag = false;
    if (!logged_toast_diag && g_log) {
        logged_toast_diag = true;
        g_log("[RouteC][Toast] Diag first draw rw=%p gotCtx=%d ctx=%p rtv=%p drawn=%d",
              rw, got_ctx ? 1 : 0, ctx, rtv, drawn ? 1 : 0);
    }
    SafeRelease(ctx);
}

void DrawPanelOverlayForRenderTarget(void* rw) {
    if (!rw || !IsConfigPanelOpen()) {
        return;
    }
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    if (TryGetContextFromRenderTarget(rw, &ctx, &rtv, &dsv) && ctx && rtv) {
        RenderConfigPanelOverlay(rw, ctx, rtv, g_log);
    }
    SafeRelease(ctx);
}

void __fastcall HookRenderWindowSwapBuffers(void* rw, uint8_t wait_for_vsync) {
    if (g_swap_hooked.load(std::memory_order_acquire)) {
        if (rw && !IsPanelWindowAttached()) {
            HWND rw_hwnd = nullptr;
            if (TryGetBorrowedRtView(rw, "WINDOW", reinterpret_cast<void**>(&rw_hwnd)) && rw_hwnd) {
                if (AttachPanelWindow(rw_hwnd) && g_log) {
                    g_log("[RouteB][Panel] Subclassed RenderWindow HWND=%p (F8 opens config panel).",
                          rw_hwnd);
                }
            }
        }
        PollRuntimeToggleHotkeys();
        DrawToastOverlayForRenderTarget(rw);
        DrawPanelOverlayForRenderTarget(rw);
    }
    if (g_orig_swap_buffers) {
        g_orig_swap_buffers(rw, wait_for_vsync);
    }
}

void InstallRenderWindowSwapBuffersHook(void* rw, const std::string& rt_name) {
    if (!rw || g_swap_hooked.load(std::memory_order_acquire)) {
        return;
    }
    ::AcquireSRWLockExclusive(&g_swap_hook_lock);
    if (!g_swap_hooked.load(std::memory_order_relaxed)) {
        void** vtable = *reinterpret_cast<void***>(rw);
        if (vtable && vtable[kRenderTargetVtblSwapBuffers] &&
            vtable[kRenderTargetVtblSwapBuffers] != reinterpret_cast<void*>(&HookRenderWindowSwapBuffers)) {
            void** slot = &vtable[kRenderTargetVtblSwapBuffers];
            DWORD old_prot = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
                g_orig_swap_buffers = reinterpret_cast<SwapBuffersFn>(*slot);
                g_swap_vtable = vtable;
                *slot = reinterpret_cast<void*>(&HookRenderWindowSwapBuffers);
                ::VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);
                g_swap_hooked.store(true, std::memory_order_release);
                if (g_toast_expire_tick_ms.load(std::memory_order_relaxed) == 0) {
                    g_toast_expire_tick_ms.store(::GetTickCount64() + 2600ULL, std::memory_order_relaxed);
                }
                if (g_log) {
                    g_log("[RouteC][Toast] Hooked RenderWindow::swapBuffers vtbl=%p orig=%p rt=\"%s\"",
                          vtable, reinterpret_cast<void*>(g_orig_swap_buffers), rt_name.c_str());
                }
            }
        }
    }
    ::ReleaseSRWLockExclusive(&g_swap_hook_lock);
}

void RemoveRenderWindowSwapBuffersHook() {
    ::AcquireSRWLockExclusive(&g_swap_hook_lock);
    void** vtable = g_swap_vtable;
    SwapBuffersFn orig = g_orig_swap_buffers;
    g_swap_hooked.store(false, std::memory_order_release);
    if (vtable && orig) {
        void** slot = &vtable[kRenderTargetVtblSwapBuffers];
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(slot, &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.State == MEM_COMMIT) {
            DWORD old_prot = 0;
            if (::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
                *slot = reinterpret_cast<void*>(orig);
                ::VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);
            }
        }
    }
    g_swap_vtable = nullptr;
    g_orig_swap_buffers = nullptr;
    ::ReleaseSRWLockExclusive(&g_swap_hook_lock);
}

void __fastcall HookRenderScene(void* sm, void* cam, void* vp, uint8_t show_overlays) {
    g_render_scene_calls.fetch_add(1, std::memory_order_relaxed);
    if (!g_orig_render_scene) {
        return;
    }
    if (!g_swap_hooked.load(std::memory_order_acquire) && vp && g_api.vp_get_target && g_api.rt_get_name) {
        if (void* swap_rt = g_api.vp_get_target(vp)) {
            const std::string& swap_rt_name = g_api.rt_get_name(swap_rt);
            if (swap_rt_name.rfind("rtt/", 0) != 0 && swap_rt_name.find("RTT") == std::string::npos) {
                InstallRenderWindowSwapBuffersHook(swap_rt, swap_rt_name);
            }
        }
    }
    if (g_scene_depth == 0 && !g_swap_hooked.load(std::memory_order_relaxed)) {
        PollRuntimeToggleHotkeys();
    }

    const bool survey_on = g_c0_survey_active.load(std::memory_order_relaxed);
    const bool ao_on = g_ao_runtime_enabled.load(std::memory_order_relaxed);
    const int dbg_mode = ao_on ? g_ao_debug_mode.load(std::memory_order_relaxed) : -1;
    const bool sp_on = g_p1_soft_particles_enabled.load(std::memory_order_relaxed);
    const bool need_depth_pipeline = (dbg_mode >= 0) || sp_on;
    const bool need_filter_fallback = (GetP2FilterPreset() > 0) && !WasP2PostShaderSeenRecently();
    if ((!survey_on && !need_depth_pipeline && !need_filter_fallback) || g_scene_depth >= kMaxSceneStack || !sm || !vp) {
        const bool prev_ready = g_world_depth_ready_for_particles.exchange(false, std::memory_order_acq_rel);
        g_orig_render_scene(sm, cam, vp, show_overlays);
        g_world_depth_ready_for_particles.store(prev_ready, std::memory_order_release);
        return;
    }

    void* active_cam = cam ? cam : (g_api.vp_get_camera ? g_api.vp_get_camera(vp) : nullptr);
    void* rt = g_api.vp_get_target ? g_api.vp_get_target(vp) : nullptr;
    if (!active_cam || !rt) {
        const bool prev_ready = g_world_depth_ready_for_particles.exchange(false, std::memory_order_acq_rel);
        g_orig_render_scene(sm, cam, vp, show_overlays);
        g_world_depth_ready_for_particles.store(prev_ready, std::memory_order_release);
        return;
    }

    int vp_l = 0, vp_t = 0, vp_w = 0, vp_h = 0;
    g_api.vp_get_dims(vp, vp_l, vp_t, vp_w, vp_h);
    const OgreMatrix4Raw& proj = g_api.frustum_get_proj_rs(active_cam);
    const bool is_persp = (std::fabs(proj.m[3][2] + 1.0f) < 1e-4f && std::fabs(proj.m[3][3]) < 1e-4f);
    const bool world_cand = is_persp && vp_w >= 256 && vp_h >= 256;
    if (world_cand) {
        g_world_candidates.fetch_add(1, std::memory_order_relaxed);
    }

    const std::string& sm_str = g_api.sm_get_name(sm);
    const std::string& cam_str = g_api.cam_get_name(active_cam);
    bool ao_eligible =
        need_depth_pipeline &&
        world_cand &&
        (sm_str == "SMInstance") &&
        (cam_str == "PlayerCam");
    bool filter_eligible =
        need_filter_fallback &&
        world_cand &&
        (sm_str == "SMInstance") &&
        (cam_str == "PlayerCam") &&
        (show_overlays != 0);

    const std::string* rt_str_ptr = nullptr;
    if (survey_on || ao_eligible || filter_eligible) {
        rt_str_ptr = &g_api.rt_get_name(rt);
        if (ao_eligible && !IsMainWorldRenderTargetName(*rt_str_ptr)) {
            ao_eligible = false;
        }
        if (filter_eligible && rt_str_ptr->rfind("rtt/", 0) == 0) {
            filter_eligible = false;
        }
    }

    ActiveSceneFrame& frame = g_scene_stack[g_scene_depth++];
    frame = {};
    frame.sm = sm;
    frame.cam = active_cam;
    frame.vp = vp;
    frame.rt = rt;
    frame.is_perspective = is_persp;
    frame.world_candidate = world_cand;
    frame.ao_eligible = ao_eligible;
    frame.filter_eligible = filter_eligible;
    frame.vp_l = vp_l;
    frame.vp_t = vp_t;
    frame.vp_w = vp_w;
    frame.vp_h = vp_h;
    frame.near_clip = g_api.cam_get_near(active_cam);
    frame.far_clip = g_api.cam_get_far(active_cam);
    frame.proj = proj;
    if (rt_str_ptr) {
        CopyTruncatedStr(frame.rt_name, sizeof(frame.rt_name), *rt_str_ptr);
    }

    if (survey_on) {
        const bool clear_every = g_api.vp_get_clear(vp);
        const bool overlays_en = g_api.vp_get_overlays(vp);
        const bool shadows_en = g_api.vp_get_shadows(vp);

        ID3D11DeviceContext* ctx = nullptr;
        ID3D11RenderTargetView* rt_rtv = nullptr;
        ID3D11DepthStencilView* rt_dsv = nullptr;
        (void)TryGetContextFromRenderTarget(rt, &ctx, &rt_rtv, &rt_dsv);

        D3DResourceInfo rtv_info = InspectRtv(rt_rtv);
        D3DResourceInfo dsv_info = InspectDsv(rt_dsv);
        const char* dsv_src = rt_dsv ? "RenderTarget" : "None";
        if ((!rtv_info.valid || !dsv_info.valid) && ctx) {
            uint32_t om_cnt = 0;
            D3DResourceInfo om_rtv0{};
            D3DResourceInfo om_dsv{};
            QueryActiveOmViews(ctx, &om_cnt, &om_rtv0, &om_dsv);
            if (!rtv_info.valid && om_rtv0.valid) {
                rtv_info = om_rtv0;
            }
            if (!dsv_info.valid && om_dsv.valid) {
                dsv_info = om_dsv;
                dsv_src = "OM";
            }
        }
        SafeRelease(ctx);

        uint64_t tkey = 14695981039346656037ULL;
        tkey = Fnv1aHashCStr(tkey, frame.rt_name);
        tkey = Fnv1aHashCStr(tkey, sm_str.c_str());
        tkey = Fnv1aHashBytes(tkey, &vp_l, sizeof(vp_l));
        tkey = Fnv1aHashBytes(tkey, &vp_t, sizeof(vp_t));
        tkey = Fnv1aHashBytes(tkey, &vp_w, sizeof(vp_w));
        tkey = Fnv1aHashBytes(tkey, &vp_h, sizeof(vp_h));
        tkey = Fnv1aHashBytes(tkey, &is_persp, sizeof(is_persp));
        tkey = Fnv1aHashBytes(tkey, &overlays_en, sizeof(overlays_en));
        tkey = Fnv1aHashBytes(tkey, &rtv_info.tex_format, sizeof(rtv_info.tex_format));
        tkey = Fnv1aHashBytes(tkey, &dsv_info.tex_format, sizeof(dsv_info.tex_format));
        tkey = Fnv1aHashBytes(tkey, &rtv_info.sample_count, sizeof(rtv_info.sample_count));
        frame.target_key = tkey;

        RecordTargetSurvey(
            frame,
            sm_str.c_str(),
            cam_str.c_str(),
            vp_l,
            vp_t,
            clear_every,
            overlays_en,
            shadows_en,
            show_overlays,
            frame.near_clip,
            frame.far_clip,
            proj,
            rtv_info,
            dsv_info,
            dsv_src);
    }

    struct SceneStackGuard {
        bool prev_particle_ready = false;
        explicit SceneStackGuard(bool prev) : prev_particle_ready(prev) {}
        ~SceneStackGuard() {
            bool exited_world_depth = false;
            if (g_scene_depth > 0) {
                --g_scene_depth;
                exited_world_depth = g_scene_stack[g_scene_depth].particle_depth_ready;
                g_scene_stack[g_scene_depth].particle_depth_ready = false;
            }
            if (exited_world_depth) {
                ResetRouteCParticleDepthSnapshot();
            }
            g_world_depth_ready_for_particles.store(prev_particle_ready, std::memory_order_release);
        }
    } stack_guard(g_world_depth_ready_for_particles.exchange(false, std::memory_order_acq_rel));

    g_orig_render_scene(sm, cam, vp, show_overlays);
}

uint8_t __fastcall HookFireRenderQueueStarted(void* sm, uint8_t queue_group_id, const std::string& invocation) {
    g_queue_started_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_scene_depth > 0) {
        ActiveSceneFrame& frame = g_scene_stack[g_scene_depth - 1];
        if (frame.sm == sm) {
            // Fallback trigger if queue 86 was absent in an empty-entity scene: inject right before q>86 starts.
            if (queue_group_id > 86 && queue_group_id < 95) {
                TryDispatchAoForFrame(frame, "Started>86", queue_group_id);
            }
            if (queue_group_id >= 95) {
                if (frame.particle_depth_ready) {
                    frame.particle_depth_ready = false;
                    g_world_depth_ready_for_particles.store(false, std::memory_order_release);
                    ResetRouteCParticleDepthSnapshot();
                }
                TryDispatchP2FilterFallbackForFrame(frame, queue_group_id);
            }
            if (g_c0_survey_active.load(std::memory_order_relaxed)) {
                frame.queue = {};
                frame.queue.active = true;
                frame.queue.queue_id = queue_group_id;
                CopyTruncatedStr(frame.queue.invocation, sizeof(frame.queue.invocation), invocation);
            } else if (frame.ao_eligible) {
                frame.queue.queue_id = queue_group_id;
            }
        }
    }
    return g_orig_queue_started ? g_orig_queue_started(sm, queue_group_id, invocation) : 1;
}

uint8_t __fastcall HookFireRenderQueueEnded(void* sm, uint8_t queue_group_id, const std::string& invocation) {
    g_queue_ended_calls.fetch_add(1, std::memory_order_relaxed);
    if (g_scene_depth > 0) {
        ActiveSceneFrame& frame = g_scene_stack[g_scene_depth - 1];
        if (frame.sm == sm) {
            if (queue_group_id == 86) {
                TryDispatchAoForFrame(frame, "Ended86", queue_group_id);
            }
            if (g_c0_survey_active.load(std::memory_order_relaxed)) {
                if (!frame.queue.active || frame.queue.queue_id != queue_group_id) {
                    frame.queue.queue_id = queue_group_id;
                    CopyTruncatedStr(frame.queue.invocation, sizeof(frame.queue.invocation), invocation);
                }

                ID3D11DeviceContext* ctx = nullptr;
                ID3D11RenderTargetView* rt_rtv = nullptr;
                ID3D11DepthStencilView* rt_dsv = nullptr;
                uint32_t om_rtv_count = 0;
                D3DResourceInfo om_rtv0{};
                D3DResourceInfo om_dsv{};
                if (TryGetContextFromRenderTarget(frame.rt, &ctx, &rt_rtv, &rt_dsv)) {
                    QueryActiveOmViews(ctx, &om_rtv_count, &om_rtv0, &om_dsv);
                    SafeRelease(ctx);
                }
                RecordQueueSurvey(frame, frame.queue, om_rtv_count, om_rtv0, om_dsv);
                frame.queue.active = false;
            }
        }
    }
    return g_orig_queue_ended ? g_orig_queue_ended(sm, queue_group_id, invocation) : 0;
}

}  // namespace

void OnRouteCScenePassObserved(
    const char* pass_name,
    bool lighting,
    bool depth_write,
    bool transparent,
    bool alpha_reject,
    bool has_vp,
    bool has_fp) {
    if (g_scene_depth <= 0) {
        return;
    }
    ActiveSceneFrame& frame = g_scene_stack[g_scene_depth - 1];
    if (frame.ao_eligible && !frame.ao_injected &&
        !has_fp && !transparent && depth_write &&
        frame.queue.queue_id <= 86) {
        ++frame.world_geom_draws;
    }
    if (!g_c0_survey_active.load(std::memory_order_relaxed) || !frame.queue.active) {
        return;
    }
    ActiveQueueCounters& q = frame.queue;
    ++q.setpass_calls;
    if (has_vp) {
        ++q.has_vp;
    }
    if (has_fp) {
        ++q.has_fp;
    }
    if (!transparent && depth_write && !alpha_reject) {
        ++q.solid_dw;
    } else if (!transparent && alpha_reject) {
        ++q.cutout_dw;
    } else if (transparent && !depth_write) {
        ++q.trans_nodw;
    } else if (transparent && depth_write) {
        ++q.trans_dw;
    } else if (!lighting && !depth_write) {
        ++q.unlit_nodw;
    }
    AppendUniqueSamplePass(q.sample_passes, sizeof(q.sample_passes), pass_name);
}

bool InstallRouteCAoPipeline(void* ogre_main_base, RouteCLogFn logger) {
    g_log = logger;
    if (g_hooked.load(std::memory_order_relaxed)) {
        return true;
    }
    if (EnvFlagIsOne("TL2_C_NO_HOOKS")) {
        if (g_log) {
            g_log("[RouteC] Hooks disabled via TL2_C_NO_HOOKS=1.");
        }
        return false;
    }
    if (!ogre_main_base) {
        return false;
    }

    auto* ogre_mod = static_cast<HMODULE>(ogre_main_base);
    if (!InitOgreC0Api(ogre_mod)) {
        if (g_log) {
            g_log("[RouteC][C0] Failed resolving required OgreMain exports; Route C disabled.");
        }
        return false;
    }

    FARPROC p_render_scene = ::GetProcAddress(ogre_mod, "?_renderScene@SceneManager@Ogre@@UEAAXPEAVCamera@2@PEAVViewport@2@_N@Z");
    FARPROC p_q_start = ::GetProcAddress(ogre_mod, "?fireRenderQueueStarted@SceneManager@Ogre@@MEAA_NEAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    FARPROC p_q_end = ::GetProcAddress(ogre_mod, "?fireRenderQueueEnded@SceneManager@Ogre@@MEAA_NEAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z");
    if (!p_render_scene || !p_q_start || !p_q_end) {
        if (g_log) {
            g_log("[RouteC][C0] Missing hook target exports in OgreMain.dll.");
        }
        return false;
    }

    void* orig_rs = nullptr;
    if (!g_detour_render_scene.Install(
            reinterpret_cast<void*>(p_render_scene),
            kExpectedRenderScene,
            sizeof(kExpectedRenderScene),
            reinterpret_cast<void*>(&HookRenderScene),
            &orig_rs)) {
        if (g_log) {
            g_log("[RouteC][C0] FAILED detour on Ogre::SceneManager::_renderScene.");
        }
        return false;
    }
    g_orig_render_scene = reinterpret_cast<RenderSceneFn>(orig_rs);

    void* orig_qs = nullptr;
    if (!g_detour_queue_started.Install(
            reinterpret_cast<void*>(p_q_start),
            kExpectedFireQueue,
            sizeof(kExpectedFireQueue),
            reinterpret_cast<void*>(&HookFireRenderQueueStarted),
            &orig_qs)) {
        if (g_log) {
            g_log("[RouteC][C0] FAILED detour on Ogre::SceneManager::fireRenderQueueStarted.");
        }
        g_detour_render_scene.Remove();
        g_orig_render_scene = nullptr;
        return false;
    }
    g_orig_queue_started = reinterpret_cast<FireQueueFn>(orig_qs);

    void* orig_qe = nullptr;
    if (!g_detour_queue_ended.Install(
            reinterpret_cast<void*>(p_q_end),
            kExpectedFireQueue,
            sizeof(kExpectedFireQueue),
            reinterpret_cast<void*>(&HookFireRenderQueueEnded),
            &orig_qe)) {
        if (g_log) {
            g_log("[RouteC][C0] FAILED detour on Ogre::SceneManager::fireRenderQueueEnded.");
        }
        g_detour_queue_started.Remove();
        g_detour_render_scene.Remove();
        g_orig_queue_started = nullptr;
        g_orig_render_scene = nullptr;
        return false;
    }
    g_orig_queue_ended = reinterpret_cast<FireQueueFn>(orig_qe);

    const bool survey_on = EnvFlagIsOne("TL2_C_C0_SURVEY");
    const bool ao_init_on = !EnvFlagIsOne("TL2_C_DISABLE_AO");
    const int dbg_mode = ResolveAoDebugMode(0);  // C2 default: DebugMode=0 (Live AO)
    g_c0_survey_active.store(survey_on, std::memory_order_relaxed);
    g_ao_runtime_enabled.store(ao_init_on, std::memory_order_relaxed);
    g_sscs_runtime_enabled.store(!EnvFlagIsZero("TL2_P0_SSCS"), std::memory_order_relaxed);
    g_p1_soft_particles_enabled.store(!EnvFlagIsZero("TL2_P1_SOFT_PARTICLES"), std::memory_order_relaxed);
    g_world_depth_ready_for_particles.store(false, std::memory_order_relaxed);
    g_ao_debug_mode.store(dbg_mode, std::memory_order_relaxed);
    g_hooked.store(true, std::memory_order_relaxed);

    LoadVisualConfigAndPresets();

    if (g_log) {
        const int p2_preset = GetP2FilterPreset();
        g_log("[RouteC][C2] Hooks installed (ao=%s, sscs=%s, p1SoftPart=%s, debugMode=%d, p2Filter=%s, f11=%s, p0rim=%s, c0_survey=%s, injectQueue=86, hotkey=F8:ConfigPanel).",
              IsRouteCAoRuntimeEnabled() ? "ON" : "OFF",
              IsRouteCSscsRuntimeEnabled() ? "ON" : "OFF",
              IsP1SoftParticlesEnabled() ? "ON" : "OFF",
              dbg_mode,
              P2FilterPresetName(p2_preset),
              IsF11VisualEnhanceEnabled() ? "ON" : "OFF",
              IsP0RimLitEnhanceEnabled() ? "ON" : "OFF",
              survey_on ? "ON" : "OFF");
    }
    return true;
}

void RemoveRouteCAoPipeline() {
    g_hooked.store(false, std::memory_order_relaxed);
    g_c0_survey_active.store(false, std::memory_order_relaxed);

    g_toast_expire_tick_ms.store(0, std::memory_order_relaxed);
    ShutdownConfigPanel();
    RemoveRenderWindowSwapBuffersHook();
    g_detour_queue_ended.Remove();
    g_detour_queue_started.Remove();
    g_detour_render_scene.Remove();
    ShutdownRouteCD3D11Resources();

    g_orig_queue_ended = nullptr;
    g_orig_queue_started = nullptr;
    g_orig_render_scene = nullptr;
}

bool VerifyRouteCShaders() {
    return VerifyRouteCShadersCompile(g_log);
}

void SetRouteCAoRuntimeEnabled(bool enabled) {
    g_ao_runtime_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsRouteCAoRuntimeEnabled() {
    return g_ao_runtime_enabled.load(std::memory_order_relaxed);
}

bool ToggleRouteCAoRuntimeEnabled() {
    bool expected = g_ao_runtime_enabled.load(std::memory_order_relaxed);
    while (!g_ao_runtime_enabled.compare_exchange_weak(expected, !expected, std::memory_order_relaxed)) {
    }
    return !expected;
}

void SetRouteCSscsRuntimeEnabled(bool enabled) {
    g_sscs_runtime_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsRouteCSscsRuntimeEnabled() {
    return g_sscs_runtime_enabled.load(std::memory_order_relaxed);
}

bool ToggleRouteCSscsRuntimeEnabled() {
    bool expected = g_sscs_runtime_enabled.load(std::memory_order_relaxed);
    while (!g_sscs_runtime_enabled.compare_exchange_weak(expected, !expected, std::memory_order_relaxed)) {
    }
    return !expected;
}

void SetP1SoftParticlesEnabled(bool enabled) {
    g_p1_soft_particles_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsP1SoftParticlesEnabled() {
    return g_p1_soft_particles_enabled.load(std::memory_order_relaxed);
}

bool ToggleP1SoftParticlesEnabled() {
    bool expected = g_p1_soft_particles_enabled.load(std::memory_order_relaxed);
    while (!g_p1_soft_particles_enabled.compare_exchange_weak(expected, !expected, std::memory_order_relaxed)) {
    }
    return !expected;
}

bool IsRouteCWorldDepthReadyForParticles() {
    return g_world_depth_ready_for_particles.load(std::memory_order_acquire) &&
           g_p1_soft_particles_enabled.load(std::memory_order_relaxed);
}

void TriggerRouteCToastForTest() {
    g_toast_expire_tick_ms.store(::GetTickCount64() + 2000ULL, std::memory_order_relaxed);
}

RouteCStats GetRouteCAoStats() {
    RouteCStats s{};
    s.render_scene_calls = g_render_scene_calls.load(std::memory_order_relaxed);
    s.perspective_world_candidates = g_world_candidates.load(std::memory_order_relaxed);
    s.queue_started_calls = g_queue_started_calls.load(std::memory_order_relaxed);
    s.queue_ended_calls = g_queue_ended_calls.load(std::memory_order_relaxed);
    ::AcquireSRWLockShared(&g_survey_lock);
    s.survey_unique_targets = g_target_record_count;
    s.survey_unique_queues = g_queue_record_count;
    ::ReleaseSRWLockShared(&g_survey_lock);
    s.hooked = g_hooked.load(std::memory_order_relaxed);
    s.c0_survey_active = g_c0_survey_active.load(std::memory_order_relaxed);
    return s;
}

void LogRouteCSurveySummary(RouteCLogFn logger) {
    if (!logger) {
        return;
    }
    const auto st = GetRouteCAoStats();
    logger(
        "[RouteC][C0][Stats] hooked=%d survey=%d renderScene=%llu worldCand=%llu qStart=%llu qEnd=%llu uniqTargets=%llu uniqQueues=%llu",
        st.hooked ? 1 : 0,
        st.c0_survey_active ? 1 : 0,
        static_cast<unsigned long long>(st.render_scene_calls),
        static_cast<unsigned long long>(st.perspective_world_candidates),
        static_cast<unsigned long long>(st.queue_started_calls),
        static_cast<unsigned long long>(st.queue_ended_calls),
        static_cast<unsigned long long>(st.survey_unique_targets),
        static_cast<unsigned long long>(st.survey_unique_queues));

    const auto d3d_st = GetRouteCD3DStats();
    logger(
        "[RouteC][C2][Stats] debugMode=%d ok=%llu skipNoOm=%llu skipFmt=%llu skipFail=%llu depthRebuilds=%u lastTex=%ux%u rtvFmt=%u dsvTexFmt=%u msaa=%ux shadersReady=%d",
        g_ao_debug_mode.load(std::memory_order_relaxed),
        static_cast<unsigned long long>(d3d_st.inject_ok),
        static_cast<unsigned long long>(d3d_st.inject_skip_no_om),
        static_cast<unsigned long long>(d3d_st.inject_skip_fmt),
        static_cast<unsigned long long>(d3d_st.inject_skip_fail),
        d3d_st.depth_rebuilds,
        d3d_st.last_tex_w,
        d3d_st.last_tex_h,
        d3d_st.last_rtv_fmt,
        d3d_st.last_dsv_tex_fmt,
        d3d_st.last_sample_count,
        d3d_st.shaders_ready ? 1 : 0);
    if (d3d_st.bench_samples > 0) {
        logger(
            "[RouteC][C2][Bench] samples=%llu avgCopy=%.3fms avgAo=%.3fms avgBlurH=%.3fms avgBlurV=%.3fms avgComp=%.3fms avgTotal=%.3fms",
            static_cast<unsigned long long>(d3d_st.bench_samples),
            d3d_st.avg_copy_ms,
            d3d_st.avg_ao_ms,
            d3d_st.avg_blur_h_ms,
            d3d_st.avg_blur_v_ms,
            d3d_st.avg_comp_ms,
            d3d_st.avg_total_ms);
    }

    ::AcquireSRWLockShared(&g_survey_lock);
    for (size_t i = 0; i < g_target_record_count; ++i) {
        logger("[RouteC][C0][TargetSummary] hits=%llu %s",
               static_cast<unsigned long long>(g_target_records[i].hits),
               g_target_records[i].summary);
    }

    QueueSurveyRecord sorted[kMaxQueueRecords]{};
    const size_t qcnt = g_queue_record_count;
    for (size_t i = 0; i < qcnt; ++i) {
        sorted[i] = g_queue_records[i];
    }
    ::ReleaseSRWLockShared(&g_survey_lock);

    std::sort(sorted, sorted + qcnt, [](const QueueSurveyRecord& a, const QueueSurveyRecord& b) {
        const int cmp = std::strcmp(a.rt_name, b.rt_name);
        if (cmp != 0) {
            return cmp < 0;
        }
        if (a.vp_w != b.vp_w) {
            return a.vp_w > b.vp_w;
        }
        return a.queue_id < b.queue_id;
    });

    for (size_t i = 0; i < qcnt; ++i) {
        const QueueSurveyRecord& r = sorted[i];
        logger(
            "[RouteC][C0][QueueSummary] hits=%llu rt=\"%s\"(%dx%d,persp=%d) q=%u inv=\"%s\" "
            "setPass=%llu solidDW=%llu cutout=%llu transNoDW=%llu transDW=%llu unlitNoDW=%llu vp=%llu fp=%llu "
            "omRtv0=(fmt=%u,msaa=%u) omDsv=(texFmt=%u,dsvFmt=%u,bind=0x%x) passes=[%s]",
            static_cast<unsigned long long>(r.hits),
            r.rt_name,
            r.vp_w,
            r.vp_h,
            r.is_perspective ? 1 : 0,
            static_cast<unsigned>(r.queue_id),
            r.invocation,
            static_cast<unsigned long long>(r.total_setpass),
            static_cast<unsigned long long>(r.total_solid_dw),
            static_cast<unsigned long long>(r.total_cutout_dw),
            static_cast<unsigned long long>(r.total_trans_nodw),
            static_cast<unsigned long long>(r.total_trans_dw),
            static_cast<unsigned long long>(r.total_unlit_nodw),
            static_cast<unsigned long long>(r.total_vp),
            static_cast<unsigned long long>(r.total_fp),
            r.om_rtv0_fmt,
            r.om_rtv0_msaa,
            r.om_dsv_tex_fmt,
            r.om_dsv_fmt,
            r.om_dsv_bind,
            r.sample_passes);
    }
}

}  // namespace routeb
