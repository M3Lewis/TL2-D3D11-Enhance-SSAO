#include "dxbc_cache.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <unknwn.h>
#include <d3dcommon.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hook_utils.h"

namespace routeb {

namespace {

constexpr GUID kIidID3D10Blob = {
    0x8ba5fb08, 0x5195, 0x40e2, {0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02}};

class CachedD3DBlob final : public ID3DBlob {
public:
    explicit CachedD3DBlob(std::vector<uint8_t> data) : data_(std::move(data)), ref_count_(1) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) {
            return E_POINTER;
        }
        if (riid == IID_IUnknown || std::memcmp(&riid, &kIidID3D10Blob, sizeof(GUID)) == 0) {
            *ppv = static_cast<ID3DBlob*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(ref_count_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const long next = ref_count_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (next == 0) {
            delete this;
            return 0;
        }
        return static_cast<ULONG>(next);
    }

    LPVOID STDMETHODCALLTYPE GetBufferPointer() override {
        return data_.empty() ? nullptr : data_.data();
    }

    SIZE_T STDMETHODCALLTYPE GetBufferSize() override {
        return data_.size();
    }

private:
    ~CachedD3DBlob() = default;
    std::vector<uint8_t> data_;
    std::atomic<long> ref_count_;
};

using D3DX11CompileFromMemoryFn = HRESULT(WINAPI*)(
    const char* pSrcData,
    SIZE_T SrcDataLen,
    const char* pFileName,
    const D3D_SHADER_MACRO* pDefines,
    void* pInclude,
    const char* pFunctionName,
    const char* pProfile,
    UINT Flags1,
    UINT Flags2,
    void* pPump,
    ID3DBlob** ppShader,
    ID3DBlob** ppErrorMsgs,
    HRESULT* pHResult);

constexpr uint32_t kCacheMagic = 0x43425852U;  // 'RXBC'
constexpr uint32_t kCacheVersion = 1U;

#pragma pack(push, 1)
struct CacheFileHeader {
    uint32_t magic;
    uint32_t version;
    uint64_t hash_hi;
    uint64_t hash_lo;
    uint64_t bytecode_size;
};
#pragma pack(pop)

IatHook g_iat_compile;
D3DX11CompileFromMemoryFn g_orig_compile = nullptr;
LogFn g_log = nullptr;
std::wstring g_cache_dir;
uint64_t g_include_epoch_hi = 0;
uint64_t g_include_epoch_lo = 0;

std::atomic<uint64_t> g_hits{0};
std::atomic<uint64_t> g_misses{0};
std::atomic<uint64_t> g_bypassed{0};
std::atomic<uint64_t> g_write_errors{0};

struct Hash128 {
    uint64_t hi = 0xcbf29ce484222325ULL;
    uint64_t lo = 0x9e3779b97f4a7c15ULL;

    void MixBytes(const void* data, size_t len) {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            hi ^= p[i];
            hi *= 0x100000001b3ULL;
            lo ^= static_cast<uint64_t>(p[i]) + (i * 0x517cc1b727220a95ULL);
            lo = (lo << 13) | (lo >> 51);
            lo *= 0x94d049bb133111ebULL;
        }
    }

    void MixStr(const char* s) {
        if (!s) {
            const uint8_t zero = 0xFF;
            MixBytes(&zero, 1);
            return;
        }
        const size_t n = std::strlen(s);
        MixBytes(&n, sizeof(n));
        MixBytes(s, n);
    }

    template <typename T>
    void MixPod(const T& v) {
        MixBytes(&v, sizeof(v));
    }
};

void ComputeIncludeEpoch(const std::wstring& hlsl4_dir) {
    Hash128 h;
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = hlsl4_dir + L"\\*.hlsl";
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }
            h.MixBytes(fd.cFileName, std::wcslen(fd.cFileName) * sizeof(wchar_t));
            h.MixPod(fd.ftLastWriteTime);
            h.MixPod(fd.nFileSizeLow);
            h.MixPod(fd.nFileSizeHigh);
        } while (::FindNextFileW(find, &fd));
        ::FindClose(find);
    }
    g_include_epoch_hi = h.hi;
    g_include_epoch_lo = h.lo;
}

