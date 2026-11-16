#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct PayloadEntry {
    int resource_id;
    const wchar_t* rel_dest_path;  // relative to game root
    bool is_vanilla_overwrite;     // backup before overwrite, restore on uninstall
};

#include "installer_payload_table.inc"

struct ExpectedPeModule {
    const wchar_t* filename;
    uint32_t expected_timestamp;
    uint32_t expected_image_size;
};

constexpr ExpectedPeModule kRequiredPeModules[] = {
    {L"OgreMain.dll", 0x6af5faa0U, 0x006b6000U},
    {L"RenderSystem_Direct3D11.dll", 0x6af5fabfU, 0x0008a000U},
    {L"OgreGUIRenderer.dll", 0x6af5fab0U, 0x00014000U},
};

constexpr wchar_t kBackupDirName[] = L"TL2_DX11_Enhance_Backup";
constexpr wchar_t kGithubUrl[] = L"https://github.com/M3Lewis";

constexpr int kCtrlPathEdit = 1001;
constexpr int kCtrlBrowseBtn = 1002;
constexpr int kCtrlCheckBtn = 1003;
constexpr int kCtrlInstallBtn = 1004;
constexpr int kCtrlUninstallBtn = 1005;
constexpr int kCtrlLogEdit = 1006;
constexpr int kCtrlLangZhBtn = 1007;
constexpr int kCtrlLangEnBtn = 1008;

constexpr int kUiResLogo = 201;
constexpr int kUiResMapBg = 202;
constexpr int kUiResMenuFrame = 203;
constexpr int kUiResCornerTl = 204;
constexpr int kUiResCornerTr = 205;
constexpr int kUiResCornerBl = 206;
constexpr int kUiResCornerBr = 207;

constexpr int kBaseWidth = 760;
constexpr int kBaseHeight = 560;

struct AppUi {
    HWND main_wnd = nullptr;
    HWND path_edit = nullptr;
    HWND browse_btn = nullptr;
    HWND check_btn = nullptr;
    HWND install_btn = nullptr;
    HWND uninstall_btn = nullptr;
    HWND lang_zh_btn = nullptr;
    HWND lang_en_btn = nullptr;
    HWND log_edit = nullptr;
    HWND hovered_btn = nullptr;
    HFONT ui_font = nullptr;
    HFONT log_font = nullptr;
    HFONT btn_font = nullptr;
    HBRUSH edit_bg_brush = nullptr;
    HBRUSH log_bg_brush = nullptr;
    HBITMAP bg_bmp = nullptr;
    RECT github_rc{};
    float github_x = 0.0f;
    float github_y = 0.0f;
    bool github_hovered = false;
    bool english = false;
    int client_w = kBaseWidth;
    int client_h = kBaseHeight;
    int dpi = 96;
};

AppUi g_ui{};

bool DetectDefaultEnglish() {
    const LANGID lang = ::GetUserDefaultUILanguage();
    return PRIMARYLANGID(lang) != LANG_CHINESE;
}

const wchar_t* T(const wchar_t* zh, const wchar_t* en) {
    return g_ui.english ? en : zh;
}

class Logger {
public:
    explicit Logger(bool cli_mode) : cli_mode_(cli_mode) {}

    void Append(const std::wstring& line) {
        AppendBi(line, line);
    }

    void AppendBi(const std::wstring& zh, const std::wstring& en) {
        entries_.push_back({zh, en});
        const std::wstring& active = g_ui.english ? en : zh;
        if (cli_mode_) {
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, active.c_str(), static_cast<int>(active.size()), nullptr, 0, nullptr, nullptr);
            if (n > 0) {
                std::string u8(static_cast<size_t>(n), '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, active.c_str(), static_cast<int>(active.size()), u8.data(), n, nullptr, nullptr);
                std::fwrite(u8.data(), 1, u8.size(), stdout);
                std::fputc('\n', stdout);
            }
        } else {
            SyncGuiEdit();
        }
    }

    void SyncGuiEdit() const {
        if (cli_mode_ || !g_ui.log_edit) {
            return;
        }
        std::wstring joined;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (i > 0) {
                joined += L"\r\n";
            }
            joined += g_ui.english ? entries_[i].second : entries_[i].first;
        }
        ::SetWindowTextW(g_ui.log_edit, joined.c_str());
        const int len = ::GetWindowTextLengthW(g_ui.log_edit);
        ::SendMessageW(g_ui.log_edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
        ::SendMessageW(g_ui.log_edit, EM_SCROLLCARET, 0, 0);
    }

    void Clear() {
        entries_.clear();
        if (!cli_mode_ && g_ui.log_edit) {
            ::SetWindowTextW(g_ui.log_edit, L"");
        }
    }

private:
    bool cli_mode_ = false;
    std::vector<std::pair<std::wstring, std::wstring>> entries_;
};

Logger g_gui_log(false);

std::wstring FormatWin32Error(DWORD err) {
    wchar_t buf[48]{};
    std::swprintf(buf, 48, T(L"Win32错误码 %lu", L"Win32 Error %lu"), static_cast<unsigned long>(err));
    return buf;
}

std::wstring FormatWin32ErrorPair(DWORD err, bool english) {
    wchar_t buf[48]{};
    std::swprintf(buf, 48, english ? L"Win32 Error %lu" : L"Win32错误码 %lu", static_cast<unsigned long>(err));
    return buf;
}

struct BiText {
    std::wstring zh;
    std::wstring en;
};

bool ReadPeSignature(const fs::path& file_path, uint32_t* out_timestamp, uint32_t* out_image_size, BiText* err_msg) {
    const std::wstring fname = file_path.filename().wstring();
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        if (err_msg) {
            *err_msg = {L"无法打开文件: " + file_path.wstring(),
                        L"Failed to open file: " + file_path.wstring()};
        }
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    ifs.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!ifs || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        if (err_msg) {
            *err_msg = {L"无效 PE DOS 头: " + fname,
                        L"Invalid PE DOS header: " + fname};
        }
        return false;
    }

    ifs.seekg(static_cast<std::streamoff>(dos.e_lfanew), std::ios::beg);
    uint32_t pe_sig = 0;
    IMAGE_FILE_HEADER file_hdr{};
    ifs.read(reinterpret_cast<char*>(&pe_sig), sizeof(pe_sig));
    ifs.read(reinterpret_cast<char*>(&file_hdr), sizeof(file_hdr));
    if (!ifs || pe_sig != IMAGE_NT_SIGNATURE) {
        if (err_msg) {
            *err_msg = {L"无效 PE NT 头: " + fname,
                        L"Invalid PE NT header: " + fname};
        }
        return false;
    }
    if (file_hdr.Machine != IMAGE_FILE_MACHINE_AMD64) {
        if (err_msg) {
            *err_msg = {fname + L" 不是 64 位 (x64) 模块，当前插件仅支持 2026 x64 D3D11 版火炬之光 2。",
                        fname + L" is not a 64-bit (x64) module. Only the 2026 x64 D3D11 Torchlight II release is supported."};
        }
        return false;
    }

    IMAGE_OPTIONAL_HEADER64 opt_hdr{};
    ifs.read(reinterpret_cast<char*>(&opt_hdr), sizeof(opt_hdr));
    if (!ifs || opt_hdr.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (err_msg) {
            *err_msg = {L"无效 PE64 OptionalHeader: " + fname,
                        L"Invalid PE64 OptionalHeader: " + fname};
        }
        return false;
    }

    *out_timestamp = file_hdr.TimeDateStamp;
    *out_image_size = opt_hdr.SizeOfImage;
    return true;
}

bool IsProcessRunningByName(const wchar_t* exe_name) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (::Process32FirstW(snap, &entry)) {
        do {
            if (::_wcsicmp(entry.szExeFile, exe_name) == 0) {
                found = true;
                break;
            }
        } while (::Process32NextW(snap, &entry));
    }
    ::CloseHandle(snap);
    return found;
}

void AppendVersionUpgradeHint(Logger& log) {
    log.AppendBi(
        L"[升级提示] 当前游戏不是 1.27 (2026 x64 D3D11) 版本，请先将游戏升级至 1.27 版本：",
        L"[Upgrade Required] Your Torchlight II build is not v1.27 (2026 x64 D3D11). Please update to v1.27:");
    log.AppendBi(
        L"          可在 Steam 库中右键游戏 →「属性」→「测试版」→ 选择 public_beta 分支进行更新。",
        L"          In Steam: Right-click Torchlight II -> Properties -> Betas -> select the public_beta branch.");
}

