#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "cegui_explicit_shader.h"
#include "dxbc_cache.h"
#include "fixed_func_manual_uniforms.h"
#include "hlsl_cbuffer_fastpath.h"
#include "hook_utils.h"
#include "phase3_scene_pipeline.h"
#include "route_c_ao.h"

namespace {

struct ExpectedModule {
    const wchar_t* module_name;
    std::uint32_t expected_timestamp;
    std::uint32_t expected_size_of_image;
};

constexpr ExpectedModule kModD3D11 = {L"RenderSystem_Direct3D11.dll", 0x6af5fabfU, 0x0008a000U};
constexpr ExpectedModule kModGui   = {L"OgreGUIRenderer.dll",         0x6af5fab0U, 0x00014000U};
constexpr ExpectedModule kModOgre  = {L"OgreMain.dll",                0x6af5faa0U, 0x006b6000U};

FILE* g_log = nullptr;
bool g_phase1_installed = false;
PVOID g_veh_handle = nullptr;
std::atomic<bool> g_crash_logged{false};
std::atomic<bool> g_stats_logged{false};
std::atomic<bool> g_cleaned_up{false};
std::atomic<int> g_cxx_throw_logged{0};

constexpr std::uintptr_t kRvaFindNamedConstStart = 0x000AF6E0;
constexpr std::uintptr_t kRvaFindNamedConstEnd   = 0x000AF900;

// Verified Ogre::Root::~Root & MemoryDataStream RVAs in OgreMain.dll (TimeDateStamp 0x6af5faa0)
constexpr std::uintptr_t kRvaRootDtor        = 0x184860;
constexpr std::uintptr_t kRvaMemStreamRead   = 0x085E60;
constexpr std::uintptr_t kRvaMemStreamSeek   = 0x0864B0;
constexpr std::uintptr_t kRvaMemStreamSkip   = 0x086530;

constexpr std::uint8_t kExpectedRootDtor[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
constexpr std::uint8_t kExpectedMemStreamRead[16] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xF9, 0x4C, 0x8B, 0xCA};
constexpr std::uint8_t kExpectedMemStreamSeek[16] = {
    0x48, 0x03, 0x51, 0x38, 0x48, 0x89, 0x51, 0x40, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
constexpr std::uint8_t kExpectedMemStreamSkip[16] = {
    0x48, 0x63, 0xC2, 0x48, 0x01, 0x41, 0x40, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

using RootDtorFn = void(__fastcall*)(void*);
routeb::Detour64 g_detour_root_dtor;
RootDtorFn g_orig_root_dtor = nullptr;

routeb::Detour64 g_detour_mem_read;
routeb::Detour64 g_detour_mem_seek;
routeb::Detour64 g_detour_mem_skip;
void* g_orig_mem_read = nullptr;
void* g_orig_mem_seek = nullptr;
void* g_orig_mem_skip = nullptr;
std::atomic<std::uint64_t> g_mem_stream_clamps{0};

std::size_t __fastcall HookMemoryStreamRead(void* self, void* buf, std::size_t count) {
    if (!self || !buf || count == 0) {
        return 0;
    }
    auto* bytes = static_cast<std::uint8_t*>(self);
    const auto data = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x38);
    const auto pos = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x40);
    const auto end = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x48);
    if (pos < data || pos >= end) {
        if (pos < data || pos > end) {
            g_mem_stream_clamps.fetch_add(1, std::memory_order_relaxed);
        }
        return 0;
    }
    const std::size_t avail = static_cast<std::size_t>(end - pos);
    const std::size_t to_copy = (count < avail) ? count : avail;
    std::memcpy(buf, reinterpret_cast<const void*>(pos), to_copy);
    *reinterpret_cast<std::uintptr_t*>(bytes + 0x40) = pos + to_copy;
    return to_copy;
}

