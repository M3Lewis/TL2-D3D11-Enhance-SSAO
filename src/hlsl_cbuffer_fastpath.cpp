#include "hlsl_cbuffer_fastpath.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include "hook_utils.h"

namespace routeb {

namespace {

// Verified RVAs for Torchlight II 2026 x64 (TimeDateStamp 0x6af5fabf / 0x6af5faa0)
constexpr uintptr_t kRvaGetConstantBuffer = 0x08260;
constexpr uintptr_t kRvaRenderLoopEntry = 0x17539;
constexpr uintptr_t kRvaRenderLoopSkipTarget = 0x176C1;
constexpr uintptr_t kRvaRenderLoopResume = 0x17549;
constexpr uintptr_t kRvaRenderCopyCall = 0x176D6;
constexpr uintptr_t kRvaRenderCopyResume = 0x176E7;
constexpr uintptr_t kRvaCopyTextureLayers = 0x33010;
constexpr uintptr_t kRvaSetParamHelper = 0x320E0;
constexpr uintptr_t kRvaSetVec3Helper = 0x32180;
constexpr uintptr_t kRvaStringDtor = 0x01960;
constexpr uintptr_t kRvaOgreGpuParamsDtor = 0x116C0;

constexpr uint8_t kExpected8260[14] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};
constexpr uint8_t kExpected17539[16] = {
    0x4C, 0x89, 0x75, 0x18, 0x49, 0x8D, 0x9F, 0x48, 0x10, 0x00, 0x00, 0xBE, 0x10, 0x00, 0x00, 0x00};
constexpr uint8_t kExpected176D6[17] = {
    0x49, 0x8D, 0x8F, 0xB0, 0x09, 0x00, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x78, 0xE8, 0x29, 0xB9, 0x01, 0x00};
constexpr uint8_t kExpected320E0[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x70};
constexpr uint8_t kExpected32180[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00};
constexpr uint8_t kExpectedOgreDtor[17] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20};

struct D3D11MappedSubresourceCompat {
    void* pData;
    UINT RowPitch;
    UINT DepthPitch;
};

#pragma pack(push, 1)
struct ShaderVarWithPosInBuf {
    uint8_t isInitialized;  // +0
    uint8_t isFloat;        // +1
    uint8_t pad0[6];        // +2..+7
    uint64_t physicalIndex; // +8
    const void* srcPtr;     // +16
    const char* name;       // +24
    uint32_t posInBuf;      // +32
    uint32_t size;          // +36
    uint8_t rest[32];       // +40..+71
};
#pragma pack(pop)
static_assert(sizeof(ShaderVarWithPosInBuf) == 72, "ShaderVarWithPosInBuf must be 72 bytes");

struct MsvcStringRaw {
    union {
        char buf[16];
        const char* ptr;
    } u;
    size_t size;
    size_t res;

    std::string_view View() const {
        const char* p = (res > 0xFULL) ? u.ptr : u.buf;
        return std::string_view(p ? p : "", size);
    }
};
static_assert(sizeof(MsvcStringRaw) == 32, "MSVC x64 std::string must be 32 bytes");

struct CachedParamDef {
    bool found = false;
    bool is_int = false;
    uint64_t physical_index = 0;
};

using GetConstantBufferFn = void*(__fastcall*)(uintptr_t a1, uintptr_t* a2);
using SetParamHelperFn = void(__fastcall*)(uintptr_t ff_progs, int prog_type, void* msvc_str, const void* src, size_t size);
using SetVec3HelperFn = void(__fastcall*)(uintptr_t ff_progs, int prog_type, void* msvc_str, const float* vec3);
using StringDtorFn = void(__fastcall*)(void* msvc_str);
using OgreGpuParamsDtorFn = void(__fastcall*)(uintptr_t params);
using FindNamedConstDefFn = const void*(__fastcall*)(uintptr_t params, const void* msvc_str, bool throw_if_not_found);

Detour64 g_detour_cb;
Detour64 g_detour_set_param;
Detour64 g_detour_set_vec3;
Detour64 g_detour_ogre_dtor;
RawPatch64 g_patch_render_loop;
RawPatch64 g_patch_render_copy;

GetConstantBufferFn g_orig_get_cb = nullptr;
SetParamHelperFn g_orig_set_param = nullptr;
SetVec3HelperFn g_orig_set_vec3 = nullptr;
StringDtorFn g_string_dtor = nullptr;
OgreGpuParamsDtorFn g_orig_ogre_dtor = nullptr;
FindNamedConstDefFn g_find_named_def = nullptr;
LogFn g_log = nullptr;

