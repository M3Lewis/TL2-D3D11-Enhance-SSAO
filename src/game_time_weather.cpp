#include "game_time_weather.h"
#include "d3d11_post_types.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace routeb {

static PostLogFn g_log = nullptr;
void SetWeatherLogger(PostLogFn logger) {
    g_log = logger;
}

// Global weather & time states
std::atomic<bool> g_weather_rain_enabled{false};
std::atomic<int> g_weather_rain_strength_pct{100};
std::atomic<int> g_time_of_day_hours{-1};
std::atomic<bool> g_time_of_day_locked{false};

using FnEditorSetTimeOfDay = void (__fastcall *)(float);
using FnEditorGetTimeOfDay = float (__fastcall *)();
using FnEditorSetUpdateTimeOfDay = void (__fastcall *)(bool);
using FnEditorSetTimeOfDayMult = void (__fastcall *)(float);
using FnEditorSetShowWeatherEffects = void (__fastcall *)(bool);
using FnEditorGetShowWeatherEffects = bool (__fastcall *)();
using FnGetActiveLevel = void* (__fastcall *)();

static FnEditorSetTimeOfDay g_fnSetTimeOfDay = nullptr;
static FnEditorGetTimeOfDay g_fnGetTimeOfDay = nullptr;
static FnEditorSetUpdateTimeOfDay g_fnSetUpdateTimeOfDay = nullptr;
static FnEditorSetTimeOfDayMult g_fnSetTimeOfDayMult = nullptr;
static FnEditorSetShowWeatherEffects g_fnSetShowWeather = nullptr;
static FnEditorGetShowWeatherEffects g_fnGetShowWeather = nullptr;
static FnGetActiveLevel g_fnGetActiveLevel = nullptr;
static int32_t g_offTimeOfDay = -1;       // Expected 0x278C from machine code
static int32_t g_offTimeOfDayMult = -1;   // Expected from SetTimeOfDayMult
static int32_t g_offUpdateTimeOfDay = -1; // Expected from SetUpdateTimeOfDay
static std::atomic<bool> g_engine_funcs_inited{false};

static void DumpBytes(const char* tag, const void* addr, size_t len) {
    if (!addr || !g_log) return;
    const auto* p = static_cast<const uint8_t*>(addr);
    char buf[256] = {};
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 4 < sizeof(buf); ++i) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02X ", p[i]);
    }
    g_log("[RouteC][TimeOfDay] %s (%p): %s", tag, addr, buf);
}

