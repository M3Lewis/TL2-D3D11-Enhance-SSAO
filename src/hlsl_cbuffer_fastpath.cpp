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
std::atomic<uint64_t> g_cache_epoch{1};

std::atomic<uint64_t> g_cb_fast_hits{0};
std::atomic<uint64_t> g_cb_fallback_calls{0};
std::atomic<uint64_t> g_cb_fb_null_cb{0};
std::atomic<uint64_t> g_cb_fb_null_params{0};
std::atomic<uint64_t> g_cb_fb_uninit{0};
std::atomic<uint64_t> g_cb_fb_bounds{0};
std::atomic<uint64_t> g_cb_fb_map_fail{0};
std::atomic<uint64_t> g_ff_param_hits{0};
std::atomic<uint64_t> g_ff_param_misses{0};
std::atomic<uint64_t> g_ff_param_skipped_missing{0};
std::atomic<uint64_t> g_p3_param_forwards{0};
std::atomic<uintptr_t> g_forward_ps_params{0};

SRWLOCK g_dir_light_lock = SRWLOCK_INIT;
bool g_cap_has_dir_light = false;
bool g_cap_has_dir_diffuse = false;
float g_cap_dir_light[3] = {0.0f, 0.0f, 0.0f};
float g_cap_dir_lum = 0.0f;
bool g_cap_has_pt0 = false;
bool g_cap_has_pt0_pending_pos = false;
float g_cap_pt0_pending_pos[3] = {0.0f, 0.0f, 0.0f};
float g_cap_pt0_best_pos[3] = {0.0f, 0.0f, 0.0f};
float g_cap_pt0_best_lum = 0.0f;

constexpr size_t kL1CacheSlots = 2048;
constexpr size_t kL1MaxNameLen = 46;

struct L1ParamEntry {
    uint64_t epoch = 0;
    uintptr_t params = 0;
    uint64_t hash = 0;
    CachedParamDef def{};
    uint8_t name_len = 0;
    char name[kL1MaxNameLen]{};
};

