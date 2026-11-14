#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace routeb {

using LogFn = void (*)(const char* fmt, ...);

constexpr uint32_t kLayerBlendOpManual = 12;   // Ogre::LBX_BLEND_MANUAL
constexpr uint32_t kLayerBlendSrcManual = 4;   // Ogre::LBS_MANUAL
constexpr size_t kMaxFfTextureStages = 16;

// Exact MSVC x64 layout verified from RenderSystem_Direct3D11.dll (sub_180033580 / sub_180034F10).
struct OgreLayerBlendModeExRaw {
    uint32_t blend_type;     // +0
    uint32_t operation;      // +4
    uint32_t source1;        // +8
    uint32_t source2;        // +12
    float colour_arg1[4];    // +16 (r, g, b, a)
    float colour_arg2[4];    // +32 (r, g, b, a)
    float alpha_arg1;        // +48
    float alpha_arg2;        // +52
    float factor;            // +56
};
static_assert(sizeof(OgreLayerBlendModeExRaw) == 60, "OgreLayerBlendModeExRaw must be 60 bytes");

struct OgreTextureLayerStateRaw {
    uint32_t tex_type;               // +0
    uint32_t coord_index;            // +4
    OgreLayerBlendModeExRaw colour;  // +8..67
    OgreLayerBlendModeExRaw alpha;   // +68..127
    uint8_t env_map_enabled;         // +128
    uint8_t pad[3];                  // +129..131
};
static_assert(sizeof(OgreTextureLayerStateRaw) == 132, "OgreTextureLayerStateRaw must be 132 bytes");

struct ManualStageSnapshot {
    bool has_col_arg1 = false;
    bool has_col_arg2 = false;
    bool has_col_factor = false;
    bool has_alpha_arg1 = false;
    bool has_alpha_arg2 = false;
    bool has_alpha_factor = false;
    float col_arg1[4] = {};
    float col_arg2[4] = {};
    float col_factor = 0.0f;
    float alpha_arg1 = 0.0f;
    float alpha_arg2 = 0.0f;
    float alpha_factor = 0.0f;
};

struct ManualFrameSnapshot {
    size_t layer_count = 0;
    bool has_any = false;
    ManualStageSnapshot stages[kMaxFfTextureStages]{};
};

struct ManualUniformMask {
    uint16_t col_arg1 = 0;
    uint16_t col_arg2 = 0;
    uint16_t col_factor = 0;
    uint16_t alpha_arg1 = 0;
    uint16_t alpha_arg2 = 0;
    uint16_t alpha_factor = 0;

    bool Any() const {
        return (col_arg1 | col_arg2 | col_factor | alpha_arg1 | alpha_arg2 | alpha_factor) != 0;
    }
};

struct FixedFuncManualStats {
    bool hooked = false;
    uint64_t normalized_lookups = 0;
    uint64_t unique_manual_shaders = 0;
    uint64_t param_updates = 0;
};

inline void CaptureAndNormalizeManualLayers(
    OgreTextureLayerStateRaw* layers,
    size_t count,
    ManualFrameSnapshot* out_snapshot) {
    if (!out_snapshot) {
        return;
    }
    out_snapshot->layer_count = 0;
    out_snapshot->has_any = false;
    if (!layers || count == 0) {
        return;
    }

    const size_t n = (std::min)(count, kMaxFfTextureStages);
    out_snapshot->layer_count = n;
    for (size_t i = 0; i < n; ++i) {
        auto& layer = layers[i];
        auto& dst = out_snapshot->stages[i];
        dst = ManualStageSnapshot{};

        if (layer.colour.source1 == kLayerBlendSrcManual) {
            dst.has_col_arg1 = true;
            std::memcpy(dst.col_arg1, layer.colour.colour_arg1, sizeof(dst.col_arg1));
            std::memset(layer.colour.colour_arg1, 0, sizeof(layer.colour.colour_arg1));
        }
        if (layer.colour.source2 == kLayerBlendSrcManual) {
            dst.has_col_arg2 = true;
            std::memcpy(dst.col_arg2, layer.colour.colour_arg2, sizeof(dst.col_arg2));
            std::memset(layer.colour.colour_arg2, 0, sizeof(layer.colour.colour_arg2));
        }
        if (layer.colour.operation == kLayerBlendOpManual) {
            dst.has_col_factor = true;
            dst.col_factor = layer.colour.factor;
            layer.colour.factor = 0.0f;
        }
        if (layer.alpha.source1 == kLayerBlendSrcManual) {
            dst.has_alpha_arg1 = true;
            dst.alpha_arg1 = layer.alpha.alpha_arg1;
            layer.alpha.alpha_arg1 = 0.0f;
        }
        if (layer.alpha.source2 == kLayerBlendSrcManual) {
            dst.has_alpha_arg2 = true;
            dst.alpha_arg2 = layer.alpha.alpha_arg2;
            layer.alpha.alpha_arg2 = 0.0f;
        }
        if (layer.alpha.operation == kLayerBlendOpManual) {
            dst.has_alpha_factor = true;
            dst.alpha_factor = layer.alpha.factor;
            layer.alpha.factor = 0.0f;
        }

        if (dst.has_col_arg1 || dst.has_col_arg2 || dst.has_col_factor ||
            dst.has_alpha_arg1 || dst.has_alpha_arg2 || dst.has_alpha_factor) {
            out_snapshot->has_any = true;
        }
    }
}