bool ValidateGameDirectory(const fs::path& game_dir, Logger& log, bool* out_version_mismatch = nullptr) {
    if (out_version_mismatch) {
        *out_version_mismatch = false;
    }
    std::error_code ec;
    if (game_dir.empty() || !fs::exists(game_dir, ec) || !fs::is_directory(game_dir, ec)) {
        log.AppendBi(L"[错误] 所选目录不存在: " + game_dir.wstring(),
                     L"[Error] Selected directory does not exist: " + game_dir.wstring());
        return false;
    }

    const fs::path exe_path = game_dir / L"Torchlight2.exe";
    if (!fs::exists(exe_path, ec)) {
        log.AppendBi(L"[错误] 未找到 Torchlight2.exe，请选择《火炬之光 2》游戏根目录。",
                     L"[Error] Torchlight2.exe not found. Please select the Torchlight II root directory.");
        return false;
    }

    for (const auto& mod : kRequiredPeModules) {
        const fs::path mod_path = game_dir / mod.filename;
        if (!fs::exists(mod_path, ec)) {
            log.AppendBi(
                std::wstring(L"[错误] 缺少 1.27 版引擎文件 ") + mod.filename +
                    L"（请确认游戏已升级至 1.27 版本，而非旧版 32 位 D3D9 客户端）。",
                std::wstring(L"[Error] Missing v1.27 engine file ") + mod.filename +
                    L" (please update to v1.27 rather than legacy 32-bit D3D9).");
            AppendVersionUpgradeHint(log);
            if (out_version_mismatch) {
                *out_version_mismatch = true;
            }
            return false;
        }
        uint32_t ts = 0;
        uint32_t img_sz = 0;
        BiText pe_err;
        if (!ReadPeSignature(mod_path, &ts, &img_sz, &pe_err)) {
            log.AppendBi(L"[错误] " + pe_err.zh, L"[Error] " + pe_err.en);
            AppendVersionUpgradeHint(log);
            if (out_version_mismatch) {
                *out_version_mismatch = true;
            }
            return false;
        }
        if (ts != mod.expected_timestamp || img_sz != mod.expected_image_size) {
            wchar_t msg_zh[256]{};
            wchar_t msg_en[256]{};
            std::swprintf(msg_zh, 256,
                          L"[错误] %s 不是 1.27 兼容版本 (TimeDateStamp=0x%08X 期望 0x%08X, SizeOfImage=0x%X 期望 0x%X)，为防崩溃已中止。",
                          mod.filename, ts, mod.expected_timestamp, img_sz, mod.expected_image_size);
            std::swprintf(msg_en, 256,
                          L"[Error] %s is not a compatible v1.27 build (TimeDateStamp=0x%08X expected 0x%08X, SizeOfImage=0x%X expected 0x%X); aborted.",
                          mod.filename, ts, mod.expected_timestamp, img_sz, mod.expected_image_size);
            log.AppendBi(msg_zh, msg_en);
            AppendVersionUpgradeHint(log);
            if (out_version_mismatch) {
                *out_version_mismatch = true;
            }
            return false;
        }
    }

    log.AppendBi(L"[检查通过] 已识别兼容的 1.27 (2026 x64 D3D11) 版《火炬之光 2》目录。",
                 L"[Check OK] Compatible Torchlight II v1.27 (2026 x64 D3D11) installation detected.");
    return true;
}

bool ExtractResourceToFile(int resource_id, const fs::path& dest_path, BiText* err_msg) {
    HRSRC res = ::FindResourceW(nullptr, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!res) {
        if (err_msg) {
            *err_msg = {L"找不到内嵌资源 ID=" + std::to_wstring(resource_id),
                        L"Embedded resource not found: ID=" + std::to_wstring(resource_id)};
        }
        return false;
    }
    HGLOBAL loaded = ::LoadResource(nullptr, res);
    if (!loaded) {
        const DWORD err = ::GetLastError();
        if (err_msg) {
            *err_msg = {L"加载内嵌资源失败: " + FormatWin32ErrorPair(err, false),
                        L"Failed to load embedded resource: " + FormatWin32ErrorPair(err, true)};
        }
        return false;
    }
    const DWORD size = ::SizeofResource(nullptr, res);
    const void* data = ::LockResource(loaded);
    if (!data || size == 0) {
        if (err_msg) {
            *err_msg = {L"锁定内嵌资源数据失败。", L"Failed to lock embedded resource data."};
        }
        return false;
    }

    std::error_code ec;
    fs::create_directories(dest_path.parent_path(), ec);
    if (ec) {
        if (err_msg) {
            *err_msg = {L"创建目标目录失败: " + dest_path.parent_path().wstring(),
                        L"Failed to create target folder: " + dest_path.parent_path().wstring()};
        }
        return false;
    }

    std::ofstream ofs(dest_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        if (err_msg) {
            *err_msg = {L"写入文件失败（请检查目录写入权限或关闭占用程序）: " + dest_path.wstring(),
                        L"Failed to write file (check permissions / file locks): " + dest_path.wstring()};
        }
        return false;
    }
    ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    ofs.flush();
    if (!ofs) {
        if (err_msg) {
            *err_msg = {L"磁盘写入未完成: " + dest_path.wstring(),
                        L"Incomplete disk write: " + dest_path.wstring()};
        }
        return false;
    }
    return true;
}

bool BackupFileOnce(const fs::path& game_dir, const fs::path& rel_path, Logger& log) {
    const fs::path src = game_dir / rel_path;
    const fs::path dst = game_dir / kBackupDirName / rel_path;
    std::error_code ec;
    if (!fs::exists(src, ec) || fs::exists(dst, ec)) {
        return true;
    }
    fs::create_directories(dst.parent_path(), ec);
    if (ec) {
        log.AppendBi(L"[错误] 创建备份目录失败: " + dst.parent_path().wstring(),
                     L"[Error] Failed to create backup folder: " + dst.parent_path().wstring());
        return false;
    }
    fs::copy_file(src, dst, fs::copy_options::none, ec);
    if (ec) {
        log.AppendBi(L"[错误] 备份原版文件失败: " + rel_path.wstring(),
                     L"[Error] Failed to back up vanilla file: " + rel_path.wstring());
        return false;
    }
    const std::wstring backed_rel = (fs::path(kBackupDirName) / rel_path).wstring();
    log.AppendBi(L"[备份] 已保留原版文件 -> " + backed_rel,
                 L"[Backup] Saved vanilla file -> " + backed_rel);
    return true;
}