uint64_t HashL1Key(uintptr_t params, std::string_view sv) {
    uint64_t h = 14695981039346656037ULL ^ (static_cast<uint64_t>(params) * 0x9E3779B97F4A7C15ULL);
    for (unsigned char c : sv) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

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
        // Mirror vanilla sub_180008260 when mConstantBuffer is null: release SharedPtr and return null without detour overhead.
        g_cb_fb_null_cb.fetch_add(1, std::memory_order_relaxed);
        ReleaseSharedPtrGpuParams(a2);
        return nullptr;
    }

    const uint32_t var_count = *reinterpret_cast<const uint32_t*>(a1 + 716);
    const auto* vars = *reinterpret_cast<const ShaderVarWithPosInBuf* const*>(a1 + 1048);
    const uintptr_t params = a2[1];
    if (!params || (var_count > 0 && !vars)) {
        g_cb_fb_null_params.fetch_add(1, std::memory_order_relaxed);
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
            g_cb_fb_uninit.fetch_add(1, std::memory_order_relaxed);
            g_cb_fallback_calls.fetch_add(1, std::memory_order_relaxed);
            return g_orig_get_cb(a1, a2);
        }
        const size_t avail = v.isFloat ? float_bytes : int_bytes;
        const size_t byte_off = static_cast<size_t>(v.physicalIndex) * 4;
        if (byte_off > avail || v.size > (avail - byte_off)) {
            g_cb_fb_bounds.fetch_add(1, std::memory_order_relaxed);
            g_cb_fallback_calls.fetch_add(1, std::memory_order_relaxed);
            return g_orig_get_cb(a1, a2);
        }
    }

    const uintptr_t dev_wrap = *reinterpret_cast<const uintptr_t*>(a1 + 688);
    void* ctx = dev_wrap ? *reinterpret_cast<void**>(dev_wrap + 8) : nullptr;
    if (!ctx) {
        g_cb_fb_map_fail.fetch_add(1, std::memory_order_relaxed);
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
        g_cb_fb_map_fail.fetch_add(1, std::memory_order_relaxed);
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
    const uint64_t cur_epoch = g_cache_epoch.load(std::memory_order_acquire);
    const bool use_l1 = (sv.size() <= kL1MaxNameLen);
    thread_local L1ParamEntry tls_l1[kL1CacheSlots]{};

    uint64_t l1_hash = 0;
    L1ParamEntry* l1_slot = nullptr;
    if (use_l1) {
        l1_hash = HashL1Key(params, sv);
        l1_slot = &tls_l1[l1_hash & (kL1CacheSlots - 1)];
        if (l1_slot->epoch == cur_epoch &&
            l1_slot->params == params &&
            l1_slot->hash == l1_hash &&
            l1_slot->name_len == sv.size() &&
            std::memcmp(l1_slot->name, sv.data(), sv.size()) == 0) {
            g_ff_param_hits.fetch_add(1, std::memory_order_relaxed);
            return l1_slot->def;
        }
    }

    ::AcquireSRWLockShared(&g_cache_lock);
    auto pit = g_param_cache.find(params);
    if (pit != g_param_cache.end()) {
        auto kit = pit->second.find(sv);
        if (kit != pit->second.end()) {
            const CachedParamDef found = kit->second;
            ::ReleaseSRWLockShared(&g_cache_lock);
            if (l1_slot && cur_epoch == g_cache_epoch.load(std::memory_order_acquire)) {
                l1_slot->epoch = cur_epoch;
                l1_slot->params = params;
                l1_slot->hash = l1_hash;
                l1_slot->def = found;
                l1_slot->name_len = static_cast<uint8_t>(sv.size());
                std::memcpy(l1_slot->name, sv.data(), sv.size());
            }
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

    if (l1_slot && cur_epoch == g_cache_epoch.load(std::memory_order_acquire)) {
        l1_slot->epoch = cur_epoch;
        l1_slot->params = params;
        l1_slot->hash = l1_hash;
        l1_slot->def = resolved;
        l1_slot->name_len = static_cast<uint8_t>(sv.size());
        std::memcpy(l1_slot->name, sv.data(), sv.size());
    }
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

void MaybeCaptureDirectionalLight(const void* msvc_str, const void* src, size_t size) {
    if (!msvc_str || !src || size < sizeof(float) * 3) {
        return;
    }
    const auto* raw_str = reinterpret_cast<const MsvcStringRaw*>(msvc_str);
    if (raw_str->size != 27 && raw_str->size != 25 &&
        raw_str->size != 20 && raw_str->size != 19) {
        return;
    }
    const std::string_view name = raw_str->View();
    const auto* f = static_cast<const float*>(src);
    if (name == "DirectionalLight0_Direction") {
        ::AcquireSRWLockExclusive(&g_dir_light_lock);
        g_cap_dir_light[0] = f[0];
        g_cap_dir_light[1] = f[1];
        g_cap_dir_light[2] = f[2];
        g_cap_has_dir_light = true;
        ::ReleaseSRWLockExclusive(&g_dir_light_lock);
    } else if (name == "DirectionalLight0_Diffuse") {
        ::AcquireSRWLockExclusive(&g_dir_light_lock);
        g_cap_dir_lum = 0.299f * f[0] + 0.587f * f[1] + 0.114f * f[2];
        g_cap_has_dir_diffuse = true;
        ::ReleaseSRWLockExclusive(&g_dir_light_lock);
    } else if (name == "PointLight0_Position") {
        ::AcquireSRWLockExclusive(&g_dir_light_lock);
        g_cap_pt0_pending_pos[0] = f[0];
        g_cap_pt0_pending_pos[1] = f[1];
        g_cap_pt0_pending_pos[2] = f[2];
        g_cap_has_pt0_pending_pos = true;
        ::ReleaseSRWLockExclusive(&g_dir_light_lock);
    } else if (name == "PointLight0_Diffuse") {
        const float pt_lum = 0.299f * f[0] + 0.587f * f[1] + 0.114f * f[2];
        if (pt_lum > 0.20f) {
            ::AcquireSRWLockExclusive(&g_dir_light_lock);
            if (g_cap_has_pt0_pending_pos && pt_lum >= g_cap_pt0_best_lum) {
                g_cap_pt0_best_pos[0] = g_cap_pt0_pending_pos[0];
                g_cap_pt0_best_pos[1] = g_cap_pt0_pending_pos[1];
                g_cap_pt0_best_pos[2] = g_cap_pt0_pending_pos[2];
                g_cap_pt0_best_lum = pt_lum;
                g_cap_has_pt0 = true;
            }
            ::ReleaseSRWLockExclusive(&g_dir_light_lock);
        }
    }
}

void ForwardToActiveExplicitPs(uintptr_t ff_target_params, const void* msvc_str, const void* src, size_t size) {
    const uintptr_t extra_ps = g_forward_ps_params.load(std::memory_order_relaxed);
    if (!extra_ps || extra_ps == ff_target_params) {
        return;
    }
    const CachedParamDef extra_def = ResolveParamCached(extra_ps, msvc_str);
    if (extra_def.found) {
        WriteCachedParam(extra_ps, extra_def, src, size);
        g_p3_param_forwards.fetch_add(1, std::memory_order_relaxed);
    }
}

void __fastcall HookSetParamHelper(uintptr_t ff_progs, int prog_type, void* msvc_str, const void* src, size_t size) {
    if ((prog_type == 0 || prog_type == 1) && g_find_named_def) {
        MaybeCaptureDirectionalLight(msvc_str, src, size);
        const uintptr_t usage = ff_progs + (prog_type == 0 ? 16 : 56);
        const uintptr_t params = *reinterpret_cast<const uintptr_t*>(usage + 8);
        if (params) {
            const CachedParamDef def = ResolveParamCached(params, msvc_str);
            WriteCachedParam(params, def, src, size);
            ForwardToActiveExplicitPs(params, msvc_str, src, size);
            g_string_dtor(msvc_str);
            return;
        }
    }
    g_orig_set_param(ff_progs, prog_type, msvc_str, src, size);
}

void __fastcall HookSetVec3Helper(uintptr_t ff_progs, int prog_type, void* msvc_str, const float* vec3) {
    if ((prog_type == 0 || prog_type == 1) && vec3 && g_find_named_def) {
        const float copy[3] = {vec3[0], vec3[1], vec3[2]};
        MaybeCaptureDirectionalLight(msvc_str, copy, sizeof(copy));
        const uintptr_t usage = ff_progs + (prog_type == 0 ? 16 : 56);
        const uintptr_t params = *reinterpret_cast<const uintptr_t*>(usage + 8);
        if (params) {
            const CachedParamDef def = ResolveParamCached(params, msvc_str);
            WriteCachedParam(params, def, copy, sizeof(copy));
            ForwardToActiveExplicitPs(params, msvc_str, copy, sizeof(copy));
            g_string_dtor(msvc_str);
            return;
        }
    }
    g_orig_set_vec3(ff_progs, prog_type, msvc_str, vec3);
}

void __fastcall HookOgreGpuParamsDtor(uintptr_t params) {
    uintptr_t expected = params;
    g_forward_ps_params.compare_exchange_strong(expected, 0, std::memory_order_relaxed);
    ::AcquireSRWLockExclusive(&g_cache_lock);
    const size_t erased = g_param_cache.erase(params);
    ::ReleaseSRWLockExclusive(&g_cache_lock);
    if (erased > 0) {
        g_cache_epoch.fetch_add(1, std::memory_order_release);
    }
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
    //   mov esi, 10h (also needed later at 0x17834 sub esi, edx!)
    //   if ([r15+0EF0h] != 0 && [r15+0EF8h] != 0) jmp base+0x176C1
    //   else lea rbx,[r15+1048h]; jmp base+0x17549
    std::vector<uint8_t> stub1 = {
        0x4C, 0x89, 0x75, 0x18,                          // mov qword ptr [rbp+18h], r14
        0xBE, 0x10, 0x00, 0x00, 0x00,                    // mov esi, 10h
        0x49, 0x83, 0xBF, 0xF0, 0x0E, 0x00, 0x00, 0x00,  // cmp qword ptr [r15+0EF0h], 0
        0x74, 0x18,                                      // je slow (+24)
        0x49, 0x83, 0xBF, 0xF8, 0x0E, 0x00, 0x00, 0x00,  // cmp qword ptr [r15+0EF8h], 0
        0x74, 0x0E,                                      // je slow (+14)
    };
    AppendAbsJmp(stub1, base + kRvaRenderLoopSkipTarget);
    const uint8_t stub1_slow[7] = {
        0x49, 0x8D, 0x9F, 0x48, 0x10, 0x00, 0x00,  // lea rbx, [r15+1048h]
    };
    stub1.insert(stub1.end(), stub1_slow, stub1_slow + 7);
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
    g_forward_ps_params.store(0, std::memory_order_relaxed);
    g_detour_set_vec3.Remove();
    g_detour_set_param.Remove();
    g_detour_ogre_dtor.Remove();
    g_patch_render_copy.Remove();
    g_patch_render_loop.Remove();
    g_detour_cb.Remove();

    ::AcquireSRWLockExclusive(&g_cache_lock);
    g_param_cache.clear();
    ::ReleaseSRWLockExclusive(&g_cache_lock);
    g_cache_epoch.fetch_add(1, std::memory_order_release);
}

FastPathStats GetFastPathStats() {
    FastPathStats s{};
    s.cb_fast_hits = g_cb_fast_hits.load(std::memory_order_relaxed);
    s.cb_fallback_calls = g_cb_fallback_calls.load(std::memory_order_relaxed);
    s.cb_fb_null_cb = g_cb_fb_null_cb.load(std::memory_order_relaxed);
    s.cb_fb_null_params = g_cb_fb_null_params.load(std::memory_order_relaxed);
    s.cb_fb_uninit = g_cb_fb_uninit.load(std::memory_order_relaxed);
    s.cb_fb_bounds = g_cb_fb_bounds.load(std::memory_order_relaxed);
    s.cb_fb_map_fail = g_cb_fb_map_fail.load(std::memory_order_relaxed);
    s.ff_param_hits = g_ff_param_hits.load(std::memory_order_relaxed);
    s.ff_param_misses = g_ff_param_misses.load(std::memory_order_relaxed);
    s.ff_param_skipped_missing = g_ff_param_skipped_missing.load(std::memory_order_relaxed);
    s.p3_param_forwards = g_p3_param_forwards.load(std::memory_order_relaxed);
    return s;
}

void SetActiveExplicitPsForwardTarget(uintptr_t explicit_ps_params) {
    g_forward_ps_params.store(explicit_ps_params, std::memory_order_relaxed);
}

bool WriteCachedFloatParamByMsvcStr(uintptr_t params, const void* msvc_str, const float* src, size_t size) {
    if (!params || !msvc_str || !src || !g_find_named_def) {
        return false;
    }
    const CachedParamDef def = ResolveParamCached(params, msvc_str);
    if (!def.found) {
        return false;
    }
    WriteCachedParam(params, def, src, size);
    return true;
}

bool WriteExplicitPsNamedFloat4(uintptr_t params, const char* name, const float v[4]) {
    if (!name) {
        return false;
    }
    static_assert(sizeof(std::string) == sizeof(MsvcStringRaw), "std::string ABI mismatch");
    const std::string key(name);
    return WriteCachedFloatParamByMsvcStr(params, &key, v, sizeof(float) * 4);
}

void ResetCapturedDirectionalLight() {
    ::AcquireSRWLockExclusive(&g_dir_light_lock);
    g_cap_has_dir_light = false;
    g_cap_has_dir_diffuse = false;
    g_cap_dir_light[0] = 0.0f;
    g_cap_dir_light[1] = 0.0f;
    g_cap_dir_light[2] = 0.0f;
    g_cap_dir_lum = 0.0f;
    g_cap_has_pt0 = false;
    g_cap_has_pt0_pending_pos = false;
    g_cap_pt0_pending_pos[0] = 0.0f;
    g_cap_pt0_pending_pos[1] = 0.0f;
    g_cap_pt0_pending_pos[2] = 0.0f;
    g_cap_pt0_best_pos[0] = 0.0f;
    g_cap_pt0_best_pos[1] = 0.0f;
    g_cap_pt0_best_pos[2] = 0.0f;
    g_cap_pt0_best_lum = 0.0f;
    ::ReleaseSRWLockExclusive(&g_dir_light_lock);
}

bool GetCapturedDirectionalLight(float out_dir[3], float* out_lum) {
    if (!out_dir || !out_lum) {
        return false;
    }
    ::AcquireSRWLockShared(&g_dir_light_lock);
    const bool ok = g_cap_has_dir_light;
    out_dir[0] = g_cap_dir_light[0];
    out_dir[1] = g_cap_dir_light[1];
    out_dir[2] = g_cap_dir_light[2];
    *out_lum = g_cap_has_dir_diffuse ? g_cap_dir_lum : 1.0f;
    ::ReleaseSRWLockShared(&g_dir_light_lock);
    return ok;
}

bool GetCapturedDominantPointLight(float out_world_pos[3], float* out_lum) {
    if (!out_world_pos || !out_lum) {
        return false;
    }
    ::AcquireSRWLockShared(&g_dir_light_lock);
    const bool ok = g_cap_has_pt0;
    out_world_pos[0] = g_cap_pt0_best_pos[0];
    out_world_pos[1] = g_cap_pt0_best_pos[1];
    out_world_pos[2] = g_cap_pt0_best_pos[2];
    *out_lum = g_cap_pt0_best_lum;
    ::ReleaseSRWLockShared(&g_dir_light_lock);
    return ok;
}

bool WriteExplicitPsNamedFloat(uintptr_t params, const char* name, float v) {
    if (!name) {
        return false;
    }
    static_assert(sizeof(std::string) == sizeof(MsvcStringRaw), "std::string ABI mismatch");
    const std::string key(name);
    return WriteCachedFloatParamByMsvcStr(params, &key, &v, sizeof(float));
}

void ResetExplicitPsLightingScratch(uintptr_t params) {
    if (!params || !g_find_named_def) {
        return;
    }
    struct ScratchResetSpec {
        std::string name;
        size_t size;
    };
    static const ScratchResetSpec kResetSpecs[] = {
        {"DirectionalLight0_Diffuse", sizeof(float) * 4},
        {"DirectionalLight0_Direction", sizeof(float) * 4},
        {"BaseLightAmbient", sizeof(float) * 4},
        {"PointLight0_Diffuse", sizeof(float) * 4},
        {"PointLight0_Specular", sizeof(float) * 4},
        {"PointLight0_Position", sizeof(float) * 3},
        {"PointLight0_Range", sizeof(float)},
        {"PointLight0_Attenuation", sizeof(float) * 3},
        {"PointLight1_Diffuse", sizeof(float) * 4},
        {"PointLight1_Specular", sizeof(float) * 4},
        {"PointLight1_Position", sizeof(float) * 3},
        {"PointLight1_Range", sizeof(float)},
        {"PointLight1_Attenuation", sizeof(float) * 3},
        {"PointLight2_Diffuse", sizeof(float) * 4},
        {"PointLight2_Specular", sizeof(float) * 4},
        {"PointLight2_Position", sizeof(float) * 3},
        {"PointLight2_Range", sizeof(float)},
        {"PointLight2_Attenuation", sizeof(float) * 3},
    };
    static constexpr float kZero4[4] = {0.0f, 0.0f, 0.0f, 0.0f};

    for (const auto& spec : kResetSpecs) {
        const CachedParamDef d = ResolveParamCached(params, &spec.name);
        if (d.found) {
            WriteCachedParam(params, d, kZero4, spec.size);
        }
    }
}

}  // namespace routeb