inline void RestoreManualLayers(
    OgreTextureLayerStateRaw* layers,
    size_t count,
    const ManualFrameSnapshot& snapshot) {
    if (!layers || count == 0 || !snapshot.has_any) {
        return;
    }
    const size_t n = (std::min)(count, snapshot.layer_count);
    for (size_t i = 0; i < n; ++i) {
        auto& layer = layers[i];
        const auto& src = snapshot.stages[i];
        if (src.has_col_arg1) {
            std::memcpy(layer.colour.colour_arg1, src.col_arg1, sizeof(src.col_arg1));
        }
        if (src.has_col_arg2) {
            std::memcpy(layer.colour.colour_arg2, src.col_arg2, sizeof(src.col_arg2));
        }
        if (src.has_col_factor) {
            layer.colour.factor = src.col_factor;
        }
        if (src.has_alpha_arg1) {
            layer.alpha.alpha_arg1 = src.alpha_arg1;
        }
        if (src.has_alpha_arg2) {
            layer.alpha.alpha_arg2 = src.alpha_arg2;
        }
        if (src.has_alpha_factor) {
            layer.alpha.factor = src.alpha_factor;
        }
    }
}

inline std::string BuildManualUniformDeclarations(const ManualUniformMask& mask) {
    if (!mask.Any()) {
        return {};
    }
    std::string decls;
    auto append_vec4 = [&](uint16_t bits, const char* suffix) {
        char buf[40]{};
        for (size_t s = 0; s < kMaxFfTextureStages; ++s) {
            if (bits & (1U << s)) {
                std::snprintf(buf, sizeof(buf), "float4 FFManC%zu%s;\n", s, suffix);
                decls += buf;
            }
        }
    };
    auto append_scalar = [&](uint16_t bits, char channel, const char* suffix) {
        char buf[40]{};
        for (size_t s = 0; s < kMaxFfTextureStages; ++s) {
            if (bits & (1U << s)) {
                std::snprintf(buf, sizeof(buf), "float FFMan%c%zu%s;\n", channel, s, suffix);
                decls += buf;
            }
        }
    };

    append_vec4(mask.col_arg1, "A1");
    append_vec4(mask.col_arg2, "A2");
    append_scalar(mask.col_factor, 'C', "F");
    append_scalar(mask.alpha_arg1, 'A', "A1");
    append_scalar(mask.alpha_arg2, 'A', "A2");
    append_scalar(mask.alpha_factor, 'A', "F");
    return decls;
}

inline bool InjectManualUniformDeclarations(std::string& hlsl_src, const ManualUniformMask& mask) {
    if (!mask.Any()) {
        return false;
    }
    const std::string decls = BuildManualUniformDeclarations(mask);
    static constexpr const char* kAnchor = "float AlphaRejectValue;\n";
    const size_t anchor_pos = hlsl_src.find(kAnchor);
    if (anchor_pos != std::string::npos) {
        hlsl_src.insert(anchor_pos + std::strlen(kAnchor), decls);
        return true;
    }
    const size_t struct_end = hlsl_src.find("};\n");
    if (struct_end != std::string::npos) {
        hlsl_src.insert(struct_end + 3, decls);
        return true;
    }
    return false;
}

// Hook lifecycle & Phase 3 integration callbacks.
bool InstallFixedFuncManualUniforms(void* d3d11_base, LogFn logger);
void RemoveFixedFuncManualUniforms();
void BeginFixedFuncManualShaderGen();
void EndFixedFuncManualShaderGen(std::string* out_src);
void ApplyFixedFuncManualUniforms(uintptr_t ff_progs);
FixedFuncManualStats GetFixedFuncManualStats();

}  // namespace routeb
