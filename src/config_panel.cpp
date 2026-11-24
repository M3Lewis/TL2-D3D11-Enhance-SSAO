#include "config_panel.h"

#include "phase3_scene_pipeline.h"
#include "render_hook_manager.h"
#include "game_time_weather.h"
#include "d3d11_post_renderer.h"
#include "d3d11_smaa_pass.h"
#include "d3d11_overlay_pass.h"

#include <d3d11.h>
#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace routeb {
namespace {

constexpr uint32_t kPanelWidth = 610;
constexpr uint32_t kPanelHeight = 924;
constexpr float kPanelMargin = 16.0f;
constexpr int kTitleBarHeight = 40;
constexpr int kPaddingX = 16;

constexpr int kSliderZoneLeft = 408;
constexpr int kSliderZoneRight = 594;
constexpr int kSliderTrackLeft = 414;
constexpr int kSliderTrackRight = 540;
constexpr int kSliderReadoutLeft = 544;
constexpr int kSliderReadoutRight = 590;
constexpr int kSliderMaxPct = 200;
constexpr int kSliderTimeIndex = 100;

enum class HitTargetKind {
    None = 0,
    TitleDrag,
    CloseButton,
    LangZh,
    LangEn,
    ToggleAllOn,
    ToggleAllOff,
    TogglePhase3,
    ToggleAfPoint,
    ToggleP0RimLit,
    TogglePhase3IblWrap,
    ToggleAo,
    ToggleP0Sscs,
    ToggleShadowMap,
    ToggleP1SoftParticles,
    ToggleP2AerialVignette,
    ToggleP2Ssr,
    FilterOff,
    FilterHob,
    FilterDark,
    LutOff,
    LutTealOrange,
    LutColdNordic,
    LutCustomCube,
    HdrOff,
    HdrAces,
    HdrAgx,
    SmaaOff,
    SmaaColor,
    SmaaDepthColor,
    ToggleWeatherRain,
    TimeAuto,
    TimeDawn,
    TimeNoon,
    TimeDusk,
    TimeMidnight,
    SliderTime,
    BuiltinFullHd,
    BuiltinDarkDungeon,
    BuiltinVanilla,
    CustomLoad0,
    CustomLoad1,
    CustomLoad2,
    CustomLoad3,
    CustomLoad4,
    CustomLoad5,
    CustomLoad6,
    CustomLoad7,
    CustomLoad8,
    CustomLoad9,
    CustomSave0,
    CustomSave1,
    CustomSave2,
    CustomSave3,
    CustomSave4,
    CustomSave5,
    CustomSave6,
    CustomSave7,
    CustomSave8,
    CustomSave9,
    ResetStrengths,
    Slider0,
    Slider1,
    Slider2,
    Slider3,
    Slider4,
    Slider5,
    Slider6,
    Slider7,
    Slider8,
    Slider9,
    Slider10,
    Slider11,
    Slider12,
    Slider13,
};

struct HitZone {
    RECT rect{};
    HitTargetKind kind = HitTargetKind::None;
    const wchar_t* label_zh = L"";
    const wchar_t* label_en = L"";
};

constexpr const wchar_t* kDefaultStatusZh = L"F8/ESC关闭 · 拖动滑块/滚轮调强度 · 右键滑块归位100%";
constexpr const wchar_t* kDefaultStatusEn = L"F8/ESC Close · Drag/Wheel Adjust · Right-Click Reset 100%";

struct PanelRuntimeState {
    std::mutex mutex;
    bool open = false;
    bool dirty = true;
    bool has_custom_pos = false;
    float pos_x = 80.0f;
    float pos_y = 60.0f;
    float last_vp_w = 1280.0f;
    float last_vp_h = 720.0f;

    bool dragging = false;
    int drag_offset_x = 0;
    int drag_offset_y = 0;
    int active_slider_index = -1;

    int hover_index = -1;
    POINT cursor_panel{-1, -1};
    bool cursor_inside = false;

    UiLanguage language = UiLanguage::ZhCn;
    std::array<PresetSlot, kCustomPresetCount> presets{};
    int active_custom_slot = -1;
    std::wstring ini_path;
    std::wstring status_zh = kDefaultStatusZh;
    std::wstring status_en = kDefaultStatusEn;

    HWND hwnd = nullptr;
    bool swallow_esc_keyup = false;
    bool swallow_f8_keyup = false;

    HDC mem_dc = nullptr;
    HBITMAP dib_bitmap = nullptr;
    HGDIOBJ prev_bitmap = nullptr;
    HFONT font_title = nullptr;
    HFONT font_section = nullptr;
    HFONT font_body = nullptr;
    HFONT font_small = nullptr;
    uint32_t* dib_pixels = nullptr;
};

PanelRuntimeState g_panel;
std::atomic<bool> g_wndproc_attached{false};

bool IsEnglishLocked() {
    return g_panel.language == UiLanguage::EnUs;
}

const wchar_t* PickLang(const wchar_t* zh, const wchar_t* en) {
    return IsEnglishLocked() ? en : zh;
}

const wchar_t* ZoneLabel(const HitZone& z) {
    return IsEnglishLocked() ? z.label_en : z.label_zh;
}

void SetStatusBilingualLocked(const wchar_t* zh, const wchar_t* en) {
    g_panel.status_zh = zh ? zh : kDefaultStatusZh;
    g_panel.status_en = en ? en : kDefaultStatusEn;
}

int ClampFilterPreset(int v) {
    if (v < 0 || v > 2) return 1;
    return v;
}

int ClampLutPreset(int v) {
    if (v < 0 || v > 3) return 0;
    return v;
}

int ClampHdrTonemapMode(int v) {
    if (v < 0 || v > 2) return 2;
    return v;
}

int ClampStrengthPct(int v) {
    return (std::clamp)(v, 0, kSliderMaxPct);
}

struct EffectSliderSpec {
    const wchar_t* ini_key;
    const wchar_t* label_zh;
    const wchar_t* label_en;
    int VisualConfig::*member;
    void (*set_fn)(int);
    int (*get_fn)();
};

constexpr size_t kEffectSliderCount = 14;
constexpr std::array<EffectSliderSpec, kEffectSliderCount> kSliderSpecs = {{
    {L"Phase3Strength", L"材质法线与边缘光", L"Surface Normals & Rim", &VisualConfig::phase3_strength, &SetPhase3StrengthPct, &GetPhase3StrengthPct},
    {L"AfPointStrength", L"动态点光增强", L"Dynamic Point Lights", &VisualConfig::af_point_strength, &SetF11PointLightStrengthPct, &GetF11PointLightStrengthPct},
    {L"P0RimLitStrength", L"微法线 / 高光 / 自发光", L"Micro-Normals / Specular", &VisualConfig::p0_rimlit_strength, &SetP0RimLitStrengthPct, &GetP0RimLitStrengthPct},
    {L"AoStrength", L"环境光遮蔽 (SSAO)", L"Ambient Occlusion (SSAO)", &VisualConfig::ao_strength, &SetRouteCAoStrengthPct, &GetRouteCAoStrengthPct},
    {L"P0SscsStrength", L"接触阴影 (SSCS) + 冷色调", L"Contact Shadows (SSCS)", &VisualConfig::p0_sscs_strength, &SetRouteCSscsStrengthPct, &GetRouteCSscsStrengthPct},
    {L"P1SoftStrength", L"屏幕空间软粒子", L"Soft Particles", &VisualConfig::p1_soft_strength, &SetP1SoftParticlesStrengthPct, &GetP1SoftParticlesStrengthPct},
    {L"P2AerialStrength", L"空气透视 + 光学暗角", L"Aerial Fog + Vignette", &VisualConfig::p2_aerial_strength, &SetP2AerialVignetteStrengthPct, &GetP2AerialVignetteStrengthPct},
    {L"P2SsrStrength", L"水面反射 (SSR)", L"Water Reflections (SSR)", &VisualConfig::p2_ssr_strength, &SetP2SsrStrengthPct, &GetP2SsrStrengthPct},
    {L"P2FilterStrength", L"画面风格滤镜", L"Style Filter", &VisualConfig::p2_filter_strength, &SetP2FilterStrengthPct, &GetP2FilterStrengthPct},
    {L"P2LutStrength", L"3D LUT 电影调色", L"3D LUT Grading", &VisualConfig::p2_lut_strength, &SetP2LutStrengthPct, &GetP2LutStrengthPct},
    {L"WeatherRainStrength", L"雨天湿地反射", L"Rain & Wet Ground", &VisualConfig::weather_rain_strength, &SetWeatherRainStrengthPct, &GetWeatherRainStrengthPct},
    {L"N5IblWrapStrength", L"昼夜IBL反光与柔光", L"ToD IBL & Cloth Wrap", &VisualConfig::n5_ibl_wrap_strength, &SetPhase3IblWrapStrengthPct, &GetPhase3IblWrapStrengthPct},
    {L"ShadowMapStrength", L"2048 硬件深度阴影", L"2048 Hardware Shadows", &VisualConfig::p2_shadow_strength, &SetShadowStrengthPct, &GetShadowStrengthPct},
    {L"HdrExposurePct", L"FP16 HDR 曝光倍率", L"FP16 HDR Exposure", &VisualConfig::hdr_exposure_pct, &SetHdrExposurePct, &GetHdrExposurePct},
}};

constexpr std::array<int, 10> kSection1SliderIndices = {0, 1, 2, 11, 3, 4, 12, 5, 6, 7};

int PctFromTrackX(int local_x) {
    const float span = static_cast<float>(kSliderTrackRight - kSliderTrackLeft);
    const float norm = (std::clamp)(static_cast<float>(local_x - kSliderTrackLeft) / span, 0.0f, 1.0f);
    const int step5 = static_cast<int>(std::lround(norm * 40.0f)) * 5;
    return ClampStrengthPct(step5);
}

int HourFromTrackX(int local_x) {
    const float span = static_cast<float>(kSliderTrackRight - kSliderTrackLeft);
    const float norm = (std::clamp)(static_cast<float>(local_x - kSliderTrackLeft) / span, 0.0f, 1.0f);
    const int hour = static_cast<int>(std::lround(norm * 24.0f));
    return (std::clamp)(hour, 0, 24);
}

void ApplyEffectStrengthByIndex(int slider_idx, int pct) {
    if (slider_idx < 0 || static_cast<size_t>(slider_idx) >= kEffectSliderCount) {
        return;
    }
    const int clamped = ClampStrengthPct(pct);
    const auto& spec = kSliderSpecs[static_cast<size_t>(slider_idx)];
    spec.set_fn(clamped);
    wchar_t msg_zh[96] = {};
    wchar_t msg_en[96] = {};
    swprintf_s(msg_zh, L"%ls 强度已调至: %d%%", spec.label_zh, clamped);
    swprintf_s(msg_en, L"%ls strength: %d%%", spec.label_en, clamped);
    SetStatusBilingualLocked(msg_zh, msg_en);
    g_panel.active_custom_slot = -1;
    g_panel.dirty = true;
}

int GetEffectStrengthByIndex(int slider_idx) {
    if (slider_idx < 0 || static_cast<size_t>(slider_idx) >= kEffectSliderCount) {
        return 100;
    }
    return ClampStrengthPct(kSliderSpecs[static_cast<size_t>(slider_idx)].get_fn());
}

std::wstring GetExecutableDirectory() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD len = ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return L".";
    }
    std::wstring path(buf, len);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

void WriteIniInt(const wchar_t* section, const wchar_t* key, int value, const wchar_t* path) {
    wchar_t buf[32] = {};
    swprintf_s(buf, L"%d", value);
    ::WritePrivateProfileStringW(section, key, buf, path);
}

int ReadIniInt(const wchar_t* section, const wchar_t* key, int default_val, const wchar_t* path) {
    return static_cast<int>(::GetPrivateProfileIntW(section, key, default_val, path));
}

int ClampSmaaMode(int mode) {
    return std::clamp(mode, 0, 3);
}

void SaveVisualConfigToSection(const wchar_t* section, const VisualConfig& cfg, const wchar_t* path) {
    WriteIniInt(section, L"Phase3Enhance", cfg.phase3_enhance ? 1 : 0, path);
    WriteIniInt(section, L"AfPointEnhance", cfg.af_point_enhance ? 1 : 0, path);
    WriteIniInt(section, L"P0RimLitEnhance", cfg.p0_rimlit_enhance ? 1 : 0, path);
    WriteIniInt(section, L"AoEnabled", cfg.ao_enabled ? 1 : 0, path);
    WriteIniInt(section, L"P0SscsEnabled", cfg.p0_sscs_enabled ? 1 : 0, path);
    WriteIniInt(section, L"P1SoftParticles", cfg.p1_soft_particles ? 1 : 0, path);
    WriteIniInt(section, L"P2AerialVignette", cfg.p2_aerial_vignette ? 1 : 0, path);
    WriteIniInt(section, L"P2SsrEnabled", cfg.p2_ssr_enabled ? 1 : 0, path);
    WriteIniInt(section, L"N5IblWrapEnabled", cfg.n5_ibl_wrap_enabled ? 1 : 0, path);
    WriteIniInt(section, L"ShadowMapEnabled", cfg.p2_shadow_map_enabled ? 1 : 0, path);
    WriteIniInt(section, L"P2FilterPreset", ClampFilterPreset(cfg.p2_filter_preset), path);
    WriteIniInt(section, L"P2LutPreset", ClampLutPreset(cfg.p2_lut_preset), path);
    WriteIniInt(section, L"HdrTonemapMode", ClampHdrTonemapMode(cfg.hdr_tonemap_mode), path);
    WriteIniInt(section, L"SmaaMode", ClampSmaaMode(cfg.smaa_mode), path);
    WriteIniInt(section, L"WeatherRainEnabled", cfg.weather_rain_enabled ? 1 : 0, path);
    WriteIniInt(section, L"TimeOfDayHours", cfg.time_of_day_hours, path);
    WriteIniInt(section, L"TimeOfDayLocked", cfg.time_of_day_locked ? 1 : 0, path);
    for (const auto& spec : kSliderSpecs) {
        WriteIniInt(section, spec.ini_key, ClampStrengthPct(cfg.*(spec.member)), path);
    }
}