// Expert advice 10 (Scheme C): dynamically extract g_fnGetActiveLevel and +0x278C from EditorGetTimeOfDay
bool ResolveDirectLevelTimeAccess() {
    if (g_fnGetActiveLevel && g_offTimeOfDay >= 0) {
        return true;
    }
    if (!g_fnGetTimeOfDay && !g_fnSetTimeOfDay) {
        return false;
    }

    // Pattern in EditorGetTimeOfDay: E8 <rel32> F3 0F 10 80 <disp32>
    if (g_fnGetTimeOfDay) {
        const auto* code = reinterpret_cast<const uint8_t*>(g_fnGetTimeOfDay);
        for (size_t i = 0; i < 64; ++i) {
            if (code[i] == 0xE8 &&
                code[i + 5] == 0xF3 && code[i + 6] == 0x0F &&
                code[i + 7] == 0x10 && code[i + 8] == 0x80)
            {
                int32_t rel32 = 0;
                int32_t disp32 = 0;
                memcpy(&rel32,  code + i + 1, sizeof(rel32));
                memcpy(&disp32, code + i + 9, sizeof(disp32));

                const uint8_t* target = code + i + 5 + rel32;
                g_fnGetActiveLevel = reinterpret_cast<FnGetActiveLevel>(const_cast<uint8_t*>(target));
                g_offTimeOfDay = disp32;

                if (g_log) {
                    g_log("[RouteC][TimeOfDay] Resolved GetActiveLevel=%p, offTimeOfDay=0x%X",
                          g_fnGetActiveLevel, g_offTimeOfDay);
                    DumpBytes("GetActiveLevel 64B", (void*)g_fnGetActiveLevel, 64);
                    DumpBytes("SetTimeOfDayMult 64B", (void*)g_fnSetTimeOfDayMult, 64);
                    DumpBytes("SetUpdateTimeOfDay 64B", (void*)g_fnSetUpdateTimeOfDay, 64);
                }
                return true;
            }
        }
    }

    // Fallback pattern in EditorSetTimeOfDay: E8 <rel32> F3 0F 11 ... <disp32>
    if (g_fnSetTimeOfDay) {
        const auto* code = reinterpret_cast<const uint8_t*>(g_fnSetTimeOfDay);
        for (size_t i = 0; i < 64; ++i) {
            if (code[i] == 0xE8 &&
                code[i + 5] == 0xF3 && code[i + 6] == 0x0F &&
                code[i + 7] == 0x11 && (code[i + 8] == 0x80 || code[i + 8] == 0xB0))
            {
                int32_t rel32 = 0;
                int32_t disp32 = 0;
                memcpy(&rel32,  code + i + 1, sizeof(rel32));
                memcpy(&disp32, code + i + 9, sizeof(disp32));

                const uint8_t* target = code + i + 5 + rel32;
                g_fnGetActiveLevel = reinterpret_cast<FnGetActiveLevel>(const_cast<uint8_t*>(target));
                g_offTimeOfDay = disp32;

                if (g_log) {
                    g_log("[RouteC][TimeOfDay] Fallback resolved GetActiveLevel=%p, offTimeOfDay=0x%X",
                          g_fnGetActiveLevel, g_offTimeOfDay);
                    DumpBytes("GetActiveLevel 64B", (void*)g_fnGetActiveLevel, 64);
                    DumpBytes("SetTimeOfDayMult 64B", (void*)g_fnSetTimeOfDayMult, 64);
                    DumpBytes("SetUpdateTimeOfDay 64B", (void*)g_fnSetUpdateTimeOfDay, 64);
                }
                return true;
            }
        }
    }
    return false;
}

bool DirectSetTimeOfDay(float hours) {
    if (!ResolveDirectLevelTimeAccess() || !g_fnGetActiveLevel || g_offTimeOfDay < 0) {
        return false;
    }
    DWORD excCode = 0;
    __try {
        void* level = g_fnGetActiveLevel();
        if (!level) {
            if (g_log) {
                g_log("[RouteC][TimeOfDay] GetActiveLevel() returned NULL (in menu or loading?)");
            }
            return false;
        }
        auto* pTime = reinterpret_cast<float*>(static_cast<uint8_t*>(level) + g_offTimeOfDay);
        float before = *pTime;
        *pTime = hours;
        float after = *pTime;
        if (g_log) {
            g_log("[RouteC][TimeOfDay] DirectWrite level=%p off=0x%X: %.3f -> %.3f",
                  level, g_offTimeOfDay, before, after);
        }
        return true;
    } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        if (g_log) {
            g_log("[RouteC][TimeOfDay] DirectSetTimeOfDay EXCEPTION 0x%08X", excCode);
        }
        return false;
    }
}

float DirectGetTimeOfDay() {
    if (!ResolveDirectLevelTimeAccess() || !g_fnGetActiveLevel || g_offTimeOfDay < 0) {
        return -999.0f;
    }
    DWORD excCode = 0;
    __try {
        void* level = g_fnGetActiveLevel();
        if (!level) {
            return -999.0f;
        }
        const auto* pTime = reinterpret_cast<const float*>(static_cast<const uint8_t*>(level) + g_offTimeOfDay);
        return *pTime;
    } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        if (g_log) {
            g_log("[RouteC][TimeOfDay] DirectGetTimeOfDay EXCEPTION 0x%08X", excCode);
        }
        return -999.0f;
    }
}

