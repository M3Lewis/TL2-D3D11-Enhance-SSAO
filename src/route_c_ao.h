#pragma once

#include <cstdint>

namespace routeb {

using RouteCLogFn = void (*)(const char* fmt, ...);

struct RouteCStats {
    uint64_t render_scene_calls = 0;
    uint64_t perspective_world_candidates = 0;
    uint64_t queue_started_calls = 0;
    uint64_t queue_ended_calls = 0;
    uint64_t survey_unique_targets = 0;
    uint64_t survey_unique_queues = 0;
    bool hooked = false;
    bool c0_survey_active = false;
};

bool InstallRouteCAoPipeline(void* ogre_main_base, RouteCLogFn logger);
void RemoveRouteCAoPipeline();
bool VerifyRouteCShaders();
RouteCStats GetRouteCAoStats();
void LogRouteCSurveySummary(RouteCLogFn logger);

void SetRouteCAoRuntimeEnabled(bool enabled);
bool IsRouteCAoRuntimeEnabled();
bool ToggleRouteCAoRuntimeEnabled();
void TriggerRouteCToastForTest();

void OnRouteCScenePassObserved(
    const char* pass_name,
    bool lighting,
    bool depth_write,
    bool transparent,
    bool alpha_reject,
    bool has_vp,
    bool has_fp);

}  // namespace routeb
