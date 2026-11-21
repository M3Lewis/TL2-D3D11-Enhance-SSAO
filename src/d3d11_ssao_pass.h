#pragma once

#include "d3d11_post_types.h"

namespace routeb {

struct AoTuneConfig {
    bool initialized = false;
    float radius_world = 0.95f;
    float bias_cos = 0.24f;
    float strength = 1.20f;
    float min_ao_floor = 0.58f;
    float depth_edge_scale = 55.0f;
    float fade_start = 42.0f;
    float fade_end_cap = 78.0f;
    int bench_mode = 1;  // 0=off, 1=sample every 32 frames, 2=every frame
};

const AoTuneConfig& GetAoTuneConfig(PostLogFn logger);

bool ExecuteAoPass(const AoRenderRequest& req, PostLogFn logger);

// Legacy shim
inline bool ExecuteRouteCAoPass(const AoRenderRequest& req, PostLogFn logger) {
    return ExecuteAoPass(req, logger);
}

}  // namespace routeb
