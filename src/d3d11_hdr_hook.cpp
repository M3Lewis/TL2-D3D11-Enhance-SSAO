#include "d3d11_hdr_hook.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <mutex>

#include "hook_utils.h"

namespace routeb {
namespace {

using FnD3D11CreateDevice = HRESULT(WINAPI*)(
    IDXGIAdapter*,
    D3D_DRIVER_TYPE,
    HMODULE,
    UINT,
    const D3D_FEATURE_LEVEL*,
    UINT,
    UINT,
    ID3D11Device**,
    D3D_FEATURE_LEVEL*,
    ID3D11DeviceContext**);

using FnCreateTexture2D = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*,
    const D3D11_TEXTURE2D_DESC*,
    const D3D11_SUBRESOURCE_DATA*,
    ID3D11Texture2D**);

using FnCreateShaderResourceView = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*,
    ID3D11Resource*,
    const D3D11_SHADER_RESOURCE_VIEW_DESC*,
    ID3D11ShaderResourceView**);

using FnCreateRenderTargetView = HRESULT(STDMETHODCALLTYPE*)(
    ID3D11Device*,
    ID3D11Resource*,
    const D3D11_RENDER_TARGET_VIEW_DESC*,
    ID3D11RenderTargetView**);

IatHook g_iat_d3d11_create;
FnD3D11CreateDevice g_orig_d3d11_create = nullptr;

std::mutex g_vtable_mu;
void** g_hooked_vtable = nullptr;
FnCreateTexture2D g_orig_create_tex2d = nullptr;
FnCreateShaderResourceView g_orig_create_srv = nullptr;
FnCreateRenderTargetView g_orig_create_rtv = nullptr;

PostLogFn g_hdr_log = nullptr;
std::atomic<int> g_upgraded_tex_count{0};
std::atomic<bool> g_logged_first_upgrade{false};
thread_local int g_tls_bypass_depth = 0;

bool Is8BitSceneColorFormat(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_B8G8R8X8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

bool ShouldUpgradeCompositorRtt(
    const D3D11_TEXTURE2D_DESC* desc,
    const D3D11_SUBRESOURCE_DATA* initial_data) {
    if (!desc || initial_data != nullptr || g_tls_bypass_depth > 0) {
        return false;
    }
    if (desc->ArraySize != 1 || desc->MipLevels != 1) {
        return false;
    }
    if (desc->Usage != D3D11_USAGE_DEFAULT || desc->CPUAccessFlags != 0) {
        return false;
    }
    if ((desc->BindFlags & D3D11_BIND_DEPTH_STENCIL) != 0) {
        return false;
    }
    if ((desc->BindFlags & D3D11_BIND_RENDER_TARGET) == 0 ||
        (desc->BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0) {
        return false;
    }
    if ((desc->MiscFlags & (D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX)) != 0) {
        return false;
    }
    if (!Is8BitSceneColorFormat(desc->Format)) {
        return false;
    }
    // Compositor RTTs: full/half/quarter-res screen targets (e.g. 1920x1080, 960x540, 480x270, 320x180).
    // Skip tiny UI targets and square <=1024x1024 minimap/portrait RTTs.
    if (desc->Width < 256 || desc->Height < 144) {
        return false;
    }
    if (desc->Width == desc->Height && desc->Width <= 1024) {
        return false;
    }
    return true;
}

bool IsResourceUpgradedFloat16Tex2D(ID3D11Resource* res) {
    if (!res) {
        return false;
    }
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) || !tex) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);
    tex->Release();
    return td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

HRESULT STDMETHODCALLTYPE HookedCreateTexture2D(
    ID3D11Device* This,
    const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA* pInitialData,
    ID3D11Texture2D** ppTexture2D) {
    if (!g_orig_create_tex2d) {
        return E_FAIL;
    }
    if (ppTexture2D && ShouldUpgradeCompositorRtt(pDesc, pInitialData)) {
        D3D11_TEXTURE2D_DESC upgraded = *pDesc;
        upgraded.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const HRESULT hr = g_orig_create_tex2d(This, &upgraded, pInitialData, ppTexture2D);
        if (SUCCEEDED(hr) && *ppTexture2D) {
            const int count = g_upgraded_tex_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (g_hdr_log && !g_logged_first_upgrade.exchange(true, std::memory_order_relaxed)) {
                g_hdr_log("[HDR] Upgraded compositor RTT %ux%u fmt %u -> R16G16B16A16_FLOAT (count=%d)",
                          upgraded.Width, upgraded.Height, static_cast<unsigned>(pDesc->Format), count);
            }
            return hr;
        }
    }
    return g_orig_create_tex2d(This, pDesc, pInitialData, ppTexture2D);
}