void __fastcall HookMemoryStreamSeek(void* self, std::size_t offset) {
    if (!self) {
        return;
    }
    auto* bytes = static_cast<std::uint8_t*>(self);
    const auto data = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x38);
    const auto end = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x48);
    const std::size_t max_off = (end >= data) ? static_cast<std::size_t>(end - data) : 0;
    std::size_t clamped = offset;
    if (clamped > max_off) {
        clamped = max_off;
        g_mem_stream_clamps.fetch_add(1, std::memory_order_relaxed);
    }
    *reinterpret_cast<std::uintptr_t*>(bytes + 0x40) = data + clamped;
}

void __fastcall HookMemoryStreamSkip(void* self, long count) {
    if (!self) {
        return;
    }
    auto* bytes = static_cast<std::uint8_t*>(self);
    const auto data = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x38);
    const auto pos = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x40);
    const auto end = *reinterpret_cast<const std::uintptr_t*>(bytes + 0x48);
    if (end < data) {
        *reinterpret_cast<std::uintptr_t*>(bytes + 0x40) = end;
        g_mem_stream_clamps.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::int64_t delta = static_cast<std::int64_t>(count);
    std::uintptr_t new_pos = pos;
    if (delta >= 0) {
        const auto forward = static_cast<std::uintptr_t>(delta);
        if (pos >= end || forward > end - pos) {
            new_pos = end;
            if (forward > 0 || pos > end) {
                g_mem_stream_clamps.fetch_add(1, std::memory_order_relaxed);
            }
        } else {
            new_pos = pos + forward;
        }
    } else {
        const auto back = static_cast<std::uintptr_t>(-delta);
        if (pos <= data || back > pos - data) {
            new_pos = data;
            g_mem_stream_clamps.fetch_add(1, std::memory_order_relaxed);
        } else {
            new_pos = pos - back;
        }
    }
    *reinterpret_cast<std::uintptr_t*>(bytes + 0x40) = new_pos;
}

