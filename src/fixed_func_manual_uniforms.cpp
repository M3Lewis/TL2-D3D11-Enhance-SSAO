#include "fixed_func_manual_uniforms.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>

#include "hlsl_cbuffer_fastpath.h"
#include "hook_utils.h"

namespace routeb {

namespace {

// Preferred RVAs for RenderSystem_Direct3D11.dll (used as fast-path before signature scan fallback).
constexpr uintptr_t kRvaGetFixedFuncShaderPrograms = 0x32480;
constexpr uintptr_t kRvaGenLayerBlendOp = 0x33580;
constexpr uintptr_t kRvaGenLayerBlendOperand = 0x34F10;

// Verified FixedFuncState vector offsets (+24 std::vector<TextureLayerState> -> begin at +32, end at +40).
constexpr uintptr_t kFfStateOffLayersBegin = 32;
constexpr uintptr_t kFfStateOffLayersEnd = 40;
// Verified HlslFixedFuncPrograms offset (+56 mFragmentParameters SharedPtr -> rep at +64).
constexpr uintptr_t kFfProgsOffPsParamsRep = 64;

constexpr uint8_t kExpectedGetFfPrograms[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20};
constexpr uint8_t kScanGetFfPrograms[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89,
    0x74, 0x24, 0x20, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC};
constexpr uint8_t kExpectedGenBlendOp[14] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
constexpr uint8_t kScanGenBlendOp[24] = {
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x20, 0x55, 0x56, 0x57, 0x41, 0x54,
    0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xA8, 0xC8, 0xFE, 0xFF};
constexpr uint8_t kExpectedGenBlendOperand[15] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89, 0x4C, 0x24, 0x08};
constexpr uint8_t kScanGenBlendOperand[24] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x48, 0x89,
    0x4C, 0x24, 0x08, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48};

using GetFfShaderProgramsFn = uintptr_t(__fastcall*)(uintptr_t self, const void* lang, uintptr_t vdecl, uintptr_t ff_state);
using GenLayerBlendOpFn = std::string*(__fastcall*)(std::string* out, const OgreLayerBlendModeExRaw* blend, uint8_t is_alpha);
using GenLayerBlendOperandFn = std::string*(__fastcall*)(
    std::string* out,
    int source,
    const OgreLayerBlendModeExRaw* blend,
    uint8_t is_arg1,
    uint8_t is_alpha);

struct ShaderGenTlsState {
    bool active = false;
    uint32_t stage_idx = 0;
    ManualUniformMask mask{};
};

struct StageManualUniformNames {
    std::string col_arg1;
    std::string col_arg2;
    std::string col_factor;
    std::string alpha_arg1;
    std::string alpha_arg2;
    std::string alpha_factor;
};

const std::array<StageManualUniformNames, kMaxFfTextureStages>& GetUniformNameTable() {
    static const auto table = [] {
        std::array<StageManualUniformNames, kMaxFfTextureStages> arr{};
        char buf[24]{};
        for (size_t i = 0; i < kMaxFfTextureStages; ++i) {
            std::snprintf(buf, sizeof(buf), "FFManC%zuA1", i);
            arr[i].col_arg1 = buf;
            std::snprintf(buf, sizeof(buf), "FFManC%zuA2", i);
            arr[i].col_arg2 = buf;
            std::snprintf(buf, sizeof(buf), "FFManC%zuF", i);
            arr[i].col_factor = buf;
            std::snprintf(buf, sizeof(buf), "FFManA%zuA1", i);
            arr[i].alpha_arg1 = buf;
            std::snprintf(buf, sizeof(buf), "FFManA%zuA2", i);
            arr[i].alpha_arg2 = buf;
            std::snprintf(buf, sizeof(buf), "FFManA%zuF", i);
            arr[i].alpha_factor = buf;
        }
        return arr;
    }();
    return table;
}

Detour64 g_detour_get_programs;
Detour64 g_detour_gen_blend_op;
Detour64 g_detour_gen_blend_operand;