HRESULT STDMETHODCALLTYPE HookedCreateShaderResourceView(
    ID3D11Device* This,
    ID3D11Resource* pResource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc,
    ID3D11ShaderResourceView** ppSRView) {
    if (!g_orig_create_srv) {
        return E_FAIL;
    }
    if (pResource && pDesc && Is8BitSceneColorFormat(pDesc->Format) && IsResourceUpgradedFloat16Tex2D(pResource)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC fixed = *pDesc;
        fixed.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return g_orig_create_srv(This, pResource, &fixed, ppSRView);
    }
    return g_orig_create_srv(This, pResource, pDesc, ppSRView);
}

HRESULT STDMETHODCALLTYPE HookedCreateRenderTargetView(
    ID3D11Device* This,
    ID3D11Resource* pResource,
    const D3D11_RENDER_TARGET_VIEW_DESC* pDesc,
    ID3D11RenderTargetView** ppRTView) {
    if (!g_orig_create_rtv) {
        return E_FAIL;
    }
    if (pResource && pDesc && Is8BitSceneColorFormat(pDesc->Format) && IsResourceUpgradedFloat16Tex2D(pResource)) {
        D3D11_RENDER_TARGET_VIEW_DESC fixed = *pDesc;
        fixed.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        return g_orig_create_rtv(This, pResource, &fixed, ppRTView);
    }
    return g_orig_create_rtv(This, pResource, pDesc, ppRTView);
}

bool WriteVtableSlot(void** slot, void* new_fn) {
    DWORD old_prot = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_prot)) {
        return false;
    }
    *slot = new_fn;
    ::VirtualProtect(slot, sizeof(void*), old_prot, &old_prot);
    ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

HRESULT WINAPI HookedD3D11CreateDevice(
    IDXGIAdapter* pAdapter,
    D3D_DRIVER_TYPE DriverType,
    HMODULE Software,
    UINT Flags,
    const D3D_FEATURE_LEVEL* pFeatureLevels,
    UINT FeatureLevels,
    UINT SDKVersion,
    ID3D11Device** ppDevice,
    D3D_FEATURE_LEVEL* pFeatureLevel,
    ID3D11DeviceContext** ppImmediateContext) {
    const HRESULT hr = g_orig_d3d11_create
        ? g_orig_d3d11_create(
              pAdapter, DriverType, Software, Flags,
              pFeatureLevels, FeatureLevels, SDKVersion,
              ppDevice, pFeatureLevel, ppImmediateContext)
        : E_FAIL;
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        EnsureHdrDeviceVtableHooked(*ppDevice, g_hdr_log);
    }
    return hr;
}

}  // namespace

bool InstallHdrDeviceIatHook(PostLogFn logger) {
    g_hdr_log = logger;
    HMODULE rs = ::GetModuleHandleA("RenderSystem_Direct3D11.dll");
    if (!rs) {
        if (logger) {
            logger("[HDR] RenderSystem_Direct3D11.dll not loaded at InstallHdrDeviceIatHook");
        }
        return false;
    }
    if (g_iat_d3d11_create.IsInstalled()) {
        return true;
    }
    const bool ok = g_iat_d3d11_create.Install(
        rs,
        "d3d11.dll",
        "D3D11CreateDevice",
        reinterpret_cast<void*>(&HookedD3D11CreateDevice),
        reinterpret_cast<void**>(&g_orig_d3d11_create));
    if (logger) {
        logger("[HDR] D3D11CreateDevice IAT hook install=%s", ok ? "OK" : "FAIL");
    }
    return ok;
}