std::string TrimAscii(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        return {};
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool UpdatePluginConfigFile(const fs::path& cfg_path, Logger& log) {
    std::vector<std::string> out_lines;
    bool has_plugin_folder = false;
    bool has_d3d11 = false;
    bool has_octree = false;
    bool has_enhance = false;

    std::error_code ec;
    if (fs::exists(cfg_path, ec)) {
        std::ifstream ifs(cfg_path, std::ios::binary);
        if (!ifs) {
            log.AppendBi(L"[错误] 无法读取配置文件: " + cfg_path.filename().wstring(),
                         L"[Error] Failed to read config file: " + cfg_path.filename().wstring());
            return false;
        }
        std::string line;
        while (std::getline(ifs, line)) {
            const std::string trimmed = TrimAscii(line);
            if (trimmed.rfind("PluginFolder=", 0) == 0) {
                has_plugin_folder = true;
                out_lines.push_back(trimmed);
                continue;
            }
            if (trimmed == "Plugin=RenderSystem_Direct3D9") {
                if (!has_d3d11) {
                    out_lines.push_back("Plugin=RenderSystem_Direct3D11");
                    has_d3d11 = true;
                }
                continue;
            }
            if (trimmed == "Plugin=Plugin_CgProgramManager") {
                out_lines.push_back("#Plugin=Plugin_CgProgramManager");
                continue;
            }
            if (trimmed == "Plugin=Plugin_TL2_RouteB") {
                if (!has_enhance) {
                    out_lines.push_back("Plugin=Plugin_TL2_DX11_Enhance_SSAO");
                    has_enhance = true;
                }
                continue;
            }
            if (trimmed == "Plugin=RenderSystem_Direct3D11") {
                if (!has_d3d11) {
                    out_lines.push_back(trimmed);
                    has_d3d11 = true;
                }
                continue;
            }
            if (trimmed == "Plugin=Plugin_OctreeSceneManager") {
                if (!has_octree) {
                    out_lines.push_back(trimmed);
                    has_octree = true;
                }
                continue;
            }
            if (trimmed == "Plugin=Plugin_TL2_DX11_Enhance_SSAO") {
                if (!has_enhance) {
                    out_lines.push_back(trimmed);
                    has_enhance = true;
                }
                continue;
            }
            out_lines.push_back(trimmed);
        }
    } else {
        out_lines.push_back("# Generated by TL2 DX11 Enhance + SSAO Installer");
    }

    if (!has_plugin_folder) {
        out_lines.insert(out_lines.begin(), "PluginFolder=.");
    }
    if (!has_d3d11) {
        out_lines.push_back("Plugin=RenderSystem_Direct3D11");
    }
    if (!has_octree) {
        out_lines.push_back("Plugin=Plugin_OctreeSceneManager");
    }
    if (!has_enhance) {
        out_lines.push_back("Plugin=Plugin_TL2_DX11_Enhance_SSAO");
    }

    std::ofstream ofs(cfg_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        log.AppendBi(L"[错误] 写入配置文件失败: " + cfg_path.filename().wstring(),
                     L"[Error] Failed to write config file: " + cfg_path.filename().wstring());
        return false;
    }
    for (const auto& l : out_lines) {
        ofs << l << "\r\n";
    }
    ofs.flush();
    if (!ofs) {
        log.AppendBi(L"[错误] 配置文件保存失败: " + cfg_path.filename().wstring(),
                     L"[Error] Failed to save config file: " + cfg_path.filename().wstring());
        return false;
    }
    log.AppendBi(L"[配置] 已更新 " + cfg_path.filename().wstring() + L" (D3D11 + Plugin_TL2_DX11_Enhance_SSAO)",
                 L"[Config] Updated " + cfg_path.filename().wstring() + L" (D3D11 + Plugin_TL2_DX11_Enhance_SSAO)");
    return true;
}

bool PerformInstall(const fs::path& game_dir, Logger& log, bool* out_version_mismatch = nullptr) {
    if (out_version_mismatch) {
        *out_version_mismatch = false;
    }
    if (IsProcessRunningByName(L"Torchlight2.exe")) {
        log.AppendBi(L"[错误] 检测到 Torchlight2.exe 正在运行，请先退出游戏再安装。",
                     L"[Error] Torchlight2.exe is currently running. Please exit the game first.");
        return false;
    }
    if (!ValidateGameDirectory(game_dir, log, out_version_mismatch)) {
        return false;
    }

    // Backup original configs before modifying.
    if (!BackupFileOnce(game_dir, L"Plugins.cfg", log) ||
        !BackupFileOnce(game_dir, L"Plugins_D3D11.cfg", log)) {
        return false;
    }

    // Rename legacy Plugin_TL2_RouteB.dll if present.
    std::error_code ec;
    const fs::path legacy_dll = game_dir / L"Plugin_TL2_RouteB.dll";
    if (fs::exists(legacy_dll, ec)) {
        const fs::path legacy_bak = game_dir / L"Plugin_TL2_RouteB.dll.bak";
        if (::MoveFileExW(legacy_dll.c_str(), legacy_bak.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            log.AppendBi(L"[清理] 已将旧版 Plugin_TL2_RouteB.dll 备份重命名为 .bak",
                         L"[Cleanup] Renamed legacy Plugin_TL2_RouteB.dll to .bak");
        }
    }

    for (const auto& item : kPayloadEntries) {
        const fs::path rel(item.rel_dest_path);
        if (item.is_vanilla_overwrite) {
            if (!BackupFileOnce(game_dir, rel, log)) {
                return false;
            }
        }
        BiText err_msg;
        if (!ExtractResourceToFile(item.resource_id, game_dir / rel, &err_msg)) {
            log.AppendBi(L"[错误] " + err_msg.zh, L"[Error] " + err_msg.en);
            return false;
        }
        log.AppendBi(L"[安装] " + rel.wstring(), L"[Install] " + rel.wstring());
    }

    if (!UpdatePluginConfigFile(game_dir / L"Plugins.cfg", log)) {
        return false;
    }
    if (fs::exists(game_dir / L"Plugins_D3D11.cfg", ec)) {
        if (!UpdatePluginConfigFile(game_dir / L"Plugins_D3D11.cfg", log)) {
            return false;
        }
    }

    log.Append(L"");
    log.AppendBi(L"=== 安装完成！===", L"=== Installation Complete! ===");
    log.AppendBi(
        L"游戏内热键：按 F8 弹出交互式配置面板 (支持中英切换、10项强度滑块、水面 SSR、3D LUT 及 10 组预设)。",
        L"In-Game Hotkey: Press F8 to open the live panel (ZH/EN toggle, 10 strength sliders, SSR, 3D LUT & 10 presets).");
    log.AppendBi(
        L"外部 3D LUT 用法：将自定义 3D .cube 文件改名为 TL2_CustomLUT.cube 放入游戏根目录（含 Torchlight2.exe），再在 F8 中选择「外部 .cube」（支持运行时热重载）。",
        L"External 3D LUT: Rename any 3D .cube file to TL2_CustomLUT.cube in your game root (next to Torchlight2.exe), then pick 'Custom .cube' in F8.");
    return true;
}

bool RestoreFromBackup(const fs::path& game_dir, const fs::path& rel_path, Logger& log) {
    const fs::path bak = game_dir / kBackupDirName / rel_path;
    const fs::path dst = game_dir / rel_path;
    std::error_code ec;
    if (!fs::exists(bak, ec)) {
        return true;
    }
    fs::create_directories(dst.parent_path(), ec);
    fs::copy_file(bak, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        log.AppendBi(L"[错误] 恢复备份失败: " + rel_path.wstring(),
                     L"[Error] Failed to restore backup: " + rel_path.wstring());
        return false;
    }
    log.AppendBi(L"[恢复] " + rel_path.wstring(), L"[Restore] " + rel_path.wstring());
    return true;
}

bool PerformUninstall(const fs::path& game_dir, Logger& log) {
    if (IsProcessRunningByName(L"Torchlight2.exe")) {
        log.AppendBi(L"[错误] 检测到 Torchlight2.exe 正在运行，请先退出游戏再卸载。",
                     L"[Error] Torchlight2.exe is currently running. Please exit the game first.");
        return false;
    }
    std::error_code ec;
    if (game_dir.empty() || !fs::exists(game_dir / L"Torchlight2.exe", ec)) {
        log.AppendBi(L"[错误] 所选目录中未找到 Torchlight2.exe。",
                     L"[Error] Torchlight2.exe not found in the selected directory.");
        return false;
    }

    const fs::path backup_root = game_dir / kBackupDirName;
    if (!fs::exists(backup_root, ec)) {
        log.AppendBi(
            L"[提示] 未找到备份目录 TL2_DX11_Enhance_Backup，将仅移除插件新增文件（不会修改缺失备份的配置）。",
            L"[Notice] Backup folder TL2_DX11_Enhance_Backup not found; only added files will be removed.");
    }

    bool ok = true;
    ok &= RestoreFromBackup(game_dir, L"Plugins.cfg", log);
    ok &= RestoreFromBackup(game_dir, L"Plugins_D3D11.cfg", log);

    for (const auto& item : kPayloadEntries) {
        const fs::path rel(item.rel_dest_path);
        if (item.is_vanilla_overwrite) {
            ok &= RestoreFromBackup(game_dir, rel, log);
        } else {
            const fs::path dst = game_dir / rel;
            if (fs::exists(dst, ec)) {
                if (!::DeleteFileW(dst.c_str())) {
                    const DWORD err = ::GetLastError();
                    log.AppendBi(
                        L"[错误] 移除文件失败: " + rel.wstring() + L" (" + FormatWin32ErrorPair(err, false) + L")",
                        L"[Error] Failed to remove file: " + rel.wstring() + L" (" + FormatWin32ErrorPair(err, true) + L")");
                    ok = false;
                } else {
                    log.AppendBi(L"[移除] " + rel.wstring(), L"[Remove] " + rel.wstring());
                }
            }
        }
    }

    if (ok) {
        log.Append(L"");
        log.AppendBi(L"=== 卸载与原版恢复完成！===", L"=== Uninstall & Vanilla Restore Complete! ===");
    }
    return ok;
}

std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* value_name) {
    HKEY hkey = nullptr;
    if (::RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &hkey) != ERROR_SUCCESS) {
        if (::RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_32KEY, &hkey) != ERROR_SUCCESS) {
            return {};
        }
    }
    wchar_t buf[1024]{};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    std::wstring result;
    if (::RegQueryValueExW(hkey, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        result = buf;
    }
    ::RegCloseKey(hkey);
    return result;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring UnescapeVdfPath(const std::string& raw) {
    std::string unescaped;
    unescaped.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\\') {
            unescaped.push_back('\\');
            ++i;
        } else {
            unescaped.push_back(raw[i]);
        }
    }
    return Utf8ToWide(unescaped);
}

std::wstring DetectTorchlight2Directory() {
    std::vector<fs::path> candidates;

    const std::wstring reg_app = ReadRegString(
        HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Steam App 200710", L"InstallLocation");
    if (!reg_app.empty()) {
        candidates.emplace_back(reg_app);
    }

    std::wstring steam_path = ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam_path.empty()) {
        steam_path = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    }
    if (!steam_path.empty()) {
        fs::path steam_root(steam_path);
        candidates.push_back(steam_root / L"steamapps" / L"common" / L"Torchlight II");
        std::ifstream vdf(steam_root / L"steamapps" / L"libraryfolders.vdf", std::ios::binary);
        std::string line;
        while (std::getline(vdf, line)) {
            const size_t key_pos = line.find("\"path\"");
            if (key_pos == std::string::npos) {
                continue;
            }
            const size_t q1 = line.find('"', key_pos + 6);
            const size_t q2 = (q1 != std::string::npos) ? line.find('"', q1 + 1) : std::string::npos;
            if (q1 != std::string::npos && q2 != std::string::npos && q2 > q1 + 1) {
                const fs::path lib_root(UnescapeVdfPath(line.substr(q1 + 1, q2 - q1 - 1)));
                candidates.push_back(lib_root / L"steamapps" / L"common" / L"Torchlight II");
                candidates.push_back(lib_root / L"steamapps" / L"Common" / L"Torchlight II");
            }
        }
    }

    const wchar_t* fallback_drives[] = {L"C", L"D", L"E", L"F", L"G", L"H"};
    for (const wchar_t* drv : fallback_drives) {
        candidates.push_back(fs::path(std::wstring(drv) + L":\\Steam\\steamapps\\common\\Torchlight II"));
        candidates.push_back(fs::path(std::wstring(drv) + L":\\SteamLibrary\\steamapps\\common\\Torchlight II"));
        candidates.push_back(fs::path(std::wstring(drv) + L":\\Games\\Torchlight II"));
    }

    std::error_code ec;
    for (const auto& c : candidates) {
        if (fs::exists(c / L"Torchlight2.exe", ec)) {
            return c.wstring();
        }
    }
    return {};
}

std::wstring BrowseForGameFolder(HWND owner, const std::wstring& initial_dir) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
        return {};
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(T(L"请选择《火炬之光 2》(Torchlight II) 安装根目录",
                    L"Select the Torchlight II Root Installation Folder"));

    if (!initial_dir.empty()) {
        IShellItem* init_item = nullptr;
        if (SUCCEEDED(::SHCreateItemFromParsingName(initial_dir.c_str(), nullptr, IID_PPV_ARGS(&init_item))) && init_item) {
            dlg->SetFolder(init_item);
            init_item->Release();
        }
    }

    std::wstring chosen;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* res_item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&res_item)) && res_item) {
            PWSTR raw = nullptr;
            if (SUCCEEDED(res_item->GetDisplayName(SIGDN_FILESYSPATH, &raw)) && raw) {
                chosen = raw;
                ::CoTaskMemFree(raw);
            }
            res_item->Release();
        }
    }
    dlg->Release();
    return chosen;
}