VisualConfig LoadVisualConfigFromSection(
    const wchar_t* section,
    const VisualConfig& fallback,
    const wchar_t* path) {
    VisualConfig out;
    out.phase3_enhance = ReadIniInt(section, L"Phase3Enhance", fallback.phase3_enhance ? 1 : 0, path) != 0;
    out.af_point_enhance = ReadIniInt(section, L"AfPointEnhance", fallback.af_point_enhance ? 1 : 0, path) != 0;
    out.p0_rimlit_enhance = ReadIniInt(section, L"P0RimLitEnhance", fallback.p0_rimlit_enhance ? 1 : 0, path) != 0;
    out.ao_enabled = ReadIniInt(section, L"AoEnabled", fallback.ao_enabled ? 1 : 0, path) != 0;
    out.p0_sscs_enabled = ReadIniInt(section, L"P0SscsEnabled", fallback.p0_sscs_enabled ? 1 : 0, path) != 0;
    out.p1_soft_particles = ReadIniInt(section, L"P1SoftParticles", fallback.p1_soft_particles ? 1 : 0, path) != 0;
    out.p2_aerial_vignette = ReadIniInt(section, L"P2AerialVignette", fallback.p2_aerial_vignette ? 1 : 0, path) != 0;
    out.p2_ssr_enabled = ReadIniInt(section, L"P2SsrEnabled", fallback.p2_ssr_enabled ? 1 : 0, path) != 0;
    out.n5_ibl_wrap_enabled = ReadIniInt(section, L"N5IblWrapEnabled", fallback.n5_ibl_wrap_enabled ? 1 : 0, path) != 0;
    out.p2_shadow_map_enabled = ReadIniInt(section, L"ShadowMapEnabled", fallback.p2_shadow_map_enabled ? 1 : 0, path) != 0;
    out.p2_filter_preset = ClampFilterPreset(
        ReadIniInt(section, L"P2FilterPreset", fallback.p2_filter_preset, path));
    out.p2_lut_preset = ClampLutPreset(
        ReadIniInt(section, L"P2LutPreset", fallback.p2_lut_preset, path));
    out.hdr_tonemap_mode = ClampHdrTonemapMode(
        ReadIniInt(section, L"HdrTonemapMode", fallback.hdr_tonemap_mode, path));
    out.smaa_mode = ClampSmaaMode(
        ReadIniInt(section, L"SmaaMode", fallback.smaa_mode, path));
    out.weather_rain_enabled = ReadIniInt(section, L"WeatherRainEnabled", fallback.weather_rain_enabled ? 1 : 0, path) != 0;
    out.time_of_day_hours = ReadIniInt(section, L"TimeOfDayHours", fallback.time_of_day_hours, path);
    out.time_of_day_locked = ReadIniInt(section, L"TimeOfDayLocked", fallback.time_of_day_locked ? 1 : 0, path) != 0;
    for (const auto& spec : kSliderSpecs) {
        out.*(spec.member) = ClampStrengthPct(
            ReadIniInt(section, spec.ini_key, ClampStrengthPct(fallback.*(spec.member)), path));
    }
    return out;
}

bool SaveConfigFileLocked(const wchar_t* path_override) {
    const std::wstring target =
        (path_override && path_override[0]) ? std::wstring(path_override)
                                            : (g_panel.ini_path.empty() ? ResolveDefaultConfigIniPath()
                                                                        : g_panel.ini_path);
    g_panel.ini_path = target;
    ::WritePrivateProfileStringW(
        L"Panel", L"Language", IsEnglishLocked() ? L"en" : L"zh", target.c_str());

    const VisualConfig current = CaptureVisualConfigFromEngine();
    SaveVisualConfigToSection(L"Active", current, target.c_str());

    for (int i = 0; i < kCustomPresetCount; ++i) {
        wchar_t sec[32] = {};
        swprintf_s(sec, L"Preset%d", i + 1);
        WriteIniInt(sec, L"Used", g_panel.presets[static_cast<size_t>(i)].used ? 1 : 0, target.c_str());
        SaveVisualConfigToSection(sec, g_panel.presets[static_cast<size_t>(i)].cfg, target.c_str());
    }
    return true;
}

std::wstring FormatPresetSummary(const VisualConfig& cfg, bool english) {
    const wchar_t* filter_str = english ? L"Off" : L"原";
    if (cfg.p2_filter_preset == 1) filter_str = L"Hob";
    else if (cfg.p2_filter_preset == 2) filter_str = english ? L"Dark" : L"暗";

    const wchar_t* lut_str = L"";
    if (cfg.p2_lut_preset == 1) lut_str = english ? L"·Warm" : L"·暖";
    else if (cfg.p2_lut_preset == 2) lut_str = english ? L"·Cold" : L"·冷";
    else if (cfg.p2_lut_preset == 3) lut_str = L"·CUBE";

    const wchar_t* hdr_str = L"";
    if (cfg.hdr_tonemap_mode == 1) hdr_str = L"·ACES";
    else if (cfg.hdr_tonemap_mode == 2) hdr_str = L"·AgX";

    const wchar_t* smaa_str = L"";
    if (cfg.smaa_mode == 1) smaa_str = L"·SMAA-C";
    else if (cfg.smaa_mode == 2) smaa_str = L"·SMAA-D";

    int on_count = 0;
    if (cfg.phase3_enhance) ++on_count;
    if (cfg.af_point_enhance) ++on_count;
    if (cfg.p0_rimlit_enhance) ++on_count;
    if (cfg.ao_enabled) ++on_count;
    if (cfg.p0_sscs_enabled) ++on_count;
    if (cfg.p2_shadow_map_enabled) ++on_count;
    if (cfg.p1_soft_particles) ++on_count;
    if (cfg.p2_aerial_vignette) ++on_count;
    if (cfg.p2_ssr_enabled) ++on_count;
    if (cfg.n5_ibl_wrap_enabled) ++on_count;

    wchar_t buf[64] = {};
    swprintf_s(buf, L"%d/10·%ls%ls%ls%ls", on_count, filter_str, lut_str, hdr_str, smaa_str);
    return buf;
}

constexpr size_t kHitZoneCount = 74;

std::array<HitZone, kHitZoneCount> BuildHitZones() {
    std::array<HitZone, kHitZoneCount> zones{};
    size_t idx = 0;

    // Close [X] & explicit [中] / [EN] language buttons (must be tested before TitleDrag!)
    zones[idx++] = {RECT{static_cast<LONG>(kPanelWidth - 38), 6,
                         static_cast<LONG>(kPanelWidth - 10), 34},
                    HitTargetKind::CloseButton, L"×", L"×"};
    zones[idx++] = {RECT{484, 8, 522, 32},
                    HitTargetKind::LangZh, L"中", L"中"};
    zones[idx++] = {RECT{526, 8, 564, 32},
                    HitTargetKind::LangEn, L"EN", L"EN"};
    // Title bar drag region
    zones[idx++] = {RECT{0, 0, static_cast<LONG>(kPanelWidth), kTitleBarHeight},
                    HitTargetKind::TitleDrag, L"", L""};

    // Select All / Clear All / Reset 100% buttons on Section 1 header bar
    zones[idx++] = {RECT{428, 42, 508, 61}, HitTargetKind::ToggleAllOn, L"全部开启", L"Enable All"};
    zones[idx++] = {RECT{514, 42, 594, 61}, HitTargetKind::ToggleAllOff, L"全部关闭", L"Disable All"};

    // 10 Feature checkboxes (left half of each row)
    const struct {
        HitTargetKind kind;
        const wchar_t* label_zh;
        const wchar_t* label_en;
    } kToggles[10] = {
        {HitTargetKind::TogglePhase3,
         L"材质法线细节与边缘光增强",
         L"Surface Normals & Rim Lighting"},
        {HitTargetKind::ToggleAfPoint,
         L"16x 各向异性过滤 + 动态点光增强",
         L"16x Anisotropic Filter + Point Lights"},
        {HitTargetKind::ToggleP0RimLit,
         L"微法线细节 / 金属高光 / 纹理自发光",
         L"Micro-Normals / Specular / Emissive"},
        {HitTargetKind::TogglePhase3IblWrap,
         L"昼夜解析 IBL 金属反光 + 人物柔光 [N5]",
         L"ToD Analytical IBL Sheen & Wrap [N5]"},
        {HitTargetKind::ToggleAo,
         L"屏幕空间环境光遮蔽 (SSAO)",
         L"Screen-Space Ambient Occlusion (SSAO)"},
        {HitTargetKind::ToggleP0Sscs,
         L"全分辨率接触阴影 (SSCS) + 冷色调曲线",
         L"Contact Shadows (SSCS) + Cool Tone"},
        {HitTargetKind::ToggleShadowMap,
         L"2048 硬件深度太阳实时阴影 (Shadow Map)",
         L"2048 Hardware Sun Shadow Map (PCF16)"},
        {HitTargetKind::ToggleP1SoftParticles,
         L"屏幕空间软粒子 (消除火焰/光晕硬切线)",
         L"Soft Particles (Smooth Glow Edges)"},
        {HitTargetKind::ToggleP2AerialVignette,
         L"深度空气透视 + 光学暗角 (纵深聚焦)",
         L"Aerial Perspective + Optical Vignette"},
        {HitTargetKind::ToggleP2Ssr,
         L"局部水面/湿地屏幕空间反射 (SSR)",
         L"Water / Puddle Reflections (SSR)"},
    };
    int y = 66;
    for (const auto& t : kToggles) {
        zones[idx++] = {RECT{kPaddingX, y, kSliderZoneLeft - 4, y + 25},
                        t.kind, t.label_zh, t.label_en};
        y += 28;
    }

    // Visual Style Filter pills
    y = 374;
    const int pill_w = 186;
    const int pill_gap = 10;
    zones[idx++] = {RECT{kPaddingX, y, kPaddingX + pill_w, y + 26},
                    HitTargetKind::FilterOff, L"关闭滤镜", L"Filter Off"};
    zones[idx++] = {RECT{kPaddingX + (pill_w + pill_gap), y,
                         kPaddingX + (pill_w + pill_gap) + pill_w, y + 26},
                    HitTargetKind::FilterHob, L"Hob 明快", L"Hob Vibrant"};
    zones[idx++] = {RECT{kPaddingX + 2 * (pill_w + pill_gap), y,
                         kPaddingX + 2 * (pill_w + pill_gap) + pill_w, y + 26},
                    HitTargetKind::FilterDark, L"暗黑胶片", L"Dark Film"};

    // 3D LUT Color Grading pills (4 pills)
    y = 428;
    const int lut_w = 138;
    const int lut_gap = 8;
    const struct {
        HitTargetKind kind;
        const wchar_t* label_zh;
        const wchar_t* label_en;
    } kLuts[4] = {
        {HitTargetKind::LutOff, L"关闭调色", L"LUT Off"},
        {HitTargetKind::LutTealOrange, L"映画暖青", L"Teal & Orange"},
        {HitTargetKind::LutColdNordic, L"冷峻史诗", L"Cold Nordic"},
        {HitTargetKind::LutCustomCube, L"外部 .cube", L"Custom .cube"},
    };
    for (int i = 0; i < 4; ++i) {
        const int lx = kPaddingX + i * (lut_w + lut_gap);
        zones[idx++] = {RECT{lx, y, lx + lut_w, y + 26},
                        kLuts[i].kind, kLuts[i].label_zh, kLuts[i].label_en};
    }

    // Section 3B: FP16 Linear HDR Tonemap pills (3 pills)
    y = 482;
    zones[idx++] = {RECT{kPaddingX, y, kPaddingX + pill_w, y + 26},
                    HitTargetKind::HdrOff, L"HDR 关闭 (原生LDR)", L"HDR Off (Vanilla LDR)"};
    zones[idx++] = {RECT{kPaddingX + (pill_w + pill_gap), y,
                         kPaddingX + (pill_w + pill_gap) + pill_w, y + 26},
                    HitTargetKind::HdrAces, L"ACES Filmic", L"ACES Filmic"};
    zones[idx++] = {RECT{kPaddingX + 2 * (pill_w + pill_gap), y,
                         kPaddingX + 2 * (pill_w + pill_gap) + pill_w, y + 26},
                    HitTargetKind::HdrAgx, L"AgX Punchy (推荐)", L"AgX Punchy"};

    // Section 4: SMAA 1x Anti-Aliasing pills (3 pills)
    y = 536;
    zones[idx++] = {RECT{kPaddingX, y, kPaddingX + pill_w, y + 26},
                    HitTargetKind::SmaaOff, L"关闭抗锯齿", L"SMAA Off"};
    zones[idx++] = {RECT{kPaddingX + (pill_w + pill_gap), y,
                         kPaddingX + (pill_w + pill_gap) + pill_w, y + 26},
                    HitTargetKind::SmaaColor, L"Color (色彩)", L"Color (Fast)"};
    zones[idx++] = {RECT{kPaddingX + 2 * (pill_w + pill_gap), y,
                         kPaddingX + 2 * (pill_w + pill_gap) + pill_w, y + 26},
                    HitTargetKind::SmaaDepthColor, L"Depth+Color (双检)", L"Depth+Color (Ultra)"};

    // Section 5: Weather & Time of Day
    // Row 1: Rain toggle + Slider 10
    zones[idx++] = {RECT{kPaddingX, 590, kSliderZoneLeft - 4, 590 + 26},
                    HitTargetKind::ToggleWeatherRain,
                    L"雨天湿地倒影 (地面动态水坑与波纹)",
                    L"Rain & Wet Ground (Puddles & Ripples)"};
    zones[idx++] = {RECT{kSliderZoneLeft, 590, kSliderZoneRight, 590 + 26},
                    HitTargetKind::Slider10, L"", L""};

    // Row 2: Time of Day Slider
    zones[idx++] = {RECT{kSliderZoneLeft, 620, kSliderZoneRight, 620 + 26},
                    HitTargetKind::SliderTime, L"", L""};

    // Row 3: 5 Time Pills (Auto, 06h, 12h, 18h, 24h)
    const int time_pill_w = 110;
    const int time_pill_gap = 7;
    const struct {
        HitTargetKind kind;
        const wchar_t* label_zh;
        const wchar_t* label_en;
    } kTimePills[5] = {
        {HitTargetKind::TimeAuto, L"自然昼夜", L"Auto Time"},
        {HitTargetKind::TimeDawn, L"清晨 06:00", L"Dawn 06:00"},
        {HitTargetKind::TimeNoon, L"正午 12:00", L"Noon 12:00"},
        {HitTargetKind::TimeDusk, L"黄昏 18:00", L"Dusk 18:00"},
        {HitTargetKind::TimeMidnight, L"午夜 24:00", L"Midnight 24:00"},
    };
    for (int i = 0; i < 5; ++i) {
        const int px = kPaddingX + i * (time_pill_w + time_pill_gap);
        zones[idx++] = {RECT{px, 650, px + time_pill_w, 650 + 25},
                        kTimePills[i].kind, kTimePills[i].label_zh, kTimePills[i].label_en};
    }

    // Section 6: Built-in presets
    y = 702;
    zones[idx++] = {RECT{kPaddingX, y, kPaddingX + pill_w, y + 26},
                    HitTargetKind::BuiltinFullHd, L"全开高清", L"Full HD+"};
    zones[idx++] = {RECT{kPaddingX + (pill_w + pill_gap), y,
                         kPaddingX + (pill_w + pill_gap) + pill_w, y + 26},
                    HitTargetKind::BuiltinDarkDungeon, L"暗黑地牢", L"Dark Dungeon"};
    zones[idx++] = {RECT{kPaddingX + 2 * (pill_w + pill_gap), y,
                         kPaddingX + 2 * (pill_w + pill_gap) + pill_w, y + 26},
                    HitTargetKind::BuiltinVanilla, L"纯原版", L"Vanilla"};

    // Section 7: 10 Custom preset slots (2 columns x 5 rows, Load + Save)
    const int base_y = 755;
    const int col_w = 284;
    const int col_gap = 10;
    const int save_w = 46;
    const int save_gap = 4;
    const int load_w = col_w - save_w - save_gap;
    for (int i = 0; i < kCustomPresetCount; ++i) {
        const int col = i % 2;
        const int row = i / 2;
        const int cx = kPaddingX + col * (col_w + col_gap);
        const int ry = base_y + row * 27;
        const auto load_kind = static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::CustomLoad0) + i);
        const auto save_kind = static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::CustomSave0) + i);
        zones[idx++] = {RECT{cx, ry, cx + load_w, ry + 25}, load_kind, L"", L""};
        zones[idx++] = {RECT{cx + load_w + save_gap, ry, cx + col_w, ry + 25}, save_kind, L"保存", L"Save"};
    }

    // Reset all effect strengths back to 100%
    zones[idx++] = {RECT{332, 42, 420, 61}, HitTargetKind::ResetStrengths, L"强度归位", L"Reset 100%"};

    // Sliders on the right half of the 10 toggle rows
    y = 66;
    for (int i = 0; i < 10; ++i) {
        const int slider_idx = kSection1SliderIndices[static_cast<size_t>(i)];
        const auto s_kind = static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::Slider0) + slider_idx);
        zones[idx++] = {RECT{kSliderZoneLeft, y, kSliderZoneRight, y + 25}, s_kind, L"", L""};
        y += 28;
    }
    // Sliders 8, 9, 13 on Filter / LUT / HDR headers
    zones[idx++] = {RECT{kSliderZoneLeft, 348, kSliderZoneRight, 370}, HitTargetKind::Slider8, L"", L""};
    zones[idx++] = {RECT{kSliderZoneLeft, 402, kSliderZoneRight, 424}, HitTargetKind::Slider9, L"", L""};
    zones[idx++] = {RECT{kSliderZoneLeft, 456, kSliderZoneRight, 478}, HitTargetKind::Slider13, L"", L""};

    return zones;
}

