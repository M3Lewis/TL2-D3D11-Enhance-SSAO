#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
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

constexpr int kCtrlPathEdit = 1001;
constexpr int kCtrlBrowseBtn = 1002;
constexpr int kCtrlCheckBtn = 1003;
constexpr int kCtrlInstallBtn = 1004;
constexpr int kCtrlUninstallBtn = 1005;
constexpr int kCtrlLogEdit = 1006;

struct AppUi {
    HWND main_wnd = nullptr;
    HWND path_edit = nullptr;
    HWND browse_btn = nullptr;
    HWND check_btn = nullptr;
    HWND install_btn = nullptr;
    HWND uninstall_btn = nullptr;
    HWND log_edit = nullptr;
    HFONT ui_font = nullptr;
    HFONT title_font = nullptr;
};

AppUi g_ui{};

class Logger {
public:
    explicit Logger(bool cli_mode) : cli_mode_(cli_mode) {}

    void Append(const std::wstring& line) {
        if (!lines_.empty()) {
            lines_ += L"\r\n";
        }
        lines_ += line;
        if (cli_mode_) {
            const int n = ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
            if (n > 0) {
                std::string u8(static_cast<size_t>(n), '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), u8.data(), n, nullptr, nullptr);
                std::fwrite(u8.data(), 1, u8.size(), stdout);
                std::fputc('\n', stdout);
            }
        } else if (g_ui.log_edit) {
            ::SetWindowTextW(g_ui.log_edit, lines_.c_str());
            const int len = ::GetWindowTextLengthW(g_ui.log_edit);
            ::SendMessageW(g_ui.log_edit, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
            ::SendMessageW(g_ui.log_edit, EM_SCROLLCARET, 0, 0);
        }
    }

    void Clear() {
        lines_.clear();
        if (!cli_mode_ && g_ui.log_edit) {
            ::SetWindowTextW(g_ui.log_edit, L"");
        }
    }

private:
    bool cli_mode_ = false;
    std::wstring lines_;
};

Logger g_gui_log(false);

std::wstring FormatWin32Error(DWORD err) {
    wchar_t buf[32]{};
    std::swprintf(buf, 32, L"Win32错误码 %lu", static_cast<unsigned long>(err));
    return buf;
}

bool ReadPeSignature(const fs::path& file_path, uint32_t* out_timestamp, uint32_t* out_image_size, std::wstring* err_msg) {
    std::ifstream ifs(file_path, std::ios::binary);
    if (!ifs) {
        if (err_msg) {
            *err_msg = L"无法打开文件: " + file_path.wstring();
        }
        return false;
    }

    IMAGE_DOS_HEADER dos{};
    ifs.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!ifs || dos.e_magic != IMAGE_DOS_SIGNATURE) {
        if (err_msg) {
            *err_msg = L"无效 PE DOS 头: " + file_path.filename().wstring();
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
            *err_msg = L"无效 PE NT 头: " + file_path.filename().wstring();
        }
        return false;
    }
    if (file_hdr.Machine != IMAGE_FILE_MACHINE_AMD64) {
        if (err_msg) {
            *err_msg = file_path.filename().wstring() + L" 不是 64 位 (x64) 模块，当前插件仅支持 2026 x64 D3D11 版火炬之光 2。";
        }
        return false;
    }

    IMAGE_OPTIONAL_HEADER64 opt_hdr{};
    ifs.read(reinterpret_cast<char*>(&opt_hdr), sizeof(opt_hdr));
    if (!ifs || opt_hdr.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (err_msg) {
            *err_msg = L"无效 PE64 OptionalHeader: " + file_path.filename().wstring();
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

bool ValidateGameDirectory(const fs::path& game_dir, Logger& log) {
    std::error_code ec;
    if (game_dir.empty() || !fs::exists(game_dir, ec) || !fs::is_directory(game_dir, ec)) {
        log.Append(L"[错误] 所选目录不存在: " + game_dir.wstring());
        return false;
    }

    const fs::path exe_path = game_dir / L"Torchlight2.exe";
    if (!fs::exists(exe_path, ec)) {
        log.Append(L"[错误] 未找到 Torchlight2.exe，请选择《火炬之光 2》游戏根目录。");
        return false;
    }

    for (const auto& mod : kRequiredPeModules) {
        const fs::path mod_path = game_dir / mod.filename;
        if (!fs::exists(mod_path, ec)) {
            log.Append(std::wstring(L"[错误] 缺少必要引擎文件 ") + mod.filename +
                       L"（请确认游戏为 2026 x64 D3D11 更新版，而非旧版 32 位 D3D9 客户端）。");
            return false;
        }
        uint32_t ts = 0;
        uint32_t img_sz = 0;
        std::wstring pe_err;
        if (!ReadPeSignature(mod_path, &ts, &img_sz, &pe_err)) {
            log.Append(L"[错误] " + pe_err);
            return false;
        }
        if (ts != mod.expected_timestamp || img_sz != mod.expected_image_size) {
            wchar_t msg[256]{};
            std::swprintf(msg, 256,
                          L"[错误] %s 版本不匹配 (TimeDateStamp=0x%08X 期望 0x%08X, SizeOfImage=0x%X 期望 0x%X)，为防崩溃已中止。",
                          mod.filename, ts, mod.expected_timestamp, img_sz, mod.expected_image_size);
            log.Append(msg);
            return false;
        }
    }

    log.Append(L"[检查通过] 已识别兼容的 2026 x64 D3D11 版《火炬之光 2》目录。");
    return true;
}

bool ExtractResourceToFile(int resource_id, const fs::path& dest_path, std::wstring* err_msg) {
    HRSRC res = ::FindResourceW(nullptr, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
    if (!res) {
        if (err_msg) {
            *err_msg = L"找不到内嵌资源 ID=" + std::to_wstring(resource_id);
        }
        return false;
    }
    HGLOBAL loaded = ::LoadResource(nullptr, res);
    if (!loaded) {
        if (err_msg) {
            *err_msg = L"加载内嵌资源失败: " + FormatWin32Error(::GetLastError());
        }
        return false;
    }
    const DWORD size = ::SizeofResource(nullptr, res);
    const void* data = ::LockResource(loaded);
    if (!data || size == 0) {
        if (err_msg) {
            *err_msg = L"锁定内嵌资源数据失败。";
        }
        return false;
    }

    std::error_code ec;
    fs::create_directories(dest_path.parent_path(), ec);
    if (ec) {
        if (err_msg) {
            *err_msg = L"创建目标目录失败: " + dest_path.parent_path().wstring();
        }
        return false;
    }

    std::ofstream ofs(dest_path, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        if (err_msg) {
            *err_msg = L"写入文件失败（请检查目录写入权限或关闭占用程序）: " + dest_path.wstring();
        }
        return false;
    }
    ofs.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    ofs.flush();
    if (!ofs) {
        if (err_msg) {
            *err_msg = L"磁盘写入未完成: " + dest_path.wstring();
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
        log.Append(L"[错误] 创建备份目录失败: " + dst.parent_path().wstring());
        return false;
    }
    fs::copy_file(src, dst, fs::copy_options::none, ec);
    if (ec) {
        log.Append(L"[错误] 备份原版文件失败: " + rel_path.wstring());
        return false;
    }
    log.Append(L"[备份] 已保留原版文件 -> " + (fs::path(kBackupDirName) / rel_path).wstring());
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
            log.Append(L"[错误] 无法读取配置文件: " + cfg_path.filename().wstring());
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
        log.Append(L"[错误] 写入配置文件失败: " + cfg_path.filename().wstring());
        return false;
    }
    for (const auto& l : out_lines) {
        ofs << l << "\r\n";
    }
    ofs.flush();
    if (!ofs) {
        log.Append(L"[错误] 配置文件保存失败: " + cfg_path.filename().wstring());
        return false;
    }
    log.Append(L"[配置] 已更新 " + cfg_path.filename().wstring() + L" (D3D11 + Plugin_TL2_DX11_Enhance_SSAO)");
    return true;
}

bool PerformInstall(const fs::path& game_dir, Logger& log) {
    if (IsProcessRunningByName(L"Torchlight2.exe")) {
        log.Append(L"[错误] 检测到 Torchlight2.exe 正在运行，请先退出游戏再安装。");
        return false;
    }
    if (!ValidateGameDirectory(game_dir, log)) {
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
            log.Append(L"[清理] 已将旧版 Plugin_TL2_RouteB.dll 备份重命名为 .bak");
        }
    }

    for (const auto& item : kPayloadEntries) {
        const fs::path rel(item.rel_dest_path);
        if (item.is_vanilla_overwrite) {
            if (!BackupFileOnce(game_dir, rel, log)) {
                return false;
            }
        }
        std::wstring err_msg;
        if (!ExtractResourceToFile(item.resource_id, game_dir / rel, &err_msg)) {
            log.Append(L"[错误] " + err_msg);
            return false;
        }
        log.Append(L"[安装] " + rel.wstring());
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
    log.Append(L"=== 安装完成！===");
    log.Append(L"游戏内热键：F8 开关画质增强 | F9 开关 SSAO 环境光遮蔽（左上角带 2 秒提示）。");
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
        log.Append(L"[错误] 恢复备份失败: " + rel_path.wstring());
        return false;
    }
    log.Append(L"[恢复] " + rel_path.wstring());
    return true;
}

bool PerformUninstall(const fs::path& game_dir, Logger& log) {
    if (IsProcessRunningByName(L"Torchlight2.exe")) {
        log.Append(L"[错误] 检测到 Torchlight2.exe 正在运行，请先退出游戏再卸载。");
        return false;
    }
    std::error_code ec;
    if (game_dir.empty() || !fs::exists(game_dir / L"Torchlight2.exe", ec)) {
        log.Append(L"[错误] 所选目录中未找到 Torchlight2.exe。");
        return false;
    }

    const fs::path backup_root = game_dir / kBackupDirName;
    if (!fs::exists(backup_root, ec)) {
        log.Append(L"[提示] 未找到备份目录 TL2_DX11_Enhance_Backup，将仅移除插件新增文件（不会修改缺失备份的配置）。");
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
                    log.Append(L"[错误] 移除文件失败: " + rel.wstring() + L" (" + FormatWin32Error(::GetLastError()) + L")");
                    ok = false;
                } else {
                    log.Append(L"[移除] " + rel.wstring());
                }
            }
        }
    }

    if (ok) {
        log.Append(L"");
        log.Append(L"=== 卸载与原版恢复完成！===");
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
    dlg->SetTitle(L"请选择《火炬之光 2》(Torchlight II) 安装根目录");

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

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            g_ui.ui_font = ::CreateFontW(
                -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            g_ui.title_font = ::CreateFontW(
                -19, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");

            HWND title = ::CreateWindowW(
                L"STATIC", L"火炬之光 2 D3D11 画质增强 & SSAO 安装器 (2026 x64)",
                WS_CHILD | WS_VISIBLE, 20, 16, 620, 26, hwnd, nullptr, nullptr, nullptr);
            ApplyFontRecursive(title, g_ui.title_font);

            HWND sub = ::CreateWindowW(
                L"STATIC",
                L"包含: Plugin_TL2_DX11_Enhance_SSAO.dll + 14 个 HLSL/Material 着色器 | 热键: F8 增强开关, F9 SSAO 开关",
                WS_CHILD | WS_VISIBLE, 20, 44, 620, 20, hwnd, nullptr, nullptr, nullptr);
            ApplyFontRecursive(sub, g_ui.ui_font);

            HWND lbl_dir = ::CreateWindowW(
                L"STATIC", L"游戏根目录（含 Torchlight2.exe）：",
                WS_CHILD | WS_VISIBLE, 20, 76, 300, 20, hwnd, nullptr, nullptr, nullptr);
            ApplyFontRecursive(lbl_dir, g_ui.ui_font);

            g_ui.path_edit = ::CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                20, 98, 520, 26, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtrlPathEdit)), nullptr, nullptr);
            ApplyFontRecursive(g_ui.path_edit, g_ui.ui_font);

            g_ui.browse_btn = ::CreateWindowW(
                L"BUTTON", L"浏览...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                550, 97, 90, 28, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtrlBrowseBtn)), nullptr, nullptr);
            ApplyFontRecursive(g_ui.browse_btn, g_ui.ui_font);

            g_ui.check_btn = ::CreateWindowW(
                L"BUTTON", L"检测版本兼容性",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                20, 138, 150, 32, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtrlCheckBtn)), nullptr, nullptr);
            ApplyFontRecursive(g_ui.check_btn, g_ui.ui_font);