GetFfShaderProgramsFn g_orig_get_programs = nullptr;
GenLayerBlendOpFn g_orig_gen_blend_op = nullptr;
GenLayerBlendOperandFn g_orig_gen_blend_operand = nullptr;

LogFn g_log = nullptr;
std::atomic<bool> g_hooked{false};
std::atomic<uint64_t> g_normalized_lookups{0};
std::atomic<uint64_t> g_unique_manual_shaders{0};
std::atomic<uint64_t> g_param_updates{0};

thread_local ManualFrameSnapshot tls_manual_snapshot{};
thread_local ShaderGenTlsState tls_shader_gen{};

struct ScopedManualLayerNormalize {
    OgreTextureLayerStateRaw* layers = nullptr;
    size_t count = 0;

    ScopedManualLayerNormalize(uintptr_t ff_state, ManualFrameSnapshot* snapshot) {
        snapshot->layer_count = 0;
        snapshot->has_any = false;
        if (!ff_state) {
            return;
        }
        const auto begin_addr = *reinterpret_cast<const uintptr_t*>(ff_state + kFfStateOffLayersBegin);
        const auto end_addr = *reinterpret_cast<const uintptr_t*>(ff_state + kFfStateOffLayersEnd);
        if (!begin_addr || end_addr <= begin_addr) {
            return;
        }
        const size_t byte_len = static_cast<size_t>(end_addr - begin_addr);
        if ((byte_len % sizeof(OgreTextureLayerStateRaw)) != 0) {
            return;
        }
        layers = reinterpret_cast<OgreTextureLayerStateRaw*>(begin_addr);
        count = byte_len / sizeof(OgreTextureLayerStateRaw);
        CaptureAndNormalizeManualLayers(layers, count, snapshot);
    }

    ~ScopedManualLayerNormalize() {
        if (layers && tls_manual_snapshot.has_any) {
            RestoreManualLayers(layers, count, tls_manual_snapshot);
        }
    }
};

std::string* __fastcall HookGenLayerBlendOperand(
    std::string* out,
    int source,
    const OgreLayerBlendModeExRaw* blend,
    uint8_t is_arg1,
    uint8_t is_alpha) {
    if (tls_shader_gen.active &&
        source == static_cast<int>(kLayerBlendSrcManual) &&
        tls_shader_gen.stage_idx < kMaxFfTextureStages) {
        const uint32_t s = tls_shader_gen.stage_idx;
        const auto& names = GetUniformNameTable()[s];
        const uint16_t bit = static_cast<uint16_t>(1U << s);
        if (!is_alpha) {
            if (is_arg1) {
                tls_shader_gen.mask.col_arg1 |= bit;
                return ::new (out) std::string(names.col_arg1 + ".rgb");
            }
            tls_shader_gen.mask.col_arg2 |= bit;
            return ::new (out) std::string(names.col_arg2 + ".rgb");
        }
        if (is_arg1) {
            tls_shader_gen.mask.alpha_arg1 |= bit;
            return ::new (out) std::string(names.alpha_arg1);
        }
        tls_shader_gen.mask.alpha_arg2 |= bit;
        return ::new (out) std::string(names.alpha_arg2);
    }
    return g_orig_gen_blend_operand(out, source, blend, is_arg1, is_alpha);
}

std::string* __fastcall HookGenLayerBlendOp(
    std::string* out,
    const OgreLayerBlendModeExRaw* blend,
    uint8_t is_alpha) {
    if (!tls_shader_gen.active || !blend) {
        return g_orig_gen_blend_op(out, blend, is_alpha);
    }

    std::string* result = nullptr;
    if (blend->operation == kLayerBlendOpManual && tls_shader_gen.stage_idx < kMaxFfTextureStages) {
        std::string arg1;
        std::string arg2;
        HookGenLayerBlendOperand(&arg1, static_cast<int>(blend->source1), blend, 1, is_alpha);
        HookGenLayerBlendOperand(&arg2, static_cast<int>(blend->source2), blend, 0, is_alpha);

        const uint32_t s = tls_shader_gen.stage_idx;
        const auto& names = GetUniformNameTable()[s];
        const uint16_t bit = static_cast<uint16_t>(1U << s);
        const std::string& factor_name = is_alpha ? names.alpha_factor : names.col_factor;
        if (is_alpha) {
            tls_shader_gen.mask.alpha_factor |= bit;
        } else {
            tls_shader_gen.mask.col_factor |= bit;
        }

        // OGRE LBX_BLEND_MANUAL evaluates factor*arg1 + (1-factor)*arg2 == lerp(arg2, arg1, factor).
        result = ::new (out) std::string("lerp((" + arg2 + "),(" + arg1 + ")," + factor_name + ")");
    } else {
        result = g_orig_gen_blend_op(out, blend, is_alpha);
    }

    if (is_alpha) {
        ++tls_shader_gen.stage_idx;
    }
    return result;
}