std::wstring MakeCacheFilePath(const Hash128& h) {
    wchar_t name[64]{};
    std::swprintf(
        name,
        sizeof(name) / sizeof(name[0]),
        L"\\%016llx%016llx.dxbc",
        static_cast<unsigned long long>(h.hi),
        static_cast<unsigned long long>(h.lo));
    return g_cache_dir + name;
}

bool TryReadCachedBlob(const std::wstring& path, const Hash128& expected, std::vector<uint8_t>* out_bytes) {
    HANDLE file = ::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER file_size{};
    if (!::GetFileSizeEx(file, &file_size) || file_size.QuadPart < static_cast<LONGLONG>(sizeof(CacheFileHeader)) ||
        file_size.QuadPart > 8LL * 1024 * 1024) {
        ::CloseHandle(file);
        return false;
    }

    CacheFileHeader hdr{};
    DWORD read_bytes = 0;
    if (!::ReadFile(file, &hdr, sizeof(hdr), &read_bytes, nullptr) || read_bytes != sizeof(hdr)) {
        ::CloseHandle(file);
        return false;
    }

    if (hdr.magic != kCacheMagic || hdr.version != kCacheVersion || hdr.hash_hi != expected.hi ||
        hdr.hash_lo != expected.lo || hdr.bytecode_size == 0 ||
        sizeof(CacheFileHeader) + hdr.bytecode_size != static_cast<uint64_t>(file_size.QuadPart)) {
        ::CloseHandle(file);
        return false;
    }

    out_bytes->resize(static_cast<size_t>(hdr.bytecode_size));
    if (!::ReadFile(file, out_bytes->data(), static_cast<DWORD>(hdr.bytecode_size), &read_bytes, nullptr) ||
        read_bytes != static_cast<DWORD>(hdr.bytecode_size)) {
        ::CloseHandle(file);
        out_bytes->clear();
        return false;
    }

    ::CloseHandle(file);
    return true;
}