struct TransparentStringHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string& s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};

using ParamMap = std::unordered_map<std::string, CachedParamDef, TransparentStringHash, std::equal_to<>>;

SRWLOCK g_cache_lock = SRWLOCK_INIT;
std::unordered_map<uintptr_t, ParamMap> g_param_cache;

std::atomic<uint64_t> g_cb_fast_hits{0};
std::atomic<uint64_t> g_cb_fallback_calls{0};
std::atomic<uint64_t> g_ff_param_hits{0};
std::atomic<uint64_t> g_ff_param_misses{0};
std::atomic<uint64_t> g_ff_param_skipped_missing{0};

bool IsIntConstantType(uint32_t t) {
    return (t >= 5 && t <= 10) || (t >= 20 && t <= 23);
}

void ReleaseSharedPtrGpuParams(uintptr_t* sp) {
    auto* ref = reinterpret_cast<volatile long*>(sp[2]);
    if (ref && ::InterlockedDecrement(ref) == 0) {
        auto* vtable = *reinterpret_cast<void(__fastcall***)(void*)>(sp);
        vtable[1](sp);
    }
}

void* __fastcall HookGetConstantBuffer(uintptr_t a1, uintptr_t* a2) {
    void* cb = *reinterpret_cast<void**>(a1 + 680);
    if (!cb) {
        g_cb_fallback_calls.fetch_add(1, std::memory_order_relaxed);
        return g_orig_get_cb(a1, a2);
    }

    const uint32_t var_count = *reinterpret_cast<const uint32_t*>(a1 + 716);
    const auto* vars = *reinterpret_cast<const ShaderVarWithPosInBuf* const*>(a1 + 1048);
    const uintptr_t params = a2[1];
    if (!params || (var_count > 0 && !vars)) {
        g_cb_fallback_calls.fetch_add(1, std::memory_order_relaxed);
        return g_orig_get_cb(a1, a2);
    }

    const auto* float_begin = *reinterpret_cast<const uint8_t* const*>(params + 8);
    const auto* float_end = *reinterpret_cast<const uint8_t* const*>(params + 16);
    const auto* int_begin = *reinterpret_cast<const uint8_t* const*>(params + 40);
    const auto* int_end = *reinterpret_cast<const uint8_t* const*>(params + 48);

    const size_t float_bytes = (float_begin && float_end >= float_begin) ? static_cast<size_t>(float_end - float_begin) : 0;
    const size_t int_bytes = (int_begin && int_end >= int_begin) ? static_cast<size_t>(int_end - int_begin) : 0;

    for (uint32_t i = 0; i < var_count; ++i) {
        const auto& v = vars[i];
        if (v.isInitialized != 1) {
            g_cb_fallback_calls.fetch_add(1, std::memory_order_relaxed);
            return g_orig_get_cb(a1, a2);
        }
        const size_t avail = v.isFloat ? float_bytes : int_bytes;
        const size_t byte_off = static_cast<size_t>(v.physicalIndex) * 4;
        if (byte_off > avail || v.size > (avail - byte_off)) {
            g_cb_fallback_calls.fetch_add(1, std::memory_order_relaxed);
            return g_orig_get_cb(a1, a2);
        }
    }

    const uintptr_t dev_wrap = *reinterpret_cast<const uintptr_t*>(a1 + 688);
    void* ctx = dev_wrap ? *reinterpret_cast<void**>(dev_wrap + 8) : nullptr;
    if (!ctx) {
        g_cb_fallback_calls.fetch_add(1, std::memory_order_relaxed);
        return g_orig_get_cb(a1, a2);
    }

    auto* ctx_vtable = *reinterpret_cast<uintptr_t**>(ctx);
    using MapFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, UINT, UINT, UINT, D3D11MappedSubresourceCompat*);
    using UnmapFn = void(STDMETHODCALLTYPE*)(void*, void*, UINT);
    auto map_fn = reinterpret_cast<MapFn>(ctx_vtable[14]);
    auto unmap_fn = reinterpret_cast<UnmapFn>(ctx_vtable[15]);

    D3D11MappedSubresourceCompat mapped{};
    const HRESULT hr = map_fn(ctx, cb, 0, 4 /* D3D11_MAP_WRITE_DISCARD */, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        g_cb_fallback_calls.fetch_add(1, std::memory_order_relaxed);
        return g_orig_get_cb(a1, a2);
    }

    auto* dst_base = static_cast<uint8_t*>(mapped.pData);
    for (uint32_t i = 0; i < var_count; ++i) {
        const auto& v = vars[i];
        const uint8_t* src_base = v.isFloat ? float_begin : int_begin;
        std::memcpy(dst_base + v.posInBuf, src_base + (v.physicalIndex * 4), v.size);
    }

    unmap_fn(ctx, cb, 0);
    ReleaseSharedPtrGpuParams(a2);
    g_cb_fast_hits.fetch_add(1, std::memory_order_relaxed);
    return cb;
}