const std::array<HitZone, kHitZoneCount>& GetHitZones() {
    static const std::array<HitZone, kHitZoneCount> kZones = BuildHitZones();
    return kZones;
}

size_t ZoneIndexByKind(HitTargetKind kind) {
    const auto& zones = GetHitZones();
    for (size_t i = 0; i < zones.size(); ++i) {
        if (zones[i].kind == kind) {
            return i;
        }
    }
    return 0;
}

int HitTestPanelLocal(int local_x, int local_y) {
    if (local_x < 0 || local_y < 0 ||
        local_x >= static_cast<int>(kPanelWidth) ||
        local_y >= static_cast<int>(kPanelHeight)) {
        return -1;
    }
    const auto& zones = GetHitZones();
    for (size_t i = 0; i < zones.size(); ++i) {
        const RECT& r = zones[i].rect;
        if (local_x >= r.left && local_x < r.right && local_y >= r.top && local_y < r.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void ClampPanelPosLocked() {
    const float max_x = (std::max)(kPanelMargin, g_panel.last_vp_w - static_cast<float>(kPanelWidth) - kPanelMargin);
    const float max_y = (std::max)(kPanelMargin, g_panel.last_vp_h - static_cast<float>(kPanelHeight) - kPanelMargin);
    if (!g_panel.has_custom_pos) {
        g_panel.pos_x = (std::max)(kPanelMargin, (g_panel.last_vp_w - static_cast<float>(kPanelWidth)) * 0.5f);
        g_panel.pos_y = (std::max)(kPanelMargin, (g_panel.last_vp_h - static_cast<float>(kPanelHeight)) * 0.5f);
    }
    g_panel.pos_x = (std::clamp)(g_panel.pos_x, kPanelMargin, max_x);
    g_panel.pos_y = (std::clamp)(g_panel.pos_y, kPanelMargin, max_y);
}

bool ClientPointToRenderTarget(HWND hwnd, LPARAM lParam, float& out_x, float& out_y) {
    RECT rc{};
    if (!hwnd || !::GetClientRect(hwnd, &rc)) {
        return false;
    }
    const float client_w = static_cast<float>(rc.right - rc.left);
    const float client_h = static_cast<float>(rc.bottom - rc.top);
    if (client_w <= 1.0f || client_h <= 1.0f) {
        return false;
    }
    const int cx = GET_X_LPARAM(lParam);
    const int cy = GET_Y_LPARAM(lParam);
    out_x = static_cast<float>(cx) * (g_panel.last_vp_w / client_w);
    out_y = static_cast<float>(cy) * (g_panel.last_vp_h / client_h);
    return true;
}

void DestroyGdiSurfaceLocked() {
    if (g_panel.mem_dc && g_panel.prev_bitmap) {
        ::SelectObject(g_panel.mem_dc, g_panel.prev_bitmap);
        g_panel.prev_bitmap = nullptr;
    }
    if (g_panel.dib_bitmap) {
        ::DeleteObject(g_panel.dib_bitmap);
        g_panel.dib_bitmap = nullptr;
        g_panel.dib_pixels = nullptr;
    }
    if (g_panel.font_title) {
        ::DeleteObject(g_panel.font_title);
        g_panel.font_title = nullptr;
    }
    if (g_panel.font_section) {
        ::DeleteObject(g_panel.font_section);
        g_panel.font_section = nullptr;
    }
    if (g_panel.font_body) {
        ::DeleteObject(g_panel.font_body);
        g_panel.font_body = nullptr;
    }
    if (g_panel.font_small) {
        ::DeleteObject(g_panel.font_small);
        g_panel.font_small = nullptr;
    }
    if (g_panel.mem_dc) {
        ::DeleteDC(g_panel.mem_dc);
        g_panel.mem_dc = nullptr;
    }
}

HFONT CreatePanelFont(int height_px, int weight) {
    return ::CreateFontW(
        -height_px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

bool EnsureGdiSurfaceLocked() {
    if (g_panel.mem_dc && g_panel.dib_pixels) {
        return true;
    }
    HDC screen_dc = ::GetDC(nullptr);
    g_panel.mem_dc = ::CreateCompatibleDC(screen_dc);
    ::ReleaseDC(nullptr, screen_dc);
    if (!g_panel.mem_dc) {
        return false;
    }

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(kPanelWidth);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(kPanelHeight);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    g_panel.dib_bitmap = ::CreateDIBSection(g_panel.mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!g_panel.dib_bitmap || !bits) {
        DestroyGdiSurfaceLocked();
        return false;
    }
    g_panel.dib_pixels = static_cast<uint32_t*>(bits);
    g_panel.prev_bitmap = ::SelectObject(g_panel.mem_dc, g_panel.dib_bitmap);

    g_panel.font_title = CreatePanelFont(16, FW_BOLD);
    g_panel.font_section = CreatePanelFont(13, FW_BOLD);
    g_panel.font_body = CreatePanelFont(13, FW_NORMAL);
    g_panel.font_small = CreatePanelFont(12, FW_NORMAL);
    ::SetBkMode(g_panel.mem_dc, TRANSPARENT);
    return true;
}

void FillSolidRect(HDC dc, const RECT& r, COLORREF color) {
    HBRUSH br = ::CreateSolidBrush(color);
    ::FillRect(dc, &r, br);
    ::DeleteObject(br);
}

void DrawBorderRect(HDC dc, const RECT& r, COLORREF fill, COLORREF border, int radius = 5) {
    HBRUSH br = ::CreateSolidBrush(fill);
    HPEN pen = ::CreatePen(PS_SOLID, 1, border);
    HGDIOBJ old_br = ::SelectObject(dc, br);
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    ::RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    ::SelectObject(dc, old_pen);
    ::SelectObject(dc, old_br);
    ::DeleteObject(pen);
    ::DeleteObject(br);
}

void DrawTextBox(HDC dc, HFONT font, COLORREF color, const RECT& r, const wchar_t* text, UINT fmt) {
    HGDIOBJ old_f = font ? ::SelectObject(dc, font) : nullptr;
    ::SetTextColor(dc, color);
    RECT copy = r;
    ::DrawTextW(dc, text, -1, &copy, fmt | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (old_f) {
        ::SelectObject(dc, old_f);
    }
}

void DrawSectionHeader(HDC dc, int y, const wchar_t* title) {
    RECT tr{kPaddingX, y, static_cast<LONG>(kPanelWidth - kPaddingX), y + 18};
    DrawTextBox(dc, g_panel.font_section, RGB(126, 200, 255), tr, title, DT_LEFT | DT_VCENTER);
    HPEN pen = ::CreatePen(PS_SOLID, 1, RGB(44, 56, 78));
    HGDIOBJ old_pen = ::SelectObject(dc, pen);
    ::MoveToEx(dc, kPaddingX, y + 19, nullptr);
    ::LineTo(dc, static_cast<int>(kPanelWidth) - kPaddingX, y + 19);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(pen);
}

void DrawIntensitySlider(
    HDC dc,
    const RECT& zone_rect,
    int pct,
    bool effect_active,
    bool hovered,
    bool dragging) {
    const int clamped = ClampStrengthPct(pct);
    const int cy = (zone_rect.top + zone_rect.bottom) / 2;
    const float t = static_cast<float>(clamped) / static_cast<float>(kSliderMaxPct);
    const int knob_x = kSliderTrackLeft +
                       static_cast<int>(std::lround(t * static_cast<float>(kSliderTrackRight - kSliderTrackLeft)));

    // Track groove
    const RECT track_r{kSliderTrackLeft, cy - 3, kSliderTrackRight, cy + 3};
    DrawBorderRect(dc, track_r, RGB(16, 20, 30),
                   (hovered || dragging) ? RGB(92, 146, 212) : RGB(46, 56, 76), 4);

    // 100% baseline notch at midpoint
    const int mid_x = (kSliderTrackLeft + kSliderTrackRight) / 2;
    HPEN notch_pen = ::CreatePen(PS_SOLID, 1, RGB(84, 98, 124));
    HGDIOBJ old_pen = ::SelectObject(dc, notch_pen);
    ::MoveToEx(dc, mid_x, cy - 6, nullptr);
    ::LineTo(dc, mid_x, cy + 7);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(notch_pen);

    // Filled active bar
    if (knob_x > kSliderTrackLeft + 1) {
        const COLORREF fill_col = !effect_active ? RGB(66, 76, 94)
                                                 : (clamped > 100 ? RGB(236, 168, 72) : RGB(64, 186, 148));
        const RECT fill_r{kSliderTrackLeft + 1, cy - 2, knob_x, cy + 2};
        FillSolidRect(dc, fill_r, fill_col);
    }

    // Thumb knob
    const RECT knob_r{knob_x - 4, cy - 7, knob_x + 4, cy + 7};
    const COLORREF knob_fill = dragging ? RGB(246, 250, 255)
                                        : (hovered ? RGB(204, 228, 255)
                                                   : (effect_active ? RGB(154, 218, 245) : RGB(118, 128, 148)));
    const COLORREF knob_border = (dragging || hovered) ? RGB(114, 194, 255) : RGB(42, 64, 92);
    DrawBorderRect(dc, knob_r, knob_fill, knob_border, 4);

    // Numeric percentage readout
    wchar_t pct_buf[16] = {};
    swprintf_s(pct_buf, L"%d%%", clamped);
    const COLORREF txt_col = !effect_active
                                 ? RGB(112, 120, 138)
                                 : (clamped == 100 ? RGB(172, 224, 200)
                                                   : (clamped > 100 ? RGB(248, 198, 110) : RGB(152, 204, 248)));
    const RECT read_r{kSliderReadoutLeft, zone_rect.top, kSliderReadoutRight, zone_rect.bottom};
    DrawTextBox(dc, g_panel.font_small, txt_col, read_r, pct_buf, DT_RIGHT | DT_VCENTER);
}

void DrawTimeSlider(
    HDC dc,
    const RECT& zone_rect,
    int hours,
    bool locked,
    bool hovered,
    bool dragging) {
    const int effective_hour = (hours >= 0) ? hours : GetTimeOfDayHours();
    const int clamped_hour = (std::clamp)(effective_hour, 0, 24);
    const int cy = (zone_rect.top + zone_rect.bottom) / 2;
    const float t = static_cast<float>(clamped_hour) / 24.0f;
    const int knob_x = kSliderTrackLeft +
                       static_cast<int>(std::lround(t * static_cast<float>(kSliderTrackRight - kSliderTrackLeft)));

    // Track groove
    const RECT track_r{kSliderTrackLeft, cy - 3, kSliderTrackRight, cy + 3};
    DrawBorderRect(dc, track_r, RGB(16, 20, 30),
                   (hovered || dragging) ? RGB(92, 146, 212) : RGB(46, 56, 76), 4);

    // 06:00, 12:00, 18:00 ticks
    HPEN tick_pen = ::CreatePen(PS_SOLID, 1, RGB(84, 98, 124));
    HGDIOBJ old_pen = ::SelectObject(dc, tick_pen);
    const int span = kSliderTrackRight - kSliderTrackLeft;
    const int dawn_x = kSliderTrackLeft + span / 4;
    const int noon_x = kSliderTrackLeft + span / 2;
    const int dusk_x = kSliderTrackLeft + (span * 3) / 4;
    ::MoveToEx(dc, dawn_x, cy - 4, nullptr); ::LineTo(dc, dawn_x, cy + 5);
    ::MoveToEx(dc, noon_x, cy - 6, nullptr); ::LineTo(dc, noon_x, cy + 7);
    ::MoveToEx(dc, dusk_x, cy - 4, nullptr); ::LineTo(dc, dusk_x, cy + 5);
    ::SelectObject(dc, old_pen);
    ::DeleteObject(tick_pen);

    // Filled active bar
    if (knob_x > kSliderTrackLeft + 1) {
        const COLORREF fill_col = locked ? RGB(236, 168, 72) : RGB(86, 112, 144);
        const RECT fill_r{kSliderTrackLeft + 1, cy - 2, knob_x, cy + 2};
        FillSolidRect(dc, fill_r, fill_col);
    }

    // Thumb knob
    const RECT knob_r{knob_x - 4, cy - 7, knob_x + 4, cy + 7};
    const COLORREF knob_fill = dragging ? RGB(255, 250, 230)
                                        : (hovered ? RGB(255, 228, 170)
                                                   : (locked ? RGB(248, 198, 110) : RGB(148, 162, 182)));
    const COLORREF knob_border = (dragging || hovered) ? RGB(255, 214, 114) : RGB(64, 82, 110);
    DrawBorderRect(dc, knob_r, knob_fill, knob_border, 4);

    // Numeric hour readout
    wchar_t time_buf[16] = {};
    if (locked && hours >= 0) {
        swprintf_s(time_buf, L"%02d:00", clamped_hour);
    } else {
        swprintf_s(time_buf, PickLang(L"自然(%02d)", L"Auto(%02d)"), clamped_hour);
    }
    const COLORREF txt_col = locked ? RGB(248, 204, 118) : RGB(164, 184, 212);
    const RECT read_r{kSliderReadoutLeft, zone_rect.top, kSliderReadoutRight, zone_rect.bottom};
    DrawTextBox(dc, g_panel.font_small, txt_col, read_r, time_buf, DT_RIGHT | DT_VCENTER);
}

void PaintPanelToDibLocked() {
    if (!EnsureGdiSurfaceLocked()) {
        return;
    }
    HDC dc = g_panel.mem_dc;
    const VisualConfig cfg = CaptureVisualConfigFromEngine();
    const auto& zones = GetHitZones();

    // Base dark slate background
    FillSolidRect(dc, RECT{0, 0, static_cast<LONG>(kPanelWidth), static_cast<LONG>(kPanelHeight)},
                  RGB(18, 22, 31));
    // Title bar gradient-like solid strip
    FillSolidRect(dc, RECT{0, 0, static_cast<LONG>(kPanelWidth), kTitleBarHeight}, RGB(25, 32, 46));
    HPEN sep = ::CreatePen(PS_SOLID, 1, RGB(58, 86, 128));
    HGDIOBJ old_sep = ::SelectObject(dc, sep);
    ::MoveToEx(dc, 0, kTitleBarHeight, nullptr);
    ::LineTo(dc, static_cast<int>(kPanelWidth), kTitleBarHeight);
    ::SelectObject(dc, old_sep);
    ::DeleteObject(sep);

    const bool en = IsEnglishLocked();
    DrawTextBox(dc, g_panel.font_title, RGB(238, 245, 255),
                RECT{kPaddingX, 0, 476, kTitleBarHeight},
                PickLang(L"TL2 DX11 画质增强面板  [F8]", L"TL2 DX11 Graphics Panel  [F8]"),
                DT_LEFT | DT_VCENTER);

    // Explicit [中] and [EN] language buttons
    {
        const auto draw_lang_btn = [&](HitTargetKind kind, bool active) {
            const size_t idx = ZoneIndexByKind(kind);
            const bool hovered = (g_panel.hover_index == static_cast<int>(idx));
            const COLORREF fill = active ? RGB(42, 112, 186)
                                         : (hovered ? RGB(38, 48, 68) : RGB(24, 30, 44));
            const COLORREF border = active ? RGB(114, 194, 255)
                                           : (hovered ? RGB(92, 136, 198) : RGB(56, 70, 96));
            DrawBorderRect(dc, zones[idx].rect, fill, border, 5);
            DrawTextBox(dc, active ? g_panel.font_section : g_panel.font_small,
                        active ? RGB(248, 252, 255) : (hovered ? RGB(214, 226, 244) : RGB(156, 170, 194)),
                        zones[idx].rect, ZoneLabel(zones[idx]), DT_CENTER | DT_VCENTER);
        };
        draw_lang_btn(HitTargetKind::LangZh, !en);
        draw_lang_btn(HitTargetKind::LangEn, en);
    }

    // Close [X] button
    {
        const size_t close_idx = ZoneIndexByKind(HitTargetKind::CloseButton);
        const bool hovered = (g_panel.hover_index == static_cast<int>(close_idx));
        DrawBorderRect(dc, zones[close_idx].rect,
                       hovered ? RGB(176, 52, 62) : RGB(44, 52, 68),
                       hovered ? RGB(235, 110, 120) : RGB(78, 92, 118), 5);
        DrawTextBox(dc, g_panel.font_title, RGB(240, 244, 250), zones[close_idx].rect, L"×",
                    DT_CENTER | DT_VCENTER);
    }

    // Section 1: 10 Feature toggles + batch [Reset] / [Enable All] / [Disable All]
    DrawSectionHeader(dc, 44, PickLang(L"实时渲染与画质开关", L"Real-Time Render Toggles"));
    const bool toggle_states[10] = {
        cfg.phase3_enhance,
        cfg.af_point_enhance,
        cfg.p0_rimlit_enhance,
        cfg.n5_ibl_wrap_enabled,
        cfg.ao_enabled,
        cfg.p0_sscs_enabled,
        cfg.p2_shadow_map_enabled,
        cfg.p1_soft_particles,
        cfg.p2_aerial_vignette,
        cfg.p2_ssr_enabled,
    };
    const bool all_on = std::all_of(std::begin(toggle_states), std::end(toggle_states), [](bool v) { return v; });
    const bool all_off = std::all_of(std::begin(toggle_states), std::end(toggle_states), [](bool v) { return !v; });
    bool all_default_str = true;
    for (const auto& spec : kSliderSpecs) {
        if (ClampStrengthPct(cfg.*(spec.member)) != 100) {
            all_default_str = false;
            break;
        }
    }
    {
        const size_t rst_idx = ZoneIndexByKind(HitTargetKind::ResetStrengths);
        const bool h_rst = (g_panel.hover_index == static_cast<int>(rst_idx));
        DrawBorderRect(dc, zones[rst_idx].rect,
                       all_default_str ? RGB(26, 36, 48) : (h_rst ? RGB(58, 86, 128) : RGB(36, 56, 86)),
                       all_default_str ? RGB(58, 74, 98) : (h_rst ? RGB(126, 192, 255) : RGB(94, 152, 220)), 5);
        DrawTextBox(dc, g_panel.font_small,
                    all_default_str ? RGB(156, 176, 204) : RGB(224, 240, 255),
                    zones[rst_idx].rect, ZoneLabel(zones[rst_idx]), DT_CENTER | DT_VCENTER);

        const size_t on_idx = ZoneIndexByKind(HitTargetKind::ToggleAllOn);
        const bool h_on = (g_panel.hover_index == static_cast<int>(on_idx));
        DrawBorderRect(dc, zones[on_idx].rect,
                       all_on ? RGB(36, 120, 88) : (h_on ? RGB(42, 68, 58) : RGB(26, 36, 48)),
                       all_on ? RGB(104, 228, 174) : (h_on ? RGB(92, 186, 142) : RGB(56, 78, 96)), 5);
        DrawTextBox(dc, g_panel.font_small,
                    all_on ? RGB(240, 255, 248) : RGB(178, 224, 198),
                    zones[on_idx].rect, ZoneLabel(zones[on_idx]), DT_CENTER | DT_VCENTER);

        const size_t off_idx = ZoneIndexByKind(HitTargetKind::ToggleAllOff);
        const bool h_off = (g_panel.hover_index == static_cast<int>(off_idx));
        DrawBorderRect(dc, zones[off_idx].rect,
                       all_off ? RGB(126, 52, 58) : (h_off ? RGB(68, 42, 52) : RGB(26, 36, 48)),
                       all_off ? RGB(232, 118, 128) : (h_off ? RGB(188, 98, 108) : RGB(68, 62, 82)), 5);
        DrawTextBox(dc, g_panel.font_small,
                    all_off ? RGB(255, 240, 242) : RGB(224, 182, 188),
                    zones[off_idx].rect, ZoneLabel(zones[off_idx]), DT_CENTER | DT_VCENTER);
    }
    for (int i = 0; i < 10; ++i) {
        const size_t z_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::TogglePhase3) + i));
        const int slider_idx = kSection1SliderIndices[static_cast<size_t>(i)];
        const size_t s_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::Slider0) + slider_idx));
        const RECT& tog_r = zones[z_idx].rect;
        const RECT& sld_r = zones[s_idx].rect;
        const RECT row_r{tog_r.left, tog_r.top, sld_r.right, tog_r.bottom};
        const bool tog_hover = (g_panel.hover_index == static_cast<int>(z_idx));
        const bool sld_hover = (g_panel.hover_index == static_cast<int>(s_idx));
        const bool sld_drag = (g_panel.active_slider_index == slider_idx);
        const bool on = toggle_states[i];

        const COLORREF row_fill = (tog_hover || sld_hover || sld_drag) ? RGB(32, 41, 58) : RGB(24, 30, 42);
        const COLORREF row_border = (tog_hover || sld_hover || sld_drag) ? RGB(92, 152, 226)
                                                                         : (on ? RGB(48, 86, 118) : RGB(42, 49, 64));
        DrawBorderRect(dc, row_r, row_fill, row_border, 6);

        // Checkbox square
        const RECT box{tog_r.left + 8, tog_r.top + 4, tog_r.left + 25, tog_r.top + 21};
        DrawBorderRect(dc, box,
                       on ? RGB(42, 168, 116) : RGB(20, 24, 34),
                       on ? RGB(105, 232, 178) : RGB(96, 110, 136), 4);
        if (on) {
            DrawTextBox(dc, g_panel.font_section, RGB(245, 255, 250), box, L"✓",
                        DT_CENTER | DT_VCENTER);
        }

        // Label + status tag
        RECT text_r{tog_r.left + 32, tog_r.top, tog_r.right - 38, tog_r.bottom};
        DrawTextBox(dc, g_panel.font_body,
                    on ? RGB(232, 240, 250) : RGB(148, 158, 176),
                    text_r, ZoneLabel(zones[z_idx]), DT_LEFT | DT_VCENTER);

        RECT tag_r{tog_r.right - 38, tog_r.top, tog_r.right - 4, tog_r.bottom};
        DrawTextBox(dc, g_panel.font_small,
                    on ? RGB(102, 232, 170) : RGB(128, 134, 150),
                    tag_r, on ? PickLang(L"开启", L"ON") : PickLang(L"关闭", L"OFF"),
                    DT_RIGHT | DT_VCENTER);

        // Interactive strength slider
        DrawIntensitySlider(dc, sld_r, cfg.*(kSliderSpecs[static_cast<size_t>(slider_idx)].member),
                            on, sld_hover, sld_drag);
    }

    // Section 2: Style Filter pills + strength slider on header bar
    DrawSectionHeader(dc, 352, PickLang(L"画面风格滤镜", L"Visual Style Filter"));
    DrawTextBox(dc, g_panel.font_small, RGB(154, 176, 206),
                RECT{344, 348, 406, 370}, PickLang(L"强度", L"Strength"), DT_RIGHT | DT_VCENTER);
    {
        const size_t s8_idx = ZoneIndexByKind(HitTargetKind::Slider8);
        DrawIntensitySlider(dc, zones[s8_idx].rect, cfg.p2_filter_strength,
                            cfg.p2_filter_preset > 0,
                            g_panel.hover_index == static_cast<int>(s8_idx),
                            g_panel.active_slider_index == 8);
    }
    for (int i = 0; i < 3; ++i) {
        const size_t z_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::FilterOff) + i));
        const RECT& r = zones[z_idx].rect;
        const bool active = (cfg.p2_filter_preset == i);
        const bool hovered = (g_panel.hover_index == static_cast<int>(z_idx));
        const COLORREF fill = active ? RGB(42, 112, 186)
                                     : (hovered ? RGB(38, 48, 68) : RGB(26, 32, 46));
        const COLORREF border = active ? RGB(114, 194, 255)
                                       : (hovered ? RGB(92, 136, 198) : RGB(52, 62, 82));
        DrawBorderRect(dc, r, fill, border, 6);
        DrawTextBox(dc, active ? g_panel.font_section : g_panel.font_body,
                    active ? RGB(248, 252, 255) : RGB(196, 206, 224),
                    r, ZoneLabel(zones[z_idx]), DT_CENTER | DT_VCENTER);
    }

    // Section 3: 3D LUT Color Grading pills + hint + strength slider on header bar
    DrawSectionHeader(dc, 406, PickLang(L"3D LUT 电影调色", L"3D LUT Color Grading"));
    DrawTextBox(dc, g_panel.font_small, RGB(218, 182, 116),
                RECT{174, 402, 344, 424},
                PickLang(L"外部: TL2_CustomLUT.cube", L"File: TL2_CustomLUT.cube"),
                DT_LEFT | DT_VCENTER);
    DrawTextBox(dc, g_panel.font_small, RGB(154, 176, 206),
                RECT{344, 402, 406, 424}, PickLang(L"强度", L"Strength"), DT_RIGHT | DT_VCENTER);
    {
        const size_t s9_idx = ZoneIndexByKind(HitTargetKind::Slider9);
        DrawIntensitySlider(dc, zones[s9_idx].rect, cfg.p2_lut_strength,
                            cfg.p2_lut_preset > 0,
                            g_panel.hover_index == static_cast<int>(s9_idx),
                            g_panel.active_slider_index == 9);
    }
    for (int i = 0; i < 4; ++i) {
        const size_t z_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::LutOff) + i));
        const RECT& r = zones[z_idx].rect;
        const bool active = (cfg.p2_lut_preset == i);
        const bool hovered = (g_panel.hover_index == static_cast<int>(z_idx));
        const COLORREF fill = active ? RGB(144, 92, 38)
                                     : (hovered ? RGB(38, 48, 68) : RGB(26, 32, 46));
        const COLORREF border = active ? RGB(246, 184, 98)
                                       : (hovered ? RGB(172, 140, 92) : RGB(52, 62, 82));
        DrawBorderRect(dc, r, fill, border, 6);
        DrawTextBox(dc, active ? g_panel.font_section : g_panel.font_body,
                    active ? RGB(255, 248, 236) : RGB(196, 206, 224),
                    r, ZoneLabel(zones[z_idx]), DT_CENTER | DT_VCENTER);
    }

    // Section 3B: FP16 Linear HDR Tonemap (3 pills) + Exposure Slider 13
    DrawSectionHeader(dc, 460, PickLang(L"FP16 线性 HDR 色调映射", L"FP16 Linear HDR Tonemap"));
    DrawTextBox(dc, g_panel.font_small, RGB(154, 176, 206),
                RECT{344, 456, 406, 478}, PickLang(L"曝光", L"Exposure"), DT_RIGHT | DT_VCENTER);
    {
        const size_t s13_idx = ZoneIndexByKind(HitTargetKind::Slider13);
        DrawIntensitySlider(dc, zones[s13_idx].rect, cfg.hdr_exposure_pct,
                            cfg.hdr_tonemap_mode > 0,
                            g_panel.hover_index == static_cast<int>(s13_idx),
                            g_panel.active_slider_index == 13);
    }
    for (int i = 0; i < 3; ++i) {
        const size_t z_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::HdrOff) + i));
        const RECT& r = zones[z_idx].rect;
        const bool active = (cfg.hdr_tonemap_mode == i);
        const bool hovered = (g_panel.hover_index == static_cast<int>(z_idx));
        const COLORREF fill = active ? RGB(132, 76, 168)
                                     : (hovered ? RGB(38, 48, 68) : RGB(26, 32, 46));
        const COLORREF border = active ? RGB(204, 142, 248)
                                       : (hovered ? RGB(144, 112, 188) : RGB(52, 62, 82));
        DrawBorderRect(dc, r, fill, border, 6);
        DrawTextBox(dc, active ? g_panel.font_section : g_panel.font_body,
                    active ? RGB(250, 244, 255) : RGB(196, 206, 224),
                    r, ZoneLabel(zones[z_idx]), DT_CENTER | DT_VCENTER);
    }

    // Section 4: SMAA 1x Anti-Aliasing (3 pills)
    DrawSectionHeader(dc, 514, PickLang(L"SMAA 1x 亚像素形态学抗锯齿", L"SMAA 1x Anti-Aliasing"));
    DrawTextBox(dc, g_panel.font_small, RGB(154, 176, 206),
                RECT{344, 510, 594, 532},
                PickLang(L"0.2ms 极速 · 3D轮廓平滑 · 纯净UI", L"0.2ms · Clean UI · Crisp Silhouettes"),
                DT_RIGHT | DT_VCENTER);
    for (int i = 0; i < 3; ++i) {
        const size_t z_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::SmaaOff) + i));
        const RECT& r = zones[z_idx].rect;
        const bool active = (cfg.smaa_mode == i);
        const bool hovered = (g_panel.hover_index == static_cast<int>(z_idx));
        const COLORREF fill = active ? RGB(38, 112, 142)
                                     : (hovered ? RGB(38, 48, 68) : RGB(26, 32, 46));
        const COLORREF border = active ? RGB(98, 214, 240)
                                       : (hovered ? RGB(92, 136, 198) : RGB(52, 62, 82));
        DrawBorderRect(dc, r, fill, border, 6);
        DrawTextBox(dc, active ? g_panel.font_section : g_panel.font_body,
                    active ? RGB(244, 252, 255) : RGB(196, 206, 224),
                    r, ZoneLabel(zones[z_idx]), DT_CENTER | DT_VCENTER);
    }

    // Section 5: Weather & Time of Day
    DrawSectionHeader(dc, 568, PickLang(L"天气与游戏环境时间", L"Weather & In-Game Time of Day"));

    // Row 1: Rain toggle + Slider 10
    {
        const size_t z_idx = ZoneIndexByKind(HitTargetKind::ToggleWeatherRain);
        const size_t s_idx = ZoneIndexByKind(HitTargetKind::Slider10);
        const RECT& tog_r = zones[z_idx].rect;
        const RECT& sld_r = zones[s_idx].rect;
        const RECT row_r{tog_r.left, tog_r.top, sld_r.right, tog_r.bottom};
        const bool tog_hover = (g_panel.hover_index == static_cast<int>(z_idx));
        const bool sld_hover = (g_panel.hover_index == static_cast<int>(s_idx));
        const bool sld_drag = (g_panel.active_slider_index == 10);
        const bool rain_on = cfg.weather_rain_enabled;

        const COLORREF row_fill = (tog_hover || sld_hover || sld_drag) ? RGB(32, 41, 58) : RGB(24, 30, 42);
        const COLORREF row_border = (tog_hover || sld_hover || sld_drag) ? RGB(92, 152, 226)
                                                                         : (rain_on ? RGB(48, 86, 118) : RGB(42, 49, 64));
        DrawBorderRect(dc, row_r, row_fill, row_border, 6);

        // Checkbox square
        const RECT box{tog_r.left + 8, tog_r.top + 4, tog_r.left + 25, tog_r.top + 21};
        DrawBorderRect(dc, box,
                       rain_on ? RGB(42, 168, 116) : RGB(20, 24, 34),
                       rain_on ? RGB(105, 232, 178) : RGB(96, 110, 136), 4);
        if (rain_on) {
            DrawTextBox(dc, g_panel.font_section, RGB(245, 255, 250), box, L"✓", DT_CENTER | DT_VCENTER);
        }

        // Label + status tag
        RECT text_r{tog_r.left + 32, tog_r.top, tog_r.right - 38, tog_r.bottom};
        DrawTextBox(dc, g_panel.font_body,
                    rain_on ? RGB(232, 240, 250) : RGB(148, 158, 176),
                    text_r, ZoneLabel(zones[z_idx]), DT_LEFT | DT_VCENTER);

        RECT tag_r{tog_r.right - 38, tog_r.top, tog_r.right - 4, tog_r.bottom};
        DrawTextBox(dc, g_panel.font_small,
                    rain_on ? RGB(102, 232, 170) : RGB(128, 134, 150),
                    tag_r, rain_on ? PickLang(L"开启", L"ON") : PickLang(L"关闭", L"OFF"),
                    DT_RIGHT | DT_VCENTER);

        // Interactive rain strength slider
        DrawIntensitySlider(dc, sld_r, cfg.weather_rain_strength, rain_on, sld_hover, sld_drag);
    }

    // Row 2: Time of Day Label + Time Slider
    {
        const size_t time_s_idx = ZoneIndexByKind(HitTargetKind::SliderTime);
        const RECT label_r{kPaddingX, 620, kSliderZoneLeft - 4, 620 + 26};
        const bool sld_hover = (g_panel.hover_index == static_cast<int>(time_s_idx));
        const bool sld_drag = (g_panel.active_slider_index == kSliderTimeIndex);
        const bool locked = cfg.time_of_day_locked && (cfg.time_of_day_hours >= 0);

        const COLORREF row_fill = (sld_hover || sld_drag) ? RGB(32, 41, 58) : RGB(24, 30, 42);
        const COLORREF row_border = (sld_hover || sld_drag) ? RGB(92, 152, 226)
                                                            : (locked ? RGB(144, 108, 48) : RGB(42, 49, 64));
        const RECT row_r{kPaddingX, 620, kSliderZoneRight, 620 + 26};
        DrawBorderRect(dc, row_r, row_fill, row_border, 6);

        RECT text_r{label_r.left + 12, label_r.top, label_r.right - 10, label_r.bottom};
        DrawTextBox(dc, g_panel.font_body,
                    locked ? RGB(248, 220, 160) : RGB(196, 208, 226),
                    text_r,
                    PickLang(L"昼夜环境时间 (拖动滑块锁定 / 右键恢复自然)",
                             L"Time of Day (Drag to lock / Right-Click Auto)"),
                    DT_LEFT | DT_VCENTER);

        DrawTimeSlider(dc, zones[time_s_idx].rect, cfg.time_of_day_hours, locked, sld_hover, sld_drag);
    }

    // Row 3: 5 Time Pills
    {
        const int effective_hour = (cfg.time_of_day_hours >= 0) ? cfg.time_of_day_hours : GetTimeOfDayHours();
        const struct {
            HitTargetKind kind;
        } kTimePills[5] = {
            {HitTargetKind::TimeAuto},
            {HitTargetKind::TimeDawn},
            {HitTargetKind::TimeNoon},
            {HitTargetKind::TimeDusk},
            {HitTargetKind::TimeMidnight},
        };
        for (int i = 0; i < 5; ++i) {
            const size_t z_idx = ZoneIndexByKind(kTimePills[i].kind);
            const RECT& r = zones[z_idx].rect;
            bool active = false;
            if (i == 0) {
                active = !cfg.time_of_day_locked || (cfg.time_of_day_hours < 0);
            } else if (i == 1) {
                active = cfg.time_of_day_locked && (std::abs(effective_hour - 6) <= 1);
            } else if (i == 2) {
                active = cfg.time_of_day_locked && (std::abs(effective_hour - 12) <= 1);
            } else if (i == 3) {
                active = cfg.time_of_day_locked && (std::abs(effective_hour - 18) <= 1);
            } else if (i == 4) {
                active = cfg.time_of_day_locked && (effective_hour >= 23 || effective_hour <= 1);
            }
            const bool hovered = (g_panel.hover_index == static_cast<int>(z_idx));
            const COLORREF fill = active ? RGB(144, 98, 38)
                                         : (hovered ? RGB(38, 48, 68) : RGB(26, 32, 46));
            const COLORREF border = active ? RGB(246, 184, 98)
                                           : (hovered ? RGB(172, 140, 92) : RGB(52, 62, 82));
            DrawBorderRect(dc, r, fill, border, 5);
            DrawTextBox(dc, active ? g_panel.font_section : g_panel.font_small,
                        active ? RGB(255, 248, 236) : RGB(196, 206, 224),
                        r, ZoneLabel(zones[z_idx]), DT_CENTER | DT_VCENTER);
        }
    }

    // Section 6: Built-in presets
    DrawSectionHeader(dc, 681, PickLang(L"内置预设 (一键切换)", L"Built-in Presets (One-Click)"));
    for (int i = 0; i < 3; ++i) {
        const size_t z_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::BuiltinFullHd) + i));
        const RECT& r = zones[z_idx].rect;
        const bool matches = (cfg == MakeBuiltinPreset(i));
        const bool hovered = (g_panel.hover_index == static_cast<int>(z_idx));
        const COLORREF fill = matches ? RGB(36, 116, 98)
                                      : (hovered ? RGB(38, 48, 68) : RGB(26, 32, 46));
        const COLORREF border = matches ? RGB(98, 224, 184)
                                        : (hovered ? RGB(92, 136, 198) : RGB(52, 62, 82));
        DrawBorderRect(dc, r, fill, border, 6);
        DrawTextBox(dc, matches ? g_panel.font_section : g_panel.font_body,
                    matches ? RGB(244, 255, 250) : RGB(198, 208, 224),
                    r, ZoneLabel(zones[z_idx]), DT_CENTER | DT_VCENTER);
    }

    // Section 7: 10 Custom Presets (2 columns x 5 rows)
    DrawSectionHeader(dc, 734,
                      PickLang(L"自定义预设 (10个槽位 · 点击名称切换 / 点 [保存] 覆盖)",
                               L"Custom Presets (10 Slots · Click Slot to Load / [Save])"));
    for (int i = 0; i < kCustomPresetCount; ++i) {
        const size_t load_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::CustomLoad0) + i));
        const size_t save_idx = ZoneIndexByKind(
            static_cast<HitTargetKind>(static_cast<int>(HitTargetKind::CustomSave0) + i));
        const PresetSlot& slot = g_panel.presets[static_cast<size_t>(i)];

        const bool load_hover = (g_panel.hover_index == static_cast<int>(load_idx));
        const bool load_active = slot.used && (cfg == slot.cfg) && (g_panel.active_custom_slot == i);
        const COLORREF load_fill = load_active ? RGB(42, 102, 168)
                                               : (load_hover ? RGB(36, 46, 66) : RGB(24, 30, 42));
        const COLORREF load_border = load_active ? RGB(114, 192, 255)
                                                 : (load_hover ? RGB(92, 148, 218)
                                                               : (slot.used ? RGB(58, 76, 104) : RGB(42, 48, 62)));
        DrawBorderRect(dc, zones[load_idx].rect, load_fill, load_border, 5);

        wchar_t slot_label[96] = {};
        if (slot.used) {
            const std::wstring summary = FormatPresetSummary(slot.cfg, en);
            swprintf_s(slot_label, en ? L"Slot %d [%ls]%ls" : L"预设%d [%ls]%ls",
                       i + 1, summary.c_str(), (cfg == slot.cfg) ? L"★" : L"");
        } else {
            swprintf_s(slot_label, en ? L"Slot %d  (Empty)" : L"预设 %d  (空)", i + 1);
        }
        RECT label_r = zones[load_idx].rect;
        label_r.left += 7;
        label_r.right -= 5;
        DrawTextBox(dc, g_panel.font_small,
                    slot.used ? RGB(232, 240, 252) : RGB(128, 138, 156),
                    label_r, slot_label, DT_LEFT | DT_VCENTER);

        const bool save_hover = (g_panel.hover_index == static_cast<int>(save_idx));
        DrawBorderRect(dc, zones[save_idx].rect,
                       save_hover ? RGB(60, 120, 88) : RGB(32, 42, 58),
                       save_hover ? RGB(116, 224, 164) : RGB(66, 82, 108), 5);
        DrawTextBox(dc, g_panel.font_small, RGB(214, 242, 224), zones[save_idx].rect,
                    ZoneLabel(zones[save_idx]), DT_CENTER | DT_VCENTER);
    }

    // Bottom footer bar
    const wchar_t* footer_text = en ? g_panel.status_en.c_str() : g_panel.status_zh.c_str();
    if (g_panel.hover_index >= 0 &&
        static_cast<size_t>(g_panel.hover_index) < zones.size()) {
        const HitTargetKind hover_kind = zones[static_cast<size_t>(g_panel.hover_index)].kind;
        if (hover_kind == HitTargetKind::SmaaOff) {
            footer_text = PickLang(L"关闭 SMAA 1x 抗锯齿", L"Disable SMAA 1x anti-aliasing");
        } else if (hover_kind == HitTargetKind::SmaaColor) {
            footer_text = PickLang(L"SMAA 1x Color: 极速色彩边缘检测 (0.2ms)，消除画面绝大部分几何锯齿",
                                   L"SMAA 1x Color: Ultra-fast luma/chroma edge detection (0.2ms)");
        } else if (hover_kind == HitTargetKind::SmaaDepthColor) {
            footer_text = PickLang(L"SMAA 1x Depth+Color: 二阶线性深度梯度双检，精确锁定 3D 模型轮廓，保护地砖纹理",
                                   L"SMAA 1x Depth+Color: 2nd-order depth gradient guided silhouette AA");
        } else if (hover_kind == HitTargetKind::LutCustomCube) {
            footer_text = IsP2CustomCubeLutLoaded()
                              ? PickLang(L"已检测到 <游戏根目录>\\TL2_CustomLUT.cube，点击即可启用（支持热重载）",
                                         L"Detected <GameRoot>\\TL2_CustomLUT.cube — click to enable (hot-reload)")
                              : PickLang(L"用法: 将 3D .cube 改名为 TL2_CustomLUT.cube 放游戏根目录（支持热重载）",
                                         L"Usage: Rename 3D .cube to TL2_CustomLUT.cube in game root (hot-reload)");
        } else if (hover_kind == HitTargetKind::LangZh) {
            footer_text = L"切换界面语言为简体中文";
        } else if (hover_kind == HitTargetKind::LangEn) {
            footer_text = L"Switch UI language to English";
        }
    }
    FillSolidRect(dc, RECT{0, static_cast<LONG>(kPanelHeight - 30),
                           static_cast<LONG>(kPanelWidth), static_cast<LONG>(kPanelHeight)},
                  RGB(14, 18, 26));
    DrawTextBox(dc, g_panel.font_small, RGB(154, 186, 224),
                RECT{kPaddingX, static_cast<LONG>(kPanelHeight - 28),
                     static_cast<LONG>(kPanelWidth - kPaddingX), static_cast<LONG>(kPanelHeight - 2)},
                footer_text, DT_LEFT | DT_VCENTER);

    g_panel.dirty = false;
}

