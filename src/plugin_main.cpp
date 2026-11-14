#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "cegui_explicit_shader.h"
#include "dxbc_cache.h"
#include "hlsl_cbuffer_fastpath.h"
#include "phase3_scene_pipeline.h"

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

}  // namespace

extern "C" __declspec(dllexport) void dllStartPlugin() {
    if (!g_log) {
        ::fopen_s(&g_log, "TL2_RouteB_D3D11.log", "w");
    }
    LogLine("[RouteB][Phase1] dllStartPlugin() starting...");

    bool ok_d3d = false;
    bool ok_ogre = false;
    bool ok_gui = false;
    void* d3d_base = ValidateModulePe(kModD3D11, &ok_d3d);
    void* ogre_base = ValidateModulePe(kModOgre, &ok_ogre);
    (void)ValidateModulePe(kModGui, &ok_gui);

    if (ok_d3d && ok_ogre) {
        const bool dxbc_ok = routeb::InstallDxbcCacheHook(d3d_base, &LogLine);
        const bool fp_ok = routeb::InstallPhase1FastPaths(d3d_base, ogre_base, &LogLine);
        const bool p2_ok = routeb::InstallCeguiExplicitShaderHook(d3d_base, ogre_base, &LogLine);
        const bool p3_ok = routeb::InstallPhase3ScenePipeline(d3d_base, ogre_base, &LogLine);
        g_phase1_installed = dxbc_ok && fp_ok;
        LogLine("[RouteB][Phase1+2+3] Active status: dxbc_cache=%s fastpaths=%s cegui_shader=%s scene_p3=%s",
                dxbc_ok ? "ON" : "OFF",
                fp_ok ? "ON" : "OFF",
                p2_ok ? "ON" : "OFF",
                p3_ok ? "ON" : "OFF");
    } else {
        LogLine("[RouteB][Phase1] PE mismatch or RenderSystem_Direct3D11 not loaded; Phase 1+2+3 disabled cleanly.");
    }
}

extern "C" __declspec(dllexport) void dllStopPlugin() {
    if (g_log) {
        const auto dxbc = routeb::GetDxbcCacheStats();
        const auto fp = routeb::GetFastPathStats();
        LogLine(
            "[RouteB][Phase1][Stats] DXBC hits=%llu misses=%llu bypassed=%llu writeErr=%llu",
            static_cast<unsigned long long>(dxbc.hits),
            static_cast<unsigned long long>(dxbc.misses),
            static_cast<unsigned long long>(dxbc.bypassed),
            static_cast<unsigned long long>(dxbc.write_errors));
        LogLine(
            "[RouteB][Phase1][Stats] CB fastHits=%llu fallback=%llu | FFParam hits=%llu misses=%llu skippedMissing=%llu p3Fwd=%llu",
            static_cast<unsigned long long>(fp.cb_fast_hits),
            static_cast<unsigned long long>(fp.cb_fallback_calls),
            static_cast<unsigned long long>(fp.ff_param_hits),
            static_cast<unsigned long long>(fp.ff_param_misses),
            static_cast<unsigned long long>(fp.ff_param_skipped_missing),
            static_cast<unsigned long long>(fp.p3_param_forwards));
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
            "[RouteB][Phase3][Stats] Scene hooked=%d shared=%d setPass=%llu diffOpaque=%llu diffAlpha=%llu skinned=%llu light=%llu rim2D=%llu rimCube=%llu vsInject=%llu ffSync=%llu",
            p3.hooked ? 1 : 0,
            p3.shared_params_ready ? 1 : 0,
            static_cast<unsigned long long>(p3.setpass_calls),
            static_cast<unsigned long long>(p3.diffpass_opaque_binds),
            static_cast<unsigned long long>(p3.diffpass_alpha_binds),
            static_cast<unsigned long long>(p3.skinned_diff_binds),
            static_cast<unsigned long long>(p3.lightpass_binds),
            static_cast<unsigned long long>(p3.rimlit_2d_binds),
            static_cast<unsigned long long>(p3.rimlit_cube_binds),
            static_cast<unsigned long long>(p3.vs_texcoord_injections),
            static_cast<unsigned long long>(p3.ff_explicit_syncs));
    }

    routeb::RemovePhase3ScenePipeline();
    routeb::RemoveCeguiExplicitShaderHook();
    routeb::RemovePhase1FastPaths();
    routeb::RemoveDxbcCacheHook();
    g_phase1_installed = false;

    LogLine("[RouteB][Phase1] dllStopPlugin() cleaned up.");
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        routeb::RemovePhase3ScenePipeline();
        routeb::RemoveCeguiExplicitShaderHook();
        routeb::RemovePhase1FastPaths();
        routeb::RemoveDxbcCacheHook();
        if (g_log) {
            std::fclose(g_log);
            g_log = nullptr;
        }
    }
    return TRUE;
}
