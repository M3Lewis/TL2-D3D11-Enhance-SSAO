#pragma once

#include <cstdint>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace routeb {

using ConfigPanelLogFn = void (*)(const char* fmt, ...);

struct VisualConfig {
    bool phase3_enhance = true;
    bool af_point_enhance = true;
    bool p0_rimlit_enhance = true;
    bool ao_enabled = true;
    bool p0_sscs_enabled = true;
    bool p1_soft_particles = true;
    bool p2_aerial_vignette = true;
    bool p2_height_fog_enabled = true;
    bool p2_ssr_enabled = true;
    bool n2_cas_enabled = true;
    bool n3_light_shafts_enabled = true;
    bool n4_bent_ao_enabled = true;
    bool n5_ibl_wrap_enabled = true;
    int p2_filter_preset = 1;  // 0=Off, 1=Hob, 2=Dark Film
    int p2_lut_preset = 0;     // 0=Off, 1=CinemaTealOrange, 2=ColdNordic, 3=CustomCube
    int smaa_mode = 0;         // 0=Off, 1=Color, 2=Depth+Color
    bool weather_rain_enabled = false;
    int weather_rain_strength = 100; // 0..200 (%)
    int time_of_day_hours = -1;      // -1=Auto, 0..24
    bool time_of_day_locked = false;

    int phase3_strength = 100;         // 0..200 (%)
    int af_point_strength = 100;       // 0..200 (%)
    int p0_rimlit_strength = 100;      // 0..200 (%)
    int ao_strength = 100;             // 0..200 (%)
    int p0_sscs_strength = 100;        // 0..200 (%)
    int p1_soft_strength = 100;        // 0..200 (%)
    int p2_aerial_strength = 100;      // 0..200 (%)
    int p2_ssr_strength = 100;         // 0..200 (%)
    int p2_filter_strength = 100;      // 0..200 (%)
    int p2_lut_strength = 100;         // 0..200 (%)
    int p2_height_fog_strength = 100;  // 0..200 (%)
    int n2_cas_strength = 100;         // 0..200 (%)
    int n3_light_shafts_strength = 100;// 0..200 (%)
    int n4_bent_ao_strength = 100;     // 0..200 (%)
    int n5_ibl_wrap_strength = 100;    // 0..200 (%)

    bool operator==(const VisualConfig& other) const = default;
};

constexpr int kCustomPresetCount = 10;

enum class UiLanguage : int {
    ZhCn = 0,
    EnUs = 1,
};

struct PresetSlot {
    bool used = false;
    VisualConfig cfg;
};

// Synchronizes runtime engine flags <-> VisualConfig.
VisualConfig CaptureVisualConfigFromEngine();
void ApplyVisualConfigToEngine(const VisualConfig& cfg);

// Persistent configuration (.ini beside game executable unless overridden).
std::wstring ResolveDefaultConfigIniPath();
bool LoadVisualConfigAndPresets(const wchar_t* ini_path_override = nullptr);
bool SaveVisualConfigAndPresets(const wchar_t* ini_path_override = nullptr);

// Preset helpers (builtin: 0=全开高清, 1=暗黑地牢, 2=纯原版; custom: 0..9).
VisualConfig MakeBuiltinPreset(int builtin_index);
bool ApplyBuiltinPreset(int builtin_index);
bool SetAllRenderTogglesEnabled(bool enabled);
bool SaveCustomPreset(int slot_index);
bool ApplyCustomPreset(int slot_index);
PresetSlot GetCustomPresetSlot(int slot_index);
void ResetConfigPanelStateForTest();

// Interactive panel visibility, language & WndProc subclassing.
UiLanguage DetectSystemDefaultUiLanguage();
UiLanguage GetUiLanguage();
void SetUiLanguage(UiLanguage lang);
UiLanguage ToggleUiLanguage();

void ToggleConfigPanelOpen();
void SetConfigPanelOpen(bool open);
bool IsConfigPanelOpen();

bool AttachPanelWindow(HWND hwnd);
void DetachPanelWindow();
bool IsPanelWindowAttached();
void ShutdownConfigPanel();

// Fallback hotkey polling when WndProc is not yet subclassed.
bool PollFallbackConfigPanelHotkey();

// Renders the interactive configuration panel and crisp cursor overlay when open.
bool RenderConfigPanelOverlay(
    void* rw,
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    ConfigPanelLogFn logger);

}  // namespace routeb
