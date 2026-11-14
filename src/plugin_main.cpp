#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

struct ExpectedModule {
    const wchar_t* module_name;
    std::uint32_t expected_timestamp;
    std::uint32_t expected_size_of_image;
};

constexpr ExpectedModule kExpectedModules[] = {
    {L"RenderSystem_Direct3D11.dll", 0x6af5fabfU, 0x0008a000U},
    {L"OgreGUIRenderer.dll",         0x6af5fab0U, 0x00014000U},
    {L"OgreMain.dll",                0x6af5faa0U, 0x006b6000U},
};

FILE* g_log = nullptr;

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

bool ValidateModulePe(const ExpectedModule& spec, std::uintptr_t* out_base) {
    HMODULE mod = ::GetModuleHandleW(spec.module_name);
    if (!mod) {
        LogLine("[RouteB][Phase0] Module not loaded yet: %ls", spec.module_name);
        return false;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        LogLine("[RouteB][Phase0] Invalid DOS header on %ls", spec.module_name);
        return false;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        LogLine("[RouteB][Phase0] Invalid NT header on %ls", spec.module_name);
        return false;
    }

    const std::uint32_t ts = nt->FileHeader.TimeDateStamp;
    const std::uint32_t img_size = nt->OptionalHeader.SizeOfImage;
    const bool matched = (ts == spec.expected_timestamp) && (img_size == spec.expected_size_of_image);

    LogLine(
        "[RouteB][Phase0] %ls base=0x%llx TimeDateStamp=0x%08x (exp 0x%08x) SizeOfImage=0x%x (exp 0x%x) match=%s",
        spec.module_name,
        static_cast<unsigned long long>(base),
        ts,
        spec.expected_timestamp,
        img_size,
        spec.expected_size_of_image,
        matched ? "OK" : "MISMATCH");

    if (out_base) {
        *out_base = base;
    }
    return matched;
}

}  // namespace

extern "C" __declspec(dllexport) void dllStartPlugin() {
    if (!g_log) {
        ::fopen_s(&g_log, "TL2_RouteB_D3D11.log", "w");
    }
    LogLine("[RouteB][Phase0] dllStartPlugin() initialized (Diagnostic Baseline Mode - no hooks installed).");
    for (const auto& spec : kExpectedModules) {
        std::uintptr_t base = 0;
        (void)ValidateModulePe(spec, &base);
    }
}

extern "C" __declspec(dllexport) void dllStopPlugin() {
    LogLine("[RouteB][Phase0] dllStopPlugin() called.");
    if (g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_log) {
        std::fclose(g_log);
        g_log = nullptr;
    }
    return TRUE;
}