void RemoveHdrDeviceIatHook() {
    {
        std::lock_guard<std::mutex> lock(g_vtable_mu);
        if (g_hooked_vtable) {
            if (g_orig_create_tex2d && g_hooked_vtable[5] == reinterpret_cast<void*>(&HookedCreateTexture2D)) {
                WriteVtableSlot(&g_hooked_vtable[5], reinterpret_cast<void*>(g_orig_create_tex2d));
            }
            if (g_orig_create_srv && g_hooked_vtable[7] == reinterpret_cast<void*>(&HookedCreateShaderResourceView)) {
                WriteVtableSlot(&g_hooked_vtable[7], reinterpret_cast<void*>(g_orig_create_srv));
            }
            if (g_orig_create_rtv && g_hooked_vtable[9] == reinterpret_cast<void*>(&HookedCreateRenderTargetView)) {
                WriteVtableSlot(&g_hooked_vtable[9], reinterpret_cast<void*>(g_orig_create_rtv));
            }
            g_hooked_vtable = nullptr;
            g_orig_create_tex2d = nullptr;
            g_orig_create_srv = nullptr;
            g_orig_create_rtv = nullptr;
        }
    }
    g_iat_d3d11_create.Remove();
    g_orig_d3d11_create = nullptr;
    g_upgraded_tex_count.store(0, std::memory_order_relaxed);
    g_logged_first_upgrade.store(false, std::memory_order_relaxed);
}

void EnsureHdrDeviceVtableHooked(ID3D11Device* dev, PostLogFn logger) {
    if (!dev) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_vtable_mu);
    if (logger && !g_hdr_log) {
        g_hdr_log = logger;
    }
    void** vtable = *reinterpret_cast<void***>(dev);
    if (!vtable || g_hooked_vtable == vtable) {
        return;
    }
    // Check slots 5 (CreateTexture2D), 7 (CreateShaderResourceView), 9 (CreateRenderTargetView)
    if (vtable[5] == reinterpret_cast<void*>(&HookedCreateTexture2D)) {
        g_hooked_vtable = vtable;
        return;
    }
    g_orig_create_tex2d = reinterpret_cast<FnCreateTexture2D>(vtable[5]);
    g_orig_create_srv = reinterpret_cast<FnCreateShaderResourceView>(vtable[7]);
    g_orig_create_rtv = reinterpret_cast<FnCreateRenderTargetView>(vtable[9]);

    const bool ok5 = WriteVtableSlot(&vtable[5], reinterpret_cast<void*>(&HookedCreateTexture2D));
    const bool ok7 = WriteVtableSlot(&vtable[7], reinterpret_cast<void*>(&HookedCreateShaderResourceView));
    const bool ok9 = WriteVtableSlot(&vtable[9], reinterpret_cast<void*>(&HookedCreateRenderTargetView));
    if (ok5 && ok7 && ok9) {
        g_hooked_vtable = vtable;
        if (g_hdr_log) {
            g_hdr_log("[HDR] Hooked ID3D11Device vtable (CreateTexture2D/SRV/RTV)");
        }
    } else {
        if (ok5) WriteVtableSlot(&vtable[5], reinterpret_cast<void*>(g_orig_create_tex2d));
        if (ok7) WriteVtableSlot(&vtable[7], reinterpret_cast<void*>(g_orig_create_srv));
        if (ok9) WriteVtableSlot(&vtable[9], reinterpret_cast<void*>(g_orig_create_rtv));
        g_orig_create_tex2d = nullptr;
        g_orig_create_srv = nullptr;
        g_orig_create_rtv = nullptr;
        if (g_hdr_log) {
            g_hdr_log("[HDR] Failed to patch ID3D11Device vtable");
        }
    }
}

void BeginHdrUpgradeBypass() {
    ++g_tls_bypass_depth;
}

void EndHdrUpgradeBypass() {
    if (g_tls_bypass_depth > 0) {
        --g_tls_bypass_depth;
    }
}

bool IsHdrPipelineActive() {
    return g_upgraded_tex_count.load(std::memory_order_relaxed) > 0;
}

int GetHdrUpgradedTextureCount() {
    return g_upgraded_tex_count.load(std::memory_order_relaxed);
}

}  // namespace routeb