void HandleHitClickLocked(int hit_idx) {
    if (hit_idx < 0 || hit_idx >= static_cast<int>(GetHitZones().size())) {
        return;
    }
    const HitTargetKind kind = GetHitZones()[static_cast<size_t>(hit_idx)].kind;
    switch (kind) {
        case HitTargetKind::CloseButton:
            g_panel.open = false;
            g_panel.dragging = false;
            g_panel.active_slider_index = -1;
            return;
        case HitTargetKind::LangZh:
            g_panel.language = UiLanguage::ZhCn;
            SetStatusBilingualLocked(L"界面语言已切换为: 简体中文",
                                     L"界面语言已切换为: 简体中文");
            break;
        case HitTargetKind::LangEn:
            g_panel.language = UiLanguage::EnUs;
            SetStatusBilingualLocked(L"UI language switched to: English",
                                     L"UI language switched to: English");
            break;
        case HitTargetKind::ResetStrengths: {
            for (const auto& spec : kSliderSpecs) {
                spec.set_fn(100);
            }
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"已将全部 14 项渲染强度恢复为默认 100%",
                                     L"All 14 render strength sliders reset to 100%");
            break;
        }
        case HitTargetKind::ToggleAllOn:
        case HitTargetKind::ToggleAllOff: {
            const bool enable_all = (kind == HitTargetKind::ToggleAllOn);
            VisualConfig next_cfg = CaptureVisualConfigFromEngine();
            next_cfg.phase3_enhance = enable_all;
            next_cfg.af_point_enhance = enable_all;
            next_cfg.p0_rimlit_enhance = enable_all;
            next_cfg.n5_ibl_wrap_enabled = enable_all;
            next_cfg.ao_enabled = enable_all;
            next_cfg.p0_sscs_enabled = enable_all;
            next_cfg.p2_shadow_map_enabled = enable_all;
            next_cfg.p1_soft_particles = enable_all;
            next_cfg.p2_aerial_vignette = enable_all;
            next_cfg.p2_ssr_enabled = enable_all;
            ApplyVisualConfigToEngine(next_cfg);
            g_panel.active_custom_slot = -1;
            if (enable_all) {
                SetStatusBilingualLocked(L"已全部开启 10 项实时渲染效果",
                                         L"Enabled all 10 real-time render effects");
            } else {
                SetStatusBilingualLocked(L"已全部关闭 10 项实时渲染效果 (便于原版对比)",
                                         L"Disabled all 10 render effects (vanilla comparison)");
            }
            break;
        }
        case HitTargetKind::TogglePhase3: {
            const bool next = TogglePhase3RuntimeEnhance();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 材质法线细节与边缘光",
                                         L"Enabled: Surface Normals & Rim Lighting");
            } else {
                SetStatusBilingualLocked(L"已关闭: 材质法线细节与边缘光",
                                         L"Disabled: Surface Normals & Rim Lighting");
            }
            break;
        }
        case HitTargetKind::ToggleAfPoint: {
            const bool next = ToggleF11VisualEnhance();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 16x 各向异性过滤 + 动态点光增强",
                                         L"Enabled: 16x Anisotropic Filter + Point Lights");
            } else {
                SetStatusBilingualLocked(L"已关闭: 16x 各向异性过滤 + 动态点光增强",
                                         L"Disabled: 16x Anisotropic Filter + Point Lights");
            }
            break;
        }
        case HitTargetKind::ToggleP0RimLit: {
            const bool next = ToggleP0RimLitEnhance();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 微法线细节 / 金属高光 / 纹理自发光",
                                         L"Enabled: Micro-Normals / Specular / Emissive");
            } else {
                SetStatusBilingualLocked(L"已关闭: 微法线细节 / 金属高光 / 纹理自发光",
                                         L"Disabled: Micro-Normals / Specular / Emissive");
            }
            break;
        }
        case HitTargetKind::ToggleAo: {
            const bool next = ToggleRouteCAoRuntimeEnabled();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 屏幕空间环境光遮蔽 (SSAO)",
                                         L"Enabled: Screen-Space Ambient Occlusion (SSAO)");
            } else {
                SetStatusBilingualLocked(L"已关闭: 屏幕空间环境光遮蔽 (SSAO)",
                                         L"Disabled: Screen-Space Ambient Occlusion (SSAO)");
            }
            break;
        }
        case HitTargetKind::ToggleP0Sscs: {
            const bool next = !IsRouteCSscsRuntimeEnabled();
            SetRouteCSscsRuntimeEnabled(next);
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 全分辨率接触阴影 (SSCS) + 冷色调",
                                         L"Enabled: Contact Shadows (SSCS) + Cool Tone");
            } else {
                SetStatusBilingualLocked(L"已关闭: 全分辨率接触阴影 (SSCS) + 冷色调",
                                         L"Disabled: Contact Shadows (SSCS) + Cool Tone");
            }
            break;
        }
        case HitTargetKind::ToggleShadowMap: {
            const bool next = ToggleShadowMapEnabled();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 2048 硬件深度太阳实时阴影 (16-tap PCF)",
                                         L"Enabled: 2048 Hardware Depth Sun Shadow Map (PCF16)");
            } else {
                SetStatusBilingualLocked(L"已关闭: 2048 硬件深度太阳实时阴影",
                                         L"Disabled: 2048 Hardware Depth Sun Shadow Map");
            }
            break;
        }
        case HitTargetKind::ToggleP1SoftParticles: {
            const bool next = ToggleP1SoftParticlesEnabled();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 屏幕空间软粒子",
                                         L"Enabled: Screen-Space Soft Particles");
            } else {
                SetStatusBilingualLocked(L"已关闭: 屏幕空间软粒子",
                                         L"Disabled: Screen-Space Soft Particles");
            }
            break;
        }
        case HitTargetKind::ToggleP2AerialVignette: {
            const bool next = ToggleP2AerialVignetteEnabled();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 深度空气透视 + 光学暗角",
                                         L"Enabled: Aerial Perspective + Optical Vignette");
            } else {
                SetStatusBilingualLocked(L"已关闭: 深度空气透视 + 光学暗角",
                                         L"Disabled: Aerial Perspective + Optical Vignette");
            }
            break;
        }
        case HitTargetKind::ToggleP2Ssr: {
            const bool next = ToggleP2SsrEnabled();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 局部水面/湿地屏幕空间反射 (SSR)",
                                         L"Enabled: Water / Puddle Reflections (SSR)");
            } else {
                SetStatusBilingualLocked(L"已关闭: 局部水面/湿地屏幕空间反射",
                                         L"Disabled: Water / Puddle Reflections (SSR)");
            }
            break;
        }
        case HitTargetKind::TogglePhase3IblWrap: {
            const bool next = TogglePhase3IblWrap();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 昼夜解析 IBL 金属反光 + 人物柔光 [N5]",
                                         L"Enabled: ToD Analytical IBL Sheen & Wrap [N5]");
            } else {
                SetStatusBilingualLocked(L"已关闭: 昼夜解析 IBL 金属反光 + 人物柔光 [N5]",
                                         L"Disabled: ToD Analytical IBL Sheen & Wrap [N5]");
            }
            break;
        }
        case HitTargetKind::FilterOff:
            SetP2FilterPreset(0);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"画面风格滤镜已设为: 关闭",
                                     L"Visual Style Filter set to: Off");
            break;
        case HitTargetKind::FilterHob:
            SetP2FilterPreset(1);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"画面风格滤镜已设为: Hob 明快",
                                     L"Visual Style Filter set to: Hob Vibrant");
            break;
        case HitTargetKind::FilterDark:
            SetP2FilterPreset(2);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"画面风格滤镜已设为: 暗黑胶片",
                                     L"Visual Style Filter set to: Dark Film");
            break;
        case HitTargetKind::LutOff:
            SetP2LutPreset(0);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"3D LUT 调色已设为: 关闭",
                                     L"3D LUT Color Grading set to: Off");
            break;
        case HitTargetKind::LutTealOrange:
            SetP2LutPreset(1);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"3D LUT 调色已设为: 映画暖青 (Teal & Orange)",
                                     L"3D LUT Color Grading set to: Teal & Orange");
            break;
        case HitTargetKind::LutColdNordic:
            SetP2LutPreset(2);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"3D LUT 调色已设为: 冷峻史诗 (Cold Nordic)",
                                     L"3D LUT Color Grading set to: Cold Nordic");
            break;
        case HitTargetKind::LutCustomCube:
            SetP2LutPreset(3);
            g_panel.active_custom_slot = -1;
            if (IsP2CustomCubeLutLoaded()) {
                SetStatusBilingualLocked(L"3D LUT 已载入: <游戏根目录>\\TL2_CustomLUT.cube",
                                         L"3D LUT loaded: <GameRoot>\\TL2_CustomLUT.cube");
            } else {
                SetStatusBilingualLocked(L"未找到文件: 请将 3D .cube 改名为 TL2_CustomLUT.cube 放游戏根目录",
                                         L"Not found: Rename 3D .cube to TL2_CustomLUT.cube in game root");
            }
            break;
        case HitTargetKind::HdrOff:
            SetHdrTonemapMode(0);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"FP16 HDR 色调映射已关闭 (原生 LDR 输出)",
                                     L"FP16 HDR Tonemap disabled (Vanilla LDR)");
            break;
        case HitTargetKind::HdrAces:
            SetHdrTonemapMode(1);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"FP16 HDR 色调映射已设为: ACES Filmic (高动态映画)",
                                     L"FP16 HDR Tonemap set to: ACES Filmic");
            break;
        case HitTargetKind::HdrAgx:
            SetHdrTonemapMode(2);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"FP16 HDR 色调映射已设为: AgX Punchy (柔和亮部去饱和)",
                                     L"FP16 HDR Tonemap set to: AgX Punchy");
            break;
        case HitTargetKind::SmaaOff:
            SetSmaaMode(0);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"抗锯齿已设为: 关闭 (原生边缘)",
                                     L"Anti-Aliasing set to: Off");
            break;
        case HitTargetKind::SmaaColor:
            SetSmaaMode(1);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"抗锯齿已设为: Color (极速色彩边缘检测, 0.2ms)",
                                     L"Anti-Aliasing set to: Color Edge Detection (0.2ms)");
            break;
        case HitTargetKind::SmaaDepthColor:
            SetSmaaMode(2);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"抗锯齿已设为: Depth+Color (二阶线性深度双检, 极致平滑)",
                                     L"Anti-Aliasing set to: Depth+Color (2nd-Order Depth Dual)");
            break;
        case HitTargetKind::ToggleWeatherRain: {
            const bool next = ToggleWeatherRainEnabled();
            g_panel.active_custom_slot = -1;
            if (next) {
                SetStatusBilingualLocked(L"已开启: 雨天湿地倒影 (地面动态水坑与波纹)",
                                         L"Enabled: Rain & Wet Ground Reflections (Puddles)");
            } else {
                SetStatusBilingualLocked(L"已关闭: 雨天湿地倒影",
                                         L"Disabled: Rain & Wet Ground Reflections");
            }
            break;
        }
        case HitTargetKind::TimeAuto: {
            SetTimeOfDayLocked(false);
            SetTimeOfDayHours(-1);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"游戏时间已恢复为: 自然昼夜循环",
                                     L"Time of day restored to: Auto day/night cycle");
            break;
        }
        case HitTargetKind::TimeDawn: {
            SetTimeOfDayHours(6);
            SetTimeOfDayLocked(true);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"游戏时间已设定为: 清晨黎明 (06:00)",
                                     L"Game time set to: Dawn (06:00)");
            break;
        }
        case HitTargetKind::TimeNoon: {
            SetTimeOfDayHours(12);
            SetTimeOfDayLocked(true);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"游戏时间已设定为: 正午日光 (12:00)",
                                     L"Game time set to: Noon (12:00)");
            break;
        }
        case HitTargetKind::TimeDusk: {
            SetTimeOfDayHours(18);
            SetTimeOfDayLocked(true);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"游戏时间已设定为: 傍晚黄昏 (18:00)",
                                     L"Game time set to: Dusk (18:00)");
            break;
        }
        case HitTargetKind::TimeMidnight: {
            SetTimeOfDayHours(24);
            SetTimeOfDayLocked(true);
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"游戏时间已设定为: 深夜午夜 (24:00)",
                                     L"Game time set to: Midnight (24:00)");
            break;
        }
        case HitTargetKind::BuiltinFullHd:
            ApplyVisualConfigToEngine(MakeBuiltinPreset(0));
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"已应用内置预设: 全开高清",
                                     L"Applied built-in preset: Full HD+");
            break;
        case HitTargetKind::BuiltinDarkDungeon:
            ApplyVisualConfigToEngine(MakeBuiltinPreset(1));
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"已应用内置预设: 暗黑地牢",
                                     L"Applied built-in preset: Dark Dungeon");
            break;
        case HitTargetKind::BuiltinVanilla:
            ApplyVisualConfigToEngine(MakeBuiltinPreset(2));
            g_panel.active_custom_slot = -1;
            SetStatusBilingualLocked(L"已应用内置预设: 纯原版",
                                     L"Applied built-in preset: Vanilla");
            break;
        case HitTargetKind::CustomLoad0:
        case HitTargetKind::CustomLoad1:
        case HitTargetKind::CustomLoad2:
        case HitTargetKind::CustomLoad3:
        case HitTargetKind::CustomLoad4:
        case HitTargetKind::CustomLoad5:
        case HitTargetKind::CustomLoad6:
        case HitTargetKind::CustomLoad7:
        case HitTargetKind::CustomLoad8:
        case HitTargetKind::CustomLoad9: {
            const int slot = static_cast<int>(kind) - static_cast<int>(HitTargetKind::CustomLoad0);
            if (g_panel.presets[static_cast<size_t>(slot)].used) {
                ApplyVisualConfigToEngine(g_panel.presets[static_cast<size_t>(slot)].cfg);
                g_panel.active_custom_slot = slot;
                wchar_t msg_zh[64] = {};
                wchar_t msg_en[64] = {};
                swprintf_s(msg_zh, L"已切换至自定义预设 %d", slot + 1);
                swprintf_s(msg_en, L"Loaded Custom Slot %d", slot + 1);
                SetStatusBilingualLocked(msg_zh, msg_en);
            } else {
                wchar_t msg_zh[64] = {};
                wchar_t msg_en[64] = {};
                swprintf_s(msg_zh, L"预设 %d 为空，请先点击右侧 [保存]", slot + 1);
                swprintf_s(msg_en, L"Slot %d is empty — click [Save] on the right first", slot + 1);
                SetStatusBilingualLocked(msg_zh, msg_en);
                g_panel.dirty = true;
                return;
            }
            break;
        }
        case HitTargetKind::CustomSave0:
        case HitTargetKind::CustomSave1:
        case HitTargetKind::CustomSave2:
        case HitTargetKind::CustomSave3:
        case HitTargetKind::CustomSave4:
        case HitTargetKind::CustomSave5:
        case HitTargetKind::CustomSave6:
        case HitTargetKind::CustomSave7:
        case HitTargetKind::CustomSave8:
        case HitTargetKind::CustomSave9: {
            const int slot = static_cast<int>(kind) - static_cast<int>(HitTargetKind::CustomSave0);
            g_panel.presets[static_cast<size_t>(slot)].used = true;
            g_panel.presets[static_cast<size_t>(slot)].cfg = CaptureVisualConfigFromEngine();
            g_panel.active_custom_slot = slot;
            wchar_t msg_zh[64] = {};
            wchar_t msg_en[64] = {};
            swprintf_s(msg_zh, L"已保存当前配置至 预设 %d", slot + 1);
            swprintf_s(msg_en, L"Saved current settings to Slot %d", slot + 1);
            SetStatusBilingualLocked(msg_zh, msg_en);
            break;
        }
        default:
            return;
    }

    SaveConfigFileLocked(nullptr);
    g_panel.dirty = true;
}