uintptr_t __fastcall HookGetFixedFuncShaderPrograms(
    uintptr_t self,
    const void* lang,
    uintptr_t vdecl,
    uintptr_t ff_state) {
    ScopedManualLayerNormalize guard(ff_state, &tls_manual_snapshot);
    if (tls_manual_snapshot.has_any) {
        g_normalized_lookups.fetch_add(1, std::memory_order_relaxed);
    }
    return g_orig_get_programs(self, lang, vdecl, ff_state);
}

}  // namespace

bool InstallFixedFuncManualUniforms(void* d3d11_base, LogFn logger) {
    g_log = logger;
    if (!d3d11_base) {
        return false;
    }

    void* prog_target = ResolveCodeTarget(
        d3d11_base, kRvaGetFixedFuncShaderPrograms, kExpectedGetFfPrograms, sizeof(kExpectedGetFfPrograms),
        kScanGetFfPrograms, nullptr, sizeof(kScanGetFfPrograms));
    void* orig_prog = nullptr;
    if (!prog_target || !g_detour_get_programs.Install(
            prog_target,
            kExpectedGetFfPrograms,
            sizeof(kExpectedGetFfPrograms),
            reinterpret_cast<void*>(&HookGetFixedFuncShaderPrograms),
            &orig_prog)) {
        if (g_log) {
            g_log("[RouteB][FFManual] FAILED detour on getFixedFuncShaderPrograms");
        }
        return false;
    }
    g_orig_get_programs = reinterpret_cast<GetFfShaderProgramsFn>(orig_prog);

    void* op_target = ResolveCodeTarget(
        d3d11_base, kRvaGenLayerBlendOp, kExpectedGenBlendOp, sizeof(kExpectedGenBlendOp),
        kScanGenBlendOp, nullptr, sizeof(kScanGenBlendOp));
    void* orig_op = nullptr;
    if (!op_target || !g_detour_gen_blend_op.Install(
            op_target,
            kExpectedGenBlendOp,
            sizeof(kExpectedGenBlendOp),
            reinterpret_cast<void*>(&HookGenLayerBlendOp),
            &orig_op)) {
        if (g_log) {
            g_log("[RouteB][FFManual] FAILED detour on genLayerBlendOp");
        }
        g_detour_get_programs.Remove();
        return false;
    }
    g_orig_gen_blend_op = reinterpret_cast<GenLayerBlendOpFn>(orig_op);

    void* operand_target = ResolveCodeTarget(
        d3d11_base, kRvaGenLayerBlendOperand, kExpectedGenBlendOperand, sizeof(kExpectedGenBlendOperand),
        kScanGenBlendOperand, nullptr, sizeof(kScanGenBlendOperand));
    void* orig_operand = nullptr;
    if (!operand_target || !g_detour_gen_blend_operand.Install(
            operand_target,
            kExpectedGenBlendOperand,
            sizeof(kExpectedGenBlendOperand),
            reinterpret_cast<void*>(&HookGenLayerBlendOperand),
            &orig_operand)) {
        if (g_log) {
            g_log("[RouteB][FFManual] FAILED detour on genLayerBlendOperand");
        }
        g_detour_gen_blend_op.Remove();
        g_detour_get_programs.Remove();
        return false;
    }
    g_orig_gen_blend_operand = reinterpret_cast<GenLayerBlendOperandFn>(orig_operand);

    // Eagerly construct static name table once on startup thread.
    (void)GetUniformNameTable();
    g_hooked.store(true, std::memory_order_relaxed);
    if (g_log) {
        g_log("[RouteB][FFManual] Uniform parameterization active for LBS_MANUAL / LBX_BLEND_MANUAL");
    }
    return true;
}

