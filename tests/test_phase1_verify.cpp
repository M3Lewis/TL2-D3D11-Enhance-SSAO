#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <d3dcommon.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

using PluginFn = void (*)();
using D3DX11CompileFromMemoryFn = HRESULT(WINAPI*)(
    const char*, SIZE_T, const char*, const D3D_SHADER_MACRO*, void*,
    const char*, const char*, UINT, UINT, void*, ID3DBlob**, ID3DBlob**, HRESULT*);

void** FindIatSlot(HMODULE mod, const char* dll_name, const char* func_name) {
    const auto base = reinterpret_cast<uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        if (::_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll_name) != 0) continue;
        const auto* ot = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + desc->OriginalFirstThunk);
        auto* ft = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + desc->FirstThunk);
        for (; ot->u1.AddressOfData != 0; ++ot, ++ft) {
            if (IMAGE_SNAP_BY_ORDINAL64(ot->u1.Ordinal)) continue;
            const auto* imp = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + ot->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(imp->Name), func_name) == 0) {
                return reinterpret_cast<void**>(&ft->u1.Function);
            }
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    ::SetDllDirectoryW(LR"(H:\Steam\steamapps\Common\Torchlight II)");
    ::SetCurrentDirectoryW(LR"(J:\Software\TL2-RE\自制2x高清mod 20260705\源文件\mods\tl2_d3d11_route_b\obj)");

    HMODULE ogre = ::LoadLibraryW(L"OgreMain.dll");
    HMODULE gui  = ::LoadLibraryW(L"OgreGUIRenderer.dll");
    HMODULE d3d  = ::LoadLibraryW(L"RenderSystem_Direct3D11.dll");
    if (!ogre || !gui || !d3d) {
        std::printf("FAILED LoadLibrary game DLLs (ogre=%p gui=%p d3d=%p err=%lu)\n", ogre, gui, d3d, ::GetLastError());
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

    // Snapshot original bytes at patched addresses before hook
    uint8_t pre_8260[14]{};
    uint8_t pre_17539[16]{};
    uint8_t pre_176d6[17]{};
    uint8_t pre_mem_read[16]{};
    uint8_t pre_mem_seek[16]{};
    uint8_t pre_mem_skip[16]{};
    const auto d3d_base = reinterpret_cast<uintptr_t>(d3d);
    const void* p_mem_read = reinterpret_cast<const void*>(::GetProcAddress(ogre, "?read@MemoryDataStream@Ogre@@UEAA_KPEAX_K@Z"));
    const void* p_mem_seek = reinterpret_cast<const void*>(::GetProcAddress(ogre, "?seek@MemoryDataStream@Ogre@@UEAAX_K@Z"));
    const void* p_mem_skip = reinterpret_cast<const void*>(::GetProcAddress(ogre, "?skip@MemoryDataStream@Ogre@@UEAAXJ@Z"));
    if (!p_mem_read || !p_mem_seek || !p_mem_skip) {
        std::printf("FAILED GetProcAddress for MemoryDataStream exports\n");
        return 1;
    }
    std::memcpy(pre_8260,     reinterpret_cast<const void*>(d3d_base + 0x8260),   sizeof(pre_8260));
    std::memcpy(pre_17539,    reinterpret_cast<const void*>(d3d_base + 0x17539),  sizeof(pre_17539));
    std::memcpy(pre_176d6,    reinterpret_cast<const void*>(d3d_base + 0x176D6),  sizeof(pre_176d6));
    std::memcpy(pre_mem_read, p_mem_read, sizeof(pre_mem_read));
    std::memcpy(pre_mem_seek, p_mem_seek, sizeof(pre_mem_seek));
    std::memcpy(pre_mem_skip, p_mem_skip, sizeof(pre_mem_skip));

    start_fn();

    // Verify patches actually rewrote target prologues with FF 25 jmp
    if (*reinterpret_cast<const uint16_t*>(d3d_base + 0x8260) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(d3d_base + 0x17539) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(d3d_base + 0x176D6) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_mem_read) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_mem_seek) != 0x25FF ||
        *reinterpret_cast<const uint16_t*>(p_mem_skip) != 0x25FF) {
        std::printf("FAILED patch verification: target prologues not patched!\n");
        stop_fn();
        return 2;
    }

    // Exercise Ogre::MemoryDataStream bounds clamping against Frost Hills DDSCodec overshoot bug
    using MemCtorFn = void(__fastcall*)(void*, void*, size_t, bool, bool);
    using MemDtorFn = void(__fastcall*)(void*);
    using MemReadFn = size_t(__fastcall*)(void*, void*, size_t);
    using MemSeekFn = void(__fastcall*)(void*, size_t);
    using MemSkipFn = void(__fastcall*)(void*, long);
    using MemTellFn = size_t(__fastcall*)(const void*);
    auto mem_ctor = reinterpret_cast<MemCtorFn>(::GetProcAddress(ogre, "??0MemoryDataStream@Ogre@@QEAA@PEAX_K_N2@Z"));
    auto mem_dtor = reinterpret_cast<MemDtorFn>(::GetProcAddress(ogre, "??1MemoryDataStream@Ogre@@UEAA@XZ"));
    auto mem_read = reinterpret_cast<MemReadFn>(::GetProcAddress(ogre, "?read@MemoryDataStream@Ogre@@UEAA_KPEAX_K@Z"));
    auto mem_seek = reinterpret_cast<MemSeekFn>(::GetProcAddress(ogre, "?seek@MemoryDataStream@Ogre@@UEAAX_K@Z"));
    auto mem_skip = reinterpret_cast<MemSkipFn>(::GetProcAddress(ogre, "?skip@MemoryDataStream@Ogre@@UEAAXJ@Z"));
    auto mem_tell = reinterpret_cast<MemTellFn>(::GetProcAddress(ogre, "?tell@MemoryDataStream@Ogre@@UEBA_KXZ"));
    if (!mem_ctor || !mem_dtor || !mem_read || !mem_seek || !mem_skip || !mem_tell) {
        std::printf("FAILED GetProcAddress on Ogre::MemoryDataStream exports\n");
        stop_fn();
        return 8;
    }
    uint8_t src_bytes[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    uint8_t dst_bytes[16]{};
    alignas(16) uint8_t stream_obj[128]{};
    mem_ctor(stream_obj, src_bytes, sizeof(src_bytes), false, true);
    mem_skip(stream_obj, 85);  // Previously jumped past end and made subsequent read() call memcpy(-69)
    if (mem_tell(stream_obj) != sizeof(src_bytes) || mem_read(stream_obj, dst_bytes, 16) != 0) {
        std::printf("FAILED MemoryDataStream forward skip clamp check!\n");
        mem_dtor(stream_obj);
        stop_fn();
        return 9;
    }
    mem_skip(stream_obj, -999);
    if (mem_tell(stream_obj) != 0 || mem_read(stream_obj, dst_bytes, 4) != 4 ||
        dst_bytes[0] != 1 || dst_bytes[3] != 4) {
        std::printf("FAILED MemoryDataStream negative skip clamp / read check!\n");
        mem_dtor(stream_obj);
        stop_fn();
        return 10;
    }
    mem_seek(stream_obj, 9999);
    if (mem_tell(stream_obj) != sizeof(src_bytes) || mem_read(stream_obj, dst_bytes, 4) != 0) {
        std::printf("FAILED MemoryDataStream seek clamp check!\n");
        mem_dtor(stream_obj);
        stop_fn();
        return 11;
    }
    mem_dtor(stream_obj);

    // Exercise IAT-hooked D3DX11CompileFromMemory (1st miss -> 2nd hit)
    void** iat_slot = FindIatSlot(d3d, "d3dx11_43.dll", "D3DX11CompileFromMemory");
    if (!iat_slot || !*iat_slot) {
        std::printf("FAILED to locate D3DX11CompileFromMemory IAT slot\n");
        stop_fn();
        return 3;
    }

    auto compile_fn = reinterpret_cast<D3DX11CompileFromMemoryFn>(*iat_slot);
    static constexpr const char kTestHlsl[] =
        "float4 main(float4 pos : POSITION) : SV_POSITION { return pos; }";

    ID3DBlob* blob1 = nullptr;
    ID3DBlob* err1 = nullptr;
    HRESULT hr1 = compile_fn(kTestHlsl, sizeof(kTestHlsl) - 1, nullptr, nullptr, nullptr, "main", "vs_4_0", 4096, 0, nullptr, &blob1, &err1, nullptr);
    if (FAILED(hr1) || !blob1) {
        std::printf("FAILED compile #1 hr=0x%08lx\n", static_cast<unsigned long>(hr1));
        if (err1) err1->Release();
        stop_fn();
        return 4;
    }

    std::vector<uint8_t> bytes1(
        static_cast<const uint8_t*>(blob1->GetBufferPointer()),
        static_cast<const uint8_t*>(blob1->GetBufferPointer()) + blob1->GetBufferSize());
    blob1->Release();
    if (err1) err1->Release();

    ID3DBlob* blob2 = nullptr;
    ID3DBlob* err2 = nullptr;
    HRESULT hr2 = compile_fn(kTestHlsl, sizeof(kTestHlsl) - 1, nullptr, nullptr, nullptr, "main", "vs_4_0", 4096, 0, nullptr, &blob2, &err2, nullptr);
    if (FAILED(hr2) || !blob2) {
        std::printf("FAILED compile #2 (cache hit) hr=0x%08lx\n", static_cast<unsigned long>(hr2));
        if (err2) err2->Release();
        stop_fn();
        return 5;
    }

    const bool same_bytes =
        blob2->GetBufferSize() == bytes1.size() &&
        std::memcmp(blob2->GetBufferPointer(), bytes1.data(), bytes1.size()) == 0;
    blob2->Release();
    if (err2) err2->Release();

    if (!same_bytes) {
        std::printf("FAILED DXBC cached blob mismatch!\n");
        stop_fn();
        return 6;
    }

    stop_fn();

    // Verify clean restoration of original bytes
    if (std::memcmp(pre_8260,     reinterpret_cast<const void*>(d3d_base + 0x8260),   sizeof(pre_8260)) != 0 ||
        std::memcmp(pre_17539,    reinterpret_cast<const void*>(d3d_base + 0x17539),  sizeof(pre_17539)) != 0 ||
        std::memcmp(pre_176d6,    reinterpret_cast<const void*>(d3d_base + 0x176D6),  sizeof(pre_176d6)) != 0 ||
        std::memcmp(pre_mem_read, p_mem_read, sizeof(pre_mem_read)) != 0 ||
        std::memcmp(pre_mem_seek, p_mem_seek, sizeof(pre_mem_seek)) != 0 ||
        std::memcmp(pre_mem_skip, p_mem_skip, sizeof(pre_mem_skip)) != 0) {
        std::printf("FAILED clean unhook byte restoration!\n");
        return 7;
    }

    ::FreeLibrary(plugin);
    ::FreeLibrary(d3d);
    ::FreeLibrary(gui);
    ::FreeLibrary(ogre);

    std::printf("[PASS] Phase 1 verification succeeded! DXBC size=%zu bytes.\n", bytes1.size());
    return 0;
}
