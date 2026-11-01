// setup.cpp - FileEncryptor 安装程序（「标准版」）
//
// 要点：
//   * **要求管理员权限**（setup.manifest: requireAdministrator），双击即提权；
//     这样才能写 Program Files、HKLM 卸载项、系统 PATH、所有用户的快捷方式。
//   * 默认安装到 C:\Program Files\FileEncryptor，界面上可改（带「浏览…」）。
//   * 组件可选：图形界面 / 命令行（至少选一个）。
//       - 勾图形界面 → 可再选「创建桌面快捷方式」
//       - 勾命令行   → 可再选「添加到环境变量（PATH）」
//   * 界面可切换语言（默认跟随系统 UI 语言）。
//   * 卸载项写 HKLM；卸载时一并清理快捷方式与 PATH 项。
//
// 注意：编译时**不要**开 /GL(LTCG)，实测会让本程序在部分机器上以
// STATUS_ILLEGAL_INSTRUCTION (0xC000001D) 崩溃。
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>

#include "setup_res.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

HINSTANCE g_inst = nullptr;
bool      g_english = false;
bool      g_alreadyInstalled = false;   // 安装界面是否处于「升级/修复」模式

inline int FE_MsgBox(HWND h, const std::wstring& text, const std::wstring& title, UINT flags) {
    return MessageBoxW(h, text.c_str(), title.c_str(), flags);
}
inline void FE_SetDlgText(HWND h, int id, const std::wstring& s) {
    SetDlgItemTextW(h, id, s.c_str());
}
#define MessageBoxW FE_MsgBox
#define SetDlgItemTextW FE_SetDlgText

const wchar_t* kDefaultDir = L"C:\\Program Files\\FileEncryptor";
const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FileEncryptor";
const wchar_t* kEnvKey =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
const wchar_t* kAppExe = L"FileEncryptor.exe";

enum {
    IDC_TITLE = 100,
    IDC_INFO,
    IDC_PATHLABEL,
    IDC_PATHEDIT,
    IDC_BROWSE,
    IDC_COMPLABEL,
    IDC_CHK_GUI,
    IDC_CHK_CLI,
    IDC_CHK_DESKTOP,
    IDC_CHK_PATH,
    IDC_STATUS,
    IDC_LANG_ZH = 150,
    IDC_LANG_EN,
    IDC_INSTALL = IDOK,
    // 维护窗口
    IDC_M_TITLE = 200,
    IDC_M_INFO,
    IDC_M_UPGRADE,
    IDC_M_UNINSTALL
};

std::wstring T(const wchar_t* zh, const wchar_t* en) { return g_english ? en : zh; }

bool IsElevated() {
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION el = {};
    DWORD cb = sizeof(el);
    const BOOL ok = GetTokenInformation(tok, TokenElevation, &el, cb, &cb);
    CloseHandle(tok);
    return ok && el.TokenIsElevated;
}

std::wstring GetSelfPath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(NULL, buf, _countof(buf));
    return buf;
}

// 本程序被复制成 uninstall.exe 放在安装目录里。
// 直接双击它时命令行**没有** /uninstall，所以要按文件名也认出来，
// 否则会弹出安装界面（用户反馈的 bug）。
bool IsUninstallerExe() {
    const std::wstring self = GetSelfPath();
    const wchar_t* name = PathFindFileNameW(self.c_str());
    return name && _wcsicmp(name, L"uninstall.exe") == 0;
}

bool EnsureDir(const std::wstring& dir) {
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    const size_t pos = dir.find_last_of(L'\\');
    if (pos != std::wstring::npos) {
        const std::wstring parent = dir.substr(0, pos);
        if (!parent.empty() && parent.size() >= 3) EnsureDir(parent);
    }
    return CreateDirectoryW(dir.c_str(), NULL) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReleaseResource(int id, const std::wstring& outPath) {
    // RT_RCDATA 是**序号**类型（10），不能传 L"RCDATA"
    HRSRC res = FindResourceW(g_inst, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(RT_RCDATA));
    if (!res) return false;
    const DWORD size = SizeofResource(g_inst, res);
    HGLOBAL mem = LoadResource(g_inst, res);
    if (!mem || size == 0) return false;
    const void* data = LockResource(mem);
    if (!data) return false;

    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, data, size, &written, NULL);
    CloseHandle(h);
    return ok && written == size;
}