CachedParamDef ResolveParamCached(uintptr_t params, const void* msvc_str) {
    const std::string_view sv = static_cast<const MsvcStringRaw*>(msvc_str)->View();

    ::AcquireSRWLockShared(&g_cache_lock);
    auto pit = g_param_cache.find(params);
    if (pit != g_param_cache.end()) {
        auto kit = pit->second.find(sv);
        if (kit != pit->second.end()) {
            const CachedParamDef found = kit->second;
            ::ReleaseSRWLockShared(&g_cache_lock);
            g_ff_param_hits.fetch_add(1, std::memory_order_relaxed);
            return found;
        }
    }
    ::ReleaseSRWLockShared(&g_cache_lock);

    g_ff_param_misses.fetch_add(1, std::memory_order_relaxed);
    CachedParamDef resolved{};
    const void* def = g_find_named_def(params, msvc_str, false);
    if (def) {
        resolved.found = true;
        resolved.is_int = IsIntConstantType(*static_cast<const uint32_t*>(def));
        resolved.physical_index = *reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(def) + 8);
    }

    std::string key(sv);
    ::AcquireSRWLockExclusive(&g_cache_lock);
    g_param_cache[params].emplace(std::move(key), resolved);
    ::ReleaseSRWLockExclusive(&g_cache_lock);
    return resolved;
}