void LogLine(const char* fmt, ...) {
    if (!g_log) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

void LogAllStats() {
    if (!g_log) {
        return;
    }
    const auto dxbc = routeb::GetDxbcCacheStats();
    const auto fp = routeb::GetFastPathStats();
    LogLine(
        "[RouteB][Phase1][Stats] DXBC hits=%llu misses=%llu bypassed=%llu writeErr=%llu",
        static_cast<unsigned long long>(dxbc.hits),
        static_cast<unsigned long long>(dxbc.misses),
        static_cast<unsigned long long>(dxbc.bypassed),
        static_cast<unsigned long long>(dxbc.write_errors));
    LogLine(
        "[RouteB][Phase1][Stats] CB fastHits=%llu fallback=%llu (nullCb=%llu nullP=%llu uninit=%llu bounds=%llu mapFail=%llu) | FFParam hits=%llu misses=%llu skippedMissing=%llu p3Fwd=%llu",
        static_cast<unsigned long long>(fp.cb_fast_hits),
        static_cast<unsigned long long>(fp.cb_fallback_calls),
        static_cast<unsigned long long>(fp.cb_fb_null_cb),
        static_cast<unsigned long long>(fp.cb_fb_null_params),
        static_cast<unsigned long long>(fp.cb_fb_uninit),
        static_cast<unsigned long long>(fp.cb_fb_bounds),
        static_cast<unsigned long long>(fp.cb_fb_map_fail),
        static_cast<unsigned long long>(fp.ff_param_hits),
        static_cast<unsigned long long>(fp.ff_param_misses),
        static_cast<unsigned long long>(fp.ff_param_skipped_missing),
        static_cast<unsigned long long>(fp.p3_param_forwards));
    const auto ffman = routeb::GetFixedFuncManualStats();
    LogLine(
        "[RouteB][FFManual][Stats] hooked=%d normLookups=%llu uniqueShaders=%llu paramUpdates=%llu",
        ffman.hooked ? 1 : 0,
        static_cast<unsigned long long>(ffman.normalized_lookups),
        static_cast<unsigned long long>(ffman.unique_manual_shaders),
        static_cast<unsigned long long>(ffman.param_updates));
    const auto cegui = routeb::GetCeguiShaderStats();
    LogLine(
        "[RouteB][Phase2][Stats] CEGUI hooked=%d ready=%d calls=%llu explicitBinds=%llu skips=%llu",
        cegui.hooked ? 1 : 0,
        cegui.shaders_ready ? 1 : 0,
        static_cast<unsigned long long>(cegui.hook_calls),
        static_cast<unsigned long long>(cegui.explicit_binds),
        static_cast<unsigned long long>(cegui.fallback_skips));
    const auto p3 = routeb::GetPhase3Stats();
    LogLine(
        "[RouteB][Phase3][Stats] Scene hooked=%d samplerHook=%d samplerCalls=%llu samplerAF=%llu shared=%d setPass=%llu diffOpaque=%llu diffAlpha=%llu skinned=%llu light=%llu rim2D=%llu rimCube=%llu vsInject=%llu vsVColFix=%llu ffSync=%llu",
        p3.hooked ? 1 : 0,
        p3.sampler_hooked ? 1 : 0,
        static_cast<unsigned long long>(p3.sampler_hook_calls),
        static_cast<unsigned long long>(p3.sampler_af_upgrades),
        p3.shared_params_ready ? 1 : 0,
        static_cast<unsigned long long>(p3.setpass_calls),
        static_cast<unsigned long long>(p3.diffpass_opaque_binds),
        static_cast<unsigned long long>(p3.diffpass_alpha_binds),
        static_cast<unsigned long long>(p3.skinned_diff_binds),
        static_cast<unsigned long long>(p3.lightpass_binds),
        static_cast<unsigned long long>(p3.rimlit_2d_binds),
        static_cast<unsigned long long>(p3.rimlit_cube_binds),
        static_cast<unsigned long long>(p3.vs_texcoord_injections),
        static_cast<unsigned long long>(p3.vs_missing_vcol_fixes),
        static_cast<unsigned long long>(p3.ff_explicit_syncs));
    routeb::LogPhase3PassDiagnostics(LogLine);
    routeb::LogRouteCSurveySummary(LogLine);
    LogLine(
        "[RouteB][OgreFix] MemoryDataStream readHook=%d seekHook=%d skipHook=%d boundsClamps=%llu",
        g_detour_mem_read.IsInstalled() ? 1 : 0,
        g_detour_mem_seek.IsInstalled() ? 1 : 0,
        g_detour_mem_skip.IsInstalled() ? 1 : 0,
        static_cast<unsigned long long>(g_mem_stream_clamps.load(std::memory_order_relaxed)));
}

bool TryFormatModuleOffset(std::uintptr_t addr, char* buf, std::size_t buf_len) {
    if (!addr || !buf || buf_len == 0) {
        return false;
    }
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(addr),
            &mod) ||
        !mod) {
        return false;
    }
    wchar_t mod_path[MAX_PATH]{};
    const DWORD len = ::GetModuleFileNameW(mod, mod_path, MAX_PATH);
    if (len == 0) {
        return false;
    }
    const wchar_t* base_name = mod_path;
    for (const wchar_t* p = mod_path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            base_name = p + 1;
        }
    }
    const std::uintptr_t rva = addr - reinterpret_cast<std::uintptr_t>(mod);
    std::snprintf(buf, buf_len, "%ls+0x%llx", base_name, static_cast<unsigned long long>(rva));
    return true;
}

