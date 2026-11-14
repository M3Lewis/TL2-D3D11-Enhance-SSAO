#include "phase3_scene_pipeline.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#include "fixed_func_manual_uniforms.h"
#include "hlsl_cbuffer_fastpath.h"
#include "hook_utils.h"

namespace routeb {

namespace {

// Verified RVAs for Torchlight II 2026 x64 (TimeDateStamp 0x6af5fabf / 0x6af5faa0)
constexpr uintptr_t kRvaGetFixedFuncShaderSource = 0x352B0;
constexpr uintptr_t kRvaSetFixedFuncParams = 0x37ED0;
constexpr uintptr_t kRvaSceneMgrSetPass = 0x1903C0;

// Verified D3D11RenderSystem offsets
constexpr uintptr_t kRsOffBoundFragmentProgram = 0x0EF8;
constexpr size_t kRsVtblBindGpuProgram = 0x300 / sizeof(uintptr_t);            // 96
constexpr size_t kRsVtblBindGpuProgramParameters = 0x308 / sizeof(uintptr_t);  // 97

// Verified GpuProgram vtable offsets (from Ogre::SceneManager::_setPass & sub_180031180)
constexpr size_t kGpuProgVtblLoad = 120 / sizeof(uintptr_t);               // 15
constexpr size_t kGpuProgVtblBindingDelegate = 424 / sizeof(uintptr_t);    // 53
constexpr size_t kGpuProgVtblCreateParameters = 440 / sizeof(uintptr_t);   // 55

// Verified SceneManager offset (+72 -> mDestRenderSystem, +17457 -> mSuppressRenderStateChanges)
constexpr uintptr_t kSmOffDestRenderSystem = 72;
constexpr uintptr_t kSmOffSuppressRenderState = 17457;

constexpr uint8_t kExpectedSetPass[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr uint8_t kExpectedGetShaderSource[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57};
constexpr uint8_t kExpectedSetFfParams[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};

// Ogre ABI compatible POD layouts (MSVC x64 /MD)
struct OgreSharedPtr {
    void* vftable = nullptr;
    void* rep = nullptr;
    long* use_count = nullptr;
    int free_method = 0;
};
static_assert(sizeof(OgreSharedPtr) == 32, "Ogre::SharedPtr must be 32 bytes");

struct OgreColourValue {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct OgreLayerBlendModeEx {
    int blendType = 0;
    int operation = 0;
};

enum OgreCompareFunction : int {
    kCmpAlwaysFail = 0,
    kCmpAlwaysPass = 1,
};

enum OgreTextureType : int {
    kTexType2D = 2,
    kTexTypeCubeMap = 4,
};

enum class PassArchetype {
    None = 0,
    DiffPassOpaque,
    DiffPassAlpha,
    SkinnedDiffOpaque,
    SkinnedDiffAlpha,
    LightPass,
    RimLit2D,
    RimLitCube,
};

struct ExplicitProgramSlot {
    const char* program_name = nullptr;
    OgreSharedPtr resource_sp{};
    OgreSharedPtr params_sp{};
    uintptr_t delegate = 0;
    bool attempted = false;
    bool ready = false;
};

struct ActiveExplicitBinding {
    uintptr_t rs = 0;
    uintptr_t ps_delegate = 0;
    const OgreSharedPtr* params_sp = nullptr;
    uintptr_t params_raw = 0;
    bool is_fixed_func_vs = false;
};

// Function pointer typedefs
using SceneMgrSetPassFn = const void*(__fastcall*)(uintptr_t sm, const void* pass, bool even_if_suppressed, bool shadow_deriv);
using GetFfShaderSourceFn = void*(__fastcall*)(uintptr_t self, std::string* out_src, const void* ve, const void* fe, const void* vdecl, const void* state);
using SetFfProgramsParamsFn = void(__fastcall*)(uintptr_t ff_progs, uintptr_t ff_state);

using GetSingletonPtrFn = uintptr_t(__cdecl*)();
using GetByNameFn = void(__fastcall*)(uintptr_t mgr, OgreSharedPtr* out_sp, const std::string& name, bool is_manual);
using CreateSharedParamsFn = void(__fastcall*)(uintptr_t mgr, OgreSharedPtr* out_sp, const std::string& name);
using GetSharedParamsFn = void(__fastcall*)(uintptr_t mgr, OgreSharedPtr* out_sp, const std::string& name);
using AddSharedConstDefFn = void(__fastcall*)(uintptr_t shared_params, const std::string& name, int const_type, size_t array_size);

using PassGetStringFn = const std::string&(__fastcall*)(const void* pass);
using PassGetBoolFn = bool(__fastcall*)(const void* pass);
using PassGetU16Fn = uint16_t(__fastcall*)(const void* pass);
using PassGetTusFn = const void*(__fastcall*)(const void* pass, uint16_t index);
using PassGetIntFn = int(__fastcall*)(const void* pass);
using PassGetU8Fn = uint8_t(__fastcall*)(const void* pass);
using PassGetFloatFn = float(__fastcall*)(const void* pass);
using PassGetColourFn = const OgreColourValue&(__fastcall*)(const void* pass);

using SmGetIntFn = int(__fastcall*)(uintptr_t sm);
using SmGetFloatFn = float(__fastcall*)(uintptr_t sm);
using SmGetColourFn = const OgreColourValue&(__fastcall*)(uintptr_t sm);

using TusGetIntFn = int(__fastcall*)(const void* tus);
using TusGetU32Fn = uint32_t(__fastcall*)(const void* tus);
using TusGetBlendModeFn = const OgreLayerBlendModeEx&(__fastcall*)(const void* tus);

struct OgreApiTable {
    GetSingletonPtrFn hl_mgr_singleton = nullptr;
    GetSingletonPtrFn gpu_mgr_singleton = nullptr;
    GetByNameFn gpu_mgr_get_by_name = nullptr;
    CreateSharedParamsFn create_shared_params = nullptr;
    GetSharedParamsFn get_shared_params = nullptr;
    AddSharedConstDefFn add_shared_const_def = nullptr;

    PassGetStringFn pass_get_name = nullptr;
    PassGetBoolFn pass_has_vp = nullptr;
    PassGetBoolFn pass_has_fp = nullptr;
    PassGetStringFn pass_get_vp_name = nullptr;
    PassGetU16Fn pass_get_num_tus = nullptr;
    PassGetTusFn pass_get_tus = nullptr;
    PassGetIntFn pass_get_alpha_reject_func = nullptr;
    PassGetU8Fn pass_get_alpha_reject_val = nullptr;
    PassGetBoolFn pass_get_fog_override = nullptr;
    PassGetIntFn pass_get_fog_mode = nullptr;
    PassGetColourFn pass_get_fog_colour = nullptr;
    PassGetFloatFn pass_get_fog_start = nullptr;
    PassGetFloatFn pass_get_fog_end = nullptr;
    PassGetFloatFn pass_get_fog_density = nullptr;

    SmGetIntFn sm_get_fog_mode = nullptr;
    SmGetColourFn sm_get_fog_colour = nullptr;
    SmGetFloatFn sm_get_fog_start = nullptr;
    SmGetFloatFn sm_get_fog_end = nullptr;
    SmGetFloatFn sm_get_fog_density = nullptr;

    TusGetIntFn tus_get_tex_type = nullptr;
    TusGetU32Fn tus_get_coord_set = nullptr;
    TusGetBlendModeFn tus_get_colour_blend = nullptr;
};

struct Phase3Toggles {
    bool diff_pass = true;
    bool skinned_diff = true;
    bool light_pass = true;
    bool rim_lit = true;
};

OgreApiTable g_api{};
Phase3Toggles g_toggles{};
Phase3LogFn g_log = nullptr;

Detour64 g_detour_set_pass;
Detour64 g_detour_get_shader_src;
Detour64 g_detour_set_ff_params;

SceneMgrSetPassFn g_orig_set_pass = nullptr;
GetFfShaderSourceFn g_orig_get_shader_src = nullptr;
SetFfProgramsParamsFn g_orig_set_ff_params = nullptr;

OgreSharedPtr g_shared_per_frame_sp{};
bool g_shared_params_init_done = false;
bool g_shared_params_ready = false;

ExplicitProgramSlot g_slots[8] = {
    {},
    {"TL2/Phase3/DiffPassOpaquePS"},
    {"TL2/Phase3/DiffPassAlphaPS"},
    {"TL2/Phase3/SkinnedDiffOpaquePS"},
    {"TL2/Phase3/SkinnedDiffAlphaPS"},
    {"TL2/Phase3/LightPassPS"},
    {"TL2/Phase3/RimLit2DPS"},
    {"TL2/Phase3/RimLitCubePS"},
};

ActiveExplicitBinding g_active_binding{};

std::atomic<uint64_t> g_setpass_calls{0};
std::atomic<uint64_t> g_diffpass_opaque_binds{0};
std::atomic<uint64_t> g_diffpass_alpha_binds{0};
std::atomic<uint64_t> g_skinned_diff_binds{0};
std::atomic<uint64_t> g_lightpass_binds{0};
std::atomic<uint64_t> g_rimlit_2d_binds{0};
std::atomic<uint64_t> g_rimlit_cube_binds{0};
std::atomic<uint64_t> g_vs_texcoord_injections{0};
std::atomic<uint64_t> g_ff_explicit_syncs{0};
std::atomic<bool> g_hooked{false};

void ReleaseOgreSharedPtr(OgreSharedPtr& sp) {
    if (sp.use_count && ::InterlockedDecrement(sp.use_count) == 0) {
        auto** vtbl = static_cast<void(**)(void*)>(sp.vftable);
        if (vtbl && vtbl[1]) {
            vtbl[1](&sp);
        }
    }
    sp = {};
}

bool EnvFlagIsOne(const char* name) {
    char buf[8] = {};
    const DWORD len = ::GetEnvironmentVariableA(name, buf, static_cast<DWORD>(sizeof(buf)));
    return len > 0 && buf[0] == '1';
}

bool ContainsAsciiInsensitive(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }
    auto it = std::search(
        haystack.begin(),
        haystack.end(),
        needle.begin(),
        needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

int MapOgreBlendOpToShader(int ogre_lbx_op) {
    // Ogre LayerBlendOperationEx: LBX_MODULATE=2, LBX_MODULATE_X2=3, LBX_ADD=5, LBX_BLEND_*_ALPHA=9..11
    switch (ogre_lbx_op) {
        case 2:
            return 1;  // modulate
        case 5:
            return 2;  // add
        case 9:
        case 10:
        case 11:
            return 3;  // alpha blend
        case 3:
            return 4;  // modulate x2
        default:
            return 0;
    }
}

void EnsureSharedPerFrameLighting() {
    if (g_shared_params_init_done) {
        return;
    }
    if (!g_api.gpu_mgr_singleton || !g_api.create_shared_params || !g_api.get_shared_params || !g_api.add_shared_const_def) {
        g_shared_params_init_done = true;
        return;
    }
    const uintptr_t mgr = g_api.gpu_mgr_singleton();
    if (!mgr) {
        return;
    }

    g_shared_params_init_done = true;
    const std::string set_name = "TL2PerFrameLighting";
    bool newly_created = false;
    try {
        g_api.create_shared_params(mgr, &g_shared_per_frame_sp, set_name);
        newly_created = (g_shared_per_frame_sp.rep != nullptr);
    } catch (const std::exception&) {
        g_shared_per_frame_sp = {};
    } catch (...) {
        g_shared_per_frame_sp = {};
    }

    if (!g_shared_per_frame_sp.rep) {
        try {
            g_api.get_shared_params(mgr, &g_shared_per_frame_sp, set_name);
        } catch (const std::exception& ex) {
            g_shared_per_frame_sp = {};
            if (g_log) {
                g_log("[RouteB][Phase3] getSharedParameters fallback failed: %s", ex.what());
            }
        } catch (...) {
            g_shared_per_frame_sp = {};
        }
    }

    if (g_shared_per_frame_sp.rep) {
        const uintptr_t sp_ptr = reinterpret_cast<uintptr_t>(g_shared_per_frame_sp.rep);
        const std::string kSunDir = "tl2SunViewDir";
        const std::string kSunCol = "tl2SunColor";
        const std::string kAmbCol = "tl2AmbientColor";
        const std::string kEnvParams = "tl2Phase3Env";
        if (newly_created) {
            // GCT_FLOAT4 = 4
            for (const std::string* param_name : {&kSunDir, &kSunCol, &kAmbCol, &kEnvParams}) {
                try {
                    g_api.add_shared_const_def(sp_ptr, *param_name, 4, 1);
                } catch (const std::exception&) {
                } catch (...) {
                }
            }
        }
        g_shared_params_ready = true;
        if (g_log) {
            g_log("[RouteB][Phase3] Registered Ogre GpuSharedParameters 'TL2PerFrameLighting' (created=%d)",
                  newly_created ? 1 : 0);
        }
    }
}

bool EnsureSlotReady(PassArchetype arch) {
    const size_t idx = static_cast<size_t>(arch);
    if (idx == 0 || idx >= std::size(g_slots)) {
        return false;
    }
    ExplicitProgramSlot& slot = g_slots[idx];
    if (slot.ready) {
        return true;
    }
    if (slot.attempted || !slot.program_name || !g_api.gpu_mgr_singleton || !g_api.gpu_mgr_get_by_name) {
        return false;
    }
    const uintptr_t mgr = g_api.gpu_mgr_singleton();
    if (!mgr) {
        return false;
    }

    slot.attempted = true;
    try {
        const std::string prog_name(slot.program_name);
        g_api.gpu_mgr_get_by_name(mgr, &slot.resource_sp, prog_name, true);
        if (!slot.resource_sp.rep) {
            if (g_log) {
                g_log("[RouteB][Phase3] Program '%s' not found in GpuProgramManager", slot.program_name);
            }
            return false;
        }

        auto* prog_vtbl = *reinterpret_cast<uintptr_t**>(slot.resource_sp.rep);
        using LoadFn = void(__fastcall*)(void*, bool);
        using GetDelegateFn = uintptr_t(__fastcall*)(void*);
        using CreateParamsFn = void(__fastcall*)(void*, OgreSharedPtr*);

        reinterpret_cast<LoadFn>(prog_vtbl[kGpuProgVtblLoad])(slot.resource_sp.rep, false);
        slot.delegate = reinterpret_cast<GetDelegateFn>(prog_vtbl[kGpuProgVtblBindingDelegate])(slot.resource_sp.rep);
        reinterpret_cast<CreateParamsFn>(prog_vtbl[kGpuProgVtblCreateParameters])(slot.resource_sp.rep, &slot.params_sp);

        slot.ready = (slot.delegate != 0 && slot.params_sp.rep != nullptr);
        if (g_log) {
            g_log("[RouteB][Phase3] Loaded explicit SM4.0 shader '%s' (ready=%d)", slot.program_name, slot.ready ? 1 : 0);
        }
    } catch (const std::exception& ex) {
        slot.ready = false;
        if (g_log) {
            g_log("[RouteB][Phase3] Exception loading '%s': %s", slot.program_name, ex.what());
        }
    } catch (...) {
        slot.ready = false;
    }
    return slot.ready;
}

PassArchetype ClassifyPass(
    const void* pass,
    float out_flags[4],
    bool* out_is_fixed_func_vs) {
    out_flags[0] = 0.0f;
    out_flags[1] = 1.0f;
    out_flags[2] = 0.0f;
    out_flags[3] = 1.0f;
    *out_is_fixed_func_vs = false;

    if (!pass || !g_api.pass_has_fp || g_api.pass_has_fp(pass)) {
        return PassArchetype::None;
    }

    const std::string& pass_name = g_api.pass_get_name(pass);
    if (ContainsAsciiInsensitive(pass_name, "Behind") ||
        ContainsAsciiInsensitive(pass_name, "Shadow") ||
        ContainsAsciiInsensitive(pass_name, "Caster")) {
        return PassArchetype::None;
    }

    const uint16_t num_tus = g_api.pass_get_num_tus(pass);
    const void* tus1 = (num_tus >= 2) ? g_api.pass_get_tus(pass, 1) : nullptr;
    const int tex1_type = (tus1 && g_api.tus_get_tex_type) ? g_api.tus_get_tex_type(tus1) : kTexType2D;

    const int reject_func = g_api.pass_get_alpha_reject_func(pass);
    const bool has_alpha_reject = (reject_func != kCmpAlwaysPass);
    out_flags[1] = static_cast<float>(reject_func);

    if (tus1 && g_api.tus_get_colour_blend && tex1_type != kTexTypeCubeMap) {
        const auto& blend = g_api.tus_get_colour_blend(tus1);
        out_flags[0] = static_cast<float>(MapOgreBlendOpToShader(blend.operation));
        const uint32_t coord_set = g_api.tus_get_coord_set ? g_api.tus_get_coord_set(tus1) : 1;
        out_flags[3] = static_cast<float>((coord_set >= 2) ? 2 : coord_set);
    }

    if (g_api.pass_has_vp(pass)) {
        const std::string& vp_name = g_api.pass_get_vp_name(pass);
        if (ContainsAsciiInsensitive(vp_name, "Behind") ||
            ContainsAsciiInsensitive(vp_name, "Shadow") ||
            ContainsAsciiInsensitive(vp_name, "Caster")) {
            return PassArchetype::None;
        }
        if (vp_name.find("HardwareSkinning") != std::string::npos) {
            if (tex1_type == kTexTypeCubeMap) {
                if (!g_toggles.rim_lit) {
                    return PassArchetype::None;
                }
                if (tus1 && g_api.tus_get_colour_blend) {
                    const auto& blend = g_api.tus_get_colour_blend(tus1);
                    const int mapped = MapOgreBlendOpToShader(blend.operation);
                    out_flags[0] = static_cast<float>(mapped > 0 ? mapped : 2);
                }
                return PassArchetype::RimLitCube;
            }
            if (!g_toggles.skinned_diff) {
                return PassArchetype::None;
            }
            if (vp_name.find("Secondary") != std::string::npos && vp_name.find("Enviro") == std::string::npos) {
                out_flags[3] = 2.0f;
            } else if (num_tus >= 2) {
                out_flags[3] = 1.0f;
            }
            return has_alpha_reject ? PassArchetype::SkinnedDiffAlpha : PassArchetype::SkinnedDiffOpaque;
        }
        return PassArchetype::None;
    }

    *out_is_fixed_func_vs = true;

    if (pass_name == "RIMLIT") {
        if (!g_toggles.rim_lit || !tus1) {
            return PassArchetype::None;
        }
        if (g_api.tus_get_colour_blend) {
            const auto& blend = g_api.tus_get_colour_blend(tus1);
            const int mapped = MapOgreBlendOpToShader(blend.operation);
            out_flags[0] = static_cast<float>(mapped > 0 ? mapped : 2);
        } else {
            out_flags[0] = 2.0f;
        }
        return (tex1_type == kTexTypeCubeMap) ? PassArchetype::RimLitCube : PassArchetype::RimLit2D;
    }

    if (pass_name == "LIGHTPASS") {
        return g_toggles.light_pass ? PassArchetype::LightPass : PassArchetype::None;
    }

    if (pass_name == "DIFFPASS") {
        if (!g_toggles.diff_pass) {
            return PassArchetype::None;
        }
        return has_alpha_reject ? PassArchetype::DiffPassAlpha : PassArchetype::DiffPassOpaque;
    }

    return PassArchetype::None;
}

void ApplyPassFogAndAlphaToExplicitPs(uintptr_t scene_mgr, const void* pass, uintptr_t ps_params, float flags[4]) {
    int fog_mode = 0;
    OgreColourValue fog_color{};
    float fog_start = 0.0f;
    float fog_end = 1.0f;
    float fog_density = 0.0f;

    if (g_api.pass_get_fog_override && g_api.pass_get_fog_override(pass)) {
        fog_mode = g_api.pass_get_fog_mode(pass);
        fog_color = g_api.pass_get_fog_colour(pass);
        fog_start = g_api.pass_get_fog_start(pass);
        fog_end = g_api.pass_get_fog_end(pass);
        fog_density = g_api.pass_get_fog_density(pass);
    } else if (g_api.sm_get_fog_mode) {
        fog_mode = g_api.sm_get_fog_mode(scene_mgr);
        fog_color = g_api.sm_get_fog_colour(scene_mgr);
        fog_start = g_api.sm_get_fog_start(scene_mgr);
        fog_end = g_api.sm_get_fog_end(scene_mgr);
        fog_density = g_api.sm_get_fog_density(scene_mgr);
    }

    flags[2] = static_cast<float>(fog_mode);
    WriteExplicitPsNamedFloat4(ps_params, "p3Flags", flags);

    const float fc[4] = {fog_color.r, fog_color.g, fog_color.b, fog_color.a};
    WriteExplicitPsNamedFloat4(ps_params, "FogColor", fc);
    WriteExplicitPsNamedFloat(ps_params, "FogStart", fog_start);
    WriteExplicitPsNamedFloat(ps_params, "FogEnd", fog_end);
    WriteExplicitPsNamedFloat(ps_params, "FogDensity", fog_density);

    const float alpha_val = static_cast<float>(g_api.pass_get_alpha_reject_val(pass)) / 255.0f;
    WriteExplicitPsNamedFloat(ps_params, "AlphaRejectValue", alpha_val);
}

void BindExplicitPsParametersByValue(uintptr_t rs, const OgreSharedPtr& params_sp) {
    if (!rs || !params_sp.rep) {
        return;
    }
    // D3D11RenderSystem::bindGpuProgramParameters takes GpuProgramParametersSharedPtr by value;
    // MSVC x64 ABI passes the caller's copy by address and destroys it inside the callee.
    OgreSharedPtr by_val_copy = params_sp;
    if (by_val_copy.use_count) {
        ::InterlockedIncrement(by_val_copy.use_count);
    }
    auto* rs_vtbl = *reinterpret_cast<uintptr_t**>(rs);
    using BindParamsFn = void(__fastcall*)(uintptr_t, int, OgreSharedPtr*, uint16_t);
    reinterpret_cast<BindParamsFn>(rs_vtbl[kRsVtblBindGpuProgramParameters])(
        rs, 1 /*GPT_FRAGMENT_PROGRAM*/, &by_val_copy, 0xFFFF);
}

void BindExplicitPsToRenderSystem(uintptr_t rs, uintptr_t ps_delegate, const OgreSharedPtr* params_sp) {
    if (!rs || !params_sp) {
        return;
    }
    auto* rs_vtbl = *reinterpret_cast<uintptr_t**>(rs);
    using BindProgFn = void(__fastcall*)(uintptr_t, uintptr_t);
    reinterpret_cast<BindProgFn>(rs_vtbl[kRsVtblBindGpuProgram])(rs, ps_delegate);
    BindExplicitPsParametersByValue(rs, *params_sp);
}

void RecordBindStat(PassArchetype arch) {
    switch (arch) {
        case PassArchetype::DiffPassOpaque:
            g_diffpass_opaque_binds.fetch_add(1, std::memory_order_relaxed);
            break;
        case PassArchetype::DiffPassAlpha:
            g_diffpass_alpha_binds.fetch_add(1, std::memory_order_relaxed);
            break;
        case PassArchetype::SkinnedDiffOpaque:
        case PassArchetype::SkinnedDiffAlpha:
            g_skinned_diff_binds.fetch_add(1, std::memory_order_relaxed);
            break;
        case PassArchetype::LightPass:
            g_lightpass_binds.fetch_add(1, std::memory_order_relaxed);
            break;
        case PassArchetype::RimLit2D:
            g_rimlit_2d_binds.fetch_add(1, std::memory_order_relaxed);
            break;
        case PassArchetype::RimLitCube:
            g_rimlit_cube_binds.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            break;
    }
}

const void* __fastcall HookSceneManagerSetPass(
    uintptr_t scene_mgr,
    const void* pass,
    bool even_if_suppressed,
    bool shadow_derivation) {
    g_setpass_calls.fetch_add(1, std::memory_order_relaxed);
    SetActiveExplicitPsForwardTarget(0);
    g_active_binding = {};

    const void* used_pass = g_orig_set_pass(scene_mgr, pass, even_if_suppressed, shadow_derivation);
    if (!scene_mgr || !used_pass) {
        return used_pass;
    }

    const bool suppressed = (*reinterpret_cast<const uint8_t*>(scene_mgr + kSmOffSuppressRenderState) != 0);
    if (suppressed && !even_if_suppressed) {
        return used_pass;
    }

    const uintptr_t rs = *reinterpret_cast<const uintptr_t*>(scene_mgr + kSmOffDestRenderSystem);
    if (!rs) {
        return used_pass;
    }

    EnsureSharedPerFrameLighting();

    float flags[4] = {};
    bool is_ff_vs = false;
    const PassArchetype arch = ClassifyPass(used_pass, flags, &is_ff_vs);
    if (arch == PassArchetype::None) {
        return used_pass;
    }

    if (!EnsureSlotReady(arch)) {
        return used_pass;
    }

    const ExplicitProgramSlot& slot = g_slots[static_cast<size_t>(arch)];
    const uintptr_t ps_params_raw = reinterpret_cast<uintptr_t>(slot.params_sp.rep);
    ApplyPassFogAndAlphaToExplicitPs(scene_mgr, used_pass, ps_params_raw, flags);
    BindExplicitPsToRenderSystem(rs, slot.delegate, &slot.params_sp);
    RecordBindStat(arch);

    if (is_ff_vs) {
        SetActiveExplicitPsForwardTarget(ps_params_raw);
        g_active_binding.rs = rs;
        g_active_binding.ps_delegate = slot.delegate;
        g_active_binding.params_sp = &slot.params_sp;
        g_active_binding.params_raw = ps_params_raw;
        g_active_binding.is_fixed_func_vs = true;
    }

    return used_pass;
}

void* __fastcall HookGetFixedFuncShaderSource(
    uintptr_t self,
    std::string* out_src,
    const void* vert_entry,
    const void* frag_entry,
    const void* vdecl,
    const void* ff_state) {
    BeginFixedFuncManualShaderGen();
    void* ret = g_orig_get_shader_src(self, out_src, vert_entry, frag_entry, vdecl, ff_state);
    EndFixedFuncManualShaderGen(out_src);
    if (!out_src) {
        return ret;
    }

    const size_t fog_pos = out_src->find("output.fogDist=");
    if (fog_pos == std::string::npos) {
        return ret;
    }
    if (out_src->find("Texcoord6") != std::string::npos || out_src->find("Texcoord7") != std::string::npos) {
        return ret;
    }

    const bool has_normal = (out_src->find("float3 N=") != std::string::npos);
    const bool has_cam = (out_src->find("cameraPos") != std::string::npos);

    std::string inject = "\noutput.Texcoord6=";
    inject += has_normal ? "float4(N,1)" : "float4(0,0,1,0)";
    inject += ";\noutput.Texcoord7=";
    inject += has_cam
        ? "float4((dot(cameraPos.xyz,cameraPos.xyz)>1e-6?normalize(-cameraPos.xyz):float3(0,0,1)),1)"
        : "float4(0,0,1,1)";
    inject += ";\n";

    out_src->insert(fog_pos, inject);
    g_vs_texcoord_injections.fetch_add(1, std::memory_order_relaxed);
    return ret;
}

void __fastcall HookSetFixedFuncProgramsParameters(uintptr_t ff_progs, uintptr_t ff_state) {
    const ActiveExplicitBinding binding = g_active_binding;
    const bool sync_explicit =
        binding.is_fixed_func_vs &&
        binding.rs != 0 &&
        binding.params_raw != 0 &&
        binding.params_sp != nullptr &&
        (*reinterpret_cast<const uintptr_t*>(binding.rs + kRsOffBoundFragmentProgram) == binding.ps_delegate);

    if (sync_explicit) {
        SetActiveExplicitPsForwardTarget(binding.params_raw);
        ResetExplicitPsLightingScratch(binding.params_raw);
    } else {
        SetActiveExplicitPsForwardTarget(0);
    }

    g_orig_set_ff_params(ff_progs, ff_state);

    if (sync_explicit) {
        BindExplicitPsParametersByValue(binding.rs, *binding.params_sp);
        g_ff_explicit_syncs.fetch_add(1, std::memory_order_relaxed);
    }
    ApplyFixedFuncManualUniforms(sync_explicit ? 0 : ff_progs);
}

template <typename Fn>
bool ResolveExport(HMODULE mod, const char* symbol, Fn& out_fn) {
    out_fn = reinterpret_cast<Fn>(::GetProcAddress(mod, symbol));
    return out_fn != nullptr;
}

bool ResolveAllOgreExports(HMODULE ogre_mod) {
    bool ok = true;
    ok &= ResolveExport(ogre_mod, "?getSingletonPtr@?$Singleton@VHighLevelGpuProgramManager@Ogre@@@Ogre@@SAPEAVHighLevelGpuProgramManager@2@XZ", g_api.hl_mgr_singleton);
    ok &= ResolveExport(ogre_mod, "?getSingletonPtr@?$Singleton@VGpuProgramManager@Ogre@@@Ogre@@SAPEAVGpuProgramManager@2@XZ", g_api.gpu_mgr_singleton);
    ok &= ResolveExport(ogre_mod, "?getByName@GpuProgramManager@Ogre@@QEAA?AV?$SharedPtr@VResource@Ogre@@@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z", g_api.gpu_mgr_get_by_name);
    ok &= ResolveExport(ogre_mod, "?createSharedParameters@GpuProgramManager@Ogre@@UEAA?AV?$SharedPtr@VGpuSharedParameters@Ogre@@@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", g_api.create_shared_params);
    ok &= ResolveExport(ogre_mod, "?getSharedParameters@GpuProgramManager@Ogre@@UEBA?AV?$SharedPtr@VGpuSharedParameters@Ogre@@@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@Z", g_api.get_shared_params);
    ok &= ResolveExport(ogre_mod, "?addConstantDefinition@GpuSharedParameters@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@W4GpuConstantType@2@_K@Z", g_api.add_shared_const_def);

    ok &= ResolveExport(ogre_mod, "?getName@Pass@Ogre@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ", g_api.pass_get_name);
    ok &= ResolveExport(ogre_mod, "?hasVertexProgram@Pass@Ogre@@QEBA_NXZ", g_api.pass_has_vp);
    ok &= ResolveExport(ogre_mod, "?hasFragmentProgram@Pass@Ogre@@QEBA_NXZ", g_api.pass_has_fp);
    ok &= ResolveExport(ogre_mod, "?getVertexProgramName@Pass@Ogre@@QEBAAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@XZ", g_api.pass_get_vp_name);
    ok &= ResolveExport(ogre_mod, "?getNumTextureUnitStates@Pass@Ogre@@QEBAGXZ", g_api.pass_get_num_tus);
    ok &= ResolveExport(ogre_mod, "?getTextureUnitState@Pass@Ogre@@QEBAPEBVTextureUnitState@2@G@Z", g_api.pass_get_tus);
    ok &= ResolveExport(ogre_mod, "?getAlphaRejectFunction@Pass@Ogre@@QEBA?AW4CompareFunction@2@XZ", g_api.pass_get_alpha_reject_func);
    ok &= ResolveExport(ogre_mod, "?getAlphaRejectValue@Pass@Ogre@@QEBAEXZ", g_api.pass_get_alpha_reject_val);
    ok &= ResolveExport(ogre_mod, "?getFogOverride@Pass@Ogre@@QEBA_NXZ", g_api.pass_get_fog_override);
    ok &= ResolveExport(ogre_mod, "?getFogMode@Pass@Ogre@@QEBA?AW4FogMode@2@XZ", g_api.pass_get_fog_mode);
    ok &= ResolveExport(ogre_mod, "?getFogColour@Pass@Ogre@@QEBAAEBVColourValue@2@XZ", g_api.pass_get_fog_colour);
    ok &= ResolveExport(ogre_mod, "?getFogStart@Pass@Ogre@@QEBAMXZ", g_api.pass_get_fog_start);
    ok &= ResolveExport(ogre_mod, "?getFogEnd@Pass@Ogre@@QEBAMXZ", g_api.pass_get_fog_end);
    ok &= ResolveExport(ogre_mod, "?getFogDensity@Pass@Ogre@@QEBAMXZ", g_api.pass_get_fog_density);

    ok &= ResolveExport(ogre_mod, "?getFogMode@SceneManager@Ogre@@UEBA?AW4FogMode@2@XZ", g_api.sm_get_fog_mode);
    ok &= ResolveExport(ogre_mod, "?getFogColour@SceneManager@Ogre@@UEBAAEBVColourValue@2@XZ", g_api.sm_get_fog_colour);
    ok &= ResolveExport(ogre_mod, "?getFogStart@SceneManager@Ogre@@UEBAMXZ", g_api.sm_get_fog_start);
    ok &= ResolveExport(ogre_mod, "?getFogEnd@SceneManager@Ogre@@UEBAMXZ", g_api.sm_get_fog_end);
    ok &= ResolveExport(ogre_mod, "?getFogDensity@SceneManager@Ogre@@UEBAMXZ", g_api.sm_get_fog_density);

    ok &= ResolveExport(ogre_mod, "?getTextureType@TextureUnitState@Ogre@@QEBA?AW4TextureType@2@XZ", g_api.tus_get_tex_type);
    ok &= ResolveExport(ogre_mod, "?getTextureCoordSet@TextureUnitState@Ogre@@QEBAIXZ", g_api.tus_get_coord_set);
    ok &= ResolveExport(ogre_mod, "?getColourBlendMode@TextureUnitState@Ogre@@QEBAAEBVLayerBlendModeEx@2@XZ", g_api.tus_get_colour_blend);
    return ok;
}

}  // namespace

bool InstallPhase3ScenePipeline(void* d3d11_base, void* ogre_main_base, Phase3LogFn logger) {
    g_log = logger;
    if (!d3d11_base || !ogre_main_base) {
        return false;
    }

    g_toggles.diff_pass = !EnvFlagIsOne("TL2_P3_DISABLE_DIFF");
    g_toggles.skinned_diff = !EnvFlagIsOne("TL2_P3_DISABLE_SKIN");
    g_toggles.light_pass = !EnvFlagIsOne("TL2_P3_DISABLE_LIGHT");
    g_toggles.rim_lit = !EnvFlagIsOne("TL2_P3_DISABLE_RIM");

    auto* ogre_mod = static_cast<HMODULE>(ogre_main_base);
    if (!ResolveAllOgreExports(ogre_mod)) {
        if (g_log) {
            g_log("[RouteB][Phase3] FAILED resolving one or more OgreMain.dll exports");
        }
        return false;
    }

    const auto d3d_base = reinterpret_cast<uintptr_t>(d3d11_base);
    const auto ogre_base = reinterpret_cast<uintptr_t>(ogre_main_base);

    void* orig_set_pass = nullptr;
    if (!g_detour_set_pass.Install(
            reinterpret_cast<void*>(ogre_base + kRvaSceneMgrSetPass),
            kExpectedSetPass,
            sizeof(kExpectedSetPass),
            reinterpret_cast<void*>(&HookSceneManagerSetPass),
            &orig_set_pass)) {
        if (g_log) {
            g_log("[RouteB][Phase3] FAILED detour on Ogre::SceneManager::_setPass (0x1903C0)");
        }
        return false;
    }
    g_orig_set_pass = reinterpret_cast<SceneMgrSetPassFn>(orig_set_pass);

    void* orig_shader_src = nullptr;
    if (!g_detour_get_shader_src.Install(
            reinterpret_cast<void*>(d3d_base + kRvaGetFixedFuncShaderSource),
            kExpectedGetShaderSource,
            sizeof(kExpectedGetShaderSource),
            reinterpret_cast<void*>(&HookGetFixedFuncShaderSource),
            &orig_shader_src)) {
        if (g_log) {
            g_log("[RouteB][Phase3] FAILED detour on Hlsl4FixedFuncEmuShaderGenerator::getShaderSource (0x352B0)");
        }
        g_detour_set_pass.Remove();
        return false;
    }
    g_orig_get_shader_src = reinterpret_cast<GetFfShaderSourceFn>(orig_shader_src);

    void* orig_ff_params = nullptr;
    if (!g_detour_set_ff_params.Install(
            reinterpret_cast<void*>(d3d_base + kRvaSetFixedFuncParams),
            kExpectedSetFfParams,
            sizeof(kExpectedSetFfParams),
            reinterpret_cast<void*>(&HookSetFixedFuncProgramsParameters),
            &orig_ff_params)) {
        if (g_log) {
            g_log("[RouteB][Phase3] FAILED detour on setFixedFuncProgramsParameters (0x37ED0)");
        }
        g_detour_get_shader_src.Remove();
        g_detour_set_pass.Remove();
        return false;
    }
    g_orig_set_ff_params = reinterpret_cast<SetFfProgramsParamsFn>(orig_ff_params);

    EnsureSharedPerFrameLighting();
    const bool ff_manual_ok = InstallFixedFuncManualUniforms(d3d11_base, g_log);
    g_hooked.store(true, std::memory_order_relaxed);
    if (g_log) {
        g_log(
            "[RouteB][Phase3] Scene pipeline installed (diff=%d skin=%d light=%d rim=%d ffManual=%d)",
            g_toggles.diff_pass ? 1 : 0,
            g_toggles.skinned_diff ? 1 : 0,
            g_toggles.light_pass ? 1 : 0,
            g_toggles.rim_lit ? 1 : 0,
            ff_manual_ok ? 1 : 0);
    }
    return true;
}

void RemovePhase3ScenePipeline() {
    g_hooked.store(false, std::memory_order_relaxed);
    SetActiveExplicitPsForwardTarget(0);
    g_active_binding = {};

    RemoveFixedFuncManualUniforms();
    g_detour_set_ff_params.Remove();
    g_detour_get_shader_src.Remove();
    g_detour_set_pass.Remove();

    for (auto& slot : g_slots) {
        ReleaseOgreSharedPtr(slot.params_sp);
        ReleaseOgreSharedPtr(slot.resource_sp);
        slot.delegate = 0;
        slot.ready = false;
        slot.attempted = false;
    }
    ReleaseOgreSharedPtr(g_shared_per_frame_sp);
    g_shared_params_init_done = false;
    g_shared_params_ready = false;
}

Phase3Stats GetPhase3Stats() {
    Phase3Stats s{};
    s.setpass_calls = g_setpass_calls.load(std::memory_order_relaxed);
    s.diffpass_opaque_binds = g_diffpass_opaque_binds.load(std::memory_order_relaxed);
    s.diffpass_alpha_binds = g_diffpass_alpha_binds.load(std::memory_order_relaxed);
    s.skinned_diff_binds = g_skinned_diff_binds.load(std::memory_order_relaxed);
    s.lightpass_binds = g_lightpass_binds.load(std::memory_order_relaxed);
    s.rimlit_2d_binds = g_rimlit_2d_binds.load(std::memory_order_relaxed);
    s.rimlit_cube_binds = g_rimlit_cube_binds.load(std::memory_order_relaxed);
    s.vs_texcoord_injections = g_vs_texcoord_injections.load(std::memory_order_relaxed);
    s.ff_explicit_syncs = g_ff_explicit_syncs.load(std::memory_order_relaxed);
    s.hooked = g_hooked.load(std::memory_order_relaxed);
    s.shared_params_ready = g_shared_params_ready;
    return s;
}

}  // namespace routeb