bool CreateShortcut(const std::wstring& lnkPath, const std::wstring& target,
                    const std::wstring& workDir, const std::wstring& desc) {
    IShellLinkW* link = nullptr;
    // 第三个参数是 dwClsContext，之前误传了 NULL(=0)，CoCreateInstance 直接失败，
    // 于是"安装成功但快捷方式没创建"。必须是 CLSCTX_INPROC_SERVER。
    if (FAILED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, (void**)&link)) || !link) {
        return false;
    }
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(workDir.c_str());
    link->SetDescription(desc.c_str());

    IPersistFile* file = nullptr;
    bool ok = false;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile, (void**)&file)) && file) {
        ok = SUCCEEDED(file->Save(lnkPath.c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
}

std::wstring CommonStartMenuLnk() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, buf))) return L"";
    return std::wstring(buf) + L"\\FileEncryptor.lnk";
}
std::wstring CommonDesktopLnk() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, 0, buf))) return L"";
    return std::wstring(buf) + L"\\FileEncryptor.lnk";
}
std::wstring UserStartMenuLnk() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, buf))) return L"";
    return std::wstring(buf) + L"\\FileEncryptor.lnk";
}
std::wstring UserDesktopLnk() {
    wchar_t buf[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, buf))) return L"";
    return std::wstring(buf) + L"\\FileEncryptor.lnk";
}

// ---------------- 系统 PATH ----------------

// 读取原始 PATH（保留 REG_EXPAND_SZ 类型与 %VAR% 写法）
bool ReadSystemPath(std::wstring& value, DWORD& type) {
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kEnvKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[32768] = {};
    DWORD cb = sizeof(buf);
    type = REG_EXPAND_SZ;
    const LONG r = RegQueryValueExW(k, L"Path", NULL, &type, (BYTE*)buf, &cb);
    RegCloseKey(k);
    if (r != ERROR_SUCCESS) return false;
    value = buf;
    return true;
}

bool WriteSystemPath(const std::wstring& value, DWORD type) {
    HKEY k = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kEnvKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return false;
    const LONG r = RegSetValueExW(k, L"Path", 0, type, (const BYTE*)value.c_str(),
                                  (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

void BroadcastEnvChange() {
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment",
                        SMTO_ABORTIFHUNG, 3000, NULL);
}

// 判断 PATH 里是否已经包含该目录（忽略大小写、忽略末尾反斜杠）
bool PathContains(const std::wstring& path, const std::wstring& dir) {
    std::wstring want = dir;
    while (want.size() > 3 && want.back() == L'\\') want.pop_back();

    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L';', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring item = path.substr(start, end - start);
        while (!item.empty() && item.front() == L' ') item.erase(item.begin());
        while (!item.empty() && item.back() == L' ') item.pop_back();
        while (item.size() > 3 && item.back() == L'\\') item.pop_back();
        if (_wcsicmp(item.c_str(), want.c_str()) == 0) return true;
        if (end == path.size()) break;
        start = end + 1;
    }
    return false;
}

bool AddToSystemPath(const std::wstring& dir, std::wstring& err) {
    std::wstring path;
    DWORD type = REG_EXPAND_SZ;
    if (!ReadSystemPath(path, type)) {
        err = T(L"无法读取系统 PATH", L"Could not read the system PATH");
        return false;
    }
    if (PathContains(path, dir)) return true;   // 已经有了，不算失败
    if (!path.empty() && path.back() != L';') path += L';';
    path += dir;
    if (!WriteSystemPath(path, type)) {
        err = T(L"无法写入系统 PATH", L"Could not write the system PATH");
        return false;
    }
    BroadcastEnvChange();
    return true;
}

void RemoveFromSystemPath(const std::wstring& dir) {
    std::wstring path;
    DWORD type = REG_EXPAND_SZ;
    if (!ReadSystemPath(path, type)) return;
    if (!PathContains(path, dir)) return;

    std::wstring want = dir;
    while (want.size() > 3 && want.back() == L'\\') want.pop_back();

    std::wstring out;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L';', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring item = path.substr(start, end - start);
        std::wstring trimmed = item;
        while (!trimmed.empty() && trimmed.front() == L' ') trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && trimmed.back() == L' ') trimmed.pop_back();
        while (trimmed.size() > 3 && trimmed.back() == L'\\') trimmed.pop_back();
        if (_wcsicmp(trimmed.c_str(), want.c_str()) != 0) {
            if (!out.empty()) out += L';';
            out += item;
        }
        if (end == path.size()) break;
        start = end + 1;
    }
    if (WriteSystemPath(out, type)) BroadcastEnvChange();
}