LONG CALLBACK OnVectoredException(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !g_log) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == 0xE06D7363UL) {
        if (const HMODULE ogre_mod = ::GetModuleHandleW(L"OgreMain.dll")) {
            const auto ogre_base = reinterpret_cast<std::uintptr_t>(ogre_mod);
            void* frames[16]{};
            const USHORT nframes = ::RtlCaptureStackBackTrace(0, 16, frames, nullptr);
            bool from_find_named = false;
            for (USHORT i = 0; i < nframes; ++i) {
                const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
                if (addr >= ogre_base + kRvaFindNamedConstStart && addr < ogre_base + kRvaFindNamedConstEnd) {
                    from_find_named = true;
                    break;
                }
            }
            if (from_find_named && g_cxx_throw_logged.fetch_add(1, std::memory_order_relaxed) < 4) {
                const auto p3 = routeb::GetPhase3Stats();
                LogLine("[RouteB][CXX] _findNamedConstantDefinition threw 0xE06D7363 (frames=%u, setPass=%llu, skinned=%llu, diffA=%llu)",
                        static_cast<unsigned>(nframes),
                        static_cast<unsigned long long>(p3.setpass_calls),
                        static_cast<unsigned long long>(p3.skinned_diff_binds),
                        static_cast<unsigned long long>(p3.diffpass_alpha_binds));
                char loc_buf[160]{};
                for (USHORT i = 0; i < nframes; ++i) {
                    const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
                    if (!TryFormatModuleOffset(addr, loc_buf, sizeof(loc_buf))) {
                        std::snprintf(loc_buf, sizeof(loc_buf), "0x%llx", static_cast<unsigned long long>(addr));
                    }
                    LogLine("[RouteB][CXX]   frame[%u] = %s", static_cast<unsigned>(i), loc_buf);
                }
            }
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_ARRAY_BOUNDS_EXCEEDED) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_crash_logged.exchange(true, std::memory_order_acq_rel)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto fault_addr = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    char loc_buf[160]{};
    if (!TryFormatModuleOffset(fault_addr, loc_buf, sizeof(loc_buf))) {
        std::snprintf(loc_buf, sizeof(loc_buf), "0x%llx", static_cast<unsigned long long>(fault_addr));
    }
    LogLine("[RouteB][CRASH] Exception 0x%08lx at %s (0x%llx)",
            static_cast<unsigned long>(code),
            loc_buf,
            static_cast<unsigned long long>(fault_addr));
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        LogLine("[RouteB][CRASH] AV AccessType=%llu TargetAddr=0x%llx",
                static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[0]),
                static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
    }

