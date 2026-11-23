#pragma once

#include <cstdint>

namespace routeb {

void EnsureEngineWeatherTimeFuncs();

void SetWeatherRainEnabled(bool enabled);
bool IsWeatherRainEnabled();
bool ToggleWeatherRainEnabled();
void SetWeatherRainStrengthPct(int pct);
int GetWeatherRainStrengthPct();

void SetTimeOfDayHours(int hours);  // -1 = Auto/Follow Engine, 0..24
int GetTimeOfDayHours();
float GetEffectiveTimeOfDayFloat();
void SetTimeOfDayLocked(bool locked);
bool IsTimeOfDayLocked();
bool ToggleTimeOfDayLocked();

bool ResolveDirectLevelTimeAccess();
bool DirectSetTimeOfDay(float hours);
float DirectGetTimeOfDay();
uintptr_t GetActiveGameLevelPtr();
bool IsActiveGameLevelLoaded();

void ComputeContinuousTodSkyRgb(float tod_hours, bool is_desert, bool is_dark, float out_rgb[3]);

}  // namespace routeb