bool IsMouseMessage(UINT msg) {
    switch (msg) {
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
            return true;
        default:
            return false;
    }
}

constexpr UINT_PTR kPanelSubclassId = 0x544C3246ULL;  // 'TL2F'

LRESULT CALLBACK PanelHookWndProc(
    HWND hwnd,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam,
    UINT_PTR uIdSubclass,
    DWORD_PTR /*dwRefData*/) {
    bool swallow = false;
    bool need_release_capture = false;
    bool need_set_capture = false;
    bool remove_self = false;

    {
        std::lock_guard<std::mutex> lock(g_panel.mutex);

        if (msg == WM_NCDESTROY) {
            g_panel.hwnd = nullptr;
            g_panel.dragging = false;
            g_panel.active_slider_index = -1;
            g_wndproc_attached.store(false, std::memory_order_release);
            remove_self = true;
        } else if (msg == WM_CAPTURECHANGED && (g_panel.dragging || g_panel.active_slider_index >= 0)) {
            g_panel.dragging = false;
            g_panel.active_slider_index = -1;
            g_panel.dirty = true;
        } else if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F8) {
            if ((lParam & (1LL << 30)) == 0) {
                g_panel.open = !g_panel.open;
                g_panel.dirty = true;
                if (!g_panel.open) {
                    g_panel.dragging = false;
                    g_panel.active_slider_index = -1;
                    need_release_capture = (::GetCapture() == hwnd);
                }
            }
            g_panel.swallow_f8_keyup = true;
            swallow = true;
        } else if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_F8 && g_panel.swallow_f8_keyup) {
            g_panel.swallow_f8_keyup = false;
            swallow = true;
        } else if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_ESCAPE && g_panel.open) {
            g_panel.open = false;
            g_panel.dragging = false;
            g_panel.active_slider_index = -1;
            g_panel.dirty = true;
            g_panel.swallow_esc_keyup = true;
            need_release_capture = (::GetCapture() == hwnd);
            swallow = true;
        } else if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && wParam == VK_ESCAPE && g_panel.swallow_esc_keyup) {
            g_panel.swallow_esc_keyup = false;
            swallow = true;
        } else if (g_panel.open && IsMouseMessage(msg)) {
            ClampPanelPosLocked();

            float rt_x = 0.0f;
            float rt_y = 0.0f;
            bool have_pt = false;
            if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) {
                POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (::ScreenToClient(hwnd, &pt)) {
                    have_pt = ClientPointToRenderTarget(hwnd, MAKELPARAM(pt.x, pt.y), rt_x, rt_y);
                }
            } else {
                have_pt = ClientPointToRenderTarget(hwnd, lParam, rt_x, rt_y);
            }

            if (have_pt) {
                if (g_panel.dragging && msg == WM_MOUSEMOVE) {
                    g_panel.has_custom_pos = true;
                    g_panel.pos_x = rt_x - static_cast<float>(g_panel.drag_offset_x);
                    g_panel.pos_y = rt_y - static_cast<float>(g_panel.drag_offset_y);
                    ClampPanelPosLocked();
                    g_panel.cursor_panel.x = static_cast<LONG>(rt_x);
                    g_panel.cursor_panel.y = static_cast<LONG>(rt_y);
                    g_panel.cursor_inside = true;
                    swallow = true;
                } else if (g_panel.dragging && msg == WM_LBUTTONUP) {
                    g_panel.dragging = false;
                    need_release_capture = (::GetCapture() == hwnd);
                    swallow = true;
                } else if (g_panel.active_slider_index >= 0 && msg == WM_MOUSEMOVE) {
                    const int local_x = static_cast<int>(rt_x - g_panel.pos_x);
                    if (g_panel.active_slider_index == kSliderTimeIndex) {
                        const int h = HourFromTrackX(local_x);
                        SetTimeOfDayHours(h);
                        SetTimeOfDayLocked(true);
                        g_panel.active_custom_slot = -1;
                        wchar_t msg_zh[64] = {};
                        wchar_t msg_en[64] = {};
                        swprintf_s(msg_zh, L"游戏时间已调节至: %02d:00 (昼夜锁定)", h);
                        swprintf_s(msg_en, L"Game time set to: %02d:00 (Locked)", h);
                        SetStatusBilingualLocked(msg_zh, msg_en);
                        g_panel.dirty = true;
                    } else {
                        ApplyEffectStrengthByIndex(g_panel.active_slider_index, PctFromTrackX(local_x));
                    }
                    g_panel.cursor_panel.x = static_cast<LONG>(rt_x);
                    g_panel.cursor_panel.y = static_cast<LONG>(rt_y);
                    g_panel.cursor_inside = true;
                    swallow = true;
                } else if (g_panel.active_slider_index >= 0 && msg == WM_LBUTTONUP) {
                    const int local_x = static_cast<int>(rt_x - g_panel.pos_x);
                    if (g_panel.active_slider_index == kSliderTimeIndex) {
                        const int h = HourFromTrackX(local_x);
                        SetTimeOfDayHours(h);
                        SetTimeOfDayLocked(true);
                        g_panel.active_custom_slot = -1;
                        wchar_t msg_zh[64] = {};
                        wchar_t msg_en[64] = {};
                        swprintf_s(msg_zh, L"游戏时间已锁定为: %02d:00", h);
                        swprintf_s(msg_en, L"Game time locked at: %02d:00", h);
                        SetStatusBilingualLocked(msg_zh, msg_en);
                    } else {
                        ApplyEffectStrengthByIndex(g_panel.active_slider_index, PctFromTrackX(local_x));
                    }
                    g_panel.active_slider_index = -1;
                    need_release_capture = (::GetCapture() == hwnd);
                    SaveConfigFileLocked(nullptr);
                    swallow = true;
                } else {
                    const int local_x = static_cast<int>(rt_x - g_panel.pos_x);
                    const int local_y = static_cast<int>(rt_y - g_panel.pos_y);
                    const bool over_panel =
                        (local_x >= 0 && local_y >= 0 &&
                         local_x < static_cast<int>(kPanelWidth) &&
                         local_y < static_cast<int>(kPanelHeight));
                    g_panel.cursor_panel.x = static_cast<LONG>(rt_x);
                    g_panel.cursor_panel.y = static_cast<LONG>(rt_y);
                    g_panel.cursor_inside = true;

                    const int new_hover = over_panel ? HitTestPanelLocal(local_x, local_y) : -1;
                    if (new_hover != g_panel.hover_index) {
                        g_panel.hover_index = new_hover;
                        g_panel.dirty = true;
                    }

                    if (over_panel) {
                        swallow = true;
                        if ((msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && new_hover >= 0) {
                            const HitTargetKind kind =
                                GetHitZones()[static_cast<size_t>(new_hover)].kind;
                            if (kind == HitTargetKind::TitleDrag) {
                                g_panel.dragging = true;
                                g_panel.drag_offset_x = local_x;
                                g_panel.drag_offset_y = local_y;
                                need_set_capture = true;
                            } else if (kind >= HitTargetKind::Slider0 && kind <= HitTargetKind::Slider13) {
                                if (local_x <= kSliderTrackRight + 8) {
                                    const int s_idx = static_cast<int>(kind) - static_cast<int>(HitTargetKind::Slider0);
                                    g_panel.active_slider_index = s_idx;
                                    need_set_capture = true;
                                    ApplyEffectStrengthByIndex(s_idx, PctFromTrackX(local_x));
                                }
                            } else if (kind == HitTargetKind::SliderTime) {
                                if (local_x <= kSliderTrackRight + 8) {
                                    g_panel.active_slider_index = kSliderTimeIndex;
                                    need_set_capture = true;
                                    const int h = HourFromTrackX(local_x);
                                    SetTimeOfDayHours(h);
                                    SetTimeOfDayLocked(true);
                                    g_panel.active_custom_slot = -1;
                                    wchar_t msg_zh[64] = {};
                                    wchar_t msg_en[64] = {};
                                    swprintf_s(msg_zh, L"游戏时间已调节至: %02d:00 (昼夜锁定)", h);
                                    swprintf_s(msg_en, L"Game time set to: %02d:00 (Locked)", h);
                                    SetStatusBilingualLocked(msg_zh, msg_en);
                                    g_panel.dirty = true;
                                }
                            } else {
                                HandleHitClickLocked(new_hover);
                                if (!g_panel.open) {
                                    need_release_capture = (::GetCapture() == hwnd);
                                }
                            }
                        } else if (msg == WM_RBUTTONDOWN && new_hover >= 0) {
                            const HitTargetKind kind =
                                GetHitZones()[static_cast<size_t>(new_hover)].kind;
                            if (kind >= HitTargetKind::Slider0 && kind <= HitTargetKind::Slider13) {
                                const int s_idx = static_cast<int>(kind) - static_cast<int>(HitTargetKind::Slider0);
                                ApplyEffectStrengthByIndex(s_idx, 100);
                                SaveConfigFileLocked(nullptr);
                            } else if (kind == HitTargetKind::SliderTime) {
                                SetTimeOfDayLocked(false);
                                SetTimeOfDayHours(-1);
                                g_panel.active_custom_slot = -1;
                                SetStatusBilingualLocked(L"游戏时间已恢复为: 自然昼夜循环",
                                                         L"Game time restored to: Auto day/night cycle");
                                g_panel.dirty = true;
                                SaveConfigFileLocked(nullptr);
                            }
                        } else if (msg == WM_MOUSEWHEEL && new_hover >= 0) {
                            const HitTargetKind kind =
                                GetHitZones()[static_cast<size_t>(new_hover)].kind;
                            int s_idx = -1;
                            if (kind >= HitTargetKind::Slider0 && kind <= HitTargetKind::Slider13) {
                                s_idx = static_cast<int>(kind) - static_cast<int>(HitTargetKind::Slider0);
                            } else if (kind >= HitTargetKind::TogglePhase3 && kind <= HitTargetKind::ToggleP2Ssr) {
                                const size_t row = static_cast<size_t>(kind) - static_cast<size_t>(HitTargetKind::TogglePhase3);
                                if (row < kSection1SliderIndices.size()) {
                                    s_idx = kSection1SliderIndices[row];
                                }
                            } else if (kind == HitTargetKind::ToggleWeatherRain) {
                                s_idx = 10;
                            } else if (kind >= HitTargetKind::FilterOff && kind <= HitTargetKind::FilterDark) {
                                s_idx = 8;
                            } else if (kind >= HitTargetKind::LutOff && kind <= HitTargetKind::LutCustomCube) {
                                s_idx = 9;
                            } else if (kind >= HitTargetKind::HdrOff && kind <= HitTargetKind::HdrAgx) {
                                s_idx = 13;
                            }
                            const int wheel_delta = GET_WHEEL_DELTA_WPARAM(wParam);
                            if (s_idx >= 0 && wheel_delta != 0) {
                                const int step = ((GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) ? 10 : 5) *
                                                 ((wheel_delta > 0) ? 1 : -1);
                                ApplyEffectStrengthByIndex(s_idx, GetEffectStrengthByIndex(s_idx) + step);
                                SaveConfigFileLocked(nullptr);
                            } else if (kind == HitTargetKind::SliderTime && wheel_delta != 0) {
                                int cur = GetTimeOfDayHours();
                                if (cur < 0) cur = 12;
                                int next_h = cur + (wheel_delta > 0 ? 1 : -1);
                                next_h = (next_h % 24 + 24) % 24;
                                SetTimeOfDayHours(next_h);
                                SetTimeOfDayLocked(true);
                                g_panel.active_custom_slot = -1;
                                wchar_t msg_zh[64] = {};
                                wchar_t msg_en[64] = {};
                                swprintf_s(msg_zh, L"游戏时间已调节至: %02d:00 (昼夜锁定)", next_h);
                                swprintf_s(msg_en, L"Game time set to: %02d:00 (Locked)", next_h);
                                SetStatusBilingualLocked(msg_zh, msg_en);
                                g_panel.dirty = true;
                                SaveConfigFileLocked(nullptr);
                            }
                        }
                    }
                }
            }
        }
    }

    if (need_set_capture) {
        ::SetCapture(hwnd);
    }
    if (need_release_capture) {
        ::ReleaseCapture();
    }
    if (remove_self) {
        ::RemoveWindowSubclass(hwnd, &PanelHookWndProc, uIdSubclass);
    }

    if (swallow) {
        return 0;
    }
    return ::DefSubclassProc(hwnd, msg, wParam, lParam);
}

}  // namespace