// ---------------- 卸载信息 ----------------

void WriteUninstallInfo(const std::wstring& dir) {
    HKEY k = NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, NULL, 0,
                        KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) {
        return;
    }
    const std::wstring exe = dir + L"\\" + kAppExe;
    const std::wstring uninst = dir + L"\\uninstall.exe";
    const std::wstring cmd = L"\"" + uninst + L"\" /uninstall";

    auto setSz = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(),
                       (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    setSz(L"DisplayName", L"FileEncryptor");
    setSz(L"DisplayVersion", L"1.0.0");     // 发版时与 FE_APP_VERSION 一起改
    setSz(L"Publisher", L"虞子尧");
    setSz(L"DisplayIcon", exe);
    setSz(L"InstallLocation", dir);
    setSz(L"UninstallString", cmd);
    setSz(L"QuietUninstallString", cmd + L" /quiet");
    DWORD one = 1;
    RegSetValueExW(k, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(k, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegCloseKey(k);
}

std::wstring ReadInstallLocation() {
    HKEY k = NULL;
    std::wstring out;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH] = {};
        DWORD cb = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(k, L"InstallLocation", NULL, &type, (BYTE*)buf, &cb) == ERROR_SUCCESS &&
            type == REG_SZ) {
            out = buf;
        }
        RegCloseKey(k);
    }
    return out;
}

void RemoveUninstallInfo() { RegDeleteTreeW(HKEY_LOCAL_MACHINE, kUninstallKey); }

// ---------------- 安装 / 卸载 ----------------

struct InstallOptions {
    std::wstring dir;
    bool gui = true;
    bool cli = false;
    bool desktopShortcut = true;
    bool addToPath = false;
};

bool DoInstall(HWND hwnd, const InstallOptions& opt, std::wstring& why) {
    if (opt.dir.size() < 3 || opt.dir.find(L':') == std::wstring::npos) {
        why = T(L"安装目录无效，请重新选择。", L"That install folder is not valid.");
        return false;
    }
    if (!opt.gui && !opt.cli) {
        why = T(L"请至少选择「图形界面」或「命令行」中的一项。",
                L"Please select at least one of GUI / CLI.");
        return false;
    }

    auto status = [&](const std::wstring& s) {
        SetDlgItemTextW(hwnd, IDC_STATUS, s);
        UpdateWindow(hwnd);
    };

    status(T(L"正在创建目录…", L"Creating folder..."));
    if (!EnsureDir(opt.dir)) {
        why = T(L"无法创建安装目录：", L"Could not create the folder: ") + opt.dir;
        return false;
    }

    if (opt.gui) {
        status(T(L"正在解压图形界面…", L"Extracting the GUI..."));
        const std::wstring gui = opt.dir + L"\\" + kAppExe;
        if (!ReleaseResource(IDR_PAYLOAD_GUI, gui)) {
            why = T(L"无法写出主程序文件。", L"Could not write the application file.");
            return false;
        }
        status(T(L"正在创建快捷方式…", L"Creating shortcuts..."));
        const std::wstring sm = CommonStartMenuLnk();
        if (!sm.empty()) CreateShortcut(sm, gui, opt.dir, T(L"文件加密工具", L"File encryption tool"));
        if (opt.desktopShortcut) {
            const std::wstring dt = CommonDesktopLnk();
            if (!dt.empty()) CreateShortcut(dt, gui, opt.dir, T(L"文件加密工具", L"File encryption tool"));
        }
    }

    if (opt.cli) {
        status(T(L"正在解压命令行…", L"Extracting the CLI..."));
        if (!ReleaseResource(IDR_PAYLOAD_CLI, opt.dir + L"\\enc.exe")) {
            why = T(L"无法写出命令行程序。", L"Could not write the CLI.");
            return false;
        }
        if (opt.addToPath) {
            status(T(L"正在添加到 PATH…", L"Adding to PATH..."));
            std::wstring e;
            if (!AddToSystemPath(opt.dir, e)) {
                // PATH 失败不阻断安装，只提示
                why = e;
            }
        }
    }

    status(T(L"正在写入卸载程序…", L"Installing the uninstaller..."));
    CopyFileW(GetSelfPath().c_str(), (opt.dir + L"\\uninstall.exe").c_str(), FALSE);

    status(T(L"正在注册卸载信息…", L"Registering uninstall information..."));
    WriteUninstallInfo(opt.dir);
    return true;
}

