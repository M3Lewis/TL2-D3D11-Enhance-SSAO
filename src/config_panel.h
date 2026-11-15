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
    int p2_filter_preset = 1;  // 0=Off, 1=Hob, 2=Dark Film

    bool operator==(const VisualConfig& other) const = default;
};

constexpr int kCustomPresetCount = 4;

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

// Preset helpers (builtin: 0=全开高清, 1=暗黑地牢, 2=纯原版; custom: 0..3).
VisualConfig MakeBuiltinPreset(int builtin_index);
bool ApplyBuiltinPreset(int builtin_index);
bool SaveCustomPreset(int slot_index);
bool ApplyCustomPreset(int slot_index);
PresetSlot GetCustomPresetSlot(int slot_index);
void ResetConfigPanelStateForTest();

// Interactive panel visibility & WndProc subclassing.
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