VisualConfig CaptureVisualConfigFromEngine() {
    VisualConfig cfg;
    cfg.phase3_enhance = IsPhase3RuntimeEnhanceEnabled();
    cfg.af_point_enhance = IsF11VisualEnhanceEnabled();
    cfg.p0_rimlit_enhance = IsP0RimLitEnhanceEnabled();
    cfg.ao_enabled = IsRouteCAoRuntimeEnabled();
    cfg.p0_sscs_enabled = IsRouteCSscsRuntimeEnabled();
    cfg.p1_soft_particles = IsP1SoftParticlesEnabled();
    cfg.p2_aerial_vignette = IsP2AerialVignetteEnabled();
    cfg.p2_ssr_enabled = IsP2SsrEnabled();
    cfg.n5_ibl_wrap_enabled = IsPhase3IblWrapEnabled();
    cfg.p2_shadow_map_enabled = IsShadowMapEnabled();
    cfg.p2_filter_preset = ClampFilterPreset(GetP2FilterPreset());
    cfg.p2_lut_preset = ClampLutPreset(GetP2LutPreset());
    cfg.hdr_tonemap_mode = ClampHdrTonemapMode(GetHdrTonemapMode());
    cfg.smaa_mode = ClampSmaaMode(GetSmaaMode());
    cfg.weather_rain_enabled = IsWeatherRainEnabled();
    cfg.weather_rain_strength = GetWeatherRainStrengthPct();
    cfg.time_of_day_locked = IsTimeOfDayLocked();
    cfg.time_of_day_hours = cfg.time_of_day_locked ? GetTimeOfDayHours() : -1;
    for (const auto& spec : kSliderSpecs) {
        cfg.*(spec.member) = ClampStrengthPct(spec.get_fn());
    }
    return cfg;
}