void WriteCachedParam(uintptr_t params, const CachedParamDef& def, const void* src, size_t size) {
    if (!def.found) {
        g_ff_param_skipped_missing.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto* base = *reinterpret_cast<uint8_t* const*>(params + (def.is_int ? 40 : 8));
    auto* end = *reinterpret_cast<uint8_t* const*>(params + (def.is_int ? 48 : 16));
    const size_t avail = (base && end >= base) ? static_cast<size_t>(end - base) : 0;
    const size_t off = static_cast<size_t>(def.physical_index) * 4;
    if (off <= avail && size <= (avail - off)) {
        std::memcpy(base + off, src, size);
    }
}

void __fastcall HookSetParamHelper(uintptr_t ff_progs, int prog_type, void* msvc_str, const void* src, size_t size) {
    if ((prog_type == 0 || prog_type == 1) && g_find_named_def) {
        const uintptr_t usage = ff_progs + (prog_type == 0 ? 16 : 56);
        const uintptr_t params = *reinterpret_cast<const uintptr_t*>(usage + 8);
        if (params) {
            const CachedParamDef def = ResolveParamCached(params, msvc_str);
            WriteCachedParam(params, def, src, size);
            g_string_dtor(msvc_str);
            return;
        }
    }
    g_orig_set_param(ff_progs, prog_type, msvc_str, src, size);
}

void __fastcall HookSetVec3Helper(uintptr_t ff_progs, int prog_type, void* msvc_str, const float* vec3) {
    if ((prog_type == 0 || prog_type == 1) && vec3 && g_find_named_def) {
        const uintptr_t usage = ff_progs + (prog_type == 0 ? 16 : 56);
        const uintptr_t params = *reinterpret_cast<const uintptr_t*>(usage + 8);
        if (params) {
            float copy[3] = {vec3[0], vec3[1], vec3[2]};
            const CachedParamDef def = ResolveParamCached(params, msvc_str);
            WriteCachedParam(params, def, copy, sizeof(copy));
            g_string_dtor(msvc_str);
            return;
        }
    }
    g_orig_set_vec3(ff_progs, prog_type, msvc_str, vec3);
}

void __fastcall HookOgreGpuParamsDtor(uintptr_t params) {
    ::AcquireSRWLockExclusive(&g_cache_lock);
    g_param_cache.erase(params);
    ::ReleaseSRWLockExclusive(&g_cache_lock);
    g_orig_ogre_dtor(params);
}

void AppendAbsJmp(std::vector<uint8_t>& out, uint64_t addr) {
    const uint8_t jmp[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    out.insert(out.end(), jmp, jmp + 6);
    const auto* p = reinterpret_cast<const uint8_t*>(&addr);
    out.insert(out.end(), p, p + 8);
}

void AppendMovRaxImm64Call(std::vector<uint8_t>& out, uint64_t addr) {
    const uint8_t rex_b8[2] = {0x48, 0xB8};
    out.insert(out.end(), rex_b8, rex_b8 + 2);
    const auto* p = reinterpret_cast<const uint8_t*>(&addr);
    out.insert(out.end(), p, p + 8);
    out.push_back(0xFF);
    out.push_back(0xD0);
}

bool InstallRenderSkipStubs(uintptr_t base) {
    // Stub 1: replace 0x17539 (16 bytes)
    //   mov [rbp+18h], r14
    //   if ([r15+0EF0h] != 0 && [r15+0EF8h] != 0) jmp base+0x176C1
    //   else lea rbx,[r15+1048h]; mov esi,10h; jmp base+0x17549
    std::vector<uint8_t> stub1 = {
        0x4C, 0x89, 0x75, 0x18,                          // mov qword ptr [rbp+18h], r14
        0x49, 0x83, 0xBF, 0xF0, 0x0E, 0x00, 0x00, 0x00,  // cmp qword ptr [r15+0EF0h], 0
        0x74, 0x18,                                      // je slow (+24)
        0x49, 0x83, 0xBF, 0xF8, 0x0E, 0x00, 0x00, 0x00,  // cmp qword ptr [r15+0EF8h], 0
        0x74, 0x0E,                                      // je slow (+14)
    };
    AppendAbsJmp(stub1, base + kRvaRenderLoopSkipTarget);
    const uint8_t stub1_slow[12] = {
        0x49, 0x8D, 0x9F, 0x48, 0x10, 0x00, 0x00,  // lea rbx, [r15+1048h]
        0xBE, 0x10, 0x00, 0x00, 0x00,              // mov esi, 10h
    };
    stub1.insert(stub1.end(), stub1_slow, stub1_slow + 12);
    AppendAbsJmp(stub1, base + kRvaRenderLoopResume);

    if (!g_patch_render_loop.InstallJumpToAllocatedStub(
            reinterpret_cast<void*>(base + kRvaRenderLoopEntry),
            kExpected17539,
            sizeof(kExpected17539),
            stub1)) {
        return false;
    }

    // Stub 2: replace 0x176D6 (17 bytes)
    //   if ([r15+0EF0h] != 0 && [r15+0EF8h] != 0) jmp base+0x176E7
    //   else lea rcx,[r15+9B0h]; lea rdx,[rsp+78h]; call 0x33010; jmp base+0x176E7
    std::vector<uint8_t> stub2 = {
        0x49, 0x83, 0xBF, 0xF0, 0x0E, 0x00, 0x00, 0x00,  // cmp qword ptr [r15+0EF0h], 0
        0x74, 0x18,                                      // je do_copy (+24)
        0x49, 0x83, 0xBF, 0xF8, 0x0E, 0x00, 0x00, 0x00,  // cmp qword ptr [r15+0EF8h], 0
        0x74, 0x0E,                                      // je do_copy (+14)
    };
    AppendAbsJmp(stub2, base + kRvaRenderCopyResume);
    const uint8_t stub2_args[12] = {
        0x49, 0x8D, 0x8F, 0xB0, 0x09, 0x00, 0x00,  // lea rcx, [r15+9B0h]
        0x48, 0x8D, 0x54, 0x24, 0x78,              // lea rdx, [rsp+78h]
    };
    stub2.insert(stub2.end(), stub2_args, stub2_args + 12);
    AppendMovRaxImm64Call(stub2, base + kRvaCopyTextureLayers);
    AppendAbsJmp(stub2, base + kRvaRenderCopyResume);

    if (!g_patch_render_copy.InstallJumpToAllocatedStub(
            reinterpret_cast<void*>(base + kRvaRenderCopyCall),
            kExpected176D6,
            sizeof(kExpected176D6),
            stub2)) {
        g_patch_render_loop.Remove();
        return false;
    }

    return true;
}

}  // namespace

bool InstallPhase1FastPaths(void* d3d11_base, void* ogre_main_base, LogFn logger) {
    g_log = logger;
    if (!d3d11_base || !ogre_main_base) {
        return false;
    }

    const auto d3d_base = reinterpret_cast<uintptr_t>(d3d11_base);
    const auto ogre_base = reinterpret_cast<uintptr_t>(ogre_main_base);

    g_string_dtor = reinterpret_cast<StringDtorFn>(d3d_base + kRvaStringDtor);
    g_find_named_def = reinterpret_cast<FindNamedConstDefFn>(
        ::GetProcAddress(
            static_cast<HMODULE>(ogre_main_base),
            "?_findNamedConstantDefinition@GpuProgramParameters@Ogre@@QEBAPEBUGpuConstantDefinition@2@AEBV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@_N@Z"));

    void* orig_cb = nullptr;
    if (!g_detour_cb.Install(
            reinterpret_cast<void*>(d3d_base + kRvaGetConstantBuffer),
            kExpected8260,
            sizeof(kExpected8260),
            reinterpret_cast<void*>(&HookGetConstantBuffer),
            &orig_cb)) {
        if (g_log) g_log("[RouteB][Phase1A] FAILED detour on getConstantBuffer (0x8260)");
        return false;
    }
    g_orig_get_cb = reinterpret_cast<GetConstantBufferFn>(orig_cb);
    if (g_log) g_log("[RouteB][Phase1A] Installed getConstantBuffer physicalIndex fast-path (RVA 0x8260)");

    if (!InstallRenderSkipStubs(d3d_base)) {
        if (g_log) g_log("[RouteB][Phase1C] FAILED _render TextureLayerState conditional skip stubs");
        RemovePhase1FastPaths();
        return false;
    }
    if (g_log) g_log("[RouteB][Phase1C] Installed _render FixedFuncState conditional skip stubs (RVA 0x17539/0x176D6)");

    if (g_find_named_def) {
        void* orig_ogre_dtor = nullptr;
        void* orig_set_param = nullptr;
        void* orig_set_vec3 = nullptr;
        const bool ok_dtor = g_detour_ogre_dtor.Install(
            reinterpret_cast<void*>(ogre_base + kRvaOgreGpuParamsDtor),
            kExpectedOgreDtor,
            sizeof(kExpectedOgreDtor),
            reinterpret_cast<void*>(&HookOgreGpuParamsDtor),
            &orig_ogre_dtor);
        const bool ok_p = g_detour_set_param.Install(
            reinterpret_cast<void*>(d3d_base + kRvaSetParamHelper),
            kExpected320E0,
            sizeof(kExpected320E0),
            reinterpret_cast<void*>(&HookSetParamHelper),
            &orig_set_param);
        const bool ok_v = g_detour_set_vec3.Install(
            reinterpret_cast<void*>(d3d_base + kRvaSetVec3Helper),
            kExpected32180,
            sizeof(kExpected32180),
            reinterpret_cast<void*>(&HookSetVec3Helper),
            &orig_set_vec3);

        if (ok_dtor && ok_p && ok_v) {
            g_orig_ogre_dtor = reinterpret_cast<OgreGpuParamsDtorFn>(orig_ogre_dtor);
            g_orig_set_param = reinterpret_cast<SetParamHelperFn>(orig_set_param);
            g_orig_set_vec3 = reinterpret_cast<SetVec3HelperFn>(orig_set_vec3);
            if (g_log) {
                g_log("[RouteB][Phase1D] Installed FixedFunc named-constant physicalIndex cache (RVA 0x320E0/0x32180 + ~GpuProgramParameters)");
            }
        } else {
            g_detour_set_vec3.Remove();
            g_detour_set_param.Remove();
            g_detour_ogre_dtor.Remove();
            if (g_log) {
                g_log("[RouteB][Phase1D] WARNING: FixedFunc constant cache skipped due to signature mismatch");
            }
        }
    } else if (g_log) {
        g_log("[RouteB][Phase1D] WARNING: _findNamedConstantDefinition export not resolved");
    }

    return true;
}

void RemovePhase1FastPaths() {
    g_detour_set_vec3.Remove();
    g_detour_set_param.Remove();
    g_detour_ogre_dtor.Remove();
    g_patch_render_copy.Remove();
    g_patch_render_loop.Remove();
    g_detour_cb.Remove();

    ::AcquireSRWLockExclusive(&g_cache_lock);
    g_param_cache.clear();
    ::ReleaseSRWLockExclusive(&g_cache_lock);
}

FastPathStats GetFastPathStats() {
    FastPathStats s{};
    s.cb_fast_hits = g_cb_fast_hits.load(std::memory_order_relaxed);
    s.cb_fallback_calls = g_cb_fallback_calls.load(std::memory_order_relaxed);
    s.ff_param_hits = g_ff_param_hits.load(std::memory_order_relaxed);
    s.ff_param_misses = g_ff_param_misses.load(std::memory_order_relaxed);
    s.ff_param_skipped_missing = g_ff_param_skipped_missing.load(std::memory_order_relaxed);
    return s;
}

}  // namespace routeb