std::wstring GetPathFromEdit() {
    const int len = ::GetWindowTextLengthW(g_ui.path_edit);
    if (len <= 0) {
        return {};
    }
    std::wstring s(static_cast<size_t>(len) + 1, L'\0');
    const int got = ::GetWindowTextW(g_ui.path_edit, s.data(), len + 1);
    s.resize(static_cast<size_t>(got > 0 ? got : 0));
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\\' || s.back() == L'/')) {
        if (s.size() <= 3 && s.size() >= 2 && s[1] == L':') {
            break;
        }
        s.pop_back();
    }
    return s;
}

void ApplyFontRecursive(HWND wnd, HFONT font) {
    ::SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

int S(int px) {
    return ::MulDiv(px, g_ui.dpi, 96);
}

float Sf(float px) {
    return px * (static_cast<float>(g_ui.dpi) / 96.0f);
}

Gdiplus::Bitmap* LoadEmbeddedPngOrJpg(int resource_id) {
    HRSRC res = ::FindResourceW(nullptr, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!res) {
        return nullptr;
    }
    HGLOBAL loaded = ::LoadResource(nullptr, res);
    if (!loaded) {
        return nullptr;
    }
    const DWORD size = ::SizeofResource(nullptr, res);
    const void* data = ::LockResource(loaded);
    if (!data || size == 0) {
        return nullptr;
    }
    IStream* stream = ::SHCreateMemStream(static_cast<const BYTE*>(data), static_cast<UINT>(size));
    if (!stream) {
        return nullptr;
    }
    Gdiplus::Bitmap* bmp = Gdiplus::Bitmap::FromStream(stream, FALSE);
    stream->Release();
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) {
        delete bmp;
        return nullptr;
    }
    return bmp;
}

