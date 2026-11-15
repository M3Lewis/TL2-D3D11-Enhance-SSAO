#include "config_panel.h"

#include "phase3_scene_pipeline.h"
#include "route_c_ao.h"
#include "route_c_d3d11.h"

#include <d3d11.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace routeb {
namespace {

constexpr uint32_t kPanelWidth = 448;
constexpr uint32_t kPanelHeight = 544;
constexpr float kPanelMargin = 16.0f;
constexpr int kTitleBarHeight = 42;
constexpr int kPaddingX = 16;

enum class HitTargetKind {
    None = 0,
    TitleDrag,
    CloseButton,
    TogglePhase3,
    ToggleAfPoint,
    ToggleP0RimLit,
    ToggleAo,
    ToggleP0Sscs,
    FilterOff,
    FilterHob,
    FilterDark,
    BuiltinFullHd,
    BuiltinDarkDungeon,
    BuiltinVanilla,
    CustomLoad0,
    CustomLoad1,
    CustomLoad2,
    CustomLoad3,
    CustomSave0,
    CustomSave1,
    CustomSave2,
    CustomSave3,
};

struct HitZone {
    RECT rect{};
    HitTargetKind kind = HitTargetKind::None;
    const wchar_t* label = L"";
};

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

    int hover_index = -1;
    POINT cursor_panel{-1, -1};
    bool cursor_inside = false;

    std::array<PresetSlot, kCustomPresetCount> presets{};
    int active_custom_slot = -1;
    std::wstring ini_path;
    std::wstring status_text = L"按 F8 / ESC 关闭  |  左键拖动标题栏";

