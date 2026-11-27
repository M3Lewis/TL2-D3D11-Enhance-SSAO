#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "cegui_explicit_shader.h"
#include "hook_utils.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

namespace routeb {
namespace {

constexpr uintptr_t kInitRenderStatesTailRva = 0x00003BD2U;
constexpr size_t kTailPatchLen = 17;

// CEGUI::OgreCEGUIRenderer::initRenderStates epilogue at RVA 0x3BD2:
//   movaps xmm6, xmmword ptr [rsp+0C0h] ; 0F 28 B4 24 C0 00 00 00
//   add    rsp, 0D0h                    ; 48 81 C4 D0 00 00 00
//   pop    rbx                          ; 5B
//   ret                                 ; C3
constexpr uint8_t kExpectedTailBytes[kTailPatchLen] = {
    0x0F, 0x28, 0xB4, 0x24, 0xC0, 0x00, 0x00, 0x00,
    0x48, 0x81, 0xC4, 0xD0, 0x00, 0x00, 0x00,
    0x5B,
    0xC3,
};

constexpr size_t kOgreRenderSysOffsetInCegui = 0x150;
constexpr size_t kD3D11ProjectionOffset = 2816;  // D3D11RenderSystem::mProjectionMatrix
constexpr size_t kVtableBindGpuProgramOffset = 0x300;
constexpr size_t kVtableBindGpuProgramParamsOffset = 0x308;
constexpr size_t kHlslProgVertexShaderOffset = 1016;
constexpr size_t kHlslProgPixelShaderOffset = 1024;

struct OgreSharedPtrRaw {
    void** vftable = nullptr;
    void* rep = nullptr;
    volatile long* use_count = nullptr;
    int free_method = 0;
    uint32_t pad = 0;

