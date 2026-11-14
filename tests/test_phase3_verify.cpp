#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace {

using PluginFn = void (*)();

constexpr uintptr_t kRvaSetPass = 0x1903C0U;
constexpr uintptr_t kRvaGetFixedFuncSrc = 0x352B0U;
constexpr uintptr_t kRvaSetFixedFuncParams = 0x37ED0U;

constexpr uint8_t kExpectedSetPass[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr uint8_t kExpectedGetShaderSrc[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57};
constexpr uint8_t kExpectedSetFfParams[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};

std::string ReadWholeLog(const char* path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

}  // namespace

int main() {
    ::SetDllDirectoryW(LR"(H:\Steam\steamapps\Common\Torchlight II)");
    ::SetCurrentDirectoryW(LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\obj)");

    HMODULE ogre = ::LoadLibraryW(L"OgreMain.dll");
    HMODULE gui = ::LoadLibraryW(L"OgreGUIRenderer.dll");
    HMODULE d3d = ::LoadLibraryW(L"RenderSystem_Direct3D11.dll");
    if (!ogre || !gui || !d3d) {
        std::printf("FAILED LoadLibrary game DLLs (ogre=%p gui=%p d3d=%p err=%lu)\n",
                    ogre, gui, d3d, ::GetLastError());
        return 1;
    }

    HMODULE plugin = ::LoadLibraryW(
        LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\bin\Plugin_TL2_RouteB.dll)");
    if (!plugin) {
        std::printf("FAILED LoadLibrary Plugin_TL2_RouteB.dll err=%lu\n", ::GetLastError());
        return 1;
    }

    auto start_fn = reinterpret_cast<PluginFn>(::GetProcAddress(plugin, "dllStartPlugin"));
    auto stop_fn = reinterpret_cast<PluginFn>(::GetProcAddress(plugin, "dllStopPlugin"));
    if (!start_fn || !stop_fn) {
        std::printf("FAILED GetProcAddress dllStartPlugin/dllStopPlugin\n");
        return 1;
    }

    const auto ogre_base = reinterpret_cast<uintptr_t>(ogre);
    const auto d3d_base = reinterpret_cast<uintptr_t>(d3d);
    const auto* p_setpass = reinterpret_cast<const uint8_t*>(ogre_base + kRvaSetPass);
    const auto* p_getsrc = reinterpret_cast<const uint8_t*>(d3d_base + kRvaGetFixedFuncSrc);
    const auto* p_setff = reinterpret_cast<const uint8_t*>(d3d_base + kRvaSetFixedFuncParams);

    if (std::memcmp(p_setpass, kExpectedSetPass, sizeof(kExpectedSetPass)) != 0 ||
        std::memcmp(p_getsrc, kExpectedGetShaderSrc, sizeof(kExpectedGetShaderSrc)) != 0 ||
        std::memcmp(p_setff, kExpectedSetFfParams, sizeof(kExpectedSetFfParams)) != 0) {
        std::printf("FAILED pre-hook prologue signature mismatch on OgreMain/D3D11\n");
        return 2;
    }

    start_fn();

    if (*reinterpret_cast<const uint16_t*>(p_setpass) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_getsrc) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_setff) != 0x25FF) {
        std::printf("FAILED Phase 3 hook prologues were not patched with FF 25 jmp\n");
        stop_fn();
        return 3;
    }

    stop_fn();

    if (std::memcmp(p_setpass, kExpectedSetPass, sizeof(kExpectedSetPass)) != 0 ||
        std::memcmp(p_getsrc, kExpectedGetShaderSrc, sizeof(kExpectedGetShaderSrc)) != 0 ||
        std::memcmp(p_setff, kExpectedSetFfParams, sizeof(kExpectedSetFfParams)) != 0) {
        std::printf("FAILED clean unhook restoration of Phase 3 prologues\n");
        return 4;
    }

    const std::string log = ReadWholeLog("TL2_RouteB_D3D11.log");
    if (log.find("scene_p3=ON") == std::string::npos ||
        log.find("[RouteB][Phase3][Stats] Scene hooked=1") == std::string::npos) {
        std::printf("FAILED log check: Phase 3 activation/stats missing from TL2_RouteB_D3D11.log\n");
        return 5;
    }

    ::FreeLibrary(plugin);
    ::FreeLibrary(d3d);
    ::FreeLibrary(gui);
    ::FreeLibrary(ogre);

    std::printf("[PASS] Phase 3 verification succeeded (SetPass + GetShaderSource + SetFixedFuncParams hooked & restored cleanly)!\n");
    return 0;
}
