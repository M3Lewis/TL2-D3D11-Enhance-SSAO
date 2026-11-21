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
void SetRouteCSscsRuntimeEnabled(bool enabled);
bool IsRouteCSscsRuntimeEnabled();
bool ToggleRouteCSscsRuntimeEnabled();
void SetP1SoftParticlesEnabled(bool enabled);
bool IsP1SoftParticlesEnabled();
bool ToggleP1SoftParticlesEnabled();
void SetP2AerialVignetteEnabled(bool enabled);
bool IsP2AerialVignetteEnabled();
bool ToggleP2AerialVignetteEnabled();
void SetP2SsrEnabled(bool enabled);
bool IsP2SsrEnabled();
bool ToggleP2SsrEnabled();
void SetP2LutPreset(int preset);
int GetP2LutPreset();
const char* P2LutPresetName(int preset);

void SetRouteCAoStrengthPct(int pct);
int GetRouteCAoStrengthPct();
void SetRouteCSscsStrengthPct(int pct);
int GetRouteCSscsStrengthPct();
void SetP1SoftParticlesStrengthPct(int pct);
int GetP1SoftParticlesStrengthPct();
void SetP2AerialVignetteStrengthPct(int pct);
int GetP2AerialVignetteStrengthPct();
void SetP2SsrStrengthPct(int pct);
int GetP2SsrStrengthPct();
void SetP2LutStrengthPct(int pct);
int GetP2LutStrengthPct();

void SetWeatherRainEnabled(bool enabled);
bool IsWeatherRainEnabled();
bool ToggleWeatherRainEnabled();
void SetWeatherRainStrengthPct(int pct);
int GetWeatherRainStrengthPct();

void SetTimeOfDayHours(int hours); // -1 = Auto/Follow Engine, 0..24
int GetTimeOfDayHours();
void SetTimeOfDayLocked(bool locked);
bool IsTimeOfDayLocked();
bool ToggleTimeOfDayLocked();

bool IsP2CustomCubeLutLoaded();
bool IsRouteCWorldDepthReadyForParticles();
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