void ApplyVisualConfigToEngine(const VisualConfig& cfg) {
    SetPhase3RuntimeEnhanceEnabled(cfg.phase3_enhance);
    SetF11VisualEnhanceEnabled(cfg.af_point_enhance);
    SetP0RimLitEnhanceEnabled(cfg.p0_rimlit_enhance);
    SetRouteCAoRuntimeEnabled(cfg.ao_enabled);
    SetRouteCSscsRuntimeEnabled(cfg.p0_sscs_enabled);
    SetP1SoftParticlesEnabled(cfg.p1_soft_particles);
    SetP2AerialVignetteEnabled(cfg.p2_aerial_vignette);
    SetP2SsrEnabled(cfg.p2_ssr_enabled);
    SetPhase3IblWrapEnabled(cfg.n5_ibl_wrap_enabled);
    SetShadowMapEnabled(cfg.p2_shadow_map_enabled);
    SetP2FilterPreset(ClampFilterPreset(cfg.p2_filter_preset));
    SetP2LutPreset(ClampLutPreset(cfg.p2_lut_preset));
    SetHdrTonemapMode(ClampHdrTonemapMode(cfg.hdr_tonemap_mode));
    SetSmaaMode(ClampSmaaMode(cfg.smaa_mode));
    SetWeatherRainEnabled(cfg.weather_rain_enabled);
    SetWeatherRainStrengthPct(cfg.weather_rain_strength);
    if (cfg.time_of_day_locked && cfg.time_of_day_hours >= 0) {
        SetTimeOfDayHours(cfg.time_of_day_hours);
        SetTimeOfDayLocked(true);
    } else {
        SetTimeOfDayLocked(false);
        SetTimeOfDayHours(-1);
    }
    for (const auto& spec : kSliderSpecs) {
        spec.set_fn(ClampStrengthPct(cfg.*(spec.member)));
    }
}