    HWND hwnd = nullptr;
    WNDPROC orig_wndproc = nullptr;
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

int ClampFilterPreset(int v) {
    if (v < 0 || v > 2) return 1;
    return v;
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

void SaveVisualConfigToSection(const wchar_t* section, const VisualConfig& cfg, const wchar_t* path) {
    WriteIniInt(section, L"Phase3Enhance", cfg.phase3_enhance ? 1 : 0, path);
    WriteIniInt(section, L"AfPointEnhance", cfg.af_point_enhance ? 1 : 0, path);
    WriteIniInt(section, L"P0RimLitEnhance", cfg.p0_rimlit_enhance ? 1 : 0, path);
    WriteIniInt(section, L"AoEnabled", cfg.ao_enabled ? 1 : 0, path);
    WriteIniInt(section, L"P0SscsEnabled", cfg.p0_sscs_enabled ? 1 : 0, path);
    WriteIniInt(section, L"P2FilterPreset", ClampFilterPreset(cfg.p2_filter_preset), path);
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
    out.p2_filter_preset = ClampFilterPreset(
        ReadIniInt(section, L"P2FilterPreset", fallback.p2_filter_preset, path));
    return out;
}

bool SaveConfigFileLocked(const wchar_t* path_override) {
    const std::wstring target =
        (path_override && path_override[0]) ? std::wstring(path_override)
                                            : (g_panel.ini_path.empty() ? ResolveDefaultConfigIniPath()
                                                                        : g_panel.ini_path);
    g_panel.ini_path = target;
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

std::wstring FormatPresetSummary(const VisualConfig& cfg) {
    const wchar_t* filter_str = L"无滤镜";
    if (cfg.p2_filter_preset == 1) filter_str = L"Hob";
    else if (cfg.p2_filter_preset == 2) filter_str = L"暗黑";

    int on_count = 0;
    if (cfg.phase3_enhance) ++on_count;
    if (cfg.af_point_enhance) ++on_count;
    if (cfg.p0_rimlit_enhance) ++on_count;
    if (cfg.ao_enabled) ++on_count;
    if (cfg.p0_sscs_enabled) ++on_count;

    wchar_t buf[64] = {};
    swprintf_s(buf, L"%d/5开 · %ls", on_count, filter_str);
    return buf;
}

std::array<HitZone, 21> BuildHitZones() {
    std::array<HitZone, 21> zones{};
    size_t idx = 0;

    // 0: Close [X] button (must be tested before TitleDrag!)
    zones[idx++] = {RECT{static_cast<LONG>(kPanelWidth - 38), 7,
                         static_cast<LONG>(kPanelWidth - 10), 35},
                    HitTargetKind::CloseButton, L"×"};
    // 1: Title bar drag region
    zones[idx++] = {RECT{0, 0, static_cast<LONG>(kPanelWidth), kTitleBarHeight},
                    HitTargetKind::TitleDrag, L""};

    // 2..6: 5 Feature checkboxes
    const struct {
        HitTargetKind kind;
        const wchar_t* label;
    } kToggles[5] = {
        {HitTargetKind::TogglePhase3, L"Phase 3 材质法线与边缘光总开关"},
        {HitTargetKind::ToggleAfPoint, L"16x 各向异性过滤 + 动态点光增强"},
        {HitTargetKind::ToggleP0RimLit, L"P0-1 RimLit 微法线 / 金属高光 / _GLO 自发光"},
        {HitTargetKind::ToggleAo, L"Route C 屏幕空间环境光遮蔽 (SSAO)"},
        {HitTargetKind::ToggleP0Sscs, L"P0-2 全分辨率接触阴影 (SSCS) + 冷色调曲线"},
    };
    int y = 72;
    for (const auto& t : kToggles) {
        zones[idx++] = {RECT{kPaddingX, y, static_cast<LONG>(kPanelWidth - kPaddingX), y + 30},
                        t.kind, t.label};
        y += 33;
    }

    // 7..9: P2 Filter pills
    y = 264;
    const int pill_w = 132;
    const int pill_gap = 10;
    zones[idx++] = {RECT{kPaddingX, y, kPaddingX + pill_w, y + 30},
                    HitTargetKind::FilterOff, L"关闭滤镜"};
    zones[idx++] = {RECT{kPaddingX + (pill_w + pill_gap), y,
                         kPaddingX + (pill_w + pill_gap) + pill_w, y + 30},
                    HitTargetKind::FilterHob, L"Hob 明快"};
    zones[idx++] = {RECT{kPaddingX + 2 * (pill_w + pill_gap), y,
                         kPaddingX + 2 * (pill_w + pill_gap) + pill_w, y + 30},
                    HitTargetKind::FilterDark, L"暗黑胶片"};

    // 10..12: Built-in presets
    y = 324;
    zones[idx++] = {RECT{kPaddingX, y, kPaddingX + pill_w, y + 30},
                    HitTargetKind::BuiltinFullHd, L"全开高清"};
    zones[idx++] = {RECT{kPaddingX + (pill_w + pill_gap), y,
                         kPaddingX + (pill_w + pill_gap) + pill_w, y + 30},
                    HitTargetKind::BuiltinDarkDungeon, L"暗黑地牢"};
    zones[idx++] = {RECT{kPaddingX + 2 * (pill_w + pill_gap), y,
                         kPaddingX + 2 * (pill_w + pill_gap) + pill_w, y + 30},
                    HitTargetKind::BuiltinVanilla, L"纯原版"};

    // 13..20: 4 Custom preset slots (Load + Save)
    y = 384;
    const HitTargetKind kLoadKinds[4] = {
        HitTargetKind::CustomLoad0, HitTargetKind::CustomLoad1,
        HitTargetKind::CustomLoad2, HitTargetKind::CustomLoad3};
    const HitTargetKind kSaveKinds[4] = {
        HitTargetKind::CustomSave0, HitTargetKind::CustomSave1,
        HitTargetKind::CustomSave2, HitTargetKind::CustomSave3};
    for (int i = 0; i < 4; ++i) {
        zones[idx++] = {RECT{kPaddingX, y, static_cast<LONG>(kPanelWidth - kPaddingX - 96), y + 29},
                        kLoadKinds[i], L""};
        zones[idx++] = {RECT{static_cast<LONG>(kPanelWidth - kPaddingX - 88), y,
                             static_cast<LONG>(kPanelWidth - kPaddingX), y + 29},
                        kSaveKinds[i], L"保存当前"};
        y += 32;
    }

    return zones;
}

const std::array<HitZone, 21>& GetHitZones() {
    static const std::array<HitZone, 21> kZones = BuildHitZones();
    return kZones;
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

    DrawTextBox(dc, g_panel.font_title, RGB(238, 245, 255),
                RECT{kPaddingX, 0, static_cast<LONG>(kPanelWidth - 46), kTitleBarHeight},
                L"TL2 DX11 画质增强面板  [F8]", DT_LEFT | DT_VCENTER);

    // Close [X] button
    {
        const bool hovered = (g_panel.hover_index == 0);
        DrawBorderRect(dc, zones[0].rect,
                       hovered ? RGB(176, 52, 62) : RGB(44, 52, 68),
                       hovered ? RGB(235, 110, 120) : RGB(78, 92, 118), 5);
        DrawTextBox(dc, g_panel.font_title, RGB(240, 244, 250), zones[0].rect, L"×",
                    DT_CENTER | DT_VCENTER);
    }

    // Section 1: 5 Feature toggles
    DrawSectionHeader(dc, 50, L"实时渲染与 P0 画质开关");
    const bool toggle_states[5] = {
        cfg.phase3_enhance,
        cfg.af_point_enhance,
        cfg.p0_rimlit_enhance,
        cfg.ao_enabled,
        cfg.p0_sscs_enabled,
    };
    for (int i = 0; i < 5; ++i) {
        const size_t z_idx = static_cast<size_t>(2 + i);
        const RECT& r = zones[z_idx].rect;
        const bool hovered = (g_panel.hover_index == static_cast<int>(z_idx));
        const bool on = toggle_states[i];

        const COLORREF row_fill = hovered ? RGB(34, 43, 61) : RGB(24, 30, 42);
        const COLORREF row_border = hovered ? RGB(92, 152, 226)
                                            : (on ? RGB(48, 86, 118) : RGB(42, 49, 64));
        DrawBorderRect(dc, r, row_fill, row_border, 6);

        // Checkbox square
        const RECT box{r.left + 8, r.top + 6, r.left + 26, r.top + 24};
        DrawBorderRect(dc, box,
                       on ? RGB(42, 168, 116) : RGB(20, 24, 34),
                       on ? RGB(105, 232, 178) : RGB(96, 110, 136), 4);
        if (on) {
            DrawTextBox(dc, g_panel.font_section, RGB(245, 255, 250), box, L"✓",
                        DT_CENTER | DT_VCENTER);
        }

        // Label + right status tag
        RECT text_r{r.left + 35, r.top, r.right - 46, r.bottom};
        DrawTextBox(dc, g_panel.font_body,
                    on ? RGB(232, 240, 250) : RGB(148, 158, 176),
                    text_r, zones[z_idx].label, DT_LEFT | DT_VCENTER);

        RECT tag_r{r.right - 44, r.top, r.right - 8, r.bottom};
        DrawTextBox(dc, g_panel.font_small,
                    on ? RGB(102, 232, 170) : RGB(128, 134, 150),
                    tag_r, on ? L"开启" : L"关闭", DT_RIGHT | DT_VCENTER);
    }

    // Section 2: P2 Filter pills
    DrawSectionHeader(dc, 242, L"P2 画面风格滤镜");
    for (int i = 0; i < 3; ++i) {
        const size_t z_idx = static_cast<size_t>(7 + i);
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
                    r, zones[z_idx].label, DT_CENTER | DT_VCENTER);
    }

    // Section 3: Built-in presets
    DrawSectionHeader(dc, 302, L"内置预设 (一键切换)");
    for (int i = 0; i < 3; ++i) {
        const size_t z_idx = static_cast<size_t>(10 + i);
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
                    r, zones[z_idx].label, DT_CENTER | DT_VCENTER);
    }

    // Section 4: 4 Custom Presets
    DrawSectionHeader(dc, 362, L"自定义预设 (保存当前配置并快速切换)");
    for (int i = 0; i < kCustomPresetCount; ++i) {
        const size_t load_idx = static_cast<size_t>(13 + i * 2);
        const size_t save_idx = load_idx + 1;
        const PresetSlot& slot = g_panel.presets[static_cast<size_t>(i)];

        const bool load_hover = (g_panel.hover_index == static_cast<int>(load_idx));
        const bool load_active = slot.used && (cfg == slot.cfg) && (g_panel.active_custom_slot == i);
        const COLORREF load_fill = load_active ? RGB(42, 102, 168)
                                               : (load_hover ? RGB(36, 46, 66) : RGB(24, 30, 42));
        const COLORREF load_border = load_active ? RGB(114, 192, 255)
                                                 : (load_hover ? RGB(92, 148, 218)
                                                               : (slot.used ? RGB(58, 76, 104) : RGB(42, 48, 62)));
        DrawBorderRect(dc, zones[load_idx].rect, load_fill, load_border, 6);

        wchar_t slot_label[96] = {};
        if (slot.used) {
            const std::wstring summary = FormatPresetSummary(slot.cfg);
            swprintf_s(slot_label, L"预设 %d  [%ls]%ls", i + 1, summary.c_str(),
                       (cfg == slot.cfg) ? L" ★" : L"");
        } else {
            swprintf_s(slot_label, L"预设 %d  (空槽位)", i + 1);
        }
        RECT label_r = zones[load_idx].rect;
        label_r.left += 10;
        label_r.right -= 8;
        DrawTextBox(dc, slot.used ? g_panel.font_body : g_panel.font_small,
                    slot.used ? RGB(232, 240, 252) : RGB(128, 138, 156),
                    label_r, slot_label, DT_LEFT | DT_VCENTER);

        const bool save_hover = (g_panel.hover_index == static_cast<int>(save_idx));
        DrawBorderRect(dc, zones[save_idx].rect,
                       save_hover ? RGB(60, 120, 88) : RGB(32, 42, 58),
                       save_hover ? RGB(116, 224, 164) : RGB(66, 82, 108), 6);
        DrawTextBox(dc, g_panel.font_small, RGB(214, 242, 224), zones[save_idx].rect, L"保存当前",
                    DT_CENTER | DT_VCENTER);
    }

    // Bottom footer bar
    FillSolidRect(dc, RECT{0, static_cast<LONG>(kPanelHeight - 30),
                           static_cast<LONG>(kPanelWidth), static_cast<LONG>(kPanelHeight)},
                  RGB(14, 18, 26));
    DrawTextBox(dc, g_panel.font_small, RGB(154, 186, 224),
                RECT{kPaddingX, static_cast<LONG>(kPanelHeight - 28),
                     static_cast<LONG>(kPanelWidth - kPaddingX), static_cast<LONG>(kPanelHeight - 2)},
                g_panel.status_text.c_str(), DT_LEFT | DT_VCENTER);

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
            return;
        case HitTargetKind::TogglePhase3: {
            const bool next = TogglePhase3RuntimeEnhance();
            g_panel.active_custom_slot = -1;
            g_panel.status_text = next ? L"已开启: Phase 3 材质/边缘光" : L"已关闭: Phase 3 (仅基础场景)";
            break;
        }
        case HitTargetKind::ToggleAfPoint: {
            const bool next = ToggleF11VisualEnhance();
            g_panel.active_custom_slot = -1;
            g_panel.status_text = next ? L"已开启: 16x AF + 点光增强" : L"已关闭: 16x AF + 点光增强";
            break;
        }
        case HitTargetKind::ToggleP0RimLit: {
            const bool next = ToggleP0RimLitEnhance();
            g_panel.active_custom_slot = -1;
            g_panel.status_text = next ? L"已开启: P0-1 RimLit / _GLO" : L"已关闭: P0-1 RimLit 增强";
            break;
        }
        case HitTargetKind::ToggleAo: {
            const bool next = ToggleRouteCAoRuntimeEnabled();
            g_panel.active_custom_slot = -1;
            g_panel.status_text = next ? L"已开启: Route C SSAO" : L"已关闭: Route C SSAO";
            break;
        }
        case HitTargetKind::ToggleP0Sscs: {
            const bool next = !IsRouteCSscsRuntimeEnabled();
            SetRouteCSscsRuntimeEnabled(next);
            g_panel.active_custom_slot = -1;
            g_panel.status_text = next ? L"已开启: P0-2 SSCS + 冷色调" : L"已关闭: P0-2 SSCS + 冷色调";
            break;
        }
        case HitTargetKind::FilterOff:
            SetP2FilterPreset(0);
            g_panel.active_custom_slot = -1;
            g_panel.status_text = L"P2 滤镜已设为: 关闭";
            break;
        case HitTargetKind::FilterHob:
            SetP2FilterPreset(1);
            g_panel.active_custom_slot = -1;
            g_panel.status_text = L"P2 滤镜已设为: Hob 明快";
            break;
        case HitTargetKind::FilterDark:
            SetP2FilterPreset(2);
            g_panel.active_custom_slot = -1;
            g_panel.status_text = L"P2 滤镜已设为: 暗黑胶片";
            break;
        case HitTargetKind::BuiltinFullHd:
            ApplyVisualConfigToEngine(MakeBuiltinPreset(0));
            g_panel.active_custom_slot = -1;
            g_panel.status_text = L"已应用内置预设: 全开高清";
            break;
        case HitTargetKind::BuiltinDarkDungeon:
            ApplyVisualConfigToEngine(MakeBuiltinPreset(1));
            g_panel.active_custom_slot = -1;
            g_panel.status_text = L"已应用内置预设: 暗黑地牢";
            break;
        case HitTargetKind::BuiltinVanilla:
            ApplyVisualConfigToEngine(MakeBuiltinPreset(2));
            g_panel.active_custom_slot = -1;
            g_panel.status_text = L"已应用内置预设: 纯原版";
            break;
        case HitTargetKind::CustomLoad0:
        case HitTargetKind::CustomLoad1:
        case HitTargetKind::CustomLoad2:
        case HitTargetKind::CustomLoad3: {
            const int slot = static_cast<int>(kind) - static_cast<int>(HitTargetKind::CustomLoad0);
            if (g_panel.presets[static_cast<size_t>(slot)].used) {
                ApplyVisualConfigToEngine(g_panel.presets[static_cast<size_t>(slot)].cfg);
                g_panel.active_custom_slot = slot;
                wchar_t msg[64] = {};
                swprintf_s(msg, L"已切换至自定义预设 %d", slot + 1);
                g_panel.status_text = msg;
            } else {
                wchar_t msg[64] = {};
                swprintf_s(msg, L"预设 %d 为空，请先点击右侧 [保存当前]", slot + 1);
                g_panel.status_text = msg;
                g_panel.dirty = true;
                return;
            }
            break;
        }
        case HitTargetKind::CustomSave0:
        case HitTargetKind::CustomSave1:
        case HitTargetKind::CustomSave2:
        case HitTargetKind::CustomSave3: {
            const int slot = static_cast<int>(kind) - static_cast<int>(HitTargetKind::CustomSave0);
            g_panel.presets[static_cast<size_t>(slot)].used = true;
            g_panel.presets[static_cast<size_t>(slot)].cfg = CaptureVisualConfigFromEngine();
            g_panel.active_custom_slot = slot;
            wchar_t msg[64] = {};
            swprintf_s(msg, L"已保存当前配置至 预设 %d", slot + 1);
            g_panel.status_text = msg;
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

LRESULT CALLBACK PanelHookWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    WNDPROC orig = nullptr;
    bool swallow = false;
    bool need_release_capture = false;
    bool need_set_capture = false;

    {
        std::lock_guard<std::mutex> lock(g_panel.mutex);
        orig = g_panel.orig_wndproc;

        if (msg == WM_NCDESTROY) {
            g_panel.hwnd = nullptr;
            g_panel.orig_wndproc = nullptr;
            g_panel.dragging = false;
            g_wndproc_attached.store(false, std::memory_order_release);
        } else if (msg == WM_CAPTURECHANGED && g_panel.dragging) {
            g_panel.dragging = false;
        } else if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && wParam == VK_F8) {
            if ((lParam & (1LL << 30)) == 0) {
                g_panel.open = !g_panel.open;
                g_panel.dirty = true;
                if (!g_panel.open) {
                    g_panel.dragging = false;
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
                        if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
                            if (new_hover >= 0) {
                                const HitTargetKind kind =
                                    GetHitZones()[static_cast<size_t>(new_hover)].kind;
                                if (kind == HitTargetKind::TitleDrag) {
                                    g_panel.dragging = true;
                                    g_panel.drag_offset_x = local_x;
                                    g_panel.drag_offset_y = local_y;
                                    need_set_capture = true;
                                } else {
                                    HandleHitClickLocked(new_hover);
                                    if (!g_panel.open) {
                                        need_release_capture = (::GetCapture() == hwnd);
                                    }
                                }
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

    if (swallow) {
        return 0;
    }
    if (orig) {
        return ::CallWindowProcW(orig, hwnd, msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

VisualConfig CaptureVisualConfigFromEngine() {
    VisualConfig cfg;
    cfg.phase3_enhance = IsPhase3RuntimeEnhanceEnabled();
    cfg.af_point_enhance = IsF11VisualEnhanceEnabled();
    cfg.p0_rimlit_enhance = IsP0RimLitEnhanceEnabled();
    cfg.ao_enabled = IsRouteCAoRuntimeEnabled();
    cfg.p0_sscs_enabled = IsRouteCSscsRuntimeEnabled();
    cfg.p2_filter_preset = ClampFilterPreset(GetP2FilterPreset());
    return cfg;
}

void ApplyVisualConfigToEngine(const VisualConfig& cfg) {
    SetPhase3RuntimeEnhanceEnabled(cfg.phase3_enhance);
    SetF11VisualEnhanceEnabled(cfg.af_point_enhance);
    SetP0RimLitEnhanceEnabled(cfg.p0_rimlit_enhance);
    SetRouteCAoRuntimeEnabled(cfg.ao_enabled);
    SetRouteCSscsRuntimeEnabled(cfg.p0_sscs_enabled);
    SetP2FilterPreset(ClampFilterPreset(cfg.p2_filter_preset));
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
    const DWORD attrs = ::GetFileAttributesW(target.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        ApplyVisualConfigToEngine(defaults);
        g_panel.presets = {};
        g_panel.active_custom_slot = -1;
        g_panel.dirty = true;
        return SaveConfigFileLocked(target.c_str());
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
            cfg.p2_filter_preset = 1;
            break;
        case 1:  // 暗黑地牢
            cfg.phase3_enhance = true;
            cfg.af_point_enhance = true;
            cfg.p0_rimlit_enhance = true;
            cfg.ao_enabled = true;
            cfg.p0_sscs_enabled = true;
            cfg.p2_filter_preset = 2;
            break;
        case 2:  // 纯原版
        default:
            cfg.phase3_enhance = false;
            cfg.af_point_enhance = false;
            cfg.p0_rimlit_enhance = false;
            cfg.ao_enabled = false;
            cfg.p0_sscs_enabled = false;
            cfg.p2_filter_preset = 0;
            break;
    }
    return cfg;
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
    g_panel.hover_index = -1;
    g_panel.presets = {};
    g_panel.active_custom_slot = -1;
    g_panel.ini_path.clear();
    g_panel.status_text = L"按 F8 / ESC 关闭  |  左键拖动标题栏";
    ApplyVisualConfigToEngine(VisualConfig{});
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
    std::lock_guard<std::mutex> lock(g_panel.mutex);
    if (g_panel.hwnd == hwnd && g_panel.orig_wndproc != nullptr) {
        return true;
    }
    if (g_panel.hwnd && g_panel.orig_wndproc && ::IsWindow(g_panel.hwnd)) {
        ::SetWindowLongPtrW(g_panel.hwnd, GWLP_WNDPROC,
                            reinterpret_cast<LONG_PTR>(g_panel.orig_wndproc));
    }
    const LONG_PTR prev = ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                              reinterpret_cast<LONG_PTR>(&PanelHookWndProc));
    if (prev == 0 && ::GetLastError() != 0) {
        return false;
    }
    g_panel.hwnd = hwnd;
    g_panel.orig_wndproc = reinterpret_cast<WNDPROC>(prev);
    g_wndproc_attached.store(true, std::memory_order_release);
    return true;
}

void DetachPanelWindow() {
    HWND hwnd = nullptr;
    WNDPROC orig = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_panel.mutex);
        hwnd = g_panel.hwnd;
        orig = g_panel.orig_wndproc;
        g_panel.hwnd = nullptr;
        g_panel.orig_wndproc = nullptr;
        g_panel.dragging = false;
        g_wndproc_attached.store(false, std::memory_order_release);
    }
    if (hwnd && orig && ::IsWindow(hwnd)) {
        if (::GetCapture() == hwnd) {
            ::ReleaseCapture();
        }
        ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
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