bool IsActiveGameLevelLoaded() {
    EnsureEngineWeatherTimeFuncs();
    if (!ResolveDirectLevelTimeAccess() || !g_fnGetActiveLevel) {
        return false;
    }
    DWORD excCode = 0;
    __try {
        return g_fnGetActiveLevel() != nullptr;
    } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void EnsureEngineWeatherTimeFuncs() {
    if (g_engine_funcs_inited.load(std::memory_order_relaxed)) {
        return;
    }
    HMODULE hExe = ::GetModuleHandleW(nullptr);
    if (hExe) {
        g_fnSetTimeOfDay = reinterpret_cast<FnEditorSetTimeOfDay>(::GetProcAddress(hExe, "EditorSetTimeOfDay"));
        g_fnGetTimeOfDay = reinterpret_cast<FnEditorGetTimeOfDay>(::GetProcAddress(hExe, "EditorGetTimeOfDay"));
        g_fnSetUpdateTimeOfDay = reinterpret_cast<FnEditorSetUpdateTimeOfDay>(::GetProcAddress(hExe, "EditorSetUpdateTimeOfDay"));
        g_fnSetTimeOfDayMult = reinterpret_cast<FnEditorSetTimeOfDayMult>(::GetProcAddress(hExe, "EditorSetTimeOfDayMult"));
        g_fnSetShowWeather = reinterpret_cast<FnEditorSetShowWeatherEffects>(::GetProcAddress(hExe, "EditorSetShowWeatherEffects"));
        g_fnGetShowWeather = reinterpret_cast<FnEditorGetShowWeatherEffects>(::GetProcAddress(hExe, "EditorGetShowWeatherEffects"));

        if (g_log) {
            g_log("[RouteC][TimeOfDay] Engine Weather/Time Funcs Init: hExe=%p Set=%p Get=%p SetUpd=%p SetMult=%p ShowWeather=%p",
                  hExe, g_fnSetTimeOfDay, g_fnGetTimeOfDay, g_fnSetUpdateTimeOfDay, g_fnSetTimeOfDayMult, g_fnSetShowWeather);
            if (g_fnSetTimeOfDay) {
                const auto* b = reinterpret_cast<const uint8_t*>(g_fnSetTimeOfDay);
                char hexbuf[256]{};
                for (int i = 0; i < 48; ++i) {
                    snprintf(hexbuf + i * 3, sizeof(hexbuf) - i * 3, "%02X ", b[i]);
                }
                g_log("[RouteC][TimeOfDay] SetTimeOfDay 48B: %s", hexbuf);
            }
            if (g_fnGetTimeOfDay) {
                const auto* b = reinterpret_cast<const uint8_t*>(g_fnGetTimeOfDay);
                char hexbuf[256]{};
                for (int i = 0; i < 48; ++i) {
                    snprintf(hexbuf + i * 3, sizeof(hexbuf) - i * 3, "%02X ", b[i]);
                }
                g_log("[RouteC][TimeOfDay] GetTimeOfDay 48B: %s", hexbuf);
            }
        }
        ResolveDirectLevelTimeAccess();
    }
    g_engine_funcs_inited.store(true, std::memory_order_relaxed);
}

void SetWeatherRainEnabled(bool enabled) {
    g_weather_rain_enabled.store(enabled, std::memory_order_relaxed);
    EnsureEngineWeatherTimeFuncs();
    if (g_fnSetShowWeather) {
        DWORD excCode = 0;
        __try {
            g_fnSetShowWeather(enabled);
            if (g_log) {
                g_log("[RouteC][Weather] SetShowWeatherEffects(%d) invoked", enabled ? 1 : 0);
            }
        } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
            if (g_log) {
                g_log("[RouteC][Weather] EXCEPTION 0x%08X during SetShowWeatherEffects(%d)!", excCode, enabled ? 1 : 0);
            }
        }
    }
}

bool IsWeatherRainEnabled() {
    return g_weather_rain_enabled.load(std::memory_order_relaxed);
}

bool ToggleWeatherRainEnabled() {
    const bool next = !IsWeatherRainEnabled();
    SetWeatherRainEnabled(next);
    return next;
}

void SetWeatherRainStrengthPct(int pct) {
    g_weather_rain_strength_pct.store((std::clamp)(pct, 0, 200), std::memory_order_relaxed);
}

int GetWeatherRainStrengthPct() {
    return (std::clamp)(g_weather_rain_strength_pct.load(std::memory_order_relaxed), 0, 200);
}