void RemoveFixedFuncManualUniforms() {
    g_hooked.store(false, std::memory_order_relaxed);
    tls_shader_gen = ShaderGenTlsState{};
    tls_manual_snapshot = ManualFrameSnapshot{};
    g_detour_gen_blend_operand.Remove();
    g_detour_gen_blend_op.Remove();
    g_detour_get_programs.Remove();
    g_orig_gen_blend_operand = nullptr;
    g_orig_gen_blend_op = nullptr;
    g_orig_get_programs = nullptr;
}

void BeginFixedFuncManualShaderGen() {
    tls_shader_gen.active = g_hooked.load(std::memory_order_relaxed);
    tls_shader_gen.stage_idx = 0;
    tls_shader_gen.mask = ManualUniformMask{};
}

void EndFixedFuncManualShaderGen(std::string* out_src) {
    const ShaderGenTlsState state = tls_shader_gen;
    tls_shader_gen = ShaderGenTlsState{};
    if (!state.active || !out_src || !state.mask.Any()) {
        return;
    }
    if (InjectManualUniformDeclarations(*out_src, state.mask)) {
        g_unique_manual_shaders.fetch_add(1, std::memory_order_relaxed);
    }
}

void ApplyFixedFuncManualUniforms(uintptr_t ff_progs) {
    if (!tls_manual_snapshot.has_any || !ff_progs) {
        tls_manual_snapshot.has_any = false;
        return;
    }
    const uintptr_t ps_params = *reinterpret_cast<const uintptr_t*>(ff_progs + kFfProgsOffPsParamsRep);
    if (!ps_params) {
        tls_manual_snapshot.has_any = false;
        return;
    }

    const auto& names = GetUniformNameTable();
    for (size_t i = 0; i < tls_manual_snapshot.layer_count; ++i) {
        const auto& s = tls_manual_snapshot.stages[i];
        const auto& n = names[i];
        if (s.has_col_arg1) {
            WriteCachedFloatParamByMsvcStr(ps_params, &n.col_arg1, s.col_arg1, sizeof(s.col_arg1));
        }
        if (s.has_col_arg2) {
            WriteCachedFloatParamByMsvcStr(ps_params, &n.col_arg2, s.col_arg2, sizeof(s.col_arg2));
        }
        if (s.has_col_factor) {
            WriteCachedFloatParamByMsvcStr(ps_params, &n.col_factor, &s.col_factor, sizeof(s.col_factor));
        }
        if (s.has_alpha_arg1) {
            WriteCachedFloatParamByMsvcStr(ps_params, &n.alpha_arg1, &s.alpha_arg1, sizeof(s.alpha_arg1));
        }
        if (s.has_alpha_arg2) {
            WriteCachedFloatParamByMsvcStr(ps_params, &n.alpha_arg2, &s.alpha_arg2, sizeof(s.alpha_arg2));
        }
        if (s.has_alpha_factor) {
            WriteCachedFloatParamByMsvcStr(ps_params, &n.alpha_factor, &s.alpha_factor, sizeof(s.alpha_factor));
        }
    }
    tls_manual_snapshot.has_any = false;
    g_param_updates.fetch_add(1, std::memory_order_relaxed);
}

FixedFuncManualStats GetFixedFuncManualStats() {
    FixedFuncManualStats stats{};
    stats.hooked = g_hooked.load(std::memory_order_relaxed);
    stats.normalized_lookups = g_normalized_lookups.load(std::memory_order_relaxed);
    stats.unique_manual_shaders = g_unique_manual_shaders.load(std::memory_order_relaxed);
    stats.param_updates = g_param_updates.load(std::memory_order_relaxed);
    return stats;
}

}  // namespace routeb
