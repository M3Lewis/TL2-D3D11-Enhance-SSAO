#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

using PluginFn = void (*)();

constexpr uintptr_t kInitRenderStatesTailRva = 0x00003BD2U;
constexpr uint8_t kExpectedOrigBytes[17] = {
    0x0F, 0x28, 0xB4, 0x24, 0xC0, 0x00, 0x00, 0x00,
    0x48, 0x81, 0xC4, 0xD0, 0x00, 0x00, 0x00,
    0x5B,
    0xC3,
};

}  // namespace

int main() {
    ::SetDllDirectoryW(LR"(H:\Steam\steamapps\Common\Torchlight II)");
    ::SetCurrentDirectoryW(LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\obj)");

    HMODULE ogre = ::LoadLibraryW(L"OgreMain.dll");
    HMODULE d3d  = ::LoadLibraryW(L"RenderSystem_Direct3D11.dll");
    if (!ogre || !d3d) {
        std::printf("FAILED LoadLibrary OgreMain/D3D11 (ogre=%p d3d=%p err=%lu)\n", ogre, d3d, ::GetLastError());
        return 1;
    }

    HMODULE plugin = ::LoadLibraryW(LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\bin\Plugin_TL2_DX11_Enhance_SSAO.dll)");
    if (!plugin) {
        std::printf("FAILED LoadLibrary Plugin_TL2_DX11_Enhance_SSAO.dll err=%lu\n", ::GetLastError());
        return 1;
    }

    auto start_fn = reinterpret_cast<PluginFn>(::GetProcAddress(plugin, "dllStartPlugin"));
    auto stop_fn  = reinterpret_cast<PluginFn>(::GetProcAddress(plugin, "dllStopPlugin"));
    if (!start_fn || !stop_fn) {
        std::printf("FAILED GetProcAddress dllStartPlugin/dllStopPlugin\n");
        return 1;
    }

    // Case A: Lazy load (matches Torchlight2.exe where OgreGUIRenderer loads after plugins).
    start_fn();
    HMODULE gui = ::LoadLibraryW(L"OgreGUIRenderer.dll");
    if (!gui) {
        std::printf("FAILED LoadLibrary OgreGUIRenderer.dll err=%lu\n", ::GetLastError());
        stop_fn();
        return 2;
    }
    const auto* gui_tail = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(gui) + kInitRenderStatesTailRva);
    if (gui_tail[0] != 0xFF || gui_tail[1] != 0x25 || gui_tail[14] != 0x90 || gui_tail[15] != 0x90 || gui_tail[16] != 0x90) {
        std::printf("FAILED Case A: LdrRegisterDllNotification did not patch OgreGUIRenderer+0x3BD2!\n");
        stop_fn();
        return 3;
    }

    stop_fn();
    if (std::memcmp(gui_tail, kExpectedOrigBytes, sizeof(kExpectedOrigBytes)) != 0) {
        std::printf("FAILED Case A: clean restore of OgreGUIRenderer+0x3BD2 failed!\n");
        return 4;
    }

    // Case B: Eager load (OgreGUIRenderer already loaded before dllStartPlugin).
    start_fn();
    if (gui_tail[0] != 0xFF || gui_tail[1] != 0x25) {
        std::printf("FAILED Case B: eager patch of OgreGUIRenderer+0x3BD2 failed!\n");
        stop_fn();
        return 5;
    }
    stop_fn();
    if (std::memcmp(gui_tail, kExpectedOrigBytes, sizeof(kExpectedOrigBytes)) != 0) {
        std::printf("FAILED Case B: clean restore of OgreGUIRenderer+0x3BD2 failed!\n");
        return 6;
    }

    ::FreeLibrary(plugin);
    ::FreeLibrary(gui);
    ::FreeLibrary(d3d);
    ::FreeLibrary(ogre);

    std::printf("[PASS] Phase 2 verification succeeded (lazy LdrNotification + eager patch + clean restore)!\n");
    return 0;
}