            g_ui.install_btn = ::CreateWindowW(
                L"BUTTON", L"一键安装 / 升级",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                186, 138, 220, 32, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtrlInstallBtn)), nullptr, nullptr);
            ApplyFontRecursive(g_ui.install_btn, g_ui.ui_font);

            g_ui.uninstall_btn = ::CreateWindowW(
                L"BUTTON", L"卸载并恢复原版",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                422, 138, 218, 32, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtrlUninstallBtn)), nullptr, nullptr);
            ApplyFontRecursive(g_ui.uninstall_btn, g_ui.ui_font);

            g_ui.log_edit = ::CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                20, 184, 620, 250, hwnd, reinterpret_cast<HMENU>(static_cast<intptr_t>(kCtrlLogEdit)), nullptr, nullptr);
            ApplyFontRecursive(g_ui.log_edit, g_ui.ui_font);

            const std::wstring detected = DetectTorchlight2Directory();
            g_gui_log.Clear();
            if (!detected.empty()) {
                ::SetWindowTextW(g_ui.path_edit, detected.c_str());
                g_gui_log.Append(L"[自动探测] 已找到游戏目录: " + detected);
            } else {
                g_gui_log.Append(L"[提示] 未自动定位游戏目录，请点击“浏览...”选择含 Torchlight2.exe 的文件夹。");
            }
            return 0;
        }
        case WM_COMMAND: {
            const int ctrl_id = LOWORD(wparam);
            if (ctrl_id == kCtrlBrowseBtn) {
                const std::wstring chosen = BrowseForGameFolder(hwnd, GetPathFromEdit());
                if (!chosen.empty()) {
                    ::SetWindowTextW(g_ui.path_edit, chosen.c_str());
                    g_gui_log.Append(L"[已选目录] " + chosen);
                }
                return 0;
            }
            if (ctrl_id == kCtrlCheckBtn) {
                ValidateGameDirectory(GetPathFromEdit(), g_gui_log);
                return 0;
            }
            if (ctrl_id == kCtrlInstallBtn) {
                const fs::path dir = GetPathFromEdit();
                const bool ok = PerformInstall(dir, g_gui_log);
                ::MessageBoxW(
                    hwnd,
                    ok ? L"安装成功！\n已自动备份原版配置与着色器。\n\n游戏内按 F8 可开关画质增强，按 F9 可开关 SSAO。"
                       : L"安装未完成，请查看日志窗口中的错误信息。",
                    L"火炬之光 2 D3D11 画质增强安装器",
                    ok ? (MB_OK | MB_ICONINFORMATION) : (MB_OK | MB_ICONERROR));
                return 0;
            }
            if (ctrl_id == kCtrlUninstallBtn) {
                if (::MessageBoxW(
                        hwnd,
                        L"确定卸载增强插件并恢复原版文件与配置吗？",
                        L"确认卸载",
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
                    return 0;
                }
                const bool ok = PerformUninstall(GetPathFromEdit(), g_gui_log);
                ::MessageBoxW(
                    hwnd,
                    ok ? L"已卸载插件并恢复原版配置！" : L"卸载过程中出现错误，请查看日志窗口。",
                    L"火炬之光 2 D3D11 画质增强安装器",
                    ok ? (MB_OK | MB_ICONINFORMATION) : (MB_OK | MB_ICONWARNING));
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            if (g_ui.ui_font) {
                ::DeleteObject(g_ui.ui_font);
                g_ui.ui_font = nullptr;
            }
            if (g_ui.title_font) {
                ::DeleteObject(g_ui.title_font);
                g_ui.title_font = nullptr;
            }
            ::PostQuitMessage(0);
            return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wparam, lparam);
}

int RunCli(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fwprintf(stderr, L"Usage: Setup.exe <--check|--install|--uninstall> <GameDir>\n");
        return 1;
    }
    const std::wstring cmd = argv[1];
    const fs::path game_dir = argv[2];
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

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MainWndProc;
    wc.hInstance = hinst;
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TL2DX11EnhanceSetupWnd";
    ::RegisterClassExW(&wc);

    RECT rc{0, 0, 660, 456};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    ::AdjustWindowRect(&rc, style, FALSE);

    HWND hwnd = ::CreateWindowExW(
        0,
        wc.lpszClassName,
        L"火炬之光 2 D3D11 画质增强 & SSAO 安装器",
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rc.right - rc.left,
        rc.bottom - rc.top,
        nullptr,
        nullptr,
        hinst,
        nullptr);

    ::ShowWindow(hwnd, nCmdShow);
    ::UpdateWindow(hwnd);

    MSG msg{};
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    ::CoUninitialize();
    return 0;
}