    void ResetUsingOgreVtable() {
        if (use_count) {
            if (::InterlockedDecrement(use_count) == 0 && vftable && vftable[1]) {
                using DestroyFn = void(__fastcall*)(void*);
                reinterpret_cast<DestroyFn>(vftable[1])(this);
            }
        }
        vftable = nullptr;
        rep = nullptr;
        use_count = nullptr;
        free_method = 0;
    }
};
static_assert(sizeof(OgreSharedPtrRaw) == 32, "Ogre::SharedPtr x64 layout must be 32 bytes");

struct SsoString16 {
    union {
        char buf[16];
        char* ptr;
    };
    size_t size;
    size_t capacity;
};
static_assert(sizeof(SsoString16) == 32, "MSVC x64 std::string SSO size must be 32 bytes");

SsoString16 MakeSsoString(const char* literal) {
    SsoString16 s{};
    const size_t len = std::strlen(literal);
    // <= 15 chars guarantees stack SSO without cross-CRT heap allocation.
    std::memcpy(s.buf, literal, len + 1);
    s.size = len;
    s.capacity = 15;
    return s;
}

using GpuMgrGetSingletonPtrFn = void*(__cdecl*)();
using GpuMgrGetByNameFn = void*(__fastcall*)(void* mgr, OgreSharedPtrRaw* out_sp, const SsoString16* name, bool prefer_hl);
using ResourceIsLoadedFn = bool(__fastcall*)(const void* res);
using ResourceLoadFn = void(__fastcall*)(void* res, bool background);
using GpuProgramGetDefaultParamsFn = void*(__fastcall*)(void* prog, OgreSharedPtrRaw* out_sp);
using ParamsSetNamedMatrix4Fn = void(__fastcall*)(void* params, const SsoString16* name, const float* mat16);
using RsBindGpuProgramFn = void(__fastcall*)(void* rs, void* gpu_prog);
using RsBindGpuProgramParamsFn = void(__fastcall*)(void* rs, int gpt_type, const OgreSharedPtrRaw* params, uint16_t mask);

// NTDLL LdrRegisterDllNotification declarations (Windows Vista+).
struct UNICODE_STRING_LDR {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};
struct LDR_DLL_LOADED_NOTIFICATION_DATA {
    ULONG Flags;
    const UNICODE_STRING_LDR* FullDllName;
    const UNICODE_STRING_LDR* BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
};
constexpr ULONG kLdrDllNotificationReasonLoaded = 1;
using LdrDllNotificationCallback = void(CALLBACK*)(ULONG reason, const LDR_DLL_LOADED_NOTIFICATION_DATA* data, PVOID context);
using LdrRegisterDllNotificationFn = LONG(NTAPI*)(ULONG flags, LdrDllNotificationCallback cb, PVOID ctx, PVOID* cookie);
using LdrUnregisterDllNotificationFn = LONG(NTAPI*)(PVOID cookie);

HMODULE g_d3d11_mod = nullptr;
HMODULE g_ogre_mod = nullptr;
LogCallbackFn g_log_fn = nullptr;

// Pre-allocated RWX stub so loader-lock callbacks perform zero heap allocations.
uint8_t* g_tail_stub = nullptr;
size_t g_tail_stub_size = 0;
std::atomic<HMODULE> g_patched_gui_mod{nullptr};
uint8_t* g_patched_tail_target = nullptr;
uint8_t g_saved_tail_bytes[kTailPatchLen]{};
PVOID g_dll_notify_cookie = nullptr;

GpuMgrGetSingletonPtrFn g_fn_get_gpu_mgr = nullptr;
GpuMgrGetByNameFn g_fn_get_by_name = nullptr;
ResourceIsLoadedFn g_fn_is_loaded = nullptr;
ResourceLoadFn g_fn_load = nullptr;
GpuProgramGetDefaultParamsFn g_fn_get_default_params = nullptr;
ParamsSetNamedMatrix4Fn g_fn_set_named_mat4 = nullptr;

std::mutex g_shader_mutex;
void* g_cached_mgr = nullptr;
OgreSharedPtrRaw g_vs_sp{};
OgreSharedPtrRaw g_ps_sp{};
OgreSharedPtrRaw g_vs_params_sp{};
OgreSharedPtrRaw g_ps_params_sp{};
float g_last_proj[16]{};
bool g_has_last_proj = false;
bool g_lookup_failed_logged = false;
std::atomic<bool> g_shaders_ready{false};

std::atomic<uint64_t> g_hook_calls{0};
std::atomic<uint64_t> g_explicit_binds{0};
std::atomic<uint64_t> g_fallback_skips{0};

bool EqualsIgnoreCaseAscii(const wchar_t* wide, size_t wchar_len, const wchar_t* target) {
    const size_t target_len = std::wcslen(target);
    if (wchar_len != target_len) {
        return false;
    }
    return ::_wcsnicmp(wide, target, target_len) == 0;
}

bool ValidateGuiModulePe(HMODULE mod) {
    if (!mod) {
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    return nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64;
}

bool PatchGuiModuleIfReady(HMODULE gui_mod) {
    HMODULE expected = nullptr;
    if (!g_tail_stub || !ValidateGuiModulePe(gui_mod)) {
        return false;
    }
    auto* target = static_cast<uint8_t*>(ResolveCodeTarget(
        gui_mod, kInitRenderStatesTailRva, kExpectedTailBytes, kTailPatchLen,
        kExpectedTailBytes, nullptr, kTailPatchLen));
    if (!target) {
        return false;
    }
    if (!g_patched_gui_mod.compare_exchange_strong(expected, gui_mod)) {
        return true;
    }

    g_patched_tail_target = target;
    std::memcpy(g_saved_tail_bytes, target, kTailPatchLen);

    uint8_t patch[kTailPatchLen];
    std::memset(patch, 0x90, sizeof(patch));
    patch[0] = 0xFF;
    patch[1] = 0x25;
    patch[2] = 0x00;
    patch[3] = 0x00;
    patch[4] = 0x00;
    patch[5] = 0x00;
    const uint64_t stub_addr = reinterpret_cast<uint64_t>(g_tail_stub);
    std::memcpy(patch + 6, &stub_addr, sizeof(stub_addr));

    DWORD old_protect = 0;
    if (!::VirtualProtect(target, kTailPatchLen, PAGE_EXECUTE_READWRITE, &old_protect)) {
        g_patched_gui_mod.store(nullptr);
        return false;
    }
    std::memcpy(target, patch, kTailPatchLen);
    ::VirtualProtect(target, kTailPatchLen, old_protect, &old_protect);
    ::FlushInstructionCache(::GetCurrentProcess(), target, kTailPatchLen);
    return true;
}

void CALLBACK OnDllLoaded(ULONG reason, const LDR_DLL_LOADED_NOTIFICATION_DATA* data, PVOID) {
    if (reason != kLdrDllNotificationReasonLoaded || !data || !data->BaseDllName || !data->BaseDllName->Buffer) {
        return;
    }
    const size_t wlen = data->BaseDllName->Length / sizeof(wchar_t);
    if (EqualsIgnoreCaseAscii(data->BaseDllName->Buffer, wlen, L"OgreGUIRenderer.dll")) {
        PatchGuiModuleIfReady(static_cast<HMODULE>(data->DllBase));
    }
}

bool EnsureOgreExportsResolved() {
    if (g_fn_get_gpu_mgr) {
        return true;
    }
    if (!g_ogre_mod) {
        return false;
    }
    g_fn_get_gpu_mgr = reinterpret_cast<GpuMgrGetSingletonPtrFn>(
        ::GetProcAddress(g_ogre_mod, "?getSingletonPtr@GpuProgramManager@Ogre@@SAPEAV12@XZ"));
    g_fn_get_by_name = reinterpret_cast<GpuMgrGetByNameFn>(
        ::GetProcAddress(g_ogre_mod, "?getByName@GpuProgramManager@Ogre@@QEAA?AV?$SharedPtr@VResource@Ogre@@@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z"));
    g_fn_is_loaded = reinterpret_cast<ResourceIsLoadedFn>(
        ::GetProcAddress(g_ogre_mod, "?isLoaded@Resource@Ogre@@UEBA_NXZ"));
    g_fn_load = reinterpret_cast<ResourceLoadFn>(
        ::GetProcAddress(g_ogre_mod, "?load@Resource@Ogre@@UEAAX_N@Z"));
    g_fn_get_default_params = reinterpret_cast<GpuProgramGetDefaultParamsFn>(
        ::GetProcAddress(g_ogre_mod, "?getDefaultParameters@GpuProgram@Ogre@@UEAA?AV?$SharedPtr@VGpuProgramParameters@Ogre@@@2@XZ"));
    g_fn_set_named_mat4 = reinterpret_cast<ParamsSetNamedMatrix4Fn>(
        ::GetProcAddress(g_ogre_mod, "?setNamedConstant@GpuProgramParameters@Ogre@@QEAAXAEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEBVMatrix4@2@@Z"));

    return g_fn_get_gpu_mgr && g_fn_get_by_name && g_fn_is_loaded &&
           g_fn_load && g_fn_get_default_params && g_fn_set_named_mat4;
}

void ResetCachedShadersUnsafe() {
    if (::GetModuleHandleW(L"OgreMain.dll") == g_ogre_mod && g_fn_get_gpu_mgr && g_fn_get_gpu_mgr() != nullptr) {
        g_vs_params_sp.ResetUsingOgreVtable();
        g_ps_params_sp.ResetUsingOgreVtable();
        g_vs_sp.ResetUsingOgreVtable();
        g_ps_sp.ResetUsingOgreVtable();
    } else {
        g_vs_params_sp = {};
        g_ps_params_sp = {};
        g_vs_sp = {};
        g_ps_sp = {};
    }
    g_cached_mgr = nullptr;
    g_has_last_proj = false;
    g_shaders_ready.store(false, std::memory_order_release);
}

bool EnsureSimpleUiProgramsReady() {
    if (!EnsureOgreExportsResolved()) {
        return false;
    }
    void* mgr = g_fn_get_gpu_mgr();
    if (!mgr) {
        return false;
    }

    if (g_shaders_ready.load(std::memory_order_acquire) && g_cached_mgr == mgr &&
        g_vs_sp.rep && g_ps_sp.rep && g_fn_is_loaded(g_vs_sp.rep) && g_fn_is_loaded(g_ps_sp.rep)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_shader_mutex);
    mgr = g_fn_get_gpu_mgr();
    if (!mgr) {
        return false;
    }
    if (g_cached_mgr != mgr) {
        ResetCachedShadersUnsafe();
    }
    if (g_shaders_ready.load(std::memory_order_relaxed) &&
        g_vs_sp.rep && g_ps_sp.rep && g_fn_is_loaded(g_vs_sp.rep) && g_fn_is_loaded(g_ps_sp.rep)) {
        return true;
    }

    if (!g_vs_sp.rep) {
        const auto vs_name = MakeSsoString("TL2/SimpleUI_VS");
        g_fn_get_by_name(mgr, &g_vs_sp, &vs_name, true);
    }
    if (!g_ps_sp.rep) {
        const auto ps_name = MakeSsoString("TL2/SimpleUI_PS");
        g_fn_get_by_name(mgr, &g_ps_sp, &ps_name, true);
    }

    if (!g_vs_sp.rep || !g_ps_sp.rep) {
        if (!g_lookup_failed_logged && g_log_fn) {
            g_lookup_failed_logged = true;
            g_log_fn("[RouteB][Phase2] TL2/SimpleUI_VS (%p) or TL2/SimpleUI_PS (%p) not found; keeping FixedFunc fallback.",
                     g_vs_sp.rep, g_ps_sp.rep);
        }
        return false;
    }

    if (!g_fn_is_loaded(g_vs_sp.rep)) {
        g_fn_load(g_vs_sp.rep, false);
    }
    if (!g_fn_is_loaded(g_ps_sp.rep)) {
        g_fn_load(g_ps_sp.rep, false);
    }

    const auto* vs_bytes = static_cast<const uint8_t*>(g_vs_sp.rep);
    const auto* ps_bytes = static_cast<const uint8_t*>(g_ps_sp.rep);
    const void* d3d_vs = *reinterpret_cast<void* const*>(vs_bytes + kHlslProgVertexShaderOffset);
    const void* d3d_ps = *reinterpret_cast<void* const*>(ps_bytes + kHlslProgPixelShaderOffset);
    if (!d3d_vs || !d3d_ps) {
        return false;
    }

    if (!g_vs_params_sp.rep) {
        g_fn_get_default_params(g_vs_sp.rep, &g_vs_params_sp);
    }
    if (!g_ps_params_sp.rep) {
        g_fn_get_default_params(g_ps_sp.rep, &g_ps_params_sp);
    }
    if (!g_vs_params_sp.rep || !g_ps_params_sp.rep) {
        return false;
    }

    g_cached_mgr = mgr;
    g_has_last_proj = false;
    g_shaders_ready.store(true, std::memory_order_release);
    if (g_log_fn) {
        g_log_fn("[RouteB][Phase2] CEGUI explicit SM4.0 shaders ready: VS=%p PS=%p", g_vs_sp.rep, g_ps_sp.rep);
    }
    return true;
}

void __fastcall OnPostInitRenderStates(void* cegui_renderer) {
    g_hook_calls.fetch_add(1, std::memory_order_relaxed);
    if (!cegui_renderer || !g_d3d11_mod) {
        g_fallback_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto* rs = *reinterpret_cast<uint8_t**>(
        static_cast<uint8_t*>(cegui_renderer) + kOgreRenderSysOffsetInCegui);
    if (!rs) {
        g_fallback_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Verify RenderSystem belongs to RenderSystem_Direct3D11.dll before poking D3D11 offsets.
    void** vtbl = *reinterpret_cast<void***>(rs);
    HMODULE vtbl_mod = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(vtbl),
            &vtbl_mod) ||
        vtbl_mod != g_d3d11_mod) {
        g_fallback_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (!EnsureSimpleUiProgramsReady()) {
        g_fallback_skips.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const auto bind_prog = reinterpret_cast<RsBindGpuProgramFn>(vtbl[kVtableBindGpuProgramOffset / 8]);
    const auto bind_params = reinterpret_cast<RsBindGpuProgramParamsFn>(vtbl[kVtableBindGpuProgramParamsOffset / 8]);

    {
        std::lock_guard<std::mutex> lock(g_shader_mutex);
        const float* proj_mat = reinterpret_cast<const float*>(rs + kD3D11ProjectionOffset);
        if (!g_has_last_proj || std::memcmp(proj_mat, g_last_proj, sizeof(g_last_proj)) != 0) {
            const auto proj_name = MakeSsoString("Projection");
            g_fn_set_named_mat4(g_vs_params_sp.rep, &proj_name, proj_mat);
            std::memcpy(g_last_proj, proj_mat, sizeof(g_last_proj));
            g_has_last_proj = true;
        }
        bind_prog(rs, g_vs_sp.rep);
        bind_prog(rs, g_ps_sp.rep);
        bind_params(rs, 0 /*GPT_VERTEX_PROGRAM*/, &g_vs_params_sp, 0xFFFFu);
        bind_params(rs, 1 /*GPT_FRAGMENT_PROGRAM*/, &g_ps_params_sp, 0xFFFFu);
    }

    g_explicit_binds.fetch_add(1, std::memory_order_relaxed);
}

bool AllocateTailStub() {
    if (g_tail_stub) {
        return true;
    }
    const uint8_t stub_prefix[] = {
        0x0F, 0x28, 0xB4, 0x24, 0xC0, 0x00, 0x00, 0x00,  // movaps xmm6, [rsp+0C0h]
        0x48, 0x89, 0xD9,                                // mov rcx, rbx
        0x48, 0x81, 0xC4, 0xD0, 0x00, 0x00, 0x00,        // add rsp, 0D0h
        0x5B,                                            // pop rbx
        0x48, 0xB8,                                      // mov rax, imm64
    };
    const size_t total = sizeof(stub_prefix) + sizeof(uint64_t) + 2;
    auto* mem = static_cast<uint8_t*>(
        ::VirtualAlloc(nullptr, total, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!mem) {
        return false;
    }

    std::memcpy(mem, stub_prefix, sizeof(stub_prefix));
    const uint64_t target_fn = reinterpret_cast<uint64_t>(&OnPostInitRenderStates);
    std::memcpy(mem + sizeof(stub_prefix), &target_fn, sizeof(target_fn));
    mem[sizeof(stub_prefix) + sizeof(uint64_t) + 0] = 0xFF;  // jmp rax
    mem[sizeof(stub_prefix) + sizeof(uint64_t) + 1] = 0xE0;
    ::FlushInstructionCache(::GetCurrentProcess(), mem, total);

    g_tail_stub = mem;
    g_tail_stub_size = total;
    return true;
}

}  // namespace

bool InstallCeguiExplicitShaderHook(void* d3d11_module_base, void* ogre_main_base, LogCallbackFn log_fn) {
    g_d3d11_mod = static_cast<HMODULE>(d3d11_module_base);
    g_ogre_mod = static_cast<HMODULE>(ogre_main_base);
    g_log_fn = log_fn;

    if (!g_d3d11_mod || !g_ogre_mod) {
        return false;
    }
    if (!AllocateTailStub()) {
        return false;
    }

    if (HMODULE gui_mod = ::GetModuleHandleW(L"OgreGUIRenderer.dll")) {
        const bool ok = PatchGuiModuleIfReady(gui_mod);
        if (log_fn) {
            log_fn("[RouteB][Phase2] OgreGUIRenderer already loaded (%p), tail hook=%s",
                   gui_mod, ok ? "OK" : "FAILED");
        }
        return ok;
    }

    if (!g_dll_notify_cookie) {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        auto reg_fn = ntdll
            ? reinterpret_cast<LdrRegisterDllNotificationFn>(::GetProcAddress(ntdll, "LdrRegisterDllNotification"))
            : nullptr;
        if (!reg_fn || reg_fn(0, &OnDllLoaded, nullptr, &g_dll_notify_cookie) != 0) {
            if (log_fn) {
                log_fn("[RouteB][Phase2] LdrRegisterDllNotification failed.");
            }
            return false;
        }
        if (log_fn) {
            log_fn("[RouteB][Phase2] Armed LdrRegisterDllNotification for OgreGUIRenderer.dll load.");
        }
    }

    // Re-check in case OgreGUIRenderer loaded concurrently during registration.
    if (HMODULE gui_mod = ::GetModuleHandleW(L"OgreGUIRenderer.dll")) {
        PatchGuiModuleIfReady(gui_mod);
    }
    return true;
}

void RemoveCeguiExplicitShaderHook() {
    if (g_dll_notify_cookie) {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        auto unreg_fn = ntdll
            ? reinterpret_cast<LdrUnregisterDllNotificationFn>(::GetProcAddress(ntdll, "LdrUnregisterDllNotification"))
            : nullptr;
        if (unreg_fn) {
            unreg_fn(g_dll_notify_cookie);
        }
        g_dll_notify_cookie = nullptr;
    }

    if (HMODULE gui_mod = g_patched_gui_mod.exchange(nullptr)) {
        uint8_t* target = g_patched_tail_target;
        g_patched_tail_target = nullptr;
        if (target && ::GetModuleHandleW(L"OgreGUIRenderer.dll") == gui_mod) {
            DWORD old_protect = 0;
            if (::VirtualProtect(target, kTailPatchLen, PAGE_EXECUTE_READWRITE, &old_protect)) {
                std::memcpy(target, g_saved_tail_bytes, kTailPatchLen);
                ::VirtualProtect(target, kTailPatchLen, old_protect, &old_protect);
                ::FlushInstructionCache(::GetCurrentProcess(), target, kTailPatchLen);
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_shader_mutex);
        ResetCachedShadersUnsafe();
    }

    if (g_tail_stub) {
        ::VirtualFree(g_tail_stub, 0, MEM_RELEASE);
        g_tail_stub = nullptr;
        g_tail_stub_size = 0;
    }
}

CeguiShaderStats GetCeguiShaderStats() {
    CeguiShaderStats stats{};
    stats.hook_calls = g_hook_calls.load(std::memory_order_relaxed);
    stats.explicit_binds = g_explicit_binds.load(std::memory_order_relaxed);
    stats.fallback_skips = g_fallback_skips.load(std::memory_order_relaxed);
    stats.hooked = g_patched_gui_mod.load(std::memory_order_relaxed) != nullptr;
    stats.shaders_ready = g_shaders_ready.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace routeb