bool DoUninstall(HWND hwnd, bool quiet) {
    std::wstring dir = ReadInstallLocation();
    if (dir.empty()) dir = kDefaultDir;

    if (!quiet) {
        const std::wstring msg = T(L"确定要卸载 FileEncryptor 吗？", L"Uninstall FileEncryptor?") +
                                 L"\n\n" + T(L"安装目录：", L"Folder: ") + dir;
        if (MessageBoxW(hwnd, msg, T(L"卸载 FileEncryptor", L"Uninstall"),
                        MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) {
            return false;
        }
    }

    for (const std::wstring& p : { CommonStartMenuLnk(), CommonDesktopLnk(),
                                   UserStartMenuLnk(), UserDesktopLnk() }) {
        if (!p.empty()) DeleteFileW(p.c_str());
    }
    RemoveFromSystemPath(dir);
    RemoveUninstallInfo();

    const std::wstring self = GetSelfPath();
    const std::wstring uninst = dir + L"\\uninstall.exe";
    const bool selfIsInDir = (_wcsicmp(self.c_str(), uninst.c_str()) == 0);

    wchar_t pattern[MAX_PATH] = {};
    wcscpy_s(pattern, (dir + L"\\*").c_str());
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring f = dir + L"\\" + fd.cFileName;
            if (selfIsInDir && _wcsicmp(f.c_str(), uninst.c_str()) == 0) {
                MoveFileExW(f.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
            }
            else {
                DeleteFileW(f.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!selfIsInDir) RemoveDirectoryW(dir.c_str());
    else MoveFileExW(dir.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);

    if (!quiet) {
        MessageBoxW(NULL,
            T(L"已卸载完成。\n\n卸载程序自身与空目录会在下次重启时自动清除。",
              L"FileEncryptor has been uninstalled.\n\nThe uninstaller itself and the empty "
              L"folder will be removed automatically on the next restart."),
            T(L"完成", L"Done"), MB_ICONINFORMATION);
    }
    return true;
}

// ---------------- 界面 ----------------

void ApplyLanguage(HWND hwnd) {
    SetWindowTextW(hwnd, T(L"FileEncryptor 安装程序", L"FileEncryptor Setup").c_str());
    SetDlgItemTextW(hwnd, IDC_INFO,
        T(L"选择要安装的内容与位置（需要管理员权限）。",
          L"Choose what to install and where (administrator rights required)."));
    SetDlgItemTextW(hwnd, IDC_PATHLABEL, T(L"安装位置：", L"Install folder:"));
    SetDlgItemTextW(hwnd, IDC_BROWSE, T(L"浏览…", L"Browse..."));
    SetDlgItemTextW(hwnd, IDC_COMPLABEL, T(L"组件：", L"Components:"));
    SetDlgItemTextW(hwnd, IDC_CHK_GUI, T(L"图形界面", L"GUI"));
    SetDlgItemTextW(hwnd, IDC_CHK_CLI, T(L"命令行", L"Command line"));
    SetDlgItemTextW(hwnd, IDC_CHK_DESKTOP, T(L"创建桌面快捷方式", L"Create a desktop shortcut"));
    SetDlgItemTextW(hwnd, IDC_CHK_PATH, T(L"添加到环境变量（PATH）", L"Add to PATH"));
    SetDlgItemTextW(hwnd, IDC_INSTALL,
        g_alreadyInstalled ? T(L"升级 / 修复", L"Upgrade / Repair")
                           : T(L"安装", L"Install"));
    SetDlgItemTextW(hwnd, IDCANCEL, T(L"取消", L"Cancel"));
}

void SyncComponentUi(HWND hwnd) {
    const bool gui = SendDlgItemMessageW(hwnd, IDC_CHK_GUI, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const bool cli = SendDlgItemMessageW(hwnd, IDC_CHK_CLI, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ShowWindow(GetDlgItem(hwnd, IDC_CHK_DESKTOP), gui ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(hwnd, IDC_CHK_PATH), cli ? SW_SHOW : SW_HIDE);
}

void OnBrowse(HWND hwnd) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    const std::wstring title = T(L"选择安装位置", L"Choose the install folder");
    bi.lpszTitle = title.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidl, path)) {
        std::wstring dir = path;
        while (dir.size() > 3 && dir.back() == L'\\') dir.pop_back();
        const bool alreadyNamed =
            (dir.size() >= 13 && _wcsicmp(dir.substr(dir.size() - 13).c_str(), L"FileEncryptor") == 0);
        if (_wcsicmp(dir.c_str(), L"C:") != 0 && !alreadyNamed) dir += L"\\FileEncryptor";
        SetDlgItemTextW(hwnd, IDC_PATHEDIT, dir);
    }
    CoTaskMemFree(pidl);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HDC dc = GetDC(hwnd);
        const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(hwnd, dc);
        auto px = [&](int v) { return MulDiv(v, dpi, 96); };

        const HFONT fTitle = CreateFontW(-px(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const HFONT fBody = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        auto mk = [&](const wchar_t* cls, const std::wstring& text, DWORD style,
                      int x, int y, int w, int h, int id, HFONT font) {
            HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style,
                                     px(x), px(y), px(w), px(h), hwnd, (HMENU)(INT_PTR)id,
                                     g_inst, NULL);
            if (c && font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };

        mk(L"STATIC", L"FileEncryptor", SS_LEFT, 24, 16, 240, 28, IDC_TITLE, fTitle);
        // 语言切换（右上角）
        mk(L"BUTTON", L"中文", BS_PUSHBUTTON, 380, 18, 60, 26, IDC_LANG_ZH, fBody);
        mk(L"BUTTON", L"English", BS_PUSHBUTTON, 444, 18, 60, 26, IDC_LANG_EN, fBody);

        mk(L"STATIC", L"", SS_LEFT, 24, 52, 464, 22, IDC_INFO, fBody);
        mk(L"STATIC", L"", SS_LEFT, 24, 84, 120, 20, IDC_PATHLABEL, fBody);
        mk(L"EDIT", kDefaultDir, WS_BORDER | ES_AUTOHSCROLL,
           24, 106, 392, 26, IDC_PATHEDIT, fBody);
        mk(L"BUTTON", L"", BS_PUSHBUTTON, 424, 106, 72, 26, IDC_BROWSE, fBody);

        mk(L"STATIC", L"", SS_LEFT, 24, 148, 120, 20, IDC_COMPLABEL, fBody);
        mk(L"BUTTON", L"", BS_AUTOCHECKBOX, 24, 172, 130, 24, IDC_CHK_GUI, fBody);
        mk(L"BUTTON", L"", BS_AUTOCHECKBOX, 176, 172, 150, 24, IDC_CHK_CLI, fBody);
        mk(L"BUTTON", L"", BS_AUTOCHECKBOX, 24, 200, 260, 24, IDC_CHK_DESKTOP, fBody);
        mk(L"BUTTON", L"", BS_AUTOCHECKBOX, 24, 226, 300, 24, IDC_CHK_PATH, fBody);

        SendDlgItemMessageW(hwnd, IDC_CHK_GUI, BM_SETCHECK, BST_CHECKED, 0);
        SendDlgItemMessageW(hwnd, IDC_CHK_DESKTOP, BM_SETCHECK, BST_CHECKED, 0);

        // 已安装过就预填原路径，并把按钮改成「升级 / 修复」
        const std::wstring existing = ReadInstallLocation();
        if (!existing.empty()) {
            g_alreadyInstalled = true;
            SetDlgItemTextW(hwnd, IDC_PATHEDIT, existing);
        }

        mk(L"STATIC", L"", SS_LEFT, 24, 256, 464, 20, IDC_STATUS, fBody);
        mk(L"BUTTON", L"", BS_DEFPUSHBUTTON, 304, 284, 88, 30, IDC_INSTALL, fBody);
        mk(L"BUTTON", L"", BS_PUSHBUTTON, 400, 284, 96, 30, IDCANCEL, fBody);

        ApplyLanguage(hwnd);
        SyncComponentUi(hwnd);
        return 0;
    }

    // STATIC 默认带灰底，让它跟白色窗口背景一致
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_LANG_ZH:
            g_english = false;
            ApplyLanguage(hwnd);
            SyncComponentUi(hwnd);
            return 0;

        case IDC_LANG_EN:
            g_english = true;
            ApplyLanguage(hwnd);
            SyncComponentUi(hwnd);
            return 0;

        case IDC_CHK_GUI:
        case IDC_CHK_CLI:
            SyncComponentUi(hwnd);
            return 0;

        case IDC_BROWSE:
            OnBrowse(hwnd);
            return 0;

        case IDC_INSTALL: {
            InstallOptions opt;
            wchar_t buf[MAX_PATH] = {};
            GetDlgItemTextW(hwnd, IDC_PATHEDIT, buf, _countof(buf));
            opt.dir = buf;
            while (opt.dir.size() > 3 && opt.dir.back() == L'\\') opt.dir.pop_back();
            opt.gui = SendDlgItemMessageW(hwnd, IDC_CHK_GUI, BM_GETCHECK, 0, 0) == BST_CHECKED;
            opt.cli = SendDlgItemMessageW(hwnd, IDC_CHK_CLI, BM_GETCHECK, 0, 0) == BST_CHECKED;
            opt.desktopShortcut =
                SendDlgItemMessageW(hwnd, IDC_CHK_DESKTOP, BM_GETCHECK, 0, 0) == BST_CHECKED;
            opt.addToPath =
                SendDlgItemMessageW(hwnd, IDC_CHK_PATH, BM_GETCHECK, 0, 0) == BST_CHECKED;

            if (!opt.gui && !opt.cli) {
                MessageBoxW(hwnd,
                    T(L"请至少选择「图形界面」或「命令行」中的一项。",
                      L"Please select at least one of GUI / CLI."),
                    T(L"未选择组件", L"Nothing selected"), MB_ICONWARNING);
                return 0;
            }

            EnableWindow(GetDlgItem(hwnd, IDC_INSTALL), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDCANCEL), FALSE);
            EnableWindow(GetDlgItem(hwnd, IDC_BROWSE), FALSE);

            std::wstring why;
            const bool ok = DoInstall(hwnd, opt, why);
            if (ok) {
                MessageBoxW(hwnd,
                    T(L"安装完成。", L"Installation complete."),
                    T(L"完成", L"Done"), MB_ICONINFORMATION);
                DestroyWindow(hwnd);
            }
            else {
                EnableWindow(GetDlgItem(hwnd, IDC_INSTALL), TRUE);
                EnableWindow(GetDlgItem(hwnd, IDCANCEL), TRUE);
                EnableWindow(GetDlgItem(hwnd, IDC_BROWSE), TRUE);
                SetDlgItemTextW(hwnd, IDC_STATUS, T(L"安装失败。", L"Installation failed."));
                MessageBoxW(hwnd, why, T(L"安装失败", L"Install failed"), MB_ICONERROR);
            }
            return 0;
        }

        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int RunWindow(HINSTANCE inst, const wchar_t* cls, WNDPROC proc, int cw, int ch,
              const std::wstring& caption) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = cls;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, cw, ch };
    AdjustWindowRectEx(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, 0);
    HWND hwnd = CreateWindowExW(0, cls, caption.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, inst, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

void RunInstallWindow(HINSTANCE inst);

// ---------------- 维护窗口（uninstall.exe）----------------
// 双击安装目录里的 uninstall.exe 时显示：可以「升级 / 修复」也可以「卸载」。
bool g_doUpgrade = false;

void ApplyMaintLanguage(HWND hwnd) {
    SetWindowTextW(hwnd, T(L"FileEncryptor 维护", L"FileEncryptor Maintenance").c_str());
    const std::wstring dir = ReadInstallLocation();
    SetDlgItemTextW(hwnd, IDC_M_INFO,
        T(L"已安装于：", L"Installed in: ") + (dir.empty() ? kDefaultDir : dir));
    SetDlgItemTextW(hwnd, IDC_M_UPGRADE, T(L"升级 / 修复", L"Upgrade / Repair"));
    SetDlgItemTextW(hwnd, IDC_M_UNINSTALL, T(L"卸载", L"Uninstall"));
    SetDlgItemTextW(hwnd, IDCANCEL, T(L"取消", L"Cancel"));
}

LRESULT CALLBACK MaintProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HDC dc = GetDC(hwnd);
        const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(hwnd, dc);
        auto px = [&](int v) { return MulDiv(v, dpi, 96); };
        const HFONT fTitle = CreateFontW(-px(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        const HFONT fBody = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        auto mk = [&](const wchar_t* cls, const std::wstring& text, DWORD style,
                      int x, int y, int w, int h, int id, HFONT font) {
            HWND c = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style,
                                     px(x), px(y), px(w), px(h), hwnd, (HMENU)(INT_PTR)id,
                                     g_inst, NULL);
            if (c && font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };
        mk(L"STATIC", L"FileEncryptor", SS_LEFT, 24, 18, 240, 28, IDC_M_TITLE, fTitle);
        mk(L"STATIC", L"", SS_LEFT, 24, 58, 400, 22, IDC_M_INFO, fBody);
        mk(L"BUTTON", L"", BS_DEFPUSHBUTTON, 20, 104, 124, 32, IDC_M_UPGRADE, fBody);
        mk(L"BUTTON", L"", BS_PUSHBUTTON, 156, 104, 116, 32, IDC_M_UNINSTALL, fBody);
        mk(L"BUTTON", L"", BS_PUSHBUTTON, 284, 104, 116, 32, IDCANCEL, fBody);
        ApplyMaintLanguage(hwnd);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_M_UPGRADE:
            g_doUpgrade = true;
            DestroyWindow(hwnd);
            return 0;
        case IDC_M_UNINSTALL:
            DoUninstall(hwnd, false);
            DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RunInstallWindow(HINSTANCE inst) {
    RunWindow(inst, L"FileEncryptorSetup", WndProc, 520, 332,
              T(L"FileEncryptor 安装程序", L"FileEncryptor Setup"));
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    g_inst = inst;
    g_english = (PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE);

    std::wstring cmd;
    {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) { cmd += argv[i]; cmd += L' '; }
            LocalFree(argv);
        }
    }
    const bool byArg   = cmd.find(L"/uninstall") != std::wstring::npos;
    const bool byName  = IsUninstallerExe();
    const bool quiet   = cmd.find(L"/quiet") != std::wstring::npos;

    if (!IsElevated()) {
        MessageBoxW(NULL,
            T(L"本程序需要管理员权限。请右键选择「以管理员身份运行」。",
              L"Administrator rights are required. Right-click and choose "
              L"\"Run as administrator\"."),
            L"FileEncryptor", MB_ICONERROR);
        return 1;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    if (byArg || byName) {
        if (quiet) {
            DoUninstall(NULL, true);
        }
        else {
            // 维护窗口：升级 / 修复 或 卸载
            RunWindow(inst, L"FileEncryptorMaint", MaintProc, 440, 168,
                      T(L"FileEncryptor 维护", L"FileEncryptor Maintenance"));
            if (g_doUpgrade) RunInstallWindow(inst);
        }
        CoUninitialize();
        return 0;
    }

    RunInstallWindow(inst);
    CoUninitialize();
    return 0;
}