#ifdef _M_AMD64
    if (info->ContextRecord) {
        const CONTEXT* c = info->ContextRecord;
        LogLine("[RouteB][CRASH] RAX=0x%llx RCX=0x%llx RDX=0x%llx RBX=0x%llx",
                c->Rax, c->Rcx, c->Rdx, c->Rbx);
        LogLine("[RouteB][CRASH] RSP=0x%llx RBP=0x%llx RSI=0x%llx RDI=0x%llx R8=0x%llx R9=0x%llx",
                c->Rsp, c->Rbp, c->Rsi, c->Rdi, c->R8, c->R9);

        const auto* stack_slots = reinterpret_cast<const std::uintptr_t*>(c->Rsp);
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(stack_slots, &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.State == MEM_COMMIT &&
            (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
            const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            for (std::size_t i = 0; i < 64; ++i) {
                const std::uintptr_t slot_addr = reinterpret_cast<std::uintptr_t>(&stack_slots[i]);
                if (slot_addr + sizeof(std::uintptr_t) > region_end) {
                    break;
                }
                const std::uintptr_t val = stack_slots[i];
                if (TryFormatModuleOffset(val, loc_buf, sizeof(loc_buf))) {
                    LogLine("[RouteB][CRASH] [RSP+0x%03zx] = %s", i * sizeof(std::uintptr_t), loc_buf);
                }
            }
        }
    }
#endif
    LogAllStats();
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLogger() {
    if (!g_veh_handle) {
        g_crash_logged.store(false, std::memory_order_relaxed);
        g_veh_handle = ::AddVectoredExceptionHandler(1, &OnVectoredException);
    }
}

void RemoveCrashLogger() {
    if (g_veh_handle) {
        ::RemoveVectoredExceptionHandler(g_veh_handle);
        g_veh_handle = nullptr;
    }
}

void* ValidateModulePe(const ExpectedModule& spec, bool* out_matched) {
    *out_matched = false;
    HMODULE mod = ::GetModuleHandleW(spec.module_name);
    if (!mod) {
        LogLine("[RouteB] Module not loaded yet: %ls", spec.module_name);
        return nullptr;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LogLine("[RouteB] Invalid DOS header on %ls", spec.module_name);
        return nullptr;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        LogLine("[RouteB] Invalid NT header on %ls", spec.module_name);
        return nullptr;
    }

    const std::uint32_t ts = nt->FileHeader.TimeDateStamp;
    const std::uint32_t img_size = nt->OptionalHeader.SizeOfImage;
    const bool matched = (ts == spec.expected_timestamp) && (img_size == spec.expected_size_of_image);
    *out_matched = matched;

    LogLine(
        "[RouteB] %ls base=0x%llx TimeDateStamp=0x%08x SizeOfImage=0x%x match=%s",
        spec.module_name,
        static_cast<unsigned long long>(base),
        ts,
        img_size,
        matched ? "OK" : "MISMATCH");

    return reinterpret_cast<void*>(base);
}

void CleanupHooksAndResources() {
    if (g_cleaned_up.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    if (!g_stats_logged.exchange(true, std::memory_order_acq_rel)) {
        LogAllStats();
    }
    routeb::SetPhase3PassObserver(nullptr);
    routeb::RemoveRouteCAoPipeline();
    routeb::RemovePhase3ScenePipeline();
    routeb::RemoveCeguiExplicitShaderHook();
    routeb::RemovePhase1FastPaths();
    routeb::RemoveDxbcCacheHook();
    g_detour_mem_skip.Remove();
    g_detour_mem_seek.Remove();
    g_detour_mem_read.Remove();
    g_orig_mem_skip = nullptr;
    g_orig_mem_seek = nullptr;
    g_orig_mem_read = nullptr;
    g_phase1_installed = false;
}

bool InstallMemoryStreamGuards(void* ogre_base) {
    if (!ogre_base) {
        return false;
    }
    struct StreamGuardSpec {
        std::uintptr_t rva;
        const std::uint8_t* expected;
        std::size_t len;
        void* hook;
        routeb::Detour64* detour;
        void** orig_slot;
        const char* label;
    };
    const StreamGuardSpec specs[] = {
        {kRvaMemStreamRead, kExpectedMemStreamRead, sizeof(kExpectedMemStreamRead),
         reinterpret_cast<void*>(&HookMemoryStreamRead), &g_detour_mem_read, &g_orig_mem_read, "read"},
        {kRvaMemStreamSeek, kExpectedMemStreamSeek, sizeof(kExpectedMemStreamSeek),
         reinterpret_cast<void*>(&HookMemoryStreamSeek), &g_detour_mem_seek, &g_orig_mem_seek, "seek"},
        {kRvaMemStreamSkip, kExpectedMemStreamSkip, sizeof(kExpectedMemStreamSkip),
         reinterpret_cast<void*>(&HookMemoryStreamSkip), &g_detour_mem_skip, &g_orig_mem_skip, "skip"},
    };
    bool all_ok = true;
    const auto base = reinterpret_cast<std::uintptr_t>(ogre_base);
    for (const auto& s : specs) {
        if (s.detour->IsInstalled()) {
            continue;
        }
        if (!s.detour->Install(reinterpret_cast<void*>(base + s.rva), s.expected, s.len, s.hook, s.orig_slot)) {
            LogLine("[RouteB][OgreFix] WARNING: Failed to detour MemoryDataStream::%s (0x%llx).",
                    s.label, static_cast<unsigned long long>(s.rva));
            all_ok = false;
        }
    }
    return all_ok;
}

void __fastcall HookRootDtor(void* root_self) {
    // Release all retained Ogre SharedPtr<HighLevelGpuProgram/GpuSharedParameters> before ~Root deletes ResourceManagers.
    CleanupHooksAndResources();
    if (g_orig_root_dtor) {
        g_orig_root_dtor(root_self);
    }
}

void InstallRootShutdownHook(void* ogre_base) {
    if (!ogre_base || g_detour_root_dtor.IsInstalled()) {
        return;
    }
    void* orig = nullptr;
    auto* target = reinterpret_cast<std::uint8_t*>(ogre_base) + kRvaRootDtor;
    if (g_detour_root_dtor.Install(
            target,
            kExpectedRootDtor,
            sizeof(kExpectedRootDtor),
            reinterpret_cast<void*>(&HookRootDtor),
            &orig)) {
        g_orig_root_dtor = reinterpret_cast<RootDtorFn>(orig);
    } else {
        LogLine("[RouteB] WARNING: Failed to detour Ogre::Root::~Root (0x184860).");
    }
}

}  // namespace

extern "C" __declspec(dllexport) void dllStartPlugin() {
    g_crash_logged.store(false, std::memory_order_relaxed);
    g_stats_logged.store(false, std::memory_order_relaxed);
    g_cleaned_up.store(false, std::memory_order_relaxed);
    if (!g_log) {
        ::fopen_s(&g_log, "TL2_RouteB_D3D11.log", "w");
    }
    InstallCrashLogger();
    LogLine("[RouteB][Phase1] dllStartPlugin() starting...");

    bool ok_d3d = false;
    bool ok_ogre = false;
    bool ok_gui = false;
    void* d3d_base = ValidateModulePe(kModD3D11, &ok_d3d);
    void* ogre_base = ValidateModulePe(kModOgre, &ok_ogre);
    (void)ValidateModulePe(kModGui, &ok_gui);

    if (ok_d3d && ok_ogre) {
        g_mem_stream_clamps.store(0, std::memory_order_relaxed);
        InstallRootShutdownHook(ogre_base);
        const bool mem_guard_ok = InstallMemoryStreamGuards(ogre_base);
        const bool dxbc_ok = routeb::InstallDxbcCacheHook(d3d_base, &LogLine);
        const bool fp_ok = routeb::InstallPhase1FastPaths(d3d_base, ogre_base, &LogLine);
        const bool p2_ok = routeb::InstallCeguiExplicitShaderHook(d3d_base, ogre_base, &LogLine);
        const bool p3_ok = routeb::InstallPhase3ScenePipeline(d3d_base, ogre_base, &LogLine);
        const bool rc_ok = routeb::InstallRouteCAoPipeline(ogre_base, &LogLine);
        if (rc_ok) {
            routeb::SetPhase3PassObserver(&routeb::OnRouteCScenePassObserved);
        }
        g_phase1_installed = dxbc_ok && fp_ok;
        LogLine("[RouteB][Phase1+2+3] Active status: dxbc_cache=%s fastpaths=%s cegui_shader=%s scene_p3=%s route_c=%s mem_guard=%s",
                dxbc_ok ? "ON" : "OFF",
                fp_ok ? "ON" : "OFF",
                p2_ok ? "ON" : "OFF",
                p3_ok ? "ON" : "OFF",
                rc_ok ? "ON" : "OFF",
                mem_guard_ok ? "ON" : "OFF");
    } else {
        LogLine("[RouteB][Phase1] PE mismatch or RenderSystem_Direct3D11 not loaded; Phase 1+2+3 disabled cleanly.");
    }
}

extern "C" __declspec(dllexport) void dllStopPlugin() {
    g_detour_root_dtor.Remove();
    g_orig_root_dtor = nullptr;
    CleanupHooksAndResources();
    RemoveCrashLogger();

    LogLine("[RouteB][Phase1] dllStopPlugin() cleaned up.");
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
}

extern "C" __declspec(dllexport) bool RouteB_VerifyRouteCShaders() {
    return routeb::VerifyRouteCShaders();
}

extern "C" __declspec(dllexport) void RouteB_TriggerToastForTest() {
    routeb::TriggerRouteCToastForTest();
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        RemoveCrashLogger();
        g_detour_root_dtor.Remove();
        g_orig_root_dtor = nullptr;
        CleanupHooksAndResources();
        if (g_log) {
            std::fclose(g_log);
            g_log = nullptr;
        }
    }
    return TRUE;
}