void SetTimeOfDayHours(int hours) {
    g_time_of_day_hours.store(hours, std::memory_order_relaxed);
    EnsureEngineWeatherTimeFuncs();
    if (hours >= 0 && hours <= 24) {
        // Scheme C: Direct Write to ActiveLevel + 0x278C
        const bool direct_ok = DirectSetTimeOfDay(static_cast<float>(hours));
        if (!direct_ok && g_fnSetTimeOfDay) {
            DWORD excCode = 0;
            __try {
                g_fnSetTimeOfDay(static_cast<float>(hours));
            } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
                if (g_log) {
                    g_log("[RouteC][TimeOfDay] EXCEPTION 0x%08X during SetTimeOfDay(%d)!", excCode, hours);
                }
            }
        }
        // Time scale control: 0.0f when locked to freeze natural progression, while keep update tick alive
        if (g_fnSetTimeOfDayMult) {
            DWORD excCode = 0;
            __try {
                g_fnSetTimeOfDayMult(g_time_of_day_locked.load(std::memory_order_relaxed) ? 0.0f : 1.0f);
            } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
                if (g_log) {
                    g_log("[RouteC][TimeOfDay] EXCEPTION 0x%08X during SetTimeOfDayMult!", excCode);
                }
            }
        }
        if (g_fnSetUpdateTimeOfDay) {
            DWORD excCode = 0;
            __try {
                g_fnSetUpdateTimeOfDay(true);
            } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
                if (g_log) {
                    g_log("[RouteC][TimeOfDay] EXCEPTION 0x%08X during SetUpdateTimeOfDay!", excCode);
                }
            }
        }
    } else {
        // -1: Auto / follow natural time
        DWORD excCode = 0;
        __try {
            if (g_fnSetTimeOfDayMult) {
                g_fnSetTimeOfDayMult(1.0f); // Restore normal time scale
            }
            if (g_fnSetUpdateTimeOfDay) {
                g_fnSetUpdateTimeOfDay(true);
            }
            if (g_log) {
                g_log("[RouteC][TimeOfDay] SetHours(-1 Auto): restored timeScale=1.0, update=true");
            }
        } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
            if (g_log) {
                g_log("[RouteC][TimeOfDay] EXCEPTION 0x%08X during Auto time restore!", excCode);
            }
        }
    }
}

int GetTimeOfDayHours() {
    const int val = g_time_of_day_hours.load(std::memory_order_relaxed);
    if (val >= 0) {
        return val;
    }
    EnsureEngineWeatherTimeFuncs();
    const float dt = DirectGetTimeOfDay();
    if (dt >= 0.0f && dt <= 24.0f) {
        return static_cast<int>(std::lround(dt));
    }
    if (g_fnGetTimeOfDay) {
        DWORD excCode = 0;
        __try {
            const float t = g_fnGetTimeOfDay();
            if (t >= 0.0f && t <= 24.0f) {
                return static_cast<int>(std::lround(t));
            }
        } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
            if (g_log) {
                g_log("[RouteC][TimeOfDay] EXCEPTION 0x%08X during GetTimeOfDay!", excCode);
            }
        }
    }
    return 12;
}

void SetTimeOfDayLocked(bool locked) {
    g_time_of_day_locked.store(locked, std::memory_order_relaxed);
    EnsureEngineWeatherTimeFuncs();
    DWORD excCode = 0;
    __try {
        if (g_fnSetTimeOfDayMult) {
            g_fnSetTimeOfDayMult(locked ? 0.0f : 1.0f);
        }
        if (g_fnSetUpdateTimeOfDay) {
            g_fnSetUpdateTimeOfDay(true); // Must remain true so light/skybox interpolates
        }
        if (g_log) {
            g_log("[RouteC][TimeOfDay] SetLocked(%d): timeScale=%.1f update=1", locked ? 1 : 0, locked ? 0.0f : 1.0f);
        }
    } __except (excCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        if (g_log) {
            g_log("[RouteC][TimeOfDay] EXCEPTION 0x%08X during SetTimeOfDayLocked(%d)!", excCode, locked ? 1 : 0);
        }
    }
}

bool IsTimeOfDayLocked() {
    return g_time_of_day_locked.load(std::memory_order_relaxed);
}

bool ToggleTimeOfDayLocked() {
    const bool next = !IsTimeOfDayLocked();
    SetTimeOfDayLocked(next);
    return next;
}

}  // namespace routeb