std::wstring ResolveDefaultConfigIniPath() {
    return GetExecutableDirectory() + L"\\TL2_RouteB_Config.ini";
}

bool LoadVisualConfigAndPresets(const wchar_t* ini_path_override) {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    const std::wstring target =
        (ini_path_override && ini_path_override[0]) ? std::wstring(ini_path_override)
                                                    : ResolveDefaultConfigIniPath();
    g_panel.ini_path = target;

    const VisualConfig defaults{};
    const UiLanguage sys_lang = DetectSystemDefaultUiLanguage();
    const DWORD attrs = ::GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        g_panel.language = sys_lang;
        ApplyVisualConfigToEngine(defaults);
        g_panel.presets = {};
        g_panel.active_custom_slot = -1;
        g_panel.dirty = true;
        return SaveConfigFileLocked(target.c_str());
    }

    const wchar_t* sys_lang_str = (sys_lang == UiLanguage::EnUs) ? L"en" : L"zh";
    wchar_t lang_buf[16] = {};
    ::GetPrivateProfileStringW(L"Panel", L"Language", sys_lang_str, lang_buf, 16, target.c_str());
    if (_wcsicmp(lang_buf, L"en") == 0 || _wcsicmp(lang_buf, L"en-us") == 0 ||
        _wcsicmp(lang_buf, L"1") == 0) {
        g_panel.language = UiLanguage::EnUs;
    } else {
        g_panel.language = UiLanguage::ZhCn;
    }

    const VisualConfig loaded_active = LoadVisualConfigFromSection(L"Active", defaults, target.c_str());
    ApplyVisualConfigToEngine(loaded_active);

    for (int i = 0; i < kCustomPresetCount; ++i) {
        wchar_t sec[32] = {};
        swprintf_s(sec, L"Preset%d", i + 1);
        const bool used = ReadIniInt(sec, L"Used", 0, target.c_str()) != 0;
        g_panel.presets[static_cast<size_t>(i)].used = used;
        g_panel.presets[static_cast<size_t>(i)].cfg =
            used ? LoadVisualConfigFromSection(sec, defaults, target.c_str()) : VisualConfig{};
    }
    g_panel.dirty = true;
    return true;
}

bool SaveVisualConfigAndPresets(const wchar_t* ini_path_override) {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    return SaveConfigFileLocked(ini_path_override);
}

VisualConfig MakeBuiltinPreset(int builtin_index) {
    VisualConfig cfg;
    switch (builtin_index) {
        case 0:  // 全开高清 (Hob 明快)
            cfg.phase3_enhance = true;
            cfg.af_point_enhance = true;
            cfg.p0_rimlit_enhance = true;
            cfg.ao_enabled = true;
            cfg.p0_sscs_enabled = true;
            cfg.p1_soft_particles = true;
            cfg.p2_aerial_vignette = true;
            cfg.p2_ssr_enabled = true;
            cfg.n5_ibl_wrap_enabled = true;
            cfg.p2_shadow_map_enabled = true;
            cfg.p2_filter_preset = 1;
            cfg.p2_lut_preset = 0;
            cfg.hdr_tonemap_mode = 2;
            cfg.smaa_mode = 2;
            break;
        case 1:  // 暗黑地牢
            cfg.phase3_enhance = true;
            cfg.af_point_enhance = true;
            cfg.p0_rimlit_enhance = true;
            cfg.ao_enabled = true;
            cfg.p0_sscs_enabled = true;
            cfg.p1_soft_particles = true;
            cfg.p2_aerial_vignette = true;
            cfg.p2_ssr_enabled = true;
            cfg.n5_ibl_wrap_enabled = true;
            cfg.p2_shadow_map_enabled = true;
            cfg.p2_filter_preset = 2;
            cfg.p2_lut_preset = 0;
            cfg.hdr_tonemap_mode = 1;
            cfg.smaa_mode = 2;
            break;
        case 2:  // 纯原版
        default:
            cfg.phase3_enhance = false;
            cfg.af_point_enhance = false;
            cfg.p0_rimlit_enhance = false;
            cfg.ao_enabled = false;
            cfg.p0_sscs_enabled = false;
            cfg.p1_soft_particles = false;
            cfg.p2_aerial_vignette = false;
            cfg.p2_ssr_enabled = false;
            cfg.n5_ibl_wrap_enabled = false;
            cfg.p2_shadow_map_enabled = false;
            cfg.p2_filter_preset = 0;
            cfg.p2_lut_preset = 0;
            cfg.hdr_tonemap_mode = 0;
            cfg.smaa_mode = 0;
            break;
    }
    return cfg;
}

bool SetAllRenderTogglesEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    VisualConfig next_cfg = CaptureVisualConfigFromEngine();
    next_cfg.phase3_enhance = enabled;
    next_cfg.af_point_enhance = enabled;
    next_cfg.p0_rimlit_enhance = enabled;
    next_cfg.ao_enabled = enabled;
    next_cfg.p0_sscs_enabled = enabled;
    next_cfg.p1_soft_particles = enabled;
    next_cfg.p2_aerial_vignette = enabled;
    next_cfg.p2_ssr_enabled = enabled;
    next_cfg.n5_ibl_wrap_enabled = enabled;
    next_cfg.p2_shadow_map_enabled = enabled;
    ApplyVisualConfigToEngine(next_cfg);
    g_panel.active_custom_slot = -1;
    g_panel.dirty = true;
    return SaveConfigFileLocked(nullptr);
}

bool ApplyBuiltinPreset(int builtin_index) {
    if (builtin_index < 0 || builtin_index > 2) return false;
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    ApplyVisualConfigToEngine(MakeBuiltinPreset(builtin_index));
    g_panel.active_custom_slot = -1;
    g_panel.dirty = true;
    return SaveConfigFileLocked(nullptr);
}

bool SaveCustomPreset(int slot_index) {
    if (slot_index < 0 || slot_index >= kCustomPresetCount) return false;
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    g_panel.presets[static_cast<size_t>(slot_index)].used = true;
    g_panel.presets[static_cast<size_t>(slot_index)].cfg = CaptureVisualConfigFromEngine();
    g_panel.active_custom_slot = slot_index;
    g_panel.dirty = true;
    return SaveConfigFileLocked(nullptr);
}

bool ApplyCustomPreset(int slot_index) {
    if (slot_index < 0 || slot_index >= kCustomPresetCount) return false;
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    const PresetSlot& slot = g_panel.presets[static_cast<size_t>(slot_index)];
    if (!slot.used) return false;
    ApplyVisualConfigToEngine(slot.cfg);
    g_panel.active_custom_slot = slot_index;
    g_panel.dirty = true;
    return SaveConfigFileLocked(nullptr);
}

PresetSlot GetCustomPresetSlot(int slot_index) {
    if (slot_index < 0 || slot_index >= kCustomPresetCount) return {};
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    return g_panel.presets[static_cast<size_t>(slot_index)];
}

void ResetConfigPanelStateForTest() {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    g_panel.open = false;
    g_panel.dirty = true;
    g_panel.has_custom_pos = false;
    g_panel.dragging = false;
    g_panel.active_slider_index = -1;
    g_panel.hover_index = -1;
    g_panel.language = UiLanguage::ZhCn;
    g_panel.presets = {};
    g_panel.active_custom_slot = -1;
    g_panel.ini_path.clear();
    SetStatusBilingualLocked(kDefaultStatusZh, kDefaultStatusEn);
    ApplyVisualConfigToEngine(VisualConfig{});
}

UiLanguage DetectSystemDefaultUiLanguage() {
    const LANGID lang = ::GetUserDefaultUILanguage();
    return (PRIMARYLANGID(lang) == LANG_CHINESE) ? UiLanguage::ZhCn : UiLanguage::EnUs;
}

UiLanguage GetUiLanguage() {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    return g_panel.language;
}

void SetUiLanguage(UiLanguage lang) {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    g_panel.language = (lang == UiLanguage::EnUs) ? UiLanguage::EnUs : UiLanguage::ZhCn;
    g_panel.dirty = true;
}

UiLanguage ToggleUiLanguage() {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    g_panel.language =
        (g_panel.language == UiLanguage::ZhCn) ? UiLanguage::EnUs : UiLanguage::ZhCn;
    g_panel.dirty = true;
    return g_panel.language;
}

void ToggleConfigPanelOpen() {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    g_panel.open = !g_panel.open;
    g_panel.dragging = false;
    g_panel.dirty = true;
}

void SetConfigPanelOpen(bool open) {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    g_panel.open = open;
    if (!open) g_panel.dragging = false;
    g_panel.dirty = true;
}

bool IsConfigPanelOpen() {
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    return g_panel.open;
}

bool AttachPanelWindow(HWND hwnd) {
    if (!hwnd || !::IsWindow(hwnd)) {
        return false;
    }
    HWND prev_hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_panel.mutex);
        if (g_panel.hwnd == hwnd && g_wndproc_attached.load(std::memory_order_acquire)) {
            return true;
        }
        prev_hwnd = g_panel.hwnd;
        g_panel.hwnd = nullptr;
        g_wndproc_attached.store(false, std::memory_order_release);
    }
    if (prev_hwnd && ::IsWindow(prev_hwnd)) {
        ::RemoveWindowSubclass(prev_hwnd, &PanelHookWndProc, kPanelSubclassId);
    }
    if (!::SetWindowSubclass(hwnd, &PanelHookWndProc, kPanelSubclassId, 0)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_panel.mutex);
        g_panel.hwnd = hwnd;
        g_wndproc_attached.store(true, std::memory_order_release);
    }
    return true;
}

void DetachPanelWindow() {
    HWND hwnd = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_panel.mutex);
        hwnd = g_panel.hwnd;
        g_panel.hwnd = nullptr;
        g_panel.dragging = false;
        g_panel.active_slider_index = -1;
        g_wndproc_attached.store(false, std::memory_order_release);
    }
    if (hwnd && ::IsWindow(hwnd)) {
        if (::GetCapture() == hwnd) {
            ::ReleaseCapture();
        }
        ::RemoveWindowSubclass(hwnd, &PanelHookWndProc, kPanelSubclassId);
    }
}

bool IsPanelWindowAttached() {
    return g_wndproc_attached.load(std::memory_order_acquire);
}

void ShutdownConfigPanel() {
    DetachPanelWindow();
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    DestroyGdiSurfaceLocked();
    g_panel.open = false;
}

bool PollFallbackConfigPanelHotkey() {
    if (g_wndproc_attached.load(std::memory_order_acquire)) {
        return false;
    }
    static bool prev_f8 = false;
    const bool f8_down = (::GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool pressed = f8_down && !prev_f8;
    prev_f8 = f8_down;
    if (pressed) {
        ToggleConfigPanelOpen();
        return true;
    }
    return false;
}

bool RenderConfigPanelOverlay(
    void* /*rw*/,
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* target_rtv,
    ConfigPanelLogFn logger) {
    if (!ctx || !target_rtv) return false;

    bool is_open = false;
    {
        std::lock_guard<std::mutex> lock(g_panel.mutex);
        is_open = g_panel.open;
    }
    if (!is_open) return false;

    // Determine backbuffer dimensions from target_rtv
    ID3D11Resource* rt_res = nullptr;
    target_rtv->GetResource(&rt_res);
    float vp_w = 1280.0f;
    float vp_h = 720.0f;
    if (rt_res) {
        ID3D11Texture2D* tex2d = nullptr;
        if (SUCCEEDED(rt_res->QueryInterface(__uuidof(ID3D11Texture2D),
                                             reinterpret_cast<void**>(&tex2d))) &&
            tex2d) {
            D3D11_TEXTURE2D_DESC desc{};
            tex2d->GetDesc(&desc);
            if (desc.Width > 0 && desc.Height > 0) {
                vp_w = static_cast<float>(desc.Width);
                vp_h = static_cast<float>(desc.Height);
            }
            tex2d->Release();
        }
        rt_res->Release();
    }

    RouteCPanelOverlayRequest req{};
    req.ctx = ctx;
    req.target_rtv = target_rtv;
    req.tex_w = kPanelWidth;
    req.tex_h = kPanelHeight;
    req.box_w = static_cast<float>(kPanelWidth);
    req.box_h = static_cast<float>(kPanelHeight);
    req.panel_alpha = 0.96f;

    {
        std::lock_guard<std::mutex> lock(g_panel.mutex);
        g_panel.last_vp_w = vp_w;
        g_panel.last_vp_h = vp_h;
        ClampPanelPosLocked();

        // Update live cursor position via GetCursorPos so cursor renders smoothly even if static.
        if (g_panel.hwnd && ::IsWindow(g_panel.hwnd)) {
            POINT pt{};
            RECT rc{};
            if (::GetCursorPos(&pt) && ::ScreenToClient(g_panel.hwnd, &pt) &&
                ::GetClientRect(g_panel.hwnd, &rc)) {
                const float cw = static_cast<float>(rc.right - rc.left);
                const float ch = static_cast<float>(rc.bottom - rc.top);
                if (cw > 1.0f && ch > 1.0f) {
                    req.cursor_x = static_cast<float>(pt.x) * (vp_w / cw);
                    req.cursor_y = static_cast<float>(pt.y) * (vp_h / ch);
                    req.draw_cursor = true;
                }
            }
        }

        req.upload_dirty = g_panel.dirty || !g_panel.dib_pixels;
        if (req.upload_dirty) {
            PaintPanelToDibLocked();
        }
        req.bgra_pixels = g_panel.dib_pixels;
        req.box_x = g_panel.pos_x;
        req.box_y = g_panel.pos_y;
    }

    if (!req.bgra_pixels) return false;
    return ExecuteRouteCPanelOverlay(req, logger);
}

}  // namespace routeb