bool WriteCachedBlobAtomic(const std::wstring& path, const Hash128& hash, const void* data, size_t size) {
    if (!data || size == 0 || g_cache_dir.empty()) {
        return false;
    }
    const std::wstring tmp_path = path + L".tmp";
    HANDLE file = ::CreateFileW(
        tmp_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    CacheFileHeader hdr{};
    hdr.magic = kCacheMagic;
    hdr.version = kCacheVersion;
    hdr.hash_hi = hash.hi;
    hdr.hash_lo = hash.lo;
    hdr.bytecode_size = static_cast<uint64_t>(size);

    DWORD written = 0;
    const bool ok =
        ::WriteFile(file, &hdr, sizeof(hdr), &written, nullptr) && written == sizeof(hdr) &&
        ::WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) &&
        written == static_cast<DWORD>(size);
    ::CloseHandle(file);

    if (!ok) {
        return false;
    }
    return ::MoveFileExW(tmp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

HRESULT WINAPI HookD3DX11CompileFromMemory(
    const char* pSrcData,
    SIZE_T SrcDataLen,
    const char* pFileName,
    const D3D_SHADER_MACRO* pDefines,
    void* pInclude,
    const char* pFunctionName,
    const char* pProfile,
    UINT Flags1,
    UINT Flags2,
    void* pPump,
    ID3DBlob** ppShader,
    ID3DBlob** ppErrorMsgs,
    HRESULT* pHResult) {
    if (!pSrcData || SrcDataLen == 0 || pPump != nullptr || !ppShader) {
        g_bypassed.fetch_add(1, std::memory_order_relaxed);
        return g_orig_compile(
            pSrcData, SrcDataLen, pFileName, pDefines, pInclude, pFunctionName, pProfile, Flags1, Flags2, pPump, ppShader, ppErrorMsgs, pHResult);
    }

    Hash128 h;
    h.MixPod(g_include_epoch_hi);
    h.MixPod(g_include_epoch_lo);
    h.MixPod(SrcDataLen);
    h.MixBytes(pSrcData, SrcDataLen);
    h.MixStr(pFunctionName);
    h.MixStr(pProfile);
    h.MixPod(Flags1);
    h.MixPod(Flags2);
    if (pDefines) {
        for (const D3D_SHADER_MACRO* m = pDefines; m->Name != nullptr; ++m) {
            h.MixStr(m->Name);
            h.MixStr(m->Definition);
        }
    }

    const std::wstring cache_path = MakeCacheFilePath(h);
    std::vector<uint8_t> cached_bytes;
    if (TryReadCachedBlob(cache_path, h, &cached_bytes)) {
        if (ppErrorMsgs) {
            *ppErrorMsgs = nullptr;
        }
        *ppShader = new CachedD3DBlob(std::move(cached_bytes));
        if (pHResult) {
            *pHResult = S_OK;
        }
        g_hits.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }

    g_misses.fetch_add(1, std::memory_order_relaxed);
    const HRESULT hr = g_orig_compile(
        pSrcData, SrcDataLen, pFileName, pDefines, pInclude, pFunctionName, pProfile, Flags1, Flags2, pPump, ppShader, ppErrorMsgs, pHResult);

    if (SUCCEEDED(hr) && *ppShader != nullptr) {
        const void* ptr = (*ppShader)->GetBufferPointer();
        const SIZE_T sz = (*ppShader)->GetBufferSize();
        if (!WriteCachedBlobAtomic(cache_path, h, ptr, sz)) {
            g_write_errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return hr;
}

}  // namespace

bool InstallDxbcCacheHook(void* d3d11_module_base, LogFn logger) {
    g_log = logger;

    wchar_t exe_path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    std::wstring base_dir(exe_path);
    const size_t slash = base_dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        base_dir.resize(slash);
    }

    const std::wstring programs_dir = base_dir + L"\\programs";
    const std::wstring hlsl4_dir = programs_dir + L"\\hlsl4";
    ::CreateDirectoryW(programs_dir.c_str(), nullptr);
    ::CreateDirectoryW(hlsl4_dir.c_str(), nullptr);
    g_cache_dir = hlsl4_dir + L"\\_dxbc_cache";
    ::CreateDirectoryW(g_cache_dir.c_str(), nullptr);

    ComputeIncludeEpoch(hlsl4_dir);

    void* orig = nullptr;
    const bool ok = g_iat_compile.Install(
        d3d11_module_base,
        "d3dx11_43.dll",
        "D3DX11CompileFromMemory",
        reinterpret_cast<void*>(&HookD3DX11CompileFromMemory),
        &orig);
    if (ok) {
        g_orig_compile = reinterpret_cast<D3DX11CompileFromMemoryFn>(orig);
        if (g_log) {
            char dir_utf8[MAX_PATH * 3]{};
            ::WideCharToMultiByte(
                CP_UTF8,
                0,
                g_cache_dir.c_str(),
                -1,
                dir_utf8,
                sizeof(dir_utf8),
                nullptr,
                nullptr);
            g_log(
                "[RouteB][Phase1B] DXBC cache IAT hook installed (orig=%p, dir=%s, epoch=%016llx)",
                orig,
                dir_utf8,
                static_cast<unsigned long long>(g_include_epoch_hi));
        }
    } else if (g_log) {
        g_log("[RouteB][Phase1B] FAILED to install D3DX11CompileFromMemory IAT hook");
    }
    return ok;
}

void RemoveDxbcCacheHook() {
    g_iat_compile.Remove();
    g_orig_compile = nullptr;
}

DxbcCacheStats GetDxbcCacheStats() {
    DxbcCacheStats s{};
    s.hits = g_hits.load(std::memory_order_relaxed);
    s.misses = g_misses.load(std::memory_order_relaxed);
    s.bypassed = g_bypassed.load(std::memory_order_relaxed);
    s.write_errors = g_write_errors.load(std::memory_order_relaxed);
    return s;
}

}  // namespace routeb