void DrawImageCover(
    Gdiplus::Graphics& g,
    Gdiplus::Image* img,
    const Gdiplus::RectF& dst,
    float alpha) {
    if (!img || img->GetWidth() == 0 || img->GetHeight() == 0) {
        return;
    }
    const float src_w = static_cast<float>(img->GetWidth());
    const float src_h = static_cast<float>(img->GetHeight());
    const float scale = (std::max)(dst.Width / src_w, dst.Height / src_h);
    const float crop_w = dst.Width / scale;
    const float crop_h = dst.Height / scale;
    const float crop_x = (src_w - crop_w) * 0.5f;
    const float crop_y = (src_h - crop_h) * 0.5f;

    Gdiplus::ImageAttributes ia;
    Gdiplus::ColorMatrix cm = {{
        {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, alpha, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
    }};
    ia.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
    g.DrawImage(img, dst, crop_x, crop_y, crop_w, crop_h, Gdiplus::UnitPixel, &ia);
}

void AddRoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    const float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void DrawTextWithShadow(
    Gdiplus::Graphics& g,
    const wchar_t* text,
    const Gdiplus::Font& font,
    const Gdiplus::RectF& rect,
    const Gdiplus::StringFormat& fmt,
    const Gdiplus::Color& fg,
    float shadow_offset_px) {
    if (shadow_offset_px > 0.0f) {
        Gdiplus::RectF sr = rect;
        sr.X += shadow_offset_px;
        sr.Y += shadow_offset_px;
        Gdiplus::SolidBrush shadow_br(Gdiplus::Color(220, 0, 0, 0));
        g.DrawString(text, -1, &font, sr, &fmt, &shadow_br);
    }
    Gdiplus::SolidBrush fg_br(fg);
    g.DrawString(text, -1, &font, rect, &fmt, &fg_br);
}

void BuildBackdropBitmap(HWND hwnd) {
    if (g_ui.bg_bmp) {
        ::DeleteObject(g_ui.bg_bmp);
        g_ui.bg_bmp = nullptr;
    }
    HDC screen_dc = ::GetDC(hwnd);
    if (!screen_dc) {
        return;
    }
    const int w = g_ui.client_w;
    const int h = g_ui.client_h;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = ::CreateDIBSection(screen_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        ::ReleaseDC(hwnd, screen_dc);
        return;
    }
    HDC mem_dc = ::CreateCompatibleDC(screen_dc);
    HGDIOBJ old_bmp = ::SelectObject(mem_dc, dib);

    {
        Gdiplus::Graphics g(mem_dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        const Gdiplus::RectF full_rect(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
        Gdiplus::SolidBrush obsidian(Gdiplus::Color(255, 8, 9, 14));
        g.FillRectangle(&obsidian, full_rect);

        // Background 1: Torchlight II Vilderan parchment map with moody dark vignette
        Gdiplus::Bitmap* map_bmp = LoadEmbeddedPngOrJpg(kUiResMapBg);
        if (map_bmp) {
            DrawImageCover(g, map_bmp, full_rect, 0.24f);
            delete map_bmp;
        }
        {
            Gdiplus::GraphicsPath vig_path;
            vig_path.AddEllipse(-w * 0.15f, -h * 0.15f, w * 1.30f, h * 1.30f);
            Gdiplus::PathGradientBrush vig(&vig_path);
            vig.SetCenterColor(Gdiplus::Color(40, 8, 9, 14));
            Gdiplus::Color edge_col(225, 4, 5, 8);
            int cnt = 1;
            vig.SetSurroundColors(&edge_col, &cnt);
            g.FillRectangle(&vig, full_rect);
        }

        // Main ornate card panel (matching comp.html .tl-card aesthetic)
        const Gdiplus::RectF card(Sf(16), Sf(14), Sf(728), Sf(532));
        {
            Gdiplus::SolidBrush card_bg(Gdiplus::Color(232, 13, 16, 24));
            g.FillRectangle(&card_bg, card);

            // Sample rich interior parchment/leather texture from Torchlight II menu_frame.png
            Gdiplus::Bitmap* frame_bmp = LoadEmbeddedPngOrJpg(kUiResMenuFrame);
            if (frame_bmp) {
                Gdiplus::ImageAttributes ia;
                Gdiplus::ColorMatrix cm = {{
                    {1.0f, 0.0f, 0.0f, 0.0f, 0.0f},
                    {0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
                    {0.0f, 0.0f, 0.0f, 0.22f, 0.0f},
                    {0.0f, 0.0f, 0.0f, 0.0f, 1.0f},
                }};
                ia.SetColorMatrix(&cm, Gdiplus::ColorMatrixFlagsDefault, Gdiplus::ColorAdjustTypeBitmap);
                g.DrawImage(frame_bmp, card, 28.0f, 70.0f, 454.0f, 490.0f, Gdiplus::UnitPixel, &ia);
                delete frame_bmp;
            }

            // Double bronze border + subtle amber inner glow outline
            Gdiplus::Pen outer_pen(Gdiplus::Color(255, 128, 96, 48), Sf(2.0f));
            g.DrawRectangle(&outer_pen, card);
            const Gdiplus::RectF inner_card(
                card.X + Sf(4), card.Y + Sf(4), card.Width - Sf(8), card.Height - Sf(8));
            Gdiplus::Pen inner_pen(Gdiplus::Color(85, 245, 166, 35), Sf(1.0f));
            g.DrawRectangle(&inner_pen, inner_card);
        }

        // Authentic Torchlight II corner metal brackets
        const float cs = Sf(30);
        auto draw_corner = [&](int res_id, float cx, float cy) {
            Gdiplus::Bitmap* cb = LoadEmbeddedPngOrJpg(res_id);
            if (cb) {
                g.DrawImage(cb, cx, cy, cs, cs);
                delete cb;
            }
        };
        draw_corner(kUiResCornerTl, Sf(14), Sf(12));
        draw_corner(kUiResCornerTr, Sf(746) - cs, Sf(12));
        draw_corner(kUiResCornerBl, Sf(14), Sf(548) - cs);
        draw_corner(kUiResCornerBr, Sf(746) - cs, Sf(548) - cs);

        // Header left: Official Torchlight II 3D emblem logo
        Gdiplus::Bitmap* logo_bmp = LoadEmbeddedPngOrJpg(kUiResLogo);
        if (logo_bmp) {
            const float lw = Sf(174);
            const float lh = Sf(61);
            // Soft amber aura behind logo
            Gdiplus::GraphicsPath glow_p;
            glow_p.AddEllipse(Sf(26), Sf(20), lw + Sf(20), lh + Sf(16));
            Gdiplus::PathGradientBrush glow_br(&glow_p);
            glow_br.SetCenterColor(Gdiplus::Color(68, 245, 166, 35));
            Gdiplus::Color g_end(0, 245, 166, 35);
            int cnt = 1;
            glow_br.SetSurroundColors(&g_end, &cnt);
            g.FillEllipse(&glow_br, Sf(26), Sf(20), lw + Sf(20), lh + Sf(16));

            Gdiplus::ImageAttributes logo_ia;
            logo_ia.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
            g.DrawImage(
                logo_bmp,
                Gdiplus::RectF(Sf(36), Sf(25), lw, lh),
                0.0f, 0.0f,
                static_cast<float>(logo_bmp->GetWidth()),
                static_cast<float>(logo_bmp->GetHeight()),
                Gdiplus::UnitPixel,
                &logo_ia);
            delete logo_bmp;
        }

        // Header right: Title & complete 2-line subtitle (auto-fit font so title never clips before [中]/[EN])
        Gdiplus::FontFamily ff(L"Microsoft YaHei UI");
        const wchar_t* main_title_text = T(
            L"D3D11 画质增强 & SSAO 安装器 (v1.27 x64)",
            L"D3D11 Graphics & SSAO Setup (v1.27 x64)");
        const Gdiplus::RectF title_rect(Sf(220), Sf(22), Sf(414), Sf(28));
        float chosen_title_px = 19.0f;
        {
            Gdiplus::StringFormat measure_fmt(Gdiplus::StringFormat::GenericTypographic());
            measure_fmt.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);
            for (const float cand : {19.0f, 17.5f, 16.5f, 15.5f, 14.5f}) {
                Gdiplus::Font test_f(&ff, Sf(cand), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                Gdiplus::RectF bounds;
                g.MeasureString(main_title_text, -1, &test_f, Gdiplus::PointF(0.0f, 0.0f), &measure_fmt, &bounds);
                chosen_title_px = cand;
                if (bounds.Width <= title_rect.Width - Sf(6.0f)) {
                    break;
                }
            }
        }
        Gdiplus::Font title_f(&ff, Sf(chosen_title_px), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font sub_f(&ff, Sf(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font chip_f(&ff, Sf(11.5f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font label_f(&ff, Sf(13.5f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font foot_f(&ff, Sf(11.5f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        Gdiplus::StringFormat fmt_left;
        fmt_left.SetAlignment(Gdiplus::StringAlignmentNear);
        fmt_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt_left.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);

        Gdiplus::StringFormat fmt_top_left;
        fmt_top_left.SetAlignment(Gdiplus::StringAlignmentNear);
        fmt_top_left.SetLineAlignment(Gdiplus::StringAlignmentNear);
        fmt_top_left.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);

        Gdiplus::StringFormat fmt_center;
        fmt_center.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt_center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt_center.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);

        Gdiplus::StringFormat fmt_right;
        fmt_right.SetAlignment(Gdiplus::StringAlignmentFar);
        fmt_right.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt_right.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);

        DrawTextWithShadow(
            g,
            main_title_text,
            title_f,
            title_rect,
            fmt_left,
            Gdiplus::Color(255, 255, 214, 130),
            Sf(2.0f));

        DrawTextWithShadow(
            g,
            T(L"内嵌 Plugin_TL2_DX11_Enhance_SSAO.dll 与全套 16 枚 HLSL4 着色器\n支持原版无损备份 · 游戏内按 [F8] 调出交互式画质配置面板",
              L"Bundles Plugin_TL2_DX11_Enhance_SSAO.dll + 16 HLSL4 Shaders\nLossless vanilla backup · Press [F8] in-game for live settings"),
            sub_f,
            Gdiplus::RectF(Sf(220), Sf(52), Sf(504), Sf(36)),
            fmt_top_left,
            Gdiplus::Color(255, 218, 208, 190),
            Sf(1.0f));

        // Feature chips bar (4 Torchlight-style dark amber pills)
        struct ChipSpec {
            float w;
            const wchar_t* label_zh;
            const wchar_t* label_en;
        };
        const ChipSpec chips[4] = {
            {180.0f, L"法线凹凸 / 金属高光 / SSCS", L"Normals / Specular / SSCS"},
            {134.0f, L"屏幕空间软粒子", L"Soft Particles"},
            {192.0f, L"水面 SSR · 透视 · 3D LUT", L"SSR · Aerial · 3D LUT"},
            {158.0f, L"F8 实时调节 · 10 槽位", L"F8 Live UI · 10 Slots"},
        };
        float cx = Sf(36);
        const float cy = Sf(96);
        const float ch = Sf(26);
        const float gap = Sf(8);
        for (const auto& chip : chips) {
            const float cw = Sf(chip.w);
            const Gdiplus::RectF cr(cx, cy, cw, ch);
            Gdiplus::GraphicsPath cp;
            AddRoundRectPath(cp, cr, Sf(4.0f));
            Gdiplus::SolidBrush c_fill(Gdiplus::Color(220, 18, 22, 32));
            Gdiplus::Pen c_pen(Gdiplus::Color(215, 128, 96, 48), Sf(1.0f));
            g.FillPath(&c_fill, &cp);
            g.DrawPath(&c_pen, &cp);
            DrawTextWithShadow(
                g, T(chip.label_zh, chip.label_en), chip_f, cr, fmt_center,
                Gdiplus::Color(255, 248, 198, 108), Sf(1.0f));
            cx += cw + gap;
        }

        // Bronze horizontal divider
        {
            Gdiplus::LinearGradientBrush div_br(
                Gdiplus::PointF(Sf(36), Sf(131)),
                Gdiplus::PointF(Sf(724), Sf(131)),
                Gdiplus::Color(20, 128, 96, 48),
                Gdiplus::Color(20, 128, 96, 48));
            Gdiplus::Color cols[3] = {
                Gdiplus::Color(25, 128, 96, 48),
                Gdiplus::Color(220, 214, 156, 68),
                Gdiplus::Color(25, 128, 96, 48),
            };
            float pos[3] = {0.0f, 0.5f, 1.0f};
            div_br.SetInterpolationColors(cols, pos, 3);
            Gdiplus::Pen div_pen(&div_br, Sf(1.2f));
            g.DrawLine(&div_pen, Sf(36), Sf(131), Sf(724), Sf(131));
        }

        // Directory section label & bronze border around path EDIT
        DrawTextWithShadow(
            g,
            T(L"游戏根目录（含 Torchlight2.exe）：",
              L"Game Root Directory (containing Torchlight2.exe):"),
            label_f,
            Gdiplus::RectF(Sf(36), Sf(140), Sf(500), Sf(22)),
            fmt_left,
            Gdiplus::Color(255, 232, 220, 196),
            Sf(1.0f));

        {
            const Gdiplus::RectF edit_box(Sf(36), Sf(166), Sf(580), Sf(32));
            Gdiplus::SolidBrush e_fill(Gdiplus::Color(255, 11, 14, 22));
            Gdiplus::Pen e_pen(Gdiplus::Color(255, 128, 96, 48), Sf(1.4f));
            g.FillRectangle(&e_fill, edit_box);
            g.DrawRectangle(&e_pen, edit_box);
        }

        // Log console bronze border around log EDIT
        {
            const Gdiplus::RectF log_box(Sf(36), Sf(264), Sf(688), Sf(222));
            Gdiplus::SolidBrush l_fill(Gdiplus::Color(255, 10, 13, 20));
            Gdiplus::Pen l_pen(Gdiplus::Color(255, 112, 84, 44), Sf(1.4f));
            g.FillRectangle(&l_fill, log_box);
            g.DrawRectangle(&l_pen, log_box);
        }

        // Row 1: backup & CLI hints
        DrawTextWithShadow(
            g,
            T(L"自动备份原版配置与着色器至 TL2_DX11_Enhance_Backup，随时可无损还原",
              L"Vanilla configs & shaders auto-backed up to TL2_DX11_Enhance_Backup"),
            foot_f,
            Gdiplus::RectF(Sf(36), Sf(493), Sf(450), Sf(18)),
            fmt_left,
            Gdiplus::Color(255, 162, 152, 136),
            Sf(1.0f));
        DrawTextWithShadow(
            g,
            T(L"命令行: --check / --install / --uninstall",
              L"CLI: --check / --install / --uninstall"),
            foot_f,
            Gdiplus::RectF(Sf(490), Sf(493), Sf(234), Sf(18)),
            fmt_right,
            Gdiplus::Color(255, 132, 124, 112),
            Sf(1.0f));

        // Hairline divider before author credit bar
        {
            Gdiplus::Pen hair_pen(Gdiplus::Color(90, 128, 96, 48), Sf(1.0f));
            g.DrawLine(&hair_pen, Sf(36), Sf(515), Sf(724), Sf(515));
        }

        // Row 2: Author credit & clickable GitHub link
        {
            Gdiplus::Font author_lbl_f(&ff, Sf(12.0f), Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
            Gdiplus::Font author_name_f(&ff, Sf(12.0f), Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::Font link_f(&ff, Sf(12.0f), Gdiplus::FontStyleUnderline, Gdiplus::UnitPixel);

            Gdiplus::StringFormat fmt_typo(Gdiplus::StringFormat::GenericTypographic());
            fmt_typo.SetFormatFlags(fmt_typo.GetFormatFlags() | Gdiplus::StringFormatFlagsMeasureTrailingSpaces);
            fmt_typo.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);

            const wchar_t* auth_prefix = T(L"作者: ", L"Author: ");
            const wchar_t* auth_name = L"M3Lewis";
            Gdiplus::RectF box_ap;
            Gdiplus::RectF box_an;
            g.MeasureString(auth_prefix, -1, &author_lbl_f, Gdiplus::PointF(0.0f, 0.0f), &fmt_typo, &box_ap);
            g.MeasureString(auth_name, -1, &author_name_f, Gdiplus::PointF(0.0f, 0.0f), &fmt_typo, &box_an);
            const float row2_y = Sf(519) + (Sf(22) - box_an.Height) * 0.5f;
            DrawTextWithShadow(
                g, auth_prefix, author_lbl_f,
                Gdiplus::RectF(Sf(36), row2_y, box_ap.Width + Sf(4), Sf(22)),
                fmt_typo, Gdiplus::Color(255, 190, 180, 162), Sf(1.0f));
            DrawTextWithShadow(
                g, auth_name, author_name_f,
                Gdiplus::RectF(Sf(36) + box_ap.Width, row2_y, box_an.Width + Sf(8), Sf(22)),
                fmt_typo, Gdiplus::Color(255, 255, 210, 125), Sf(1.0f));

            const wchar_t* gh_prefix = L"GitHub: ";
            const wchar_t* gh_text = L"github.com/M3Lewis";
            Gdiplus::RectF box_gp;
            Gdiplus::RectF box_gt;
            g.MeasureString(gh_prefix, -1, &author_lbl_f, Gdiplus::PointF(0.0f, 0.0f), &fmt_typo, &box_gp);
            g.MeasureString(gh_text, -1, &link_f, Gdiplus::PointF(0.0f, 0.0f), &fmt_typo, &box_gt);
            const float link_y = Sf(519) + (Sf(22) - box_gt.Height) * 0.5f;
            const float link_x = Sf(724) - box_gt.Width;
            const float pref_x = link_x - box_gp.Width;
            DrawTextWithShadow(
                g, gh_prefix, author_lbl_f,
                Gdiplus::RectF(pref_x, link_y, box_gp.Width + Sf(4), Sf(22)),
                fmt_typo, Gdiplus::Color(255, 190, 180, 162), Sf(1.0f));

            g_ui.github_x = link_x;
            g_ui.github_y = link_y;
            g_ui.github_rc = RECT{
                static_cast<LONG>(link_x - Sf(3)),
                static_cast<LONG>(link_y - Sf(2)),
                static_cast<LONG>(Sf(724) + Sf(3)),
                static_cast<LONG>(link_y + box_gt.Height + Sf(4))};
        }
    }

    ::SelectObject(mem_dc, old_bmp);
    ::DeleteDC(mem_dc);
    ::ReleaseDC(hwnd, screen_dc);
    g_ui.bg_bmp = dib;
}

void DrawOwnerButton(const DRAWITEMSTRUCT* dis) {
    if (!dis || !dis->hDC) {
        return;
    }
    const RECT rc = dis->rcItem;
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        return;
    }

    HDC mem_dc = ::CreateCompatibleDC(dis->hDC);
    HBITMAP mem_bmp = ::CreateCompatibleBitmap(dis->hDC, w, h);
    HGDIOBJ old_bmp = ::SelectObject(mem_dc, mem_bmp);

    {
        Gdiplus::Graphics g(mem_dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

        const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
        const bool hovered = (dis->hwndItem == g_ui.hovered_btn);
        const bool focused = (dis->itemState & ODS_FOCUS) != 0;

        // Clear outer pixels to main panel background so rounded corners stay crisp
        Gdiplus::SolidBrush outer_clear(Gdiplus::Color(255, 14, 17, 25));
        g.FillRectangle(&outer_clear, 0, 0, w, h);

        Gdiplus::Color top_col, bot_col, border_col, txt_col;
        if (dis->CtlID == kCtrlInstallBtn) {
            top_col = pressed ? Gdiplus::Color(255, 132, 72, 12)
                              : (hovered ? Gdiplus::Color(255, 212, 124, 24) : Gdiplus::Color(255, 184, 102, 18));
            bot_col = pressed ? Gdiplus::Color(255, 92, 48, 8)
                              : (hovered ? Gdiplus::Color(255, 134, 68, 12) : Gdiplus::Color(255, 112, 56, 10));
            border_col = hovered ? Gdiplus::Color(255, 255, 212, 110) : Gdiplus::Color(255, 245, 166, 35);
            txt_col = Gdiplus::Color(255, 255, 246, 224);
        } else if (dis->CtlID == kCtrlCheckBtn) {
            top_col = pressed ? Gdiplus::Color(255, 22, 34, 54)
                              : (hovered ? Gdiplus::Color(255, 38, 58, 92) : Gdiplus::Color(255, 28, 44, 70));
            bot_col = pressed ? Gdiplus::Color(255, 16, 24, 38)
                              : (hovered ? Gdiplus::Color(255, 24, 36, 58) : Gdiplus::Color(255, 18, 28, 46));
            border_col = hovered ? Gdiplus::Color(255, 116, 168, 232) : Gdiplus::Color(255, 78, 118, 168);
            txt_col = Gdiplus::Color(255, 220, 236, 255);
        } else if (dis->CtlID == kCtrlUninstallBtn) {
            top_col = pressed ? Gdiplus::Color(255, 44, 18, 20)
                              : (hovered ? Gdiplus::Color(255, 76, 30, 34) : Gdiplus::Color(255, 58, 22, 26));
            bot_col = pressed ? Gdiplus::Color(255, 30, 12, 14)
                              : (hovered ? Gdiplus::Color(255, 50, 18, 22) : Gdiplus::Color(255, 36, 14, 16));
            border_col = hovered ? Gdiplus::Color(255, 212, 104, 104) : Gdiplus::Color(255, 142, 62, 62);
            txt_col = Gdiplus::Color(255, 248, 212, 212);
        } else if (dis->CtlID == kCtrlLangZhBtn || dis->CtlID == kCtrlLangEnBtn) {
            const bool active_lang =
                (dis->CtlID == kCtrlLangZhBtn) ? (!g_ui.english) : g_ui.english;
            if (active_lang) {
                top_col = Gdiplus::Color(255, 184, 102, 18);
                bot_col = Gdiplus::Color(255, 112, 56, 10);
                border_col = Gdiplus::Color(255, 245, 166, 35);
                txt_col = Gdiplus::Color(255, 255, 246, 224);
            } else {
                top_col = pressed ? Gdiplus::Color(255, 24, 28, 38)
                                  : (hovered ? Gdiplus::Color(255, 42, 48, 62) : Gdiplus::Color(255, 24, 28, 38));
                bot_col = pressed ? Gdiplus::Color(255, 16, 18, 26)
                                  : (hovered ? Gdiplus::Color(255, 28, 32, 42) : Gdiplus::Color(255, 18, 20, 28));
                border_col = hovered ? Gdiplus::Color(255, 214, 162, 78) : Gdiplus::Color(255, 84, 68, 42);
                txt_col = hovered ? Gdiplus::Color(255, 238, 226, 202) : Gdiplus::Color(255, 168, 160, 146);
            }
        } else {
            top_col = pressed ? Gdiplus::Color(255, 28, 32, 42)
                              : (hovered ? Gdiplus::Color(255, 48, 52, 64) : Gdiplus::Color(255, 34, 38, 48));
            bot_col = pressed ? Gdiplus::Color(255, 18, 20, 28)
                              : (hovered ? Gdiplus::Color(255, 32, 34, 44) : Gdiplus::Color(255, 22, 24, 32));
            border_col = hovered ? Gdiplus::Color(255, 214, 162, 78) : Gdiplus::Color(255, 128, 96, 48);
            txt_col = Gdiplus::Color(255, 238, 226, 202);
        }

        const Gdiplus::RectF btn_r(0.5f, 0.5f, static_cast<float>(w) - 1.5f, static_cast<float>(h) - 1.5f);
        Gdiplus::GraphicsPath gp;
        AddRoundRectPath(gp, btn_r, Sf(5.0f));
        Gdiplus::LinearGradientBrush bg_br(
            Gdiplus::PointF(0.0f, 0.0f),
            Gdiplus::PointF(0.0f, static_cast<float>(h)),
            top_col,
            bot_col);
        g.FillPath(&bg_br, &gp);

        Gdiplus::Pen b_pen(border_col, Sf(focused ? 1.8f : 1.3f));
        g.DrawPath(&b_pen, &gp);

        // Top metallic specular highlight line
        const Gdiplus::RectF hi_r(
            Sf(2.0f), Sf(2.0f), static_cast<float>(w) - Sf(4.0f), Sf(1.0f));
        Gdiplus::SolidBrush hi_br(Gdiplus::Color(hovered ? 75 : 42, 255, 244, 210));
        g.FillRectangle(&hi_br, hi_r);

        wchar_t text[128]{};
        ::GetWindowTextW(dis->hwndItem, text, 128);
        Gdiplus::FontFamily ff(L"Microsoft YaHei UI");
        const bool is_lang_btn = (dis->CtlID == kCtrlLangZhBtn || dis->CtlID == kCtrlLangEnBtn);
        const bool is_active_lang =
            (dis->CtlID == kCtrlLangZhBtn && !g_ui.english) ||
            (dis->CtlID == kCtrlLangEnBtn && g_ui.english);
        const int weight = (dis->CtlID == kCtrlInstallBtn || is_active_lang)
                               ? Gdiplus::FontStyleBold
                               : Gdiplus::FontStyleRegular;
        const float pt_sz = is_lang_btn ? 12.0f : 14.0f;
        Gdiplus::Font btn_f(&ff, Sf(pt_sz), weight, Gdiplus::UnitPixel);
        Gdiplus::StringFormat fmt;
        fmt.SetAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        fmt.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);
        Gdiplus::RectF txt_r(0.0f, pressed ? Sf(1.0f) : 0.0f, static_cast<float>(w), static_cast<float>(h));
        DrawTextWithShadow(g, text, btn_f, txt_r, fmt, txt_col, Sf(1.2f));
    }

    ::BitBlt(dis->hDC, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);
    ::SelectObject(mem_dc, old_bmp);
    ::DeleteObject(mem_bmp);
    ::DeleteDC(mem_dc);
}

LRESULT CALLBACK ButtonHoverSubclassProc(
    HWND hwnd,
    UINT msg,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR,
    DWORD_PTR) {
    if (msg == WM_MOUSEMOVE) {
        if (g_ui.hovered_btn != hwnd) {
            g_ui.hovered_btn = hwnd;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            ::TrackMouseEvent(&tme);
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (msg == WM_MOUSELEAVE) {
        if (g_ui.hovered_btn == hwnd) {
            g_ui.hovered_btn = nullptr;
            ::InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    return ::DefSubclassProc(hwnd, msg, wparam, lparam);
}

void TryApplyDarkTitleBar(HWND hwnd) {
    HMODULE dwm = ::LoadLibraryW(L"dwmapi.dll");
    if (!dwm) {
        return;
    }
    using DwmSetWindowAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<DwmSetWindowAttrFn>(::GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (fn) {
        const BOOL dark = TRUE;
        fn(hwnd, 20, &dark, sizeof(dark));  // DWMWA_USE_IMMERSIVE_DARK_MODE
    }
    ::FreeLibrary(dwm);
}

void TryApplyDarkScrollbar(HWND hwnd) {
    HMODULE ux = ::LoadLibraryW(L"uxtheme.dll");
    if (!ux) {
        return;
    }
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    auto fn = reinterpret_cast<SetWindowThemeFn>(::GetProcAddress(ux, "SetWindowTheme"));
    if (fn) {
        fn(hwnd, L"DarkMode_Explorer", nullptr);
    }
    ::FreeLibrary(ux);
}

void RefreshUiLanguage(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    ::SetWindowTextW(hwnd, T(L"火炬之光 2 D3D11 画质增强 & SSAO 安装器",
                             L"Torchlight II D3D11 Graphics & SSAO Setup"));
    if (g_ui.lang_zh_btn) ::InvalidateRect(g_ui.lang_zh_btn, nullptr, FALSE);
    if (g_ui.lang_en_btn) ::InvalidateRect(g_ui.lang_en_btn, nullptr, FALSE);
    if (g_ui.browse_btn) ::SetWindowTextW(g_ui.browse_btn, T(L"浏览...", L"Browse..."));
    if (g_ui.check_btn) ::SetWindowTextW(g_ui.check_btn, T(L"检测版本兼容性", L"Check Compatibility"));
    if (g_ui.install_btn) ::SetWindowTextW(g_ui.install_btn, T(L"一键安装 / 升级", L"Install / Upgrade"));
    if (g_ui.uninstall_btn) ::SetWindowTextW(g_ui.uninstall_btn, T(L"卸载并恢复原版", L"Uninstall & Restore"));
    BuildBackdropBitmap(hwnd);
    g_gui_log.SyncGuiEdit();
    ::InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            g_ui.main_wnd = hwnd;
            TryApplyDarkTitleBar(hwnd);

            g_ui.ui_font = ::CreateFontW(
                -S(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_ui.log_font = ::CreateFontW(
                -S(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_ui.btn_font = ::CreateFontW(
                -S(14), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_ui.edit_bg_brush = ::CreateSolidBrush(RGB(11, 14, 22));
            g_ui.log_bg_brush = ::CreateSolidBrush(RGB(10, 13, 20));

            g_ui.path_edit = ::CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                S(44), S(172), S(564), S(21),
                hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtrlPathEdit)), nullptr, nullptr);
            ApplyFontRecursive(g_ui.path_edit, g_ui.ui_font);

            auto make_btn = [&](const wchar_t* text, int x, int y, int bw, int bh, int id) -> HWND {
                HWND b = ::CreateWindowW(
                    L"BUTTON", text,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                    S(x), S(y), S(bw), S(bh),
                    hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(id)), nullptr, nullptr);
                ApplyFontRecursive(b, (id == kCtrlInstallBtn) ? g_ui.btn_font : g_ui.ui_font);
                ::SetWindowSubclass(b, &ButtonHoverSubclassProc, 1, 0);
                return b;
            };

            g_ui.lang_zh_btn = make_btn(L"中", 640, 24, 40, 24, kCtrlLangZhBtn);
            g_ui.lang_en_btn = make_btn(L"EN", 684, 24, 40, 24, kCtrlLangEnBtn);
            g_ui.browse_btn = make_btn(T(L"浏览...", L"Browse..."), 626, 166, 98, 32, kCtrlBrowseBtn);
            g_ui.check_btn = make_btn(T(L"检测版本兼容性", L"Check Compatibility"), 36, 212, 166, 38, kCtrlCheckBtn);
            g_ui.install_btn = make_btn(T(L"一键安装 / 升级", L"Install / Upgrade"), 214, 212, 292, 38, kCtrlInstallBtn);
            g_ui.uninstall_btn = make_btn(T(L"卸载并恢复原版", L"Uninstall & Restore"), 518, 212, 206, 38, kCtrlUninstallBtn);

            g_ui.log_edit = ::CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                S(44), S(271), S(672), S(208),
                hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtrlLogEdit)), nullptr, nullptr);
            ApplyFontRecursive(g_ui.log_edit, g_ui.log_font);
            TryApplyDarkScrollbar(g_ui.log_edit);

            BuildBackdropBitmap(hwnd);

            const std::wstring detected = DetectTorchlight2Directory();
            g_gui_log.Clear();
            if (!detected.empty()) {
                ::SetWindowTextW(g_ui.path_edit, detected.c_str());
                g_gui_log.AppendBi(
                    L"[自动探测] 已找到游戏目录: " + detected,
                    L"[Auto-Detect] Found game directory: " + detected);
                g_gui_log.AppendBi(
                    L"[提示] 点击上方「一键安装 / 升级」即可覆盖部署最新增强插件与着色器。",
                    L"[Tip] Click 'Install / Upgrade' above to deploy the latest plugin and shaders.");
            } else {
                g_gui_log.AppendBi(
                    L"[提示] 未自动定位游戏目录，请点击“浏览...”选择含 Torchlight2.exe 的文件夹。",
                    L"[Tip] Game folder not found automatically. Click 'Browse...' to select the folder containing Torchlight2.exe.");
            }
            g_gui_log.AppendBi(
                L"[外部 LUT] 如需加载第三方 3D .cube 调色，请改名为 TL2_CustomLUT.cube 放入游戏根目录后在 F8 面板启用。",
                L"[External LUT] To load a custom 3D .cube file, rename it to TL2_CustomLUT.cube in your game root and select 'Custom .cube' in F8.");
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = ::BeginPaint(hwnd, &ps);
            if (!g_ui.bg_bmp) {
                BuildBackdropBitmap(hwnd);
            }
            if (g_ui.bg_bmp) {
                HDC mem_dc = ::CreateCompatibleDC(hdc);
                HGDIOBJ old = ::SelectObject(mem_dc, g_ui.bg_bmp);
                ::BitBlt(hdc, 0, 0, g_ui.client_w, g_ui.client_h, mem_dc, 0, 0, SRCCOPY);
                ::SelectObject(mem_dc, old);
                ::DeleteDC(mem_dc);
            }
            {
                Gdiplus::Graphics g(hdc);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                Gdiplus::FontFamily ff(L"Microsoft YaHei UI");
                Gdiplus::Font link_f(&ff, Sf(12.0f), Gdiplus::FontStyleUnderline, Gdiplus::UnitPixel);
                Gdiplus::StringFormat fmt_typo(Gdiplus::StringFormat::GenericTypographic());
                fmt_typo.SetHotkeyPrefix(Gdiplus::HotkeyPrefixNone);
                const Gdiplus::Color link_col = g_ui.github_hovered
                    ? Gdiplus::Color(255, 255, 232, 160)
                    : Gdiplus::Color(255, 245, 184, 72);
                DrawTextWithShadow(
                    g,
                    L"github.com/M3Lewis",
                    link_f,
                    Gdiplus::RectF(g_ui.github_x, g_ui.github_y, Sf(200), Sf(22)),
                    fmt_typo,
                    link_col,
                    Sf(1.0f));
            }
            ::EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lparam) == HTCLIENT) {
                POINT pt{};
                ::GetCursorPos(&pt);
                ::ScreenToClient(hwnd, &pt);
                if (::PtInRect(&g_ui.github_rc, pt)) {
                    ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
            break;
        }
        case WM_MOUSEMOVE: {
            const POINT pt{
                static_cast<int>(static_cast<short>(LOWORD(lparam))),
                static_cast<int>(static_cast<short>(HIWORD(lparam)))};
            const bool over = (::PtInRect(&g_ui.github_rc, pt) != FALSE);
            if (over != g_ui.github_hovered) {
                g_ui.github_hovered = over;
                if (over) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                    ::TrackMouseEvent(&tme);
                }
                ::InvalidateRect(hwnd, &g_ui.github_rc, FALSE);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            if (g_ui.github_hovered) {
                g_ui.github_hovered = false;
                ::InvalidateRect(hwnd, &g_ui.github_rc, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            const POINT pt{
                static_cast<int>(static_cast<short>(LOWORD(lparam))),
                static_cast<int>(static_cast<short>(HIWORD(lparam)))};
            if (::PtInRect(&g_ui.github_rc, pt)) {
                ::ShellExecuteW(hwnd, L"open", kGithubUrl, nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
            if (dis && dis->CtlType == ODT_BUTTON) {
                DrawOwnerButton(dis);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wparam);
            ::SetTextColor(hdc, RGB(240, 230, 210));
            ::SetBkColor(hdc, RGB(11, 14, 22));
            return reinterpret_cast<LRESULT>(g_ui.edit_bg_brush);
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wparam);
            HWND ctl = reinterpret_cast<HWND>(lparam);
            if (ctl == g_ui.log_edit) {
                ::SetTextColor(hdc, RGB(214, 223, 238));
                ::SetBkColor(hdc, RGB(10, 13, 20));
                return reinterpret_cast<LRESULT>(g_ui.log_bg_brush);
            }
            ::SetTextColor(hdc, RGB(232, 220, 196));
            ::SetBkColor(hdc, RGB(13, 16, 24));
            return reinterpret_cast<LRESULT>(g_ui.edit_bg_brush);
        }
        case WM_COMMAND: {
            const int ctrl_id = LOWORD(wparam);
            if (ctrl_id == kCtrlLangZhBtn) {
                if (g_ui.english) {
                    g_ui.english = false;
                    RefreshUiLanguage(hwnd);
                }
                return 0;
            }
            if (ctrl_id == kCtrlLangEnBtn) {
                if (!g_ui.english) {
                    g_ui.english = true;
                    RefreshUiLanguage(hwnd);
                }
                return 0;
            }
            if (ctrl_id == kCtrlBrowseBtn) {
                const std::wstring chosen = BrowseForGameFolder(hwnd, GetPathFromEdit());
                if (!chosen.empty()) {
                    ::SetWindowTextW(g_ui.path_edit, chosen.c_str());
                    g_gui_log.AppendBi(L"[已选目录] " + chosen, L"[Selected] " + chosen);
                }
                return 0;
            }
            if (ctrl_id == kCtrlCheckBtn) {
                bool version_mismatch = false;
                if (!ValidateGameDirectory(GetPathFromEdit(), g_gui_log, &version_mismatch) && version_mismatch) {
                    ::MessageBoxW(
                        hwnd,
                        T(L"检测到当前《火炬之光 2》不是 1.27 (2026 x64 D3D11) 兼容版本！\n\n请先将游戏升级至 1.27 版本：\n在 Steam 游戏库中右键点击《火炬之光 2》 →「属性」→「测试版」→ 在参与测试下拉框中选择 public_beta 分支更新游戏。",
                          L"Detected Torchlight II installation is not compatible v1.27 (2026 x64 D3D11)!\n\nPlease update the game to v1.27 first:\nIn Steam Library: Right-click Torchlight II -> Properties -> Betas -> select the public_beta branch to update."),
                        T(L"需要升级至 1.27 版本", L"v1.27 Update Required"),
                        MB_OK | MB_ICONWARNING);
                }
                return 0;
            }
            if (ctrl_id == kCtrlInstallBtn) {
                const fs::path dir = GetPathFromEdit();
                bool version_mismatch = false;
                const bool ok = PerformInstall(dir, g_gui_log, &version_mismatch);
                const wchar_t* err_msg = version_mismatch
                    ? T(L"安装已中止：检测到当前《火炬之光 2》不是 1.27 (2026 x64 D3D11) 兼容版本！\n\n请先将游戏升级至 1.27 版本：\n在 Steam 游戏库中右键点击《火炬之光 2》 →「属性」→「测试版」→ 选择 public_beta 分支更新游戏。",
                        L"Installation aborted: Torchlight II installation is not compatible v1.27 (2026 x64 D3D11)!\n\nPlease update the game to v1.27 first:\nIn Steam Library: Right-click Torchlight II -> Properties -> Betas -> select the public_beta branch to update.")
                    : T(L"安装未完成，请查看日志窗口中的错误信息。",
                        L"Installation incomplete. Please check the error details in the log console.");
                ::MessageBoxW(
                    hwnd,
                    ok ? T(L"安装成功！\n已自动备份原版配置与着色器。\n\n游戏内按 F8 弹出交互式配置面板：支持中英双语切换、10 项效果独立强度滑块、水面 SSR、3D LUT 调色、全开/全关对比，并保存多达 10 组预设。\n\n外部 .cube 调色说明：\n将自定义 3D .cube 文件重命名为 TL2_CustomLUT.cube 并放入游戏根目录（Torchlight2.exe 同级目录），即可在 F8 面板选择「外部 .cube」加载（支持热重载）。",
                           L"Installation successful!\nOriginal configs and shaders were backed up automatically.\n\nPress F8 in-game to open the live panel: toggle ZH/EN UI, tune 10 effect strength sliders, switch Water SSR / 3D LUT / Enable-Disable All, and save up to 10 custom presets.\n\nCustom .cube LUT usage:\nRename any 3D .cube file to TL2_CustomLUT.cube inside the game root (next to Torchlight2.exe), then pick 'Custom .cube' in F8 (hot-reload supported).")
                       : err_msg,
                    T(L"火炬之光 2 D3D11 画质增强 & SSAO 安装器",
                      L"Torchlight II D3D11 Graphics & SSAO Setup"),
                    ok ? (MB_OK | MB_ICONINFORMATION) : (MB_OK | MB_ICONERROR));
                return 0;
            }
            if (ctrl_id == kCtrlUninstallBtn) {
                if (::MessageBoxW(
                        hwnd,
                        T(L"确定卸载增强插件并恢复原版文件与配置吗？",
                          L"Uninstall the enhancement mod and restore original vanilla files & configs?"),
                        T(L"确认卸载", L"Confirm Uninstall"),
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    return 0;
                }
                const bool ok = PerformUninstall(GetPathFromEdit(), g_gui_log);
                ::MessageBoxW(
                    hwnd,
                    ok ? T(L"已卸载插件并恢复原版配置！",
                           L"Plugin removed and original vanilla files restored!")
                       : T(L"卸载过程中出现错误，请查看日志窗口。",
                           L"Errors occurred during uninstall. Please check the log console."),
                    T(L"火炬之光 2 D3D11 画质增强 & SSAO 安装器",
                      L"Torchlight II D3D11 Graphics & SSAO Setup"),
                    ok ? (MB_OK | MB_ICONINFORMATION) : (MB_OK | MB_ICONWARNING));
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            auto del_obj = [](auto& obj) {
                if (obj) {
                    ::DeleteObject(obj);
                    obj = nullptr;
                }
            };
            del_obj(g_ui.bg_bmp);
            del_obj(g_ui.edit_bg_brush);
            del_obj(g_ui.log_bg_brush);
            del_obj(g_ui.ui_font);
            del_obj(g_ui.log_font);
            del_obj(g_ui.btn_font);
            ::PostQuitMessage(0);
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

int RunCli(int argc, wchar_t** argv) {
    std::vector<std::wstring> pos;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--lang=en") {
            g_ui.english = true;
        } else if (arg == L"--lang=zh") {
            g_ui.english = false;
        } else {
            pos.push_back(arg);
        }
    }
    if (pos.size() < 2) {
        std::fwprintf(stderr, L"Usage: Setup.exe <--check|--install|--uninstall> <GameDir> [--lang=zh|--lang=en]\n");
        return 1;
    }
    const std::wstring& cmd = pos[0];
    const fs::path game_dir = pos[1];
    Logger log(true);
    if (cmd == L"--check") {
        return ValidateGameDirectory(game_dir, log) ? 0 : 2;
    }
    if (cmd == L"--install") {
        return PerformInstall(game_dir, log) ? 0 : 3;
    }
    if (cmd == L"--uninstall") {
        return PerformUninstall(game_dir, log) ? 0 : 4;
    }
    std::fwprintf(stderr, L"Unknown command: %s\n", cmd.c_str());
    return 1;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int nCmdShow) {
    ::SetProcessDPIAware();
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_ui.english = DetectDefaultEnglish();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    ::InitCommonControlsEx(&icc);

    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv && argc >= 2) {
        const int rc = RunCli(argc, argv);
        ::LocalFree(argv);
        ::CoUninitialize();
        return rc;
    }
    if (argv) {
        ::LocalFree(argv);
    }

    Gdiplus::GdiplusStartupInput gp_in;
    ULONG_PTR gp_token = 0;
    Gdiplus::GdiplusStartup(&gp_token, &gp_in, nullptr);

    {
        HDC dc = ::GetDC(nullptr);
        if (dc) {
            g_ui.dpi = (std::max)(96, ::GetDeviceCaps(dc, LOGPIXELSX));
            ::ReleaseDC(nullptr, dc);
        }
    }
    g_ui.client_w = S(kBaseWidth);
    g_ui.client_h = S(kBaseHeight);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWndProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"TL2DX11EnhanceSetupWnd";
    ::RegisterClassExW(&wc);

    RECT rc{0, 0, g_ui.client_w, g_ui.client_h};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    ::AdjustWindowRect(&rc, style, FALSE);

    const int win_w = rc.right - rc.left;
    const int win_h = rc.bottom - rc.top;
    const int scr_w = ::GetSystemMetrics(SM_CXSCREEN);
    const int scr_h = ::GetSystemMetrics(SM_CYSCREEN);
    const int win_x = (scr_w > win_w) ? (scr_w - win_w) / 2 : CW_USEDEFAULT;
    const int win_y = (scr_h > win_h) ? (scr_h - win_h) / 2 : CW_USEDEFAULT;

    HWND hwnd = ::CreateWindowExW(
        0,
        wc.lpszClassName,
        T(L"火炬之光 2 D3D11 画质增强 & SSAO 安装器",
          L"Torchlight II D3D11 Graphics & SSAO Setup"),
        style,
        win_x,
        win_y,
        win_w,
        win_h,
        nullptr,
        nullptr,
        hinst,
        nullptr);

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!::IsDialogMessageW(hwnd, &msg)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
    }

    if (gp_token) {
        Gdiplus::GdiplusShutdown(gp_token);
    }
    ::CoUninitialize();
    return 0;
}
