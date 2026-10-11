#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include <cstring>
#include <ctime>
#include "resource.h"
#include "app_settings.h"
#include "winui_password.h"
#include "winui_dialog.h"
#include "ui_theme.h"
#include "FileEncryptor_helpers.h"

// 圆角/主题绘制统一由 ui_theme 提供（基于 Direct2D），此处不再单独持有 D2D/DWrite 资源
#include <thread>
#include <atomic>
#include <functional>
#include <mutex>
#include <deque>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

// 强制使用 Windows 子系统（GUI），避免产生控制台
#pragma comment(linker, "/SUBSYSTEM:Windows")

// ---------- 全局变量 ----------
HWND g_hBtn1 = NULL;
HWND g_hBtn2 = NULL;
HWND g_hBtn3 = NULL;    // 安全删除
HWND g_hLangBtn = NULL; // 右上角：设置（齿轮图标；控件 ID 5）
bool g_hoverBtn[8] = {};      // 下标 = 控件 ID，鼠标是否悬停在该按钮上
// 有文件正被拖到「安全删除」按钮上：此时按钮高亮，文字变成「松开删除」
static bool g_dropOnDelete = false;
bool g_trackingMouse = false;
// 进度条改为自绘（标准 PROGRESS 控件做不出“绿色 + 白色扫光”的效果）
static bool      g_progVisible = false;
static ULONGLONG g_progStartTick = 0;   // 本次进度开始时刻，用于估算剩余时间
static float     g_sheenX = 0.0f;       // 高光当前位置（相对进度条宽度的比例 0..1）
static double    g_progShown = 0.0;     // 进度条当前显示值 0..100
static double    g_progTarget = 0.0;    // 进度条目标值 0..100
// 剩余时间文案做节流：数字本身在抖动，每半秒才更新一次
static std::wstring g_etaText;
static ULONGLONG    g_etaLastTick = 0;
std::atomic<bool> g_workerRunning(false);
// 关闭时选择了“后台运行”：窗口隐藏继续处理，任务结束后自动退出
static bool g_runInBackground = false;
static bool g_pendingBackgroundExit = false;   // 后台任务已结束，等弹窗关掉再退出
// 主窗口句柄
HWND g_mainWnd = NULL;

// 语言控制
bool g_langEnglish = false; // false = 中文, true = English
void UpdateLanguageUI();

// 底部状态文本（自绘，以支持深色模式）
std::wstring g_statusText;
// 进度条上方显示的当前文件名（只有文件名，不含路径）
static std::wstring g_progFileName;
// 任务队列计数：已完成 / 总数
static std::atomic<int> g_tasksDone(0);      // 已完成
static std::atomic<int> g_taskRunning(0);    // 1 = 有任务正在执行（这部分原先没被计进总数）


// 自定义消息
#define WM_APP_SET_STATUS    (WM_APP + 1)
#define WM_APP_SHOW_PROGRESS (WM_APP + 2)
#define WM_APP_UPDATE_PROGRESS (WM_APP + 3)
#define WM_APP_SHOW_MESSAGE  (WM_APP + 4)
#define WM_APP_ASK_OUTPUT_PATH (WM_APP + 5)
#define WM_APP_BACKGROUND_DONE (WM_APP + 6)   // “后台运行”的任务已结束，可以退出
#define WM_APP_TRAY            (WM_APP + 7)   // 托盘图标回调
#define WM_APP_SET_FILE        (WM_APP + 8)   // 正在处理的文件名（进度条上方显示）
#define WM_APP_QUEUE_CHANGED   (WM_APP + 9)   // 队列计数变化，刷新显示
#define WM_APP_SECURE_DELETE_DROP (WM_APP + 10)  // 有文件被拖到「安全删除」按钮上并松开
#define WM_APP_LAUNCH          (WM_APP + 11)  // 命令行 / 右键菜单带进来的请求
#define TIMER_BG_EXIT          2              // 等弹窗关闭后再退出后台进程

// “每次都询问”输出位置时，工作线程请求 UI 线程弹一次保存对话框
struct AskOutputPath {
    std::wstring suggested;
    std::wstring result;
    bool cancelled = false;
    HANDLE done = nullptr;   // 自动重置事件
};

// 进度状态：按字节统计“所有待处理文件”的整体进度
std::atomic<uint64_t> g_progTotalBytes(0);  // 本次任务所有输入文件字节总数
std::atomic<uint64_t> g_progDoneBytes(0);   // 已完整处理完的文件字节累计
std::atomic<uint64_t> g_progFileTotal(0);   // 当前文件总字节
std::atomic<uint64_t> g_progFileDone(0);    // 当前文件已处理字节

// 前置声明（在 FileEncryptor 方法中使用）
static void ShowProgressBar(bool show);
static void SetProgressFraction(double overall);
static void ProgressReset(uint64_t totalBytes);
static void ProgressBeginFile(uint64_t fileBytes);
static void ProgressAdd(uint64_t bytes);
// 直接设定本文件已完成的工作量（用于把共用实现上报的字节数归一化）
static void ProgressSetFile(uint64_t doneBytes);
static void ProgressEndFile();
static void ProgressFinish();
static void CollectFilesRecursive(const std::wstring& path, std::vector<std::wstring>& out);
// ---------- 系统托盘 ----------
// 窗口隐藏到后台时用户完全看不到任务状态，所以放一个托盘图标：
// 悬停显示当前进度，左键恢复窗口，右键可显示/退出。
#define TRAY_ICON_ID 1
static bool g_trayAdded = false;

static void TraySetTip(const std::wstring& tip) {
    if (!g_trayAdded || !g_mainWnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_TIP;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void TrayAdd() {
    if (g_trayAdded || !g_mainWnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON));
    wcsncpy_s(nid.szTip, tr(L"文件加密工具 — 正在后台处理…",
                            L"File Encryptor - running in background...").c_str(), _TRUNCATE);
    if (Shell_NotifyIconW(NIM_ADD, &nid)) g_trayAdded = true;
}

static void TrayRemove() {
    if (!g_trayAdded || !g_mainWnd) return;
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_mainWnd;
    nid.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = false;
}

// 从托盘恢复窗口（恢复后不再自动退出）
static void RestoreFromTray(HWND hWnd) {
    g_runInBackground = false;
    TrayRemove();
    ShowWindow(hWnd, SW_SHOW);
    ShowWindow(hWnd, SW_RESTORE);
    SetForegroundWindow(hWnd);
    InvalidateRect(hWnd, NULL, FALSE);
}
static void SecureDeleteFile(const std::wstring& wPath, int passes,
                             const std::function<void(uint64_t)>& onProgress = {});

// 防暴力破解策略：加密时写入文件头，由解密方读取并执行
// （定义放在这里，因为下面几个前置声明要用到）
enum class ProtectionAction { Delete = 0, Lock = 1 };

struct Protection {
    bool             enabled = false;
    int              maxTries = 3;                  // 1..9999
    ProtectionAction action = ProtectionAction::Delete;
    int              lockDays = 7;                  // 0..3650
};

static void StartProcessing(std::vector<std::wstring> files,
                            std::vector<std::string> perFilePasswords,
                            std::string singlePassword,
                            std::vector<Protection> perFileProts,
                            Protection singleProt,
                            std::wstring decoyFile = std::wstring(),
                            std::string  decoyPwd = std::string());
static void ShowUiMessage(HWND owner, UiIcon icon, const std::wstring& caption,
                          const std::wstring& text, bool topmost);
static void ApplyProgressBarTheme();
static void ShowSettingsDialog(HWND owner);
static LRESULT HandleAskOutputPath(HWND hWnd, LPARAM lParam);
static std::wstring PasswordSubtitle(const std::wstring& file, size_t index, size_t total);

// 收集密码的结果
enum class PwdCollect { Ok, Cancelled, Blocked };

// ==================== 资源管理器右键菜单：启动请求 ====================
// 类型定义放在这里（而不是 WinMain 旁边），因为 WndProc 的 WM_COPYDATA 要用到。
enum class LaunchKind { None = 0, Encrypt = 1, SecureDelete = 2 };

struct LaunchRequest {
    LaunchKind kind = LaunchKind::None;
    std::vector<std::wstring> files;
};

static const DWORD kLaunchCopyDataId = 0x46452E31;   // FE.1

// 拖放 / 启动请求共用的暂存（都在主线程操作，但转发时可跨线程到达）
static std::mutex                g_dropMutex;
static std::vector<std::wstring> g_droppedForDelete;
static std::vector<std::wstring> g_pendingLaunch;   // 主窗口建好之前的启动参数
static int                       g_pendingKind = 0;
static bool                      g_hasPendingLaunch = false;

// WM_DROPFILES 用的内存布局。
// 不直接用 shellapi.h 的 DROPFILES：不同 SDK 版本里的可见性不一致，自己定义更稳。
struct DropFilesHeader {
    DWORD pFiles;   // 文件列表相对本结构起点的字节偏移
    POINT pt;
    BOOL  fNC;
    BOOL  fWide;    // TRUE = 宽字符路径
};

// 安全删除：先弹二次确认，用户点是才真正入队
static void ConfirmAndSecureDelete(HWND owner, std::vector<std::wstring> files);
static void StartSecureDelete(std::vector<std::wstring> files, int passes);
// 定义在 WinMain 附近，这里先声明供 WM_COPYDATA 使用
static bool UnpackLaunch(const BYTE* data, DWORD size, LaunchRequest& out);
// 同样定义在 WinMain 附近；设置对话框保存后要立即应用右键菜单开关
static void ApplyShellMenuSetting();

// 把文件列表合成一个 HDROP 投给主窗口的 WM_DROPFILES。
// 这样右键菜单的「加密」和直接拖放走的是**完全同一条**流程，不会漂移。
// 接收方会调用 DragFinish 释放这块内存，所以所有权一并交出去。
static void PostFilesAsDrop(HWND hWnd, const std::vector<std::wstring>& files) {
    if (files.empty()) return;
    size_t chars = 1;                                  // 结尾双 \0
    for (const auto& f : files) chars += f.size() + 1;
    const size_t bytes = sizeof(DropFilesHeader) + chars * sizeof(wchar_t);

    HGLOBAL hMem = GlobalAlloc(GHND, bytes);
    if (!hMem) return;
    BYTE* base = (BYTE*)GlobalLock(hMem);
    if (!base) { GlobalFree(hMem); return; }

    DropFilesHeader* df = (DropFilesHeader*)base;
    df->pFiles = sizeof(DropFilesHeader);
    df->fWide = TRUE;
    wchar_t* p = (wchar_t*)(base + sizeof(DropFilesHeader));
    for (const auto& f : files) {
        memcpy(p, f.c_str(), (f.size() + 1) * sizeof(wchar_t));
        p += f.size() + 1;
    }
    *p = L'\0';
    GlobalUnlock(hMem);

    PostMessageW(hWnd, WM_DROPFILES, (WPARAM)hMem, 0);
}

// 右键多选时资源管理器会**为每个文件各启动一个进程**，转发过来的请求会密集到达。
// 用一个短定时器把它们合并，这样只弹**一次**确认框、只建**一个**任务。
#define TIMER_LAUNCH_GROUP  3
#define LAUNCH_DEBOUNCE_MS  400

static std::vector<std::wstring> g_launchFiles;
static int                       g_launchKind = 0;

// 收集一批启动请求；定时器到点后才真正处理
static void QueueLaunchFiles(HWND hWnd, const std::vector<std::wstring>& files, int kind) {
    if (files.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_dropMutex);
        if (g_launchFiles.empty()) g_launchKind = kind;   // 类型以第一批为准
        for (const auto& f : files) g_launchFiles.push_back(f);
    }
    // 每来一个请求就重置定时器，直到不再有新进程到来
    SetTimer(hWnd, TIMER_LAUNCH_GROUP, LAUNCH_DEBOUNCE_MS, NULL);
}

// 处理命令行 / 右键菜单进来的请求
static void DispatchLaunch(HWND hWnd, const std::vector<std::wstring>& files, int kind) {
    if (files.empty()) return;
    // 从托盘/最小化恢复，确认框才看得见
    if (IsIconic(hWnd)) ShowWindow(hWnd, SW_RESTORE);
    ShowWindow(hWnd, SW_SHOW);
    SetForegroundWindow(hWnd);

    if (kind == 2) {                 // --secure-delete
        ConfirmAndSecureDelete(hWnd, files);
    } else if (kind == 1) {          // --encrypt
        PostFilesAsDrop(hWnd, files);
    }
}

// 加密前收集密码，同时让用户配置该文件的防暴力破解策略
static bool CollectEncryptPassword(HWND owner, const std::wstring& subtitle,
                                   std::string& outPwd, Protection& outProt,
                                   bool allowProtection,
                                   bool allowDecoy = false,
                                   std::wstring* outDecoyFile = nullptr,
                                   std::string* outDecoyPwd = nullptr);
// 解密前收集密码：受保护文件会显示剩余次数，输错则扣减并重新弹框。
// blockedMsg 在返回 Blocked 时给出原因（锁定中 / 次数用尽）。
static PwdCollect CollectDecryptPassword(HWND owner, const std::wstring& file,
                                         std::string& outPwd, std::wstring& blockedMsg);

// 一次加/解密任务。多次提交时进入队列，由同一个 worker 顺序处理，
// 因此“有任务在跑时再次创建任务”不再报错，而是排队等待。
struct TaskRequest {
    std::vector<std::wstring> files;
    std::vector<std::string>  perFilePasswords;
    std::string               singlePassword;
    std::vector<Protection>   perFileProts;
    Protection                singleProt;
    std::wstring              decoyFile;
    std::string               decoyPwd;
    // 「安全删除」任务：不解密、不加密，只按 deletePasses 覆写删除。
    // 复用同一个队列，因此批量、进度条、后台托盘、队列计数全部自动具备。
    bool                      isSecureDelete = false;
    int                       deletePasses = 0;
};

// 待处理任务队列（std::deque 由 g_queueMutex 保护）。
// 放在 TaskRequest 之后：它要用到这个类型。
static std::mutex g_queueMutex;
static std::deque<TaskRequest> g_taskQueue;

// 投递到主线程显示的提示框负载
struct UiMessagePayload {
    UiIcon icon;
    std::wstring caption;
    std::wstring text;
    bool topmost;
};

// UTF-8 <-> UTF-16 辅助
static std::wstring utf8_to_wstring(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (len == 0) return {};
    std::wstring w; w.resize(len);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

// 智能转换：优先尝试将窄字符串按 UTF-8 解码；若失败则按当前 ANSI 代码页解码
// 将当前 ANSI 多字节字符串转换为宽字符串（根据系统区域设置）
static std::wstring ansi_to_wstring(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), NULL, 0);
    if (len == 0) return {};
    std::wstring w; w.resize(len);
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &w[0], len);
    return w;
}

// 智能转换：优先尝试将窄字符串按 UTF-8 解码；若失败则按当前 ANSI 代码页解码
static std::wstring narrow_to_wstring(const std::string& s) {
    if (s.empty()) return {};
    // 先尝试 UTF-8
    int lenUtf8 = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), (int)s.size(), NULL, 0);
    if (lenUtf8 > 0) {
        std::wstring w; w.resize(lenUtf8);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], lenUtf8);
        return w;
    }
    // 回退到 ANSI
    return ansi_to_wstring(s);
}

static std::string wstring_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (len == 0) return {};
    std::string s; s.resize(len);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], len, NULL, NULL);
    return s;
}

// ---------- 密码对话框 ----------
// 历史实现是一个资源模板对话框（IDD_PASSWORD_DIALOG）并且在 x64 配置下会
// 再启动一个自身进程来显示，导致无法跟随语言与主题切换。
// 现在统一交给进程内的 WinUI 3 风格自绘对话框层（winui_dialog.cpp）。
bool ShowPasswordDialog(HWND hWnd, std::string& password, bool confirm,
                        const std::wstring& subtitle = std::wstring(),
                        bool showProtection = false,
                        UiProtection* ioProt = nullptr,
                        int remainTries = -1,
                        bool showDecoy = false,
                        bool* ioUseDecoy = nullptr) {
    return ShowPasswordDialogWinUI(hWnd, password, confirm, subtitle,
                                   showProtection, ioProt, remainTries,
                                   showDecoy, ioUseDecoy);
}


// ---------- 加密核心类 ----------
// 文件格式 v2（AES-256-GCM，与 CLI 版完全一致）：
//   0  magic[8]      "FENC\r\n\x1a\n"
//   8  version[1]    = 2
//   9  flags[1]      bit0 启用防暴力破解, bit1 模式(0=永久删除, 1=限时锁定)
//  10  iterations[4] PBKDF2-HMAC-SHA256 迭代次数（小端），即“加密强度”
//  14  maxTries[2]   允许的错误尝试次数
//  16  lockDays[2]   限时锁定模式下的锁定时长（天）
//  18  salt[16]
//  34  iv[12]        GCM 推荐 96-bit nonce
//  46  verifier[8]   密码验证标签（快速判断密码对错，无需解密正文）
//  54  remaining[2]  ← 可变（每次解密失败 -1）
//  56  lockUntil[8]  ← 可变（Unix 秒，0 = 未锁定）
//  64  ciphertext[N] 与明文等长（GCM 是流模式，无需填充）
//  末尾 tag[16]      GCM 认证标签
//
// 偏移 8..54 作为 AAD 参与认证，保护策略无法被篡改后关闭；
// 偏移 54..64 需在无正确密码时可写，故不参与认证。
// 只支持 v2；历史格式（v1 / 最旧的 CBC）不再读取。
//
// 从加密文件头读出的保护状态
struct FileProtState {
    bool     isV2 = false;
    bool     isProtected = false;
    ProtectionAction action = ProtectionAction::Delete;
    int      maxTries = 0;
    int      remaining = 0;
    int      lockDays = 0;
    uint64_t lockUntil = 0;                         // Unix 秒，0 = 未锁定
};

// 快速校验密码的结果
enum class ProtVerify { Ok, Wrong, Unavailable };

// 把剩余秒数格式化成“X 天 Y 小时 Z 分钟”，用于锁定期提示
static std::wstring FormatDuration(uint64_t seconds) {
    const uint64_t days = seconds / 86400;
    const uint64_t hours = (seconds % 86400) / 3600;
    const uint64_t mins = (seconds % 3600) / 60;
    std::wstring s;
    if (days > 0)  s += std::to_wstring(days) + tr(L" 天 ", L"d ");
    if (hours > 0) s += std::to_wstring(hours) + tr(L" 小时 ", L"h ");
    if (days == 0 && mins > 0) s += std::to_wstring(mins) + tr(L" 分钟", L"min");
    if (s.empty()) s = tr(L"不到 1 分钟", L"less than a minute");
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s;
}

// 文件格式（magic / 头部布局 / 偏移 / 标志位 / AAD 范围 / verifier 标签 /
// 字节序读写）统一定义在 file_format.h，与 CLI 共用同一份实现 —— 避免两份漂移。
// 注意：这里**不要** using namespace fe —— GUI 自己有 Protection / FormatDuration
// 等同名符号，放开命名空间会撞名。所有共用实现一律显式写 fe::。
#include "file_format.h"
#include "file_crypto.h"   // 加解密核心的唯一实现（与 CLI 共用）
// 取文件字节数；失败返回 0
static uint64_t FileSizeOfPath(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER li = {};
    const bool ok = GetFileSizeEx(h, &li) && li.QuadPart > 0;
    CloseHandle(h);
    return ok ? (uint64_t)li.QuadPart : 0;
}

class FileEncryptor {
private:
    struct HandleGuard {
        HANDLE h;
        explicit HandleGuard(HANDLE x) : h(x) {}
        ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        HandleGuard(const HandleGuard&) = delete;
        HandleGuard& operator=(const HandleGuard&) = delete;
    };

    // 格式常量与字节序读写见文件作用域的 file_format.h（与 CLI 共用）


    // 密钥派生 / verifier 计算已统一到 file_crypto.cpp，这里不再保留副本

    static void DecryptImpl(const std::wstring& input_path, const std::wstring& out_path,
                            const std::string& password);

public:
    // ---- 防暴力破解：读取 / 扣减 / 快速校验 ----
    // 这些都在 UI 线程的“收集密码”阶段调用，不进 worker。

    static uint64_t NowUnix() { return (uint64_t)time(nullptr); }

    // 读取文件头中的保护状态；返回 false 表示不是有效的 v2 加密文件
    static bool ReadProtState(const std::wstring& path, FileProtState& out) {
        fe::FileState st;
        if (!fe::ReadProtState(path, st)) return false;
        out.isV2        = true;
        out.isProtected = st.isProtected;
        out.action      = (st.action == fe::EXHAUST_LOCK) ? ProtectionAction::Lock
                                                          : ProtectionAction::Delete;
        out.maxTries    = st.maxTries;
        out.remaining   = st.remaining;
        out.lockDays    = st.lockDays;
        out.lockUntil   = st.lockUntil;
        return true;
    }
    static bool WriteMutableState(const std::wstring& path, int remaining, uint64_t lockUntil) {
        return fe::WriteMutableState(path, remaining, lockUntil);
    }
    static ProtVerify VerifyPassword(const std::wstring& path, const std::string& password,
                                     FileProtState& out) {
        fe::FileState st;
        const fe::VerifyResult r = fe::VerifyPassword(path, password, st);
        if (r == fe::VerifyResult::Unavailable) return ProtVerify::Unavailable;
        out.isV2        = true;
        out.isProtected = st.isProtected;
        out.action      = (st.action == fe::EXHAUST_LOCK) ? ProtectionAction::Lock
                                                          : ProtectionAction::Delete;
        out.maxTries    = st.maxTries;
        out.remaining   = st.remaining;
        out.lockDays    = st.lockDays;
        out.lockUntil   = st.lockUntil;
        return (r == fe::VerifyResult::Ok) ? ProtVerify::Ok : ProtVerify::Wrong;
    }
    static int ConsumeAttempt(const std::wstring& path, const FileProtState& st,
                              std::wstring& outMsg) {
        fe::FileState fst;
        fst.isProtected = st.isProtected;
        fst.action      = (st.action == ProtectionAction::Lock) ? fe::EXHAUST_LOCK
                                                                : fe::EXHAUST_DELETE;
        fst.maxTries    = st.maxTries;
        fst.remaining   = st.remaining;
        fst.lockDays    = st.lockDays;
        fst.lockUntil   = st.lockUntil;
        std::string msg;
        const int r = fe::ConsumeAttempt(path, fst, msg);
        outMsg = utf8_to_wstring(msg);
        return r;
    }
    static void encryptFileTo(const std::wstring& input_path, const std::wstring& out_path,
                              const std::string& password, int iterations,
                              const Protection& prot,
                              const std::wstring& decoy_path = std::wstring(),
                              const std::string& decoy_password = std::string(),
                              bool single_slot = false) {
        fe::Protection fp;
        fp.enabled  = prot.enabled;
        fp.maxTries = prot.maxTries;
        fp.action   = (prot.action == ProtectionAction::Lock) ? fe::EXHAUST_LOCK
                                                              : fe::EXHAUST_DELETE;
        fp.lockDays = prot.lockDays;

        // 把共用实现上报的 (cur/total) 归一化成"本文件 = 1 个单位"。
        // 直接用它上报的字节数会在保留源文件（总量只算 1 倍）时提前冲到 100%。
        const uint64_t unit = FileSizeOfPath(input_path);
        const double u = (double)(unit ? unit : 1);
        bool started = false;
        fe::ProgressFn prog = [&](uint64_t cur, uint64_t total) {
            if (!started) { ProgressBeginFile(unit ? unit : 1); started = true; }
            ProgressSetFile(total ? (uint64_t)((double)cur / (double)total * u) : 0);
        };
        fe::EncryptFileTo(input_path, out_path, password, iterations, fp,
                          decoy_path, decoy_password, prog, single_slot);
        if (!started) ProgressBeginFile(unit ? unit : 1);
        ProgressSetFile(unit ? unit : 1);
        ProgressEndFile();
    }
    static void decryptFileTo(const std::wstring& input_path, const std::wstring& out_path,
                              const std::string& password) {
        try {
            DecryptImpl(input_path, out_path, password);
        }
        catch (...) {
            // 密码错误 / 文件被篡改 / 中途失败：不要留下残破的输出文件
            DeleteFileW(out_path.c_str());
            throw;
        }
    }
};

void FileEncryptor::DecryptImpl(const std::wstring& input_path, const std::wstring& out_path,
                                const std::string& password) {
    // 同理归一化：.enc 里有两个槽，共用实现只解其中一个，
    // 直接用它的字节数会让进度停在约 50% 再跳。
    const uint64_t unit = FileSizeOfPath(input_path);
    const double u = (double)(unit ? unit : 1);
    bool started = false;
    fe::ProgressFn prog = [&](uint64_t cur, uint64_t total) {
        if (!started) { ProgressBeginFile(unit ? unit : 1); started = true; }
        ProgressSetFile(total ? (uint64_t)((double)cur / (double)total * u) : 0);
    };
    fe::DecryptFileTo(input_path, out_path, password, prog);
    if (!started) ProgressBeginFile(unit ? unit : 1);
    ProgressSetFile(unit ? unit : 1);
    ProgressEndFile();
}
// 安全删除：passes 为覆写次数（0 = 直接删除）
static void SecureDeleteFile(const std::wstring& wPath, int passes,
                             const std::function<void(uint64_t)>& onProgress) {
    if (onProgress) {
        // 共用实现上报的是**累计**字节数，而 onProgress 的约定是“本次新增”。
        // 必须在这里转成增量 —— 否则调用方会把累计值当增量反复累加，
        // 进度呈平方级暴涨（10GB 覆写只写几十 MB 就冲到 100%）。
        uint64_t last = 0;
        fe::SecureDeleteFile(wPath, passes, [&onProgress, &last](uint64_t cur, uint64_t) {
            if (cur > last) { onProgress(cur - last); last = cur; }
        });
    } else {
        fe::SecureDeleteFile(wPath, passes);
    }
}
std::wstring g_exe_dir;

std::wstring GetExeDirectoryW() {
    wchar_t buffer[MAX_PATH];
    GetModuleFileNameW(NULL, buffer, MAX_PATH);
    std::wstring exe_path(buffer);
    size_t pos = exe_path.find_last_of(L"\\/");
    if (pos != std::wstring::npos) {
        return exe_path.substr(0, pos + 1);
    }
    return L"";
}

// 打开文件：解密时只过滤 *.enc，加密时可选所有文件
std::wstring OpenFileDialog(bool forDecrypt) {
    OPENFILENAMEW ofn = { 0 };
    wchar_t szFile[1024] = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    // 过滤器是“显示名\0通配符\0显示名\0通配符\0\0”的双零结尾串
    static std::wstring filter;
    if (forDecrypt) {
        filter = tr(L"加密文件 (*.enc)", L"Encrypted files (*.enc)") + std::wstring(1, L'\0') + L"*.enc" +
                 std::wstring(1, L'\0') +
                 tr(L"所有文件 (*.*)", L"All files (*.*)") + std::wstring(1, L'\0') + L"*.*" +
                 std::wstring(2, L'\0');
    }
    else {
        filter = tr(L"所有文件 (*.*)", L"All files (*.*)") + std::wstring(1, L'\0') + L"*.*" +
                 std::wstring(2, L'\0');
    }
    ofn.lpstrFilter = filter.c_str();
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) return std::wstring(szFile);
    return L"";
}

// “每次都询问”输出位置时使用
std::wstring SaveFileDialog(const std::wstring& suggestedName) {
    OPENFILENAMEW ofn = { 0 };
    wchar_t szFile[1024] = { 0 };
    wcsncpy_s(szFile, suggestedName.c_str(), _TRUNCATE);
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"*.*\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_EXPLORER;
    if (GetSaveFileNameW(&ofn)) return std::wstring(szFile);
    return L"";
}

// 选择“指定目录”时使用
std::wstring PickFolderDialog(HWND owner, const std::wstring& initial) {
    std::wstring result;
    IFileDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        if (!initial.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), NULL, IID_PPV_ARGS(&item))) && item) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    return result;
}

// 安全删除用的**多选**文件选择器（加密/解密按钮是单选，删除要批量更顺手）
static std::vector<std::wstring> PickFilesForDelete(HWND owner) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opts = 0;
        if (SUCCEEDED(dlg->GetOptions(&opts))) {
            dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        }
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dlg->GetResults(&items)) && items) {
                DWORD n = 0;
                items->GetCount(&n);
                for (DWORD i = 0; i < n; ++i) {
                    IShellItem* it = nullptr;
                    if (SUCCEEDED(items->GetItemAt(i, &it)) && it) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                            out.push_back(path);
                            CoTaskMemFree(path);
                        }
                        it->Release();
                    }
                }
                items->Release();
            }
        }
        dlg->Release();
    }
    return out;
}

// 状态文本由主窗口自绘（见 WM_PAINT），以便跟随深浅色主题
void SetStatusText(const std::wstring& text) {
    g_statusText = text;
    if (g_mainWnd) {
        RECT rc;
        GetClientRect(g_mainWnd, &rc);
        RECT strip = { 0, rc.bottom - UiPx(30), rc.right, rc.bottom };
        InvalidateRect(g_mainWnd, &strip, FALSE);
    }
}

static void ShowProgressBar(bool show) {
    if (g_mainWnd) {
        PostMessageW(g_mainWnd, WM_APP_SHOW_PROGRESS, show ? 1 : 0, 0);
    }
    else {
        g_progVisible = show;
    }
}

// 按“所有文件的总字节数”折算整体进度（0.0 ~ 1.0），再投递给 UI 线程
static void SetProgressFraction(double overall) {
    int pos = (int)(overall * 100.0 + 0.5);
    if (pos < 0) pos = 0;
    if (pos > 100) pos = 100;
    if (g_mainWnd) {
        PostMessageW(g_mainWnd, WM_APP_UPDATE_PROGRESS, (WPARAM)pos, 0);
    }
    else {
        g_progTarget = (double)pos;
    }
}

// ---- 进度生命周期（工作线程调用）----

static void ProgressReset(uint64_t totalBytes) {
    g_progTotalBytes = totalBytes ? totalBytes : 1;
    g_progDoneBytes = 0;
    g_progFileTotal = 0;
    g_progFileDone = 0;
}

static void ProgressBeginFile(uint64_t fileBytes) {
    g_progFileTotal = fileBytes;
    g_progFileDone = 0;
    SetProgressFraction((double)g_progDoneBytes.load() / (double)g_progTotalBytes.load());
}

static void ProgressAdd(uint64_t bytes) {
    g_progFileDone += bytes;
    const uint64_t overall = g_progDoneBytes.load() + g_progFileDone.load();
    SetProgressFraction((double)overall / (double)g_progTotalBytes.load());
}

// 直接设定"本文件已完成的工作量"。
// v3 会写两段密文（真实载荷 + 诱饵/随机填充），实际字节数约为文件的两倍，
// 所以不能拿共用实现上报的字节数直接当进度，必须按比例归一到文件大小，
// 才能和 ProgressReset 里 (文件大小 ×(1+覆写次数)) 的口径对齐。
static void ProgressSetFile(uint64_t doneBytes) {
    g_progFileDone = doneBytes;
    const uint64_t overall = g_progDoneBytes.load() + g_progFileDone.load();
    SetProgressFraction((double)overall / (double)g_progTotalBytes.load());
}

static void ProgressEndFile() {
    g_progDoneBytes += g_progFileTotal.load();
    g_progFileTotal = 0;
    g_progFileDone = 0;
    SetProgressFraction((double)g_progDoneBytes.load() / (double)g_progTotalBytes.load());
}

static void ProgressFinish() {
    SetProgressFraction(1.0);
    ShowProgressBar(false);
}

// 递归收集文件
static void CollectFilesRecursive(const std::wstring& path, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    std::wstring search = path + L"\\*";
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring full = path + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectFilesRecursive(full, out);
        }
        else {
            out.push_back(full);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------- 窗口布局 ----------
// 主窗口尺寸是固定的（不可拉伸），但会在“空闲”和“工作中”两种高度之间切换：
// 空闲时紧凑，工作时才为进度条与取消按钮让出空间，避免下方留一大片空白。
#define ANIM_TIMER_ID 1
#define ANIM_FRAME_MS 15

static bool      g_animRunning = false;
static ULONGLONG g_animLastTick = 0;
static float     g_btnHover[8] = {};          // 下标 = 控件 ID，0..1 悬停渐变
static int       g_winHFrom = 0, g_winHTo = 0;
static float     g_winHT = 1.0f;              // 高度过渡进度 0..1

static void StartAnimation();

// 主界面按钮的子类化过程：按钮自己跟踪鼠标进入/离开。
// 之前靠父窗口的 WM_MOUSEMOVE 判断，但鼠标落在子控件上时父窗口收不到该消息，
// 导致主界面按钮实际上从来没有悬停高亮。
// ==================== 「安全删除」按钮的 OLE 拖放目标 ====================
//
// 为什么要用 IDropTarget 而不是 WM_DROPFILES：
// 需求是「文件拖到按钮**上方**时按钮就高亮、文字变成松开删除」。
// DragAcceptFiles/WM_DROPFILES 只在**松手那一刻**才给一次消息，
// 拿不到「拖拽经过」的时机；OLE 的 DragEnter/DragOver/DragLeave 才可以。

// 切换按钮的「拖拽悬停」外观
static void SetDropOnDelete(bool on) {
    if (g_dropOnDelete == on) return;
    g_dropOnDelete = on;
    if (g_hBtn3) {
        SetWindowTextW(g_hBtn3, on ? tr(L"松开删除", L"Release to delete").c_str()
                                   : tr(L"安全删除", L"Secure delete").c_str());
        InvalidateRect(g_hBtn3, NULL, FALSE);
    }
}

// 数据对象里是否有文件（CF_HDROP）
static bool DropHasFiles(IDataObject* data) {
    if (!data) return false;
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    return data->QueryGetData(&fmt) == S_OK;
}

// 取出拖进来的文件（目录会递归展开）
static bool DropExtractFiles(IDataObject* data, std::vector<std::wstring>& out) {
    if (!data) return false;
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med = {};
    if (data->GetData(&fmt, &med) != S_OK) return false;

    bool ok = false;
    HDROP hDrop = (HDROP)GlobalLock(med.hGlobal);
    if (hDrop) {
        const UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < count; ++i) {
            wchar_t buf[MAX_PATH * 4];
            if (DragQueryFileW(hDrop, i, buf, _countof(buf))) {
                std::wstring p(buf);
                const DWORD attrs = GetFileAttributesW(p.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                    CollectFilesRecursive(p, out);
                else
                    out.push_back(p);
            }
        }
        GlobalUnlock(med.hGlobal);
        ok = true;
    }
    ReleaseStgMedium(&med);
    return ok;
}

class DeleteDropTarget : public IDropTarget {
public:
    explicit DeleteDropTarget(HWND owner) : owner_(owner) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return (ULONG)InterlockedIncrement(&ref_);
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = (ULONG)InterlockedDecrement(&ref_);
        if (n == 0) delete this;
        return n;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        const bool has = DropHasFiles(data);
        SetDropOnDelete(has);
        if (effect) *effect = has ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (effect) *effect = DROPEFFECT_COPY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        SetDropOnDelete(false);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        SetDropOnDelete(false);
        if (effect) *effect = DROPEFFECT_COPY;

        std::vector<std::wstring> files;
        if (!DropExtractFiles(data, files) || files.empty()) return S_OK;

        // 先把待删清单存好，再通知主线程 —— 顺序反了确认框会读到空列表
        {
            std::lock_guard<std::mutex> lk(g_dropMutex);
            g_droppedForDelete = std::move(files);
        }
        if (owner_) PostMessageW(owner_, WM_APP_SECURE_DELETE_DROP, 0, 0);
        return S_OK;
    }

private:
    LONG ref_ = 1;
    HWND owner_ = nullptr;
};

// 全局持有：Release 时自删，这里存裸指针即可
static DeleteDropTarget* g_deleteDropTarget = nullptr;

static void RegisterDeleteDropTarget(HWND owner, HWND btn) {
    if (!btn) return;
    OleInitialize(NULL);   // 幂等；已初始化会返回 S_FALSE
    g_deleteDropTarget = new DeleteDropTarget(owner);
    if (RegisterDragDrop(btn, g_deleteDropTarget) != S_OK) {
        g_deleteDropTarget->Release();
        g_deleteDropTarget = nullptr;
    }
}

static LRESULT CALLBACK MainBtnSubclassProc(HWND h, UINT msg, WPARAM wParam, LPARAM lParam,
                                            UINT_PTR id, DWORD_PTR) {
    const int ctl = (int)id;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (ctl >= 0 && ctl < 8 && !g_hoverBtn[ctl]) {
            g_hoverBtn[ctl] = true;
            StartAnimation();
        }
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
        }
        break;
    case WM_MOUSELEAVE:
        if (ctl >= 0 && ctl < 8 && g_hoverBtn[ctl]) {
            g_hoverBtn[ctl] = false;
            StartAnimation();
        }
        break;
    default:
        break;
    }
    return DefSubclassProc(h, msg, wParam, lParam);
}

// 主按钮宽度：按当前语言文字实测，只留较小的内边距，让主界面更紧凑
static int MainButtonWidth() {
    HDC dc = GetDC(NULL);
    if (!dc) return UiPx(112);
    HFONT font = UiFont(UiPx(15), false, true);   // 与按钮文字同一字体（灰度抗锯齿）
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;

    auto measure = [&](const std::wstring& s) {
        RECT r = { 0, 0, 0, 0 };
        DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        return r.right - r.left;
    };
    int textW = measure(tr(L"加密文件", L"Encrypt file"));
    const int w2 = measure(tr(L"解密文件", L"Decrypt file"));
    if (w2 > textW) textW = w2;
    // 安全删除按钮的文案更长（"松开删除" 也要放得下），一并计入
    const int w3 = measure(tr(L"安全删除", L"Secure delete"));
    if (w3 > textW) textW = w3;
    const int w3b = measure(tr(L"松开删除", L"Release to delete"));
    if (w3b > textW) textW = w3b;

    if (old) SelectObject(dc, old);
    ReleaseDC(NULL, dc);

    const int bw = textW + UiPx(52);   // 左右各 26px 内边距
    return bw > UiPx(108) ? bw : UiPx(108);
}

static int MainClientWidth() {
    // 三个按钮并排 + 两个间距 + 左右留白
    return MainButtonWidth() * 3 + UiPx(16) * 2 + UiPx(2 * 28);
}

// 顶部第一行留给右上角的设置按钮，主按钮放在它下方，两者不重叠
static const int kMainBtnY = 46;    // 96dpi 设计值

// 进度条的绘制位置（主窗口客户区坐标）；WM_PAINT 与布局共用，避免两处算不一致
static int ProgressRectTop() { return UiPx(kMainBtnY) + UiPx(44) + UiPx(16); }

static int MainClientHeight(bool busy) {
    const int btnY = UiPx(kMainBtnY);
    const int btnH = UiPx(44);
    if (!busy) return btnY + btnH + UiPx(20);   // 已去掉底部状态栏

    // 工作中：文件名行 + 进度条 + 下方“进度 / 预计剩余时间（左）+ 队列计数（右）”
    return ProgressRectTop() + UiPx(18) + UiPx(6) + UiPx(18) + UiPx(6) + UiPx(18) + UiPx(16);
}

// 三行：文件名（进度条上方）、进度条、信息行（下方）
static void ProgressRects(const RECT& rc, RECT& file, RECT& bar, RECT& info) {
    const int pad = UiPx(24);
    const int top = ProgressRectTop();
    file = { pad, top, rc.right - pad, top + UiPx(18) };
    bar  = { pad, file.bottom + UiPx(6), rc.right - pad, file.bottom + UiPx(6) + UiPx(18) };
    info = { pad, bar.bottom + UiPx(6), rc.right - pad, bar.bottom + UiPx(6) + UiPx(18) };
}

// 进度条填充色：浅色主题用 Windows 那种较深的绿，深色主题用亮一点的绿
static COLORREF ProgressGreen() {
    return UiIsDark() ? RGB(0x6C, 0xCB, 0x5E) : RGB(0x10, 0x7C, 0x10);
}

// 剩余时间文案
static std::wstring FormatEta(double seconds) {
    if (seconds < 1.0) return tr(L"不到 1 秒", L"<1s");
    const int s = (int)(seconds + 0.5);
    if (s < 60) return std::to_wstring(s) + tr(L" 秒", L"s");
    const int m = s / 60;
    const int r = s % 60;
    if (m < 60) return std::to_wstring(m) + tr(L" 分 ", L"m ") + std::to_wstring(r) + tr(L" 秒", L"s");
    return std::to_wstring(m / 60) + tr(L" 小时 ", L"h ") + std::to_wstring(m % 60) + tr(L" 分", L"m");
}

static void SetBusyLayout(bool busy) {
    if (!g_mainWnd) return;
    RECT rc;
    GetClientRect(g_mainWnd, &rc);
    const int target = MainClientHeight(busy);
    const int current = rc.bottom - rc.top;
    if (current == target && g_winHT >= 1.0f) return;

    // 在两种高度之间做 ease-out 过渡，而不是硬跳
    g_winHFrom = current;
    g_winHTo = target;
    g_winHT = (current == target) ? 1.0f : 0.0f;
    if (g_winHT < 1.0f) StartAnimation();
}

// 语言切换会改变按钮文字宽度，这里同步收紧窗口宽度
static void ApplyMainWindowWidth() {
    if (!g_mainWnd) return;
    RECT rc;
    GetClientRect(g_mainWnd, &rc);
    const int wantW = MainClientWidth();
    if (rc.right == wantW) return;

    RECT r = { 0, 0, wantW, rc.bottom };
    const DWORD style = (DWORD)GetWindowLongPtrW(g_mainWnd, GWL_STYLE);
    const DWORD exStyle = (DWORD)GetWindowLongPtrW(g_mainWnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    SetWindowPos(g_mainWnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------- 动画 ----------
// 一个 15ms 的定时器同时驱动三类过渡：按钮悬停、窗口高度、进度条数值。
// 没有动画在进行时定时器会被关掉，不产生额外开销。

static void StartAnimation() {
    if (!g_mainWnd || g_animRunning) return;
    g_animRunning = true;
    g_animLastTick = 0;
    SetTimer(g_mainWnd, ANIM_TIMER_ID, ANIM_FRAME_MS, NULL);
}

static void ApplyClientHeight(int clientH) {
    if (!g_mainWnd || clientH <= 0) return;
    RECT rc;
    GetClientRect(g_mainWnd, &rc);
    RECT r = { 0, 0, rc.right, clientH };
    const DWORD style = (DWORD)GetWindowLongPtrW(g_mainWnd, GWL_STYLE);
    const DWORD exStyle = (DWORD)GetWindowLongPtrW(g_mainWnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, style, FALSE, exStyle);
    SetWindowPos(g_mainWnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void TickAnimation(HWND hWnd) {
    const ULONGLONG now = GetTickCount64();
    float dt = g_animLastTick ? (float)(now - g_animLastTick) / 1000.0f
                              : (float)ANIM_FRAME_MS / 1000.0f;
    g_animLastTick = now;
    if (dt <= 0.0f) dt = 0.001f;
    if (dt > 0.12f) dt = 0.12f;   // 防止卡顿后跳变

    bool active = false;

    // 1) 按钮悬停渐变（约 120ms）
    struct BtnTarget { int id; HWND hwnd; float target; };
    const BtnTarget targets[] = {
        { 1, g_hBtn1,    g_hoverBtn[1] ? 1.0f : 0.0f },
        { 2, g_hBtn2,    g_hoverBtn[2] ? 1.0f : 0.0f },
        { 3, g_hBtn3,    g_hoverBtn[3] ? 1.0f : 0.0f },
        { 5, g_hLangBtn, g_hoverBtn[5] ? 1.0f : 0.0f },
    };
    const float hoverStep = dt / 0.12f;
    for (const BtnTarget& t : targets) {
        if (t.id < 0 || t.id > 7) continue;
        float& v = g_btnHover[t.id];
        if (v == t.target) continue;
        if (t.target > v) { v += hoverStep; if (v > t.target) v = t.target; }
        else              { v -= hoverStep; if (v < t.target) v = t.target; }
        if (t.hwnd) InvalidateRect(t.hwnd, NULL, FALSE);
        if (v != t.target) active = true;
    }

    // 2) 窗口高度过渡（约 180ms，ease-out）
    if (g_winHT < 1.0f) {
        g_winHT += dt / 0.18f;
        if (g_winHT >= 1.0f) {
            g_winHT = 1.0f;
            ApplyClientHeight(g_winHTo);   // 收尾精确对齐
        }
        else {
            const float e = UiEaseOutCubic(g_winHT);
            ApplyClientHeight((int)(g_winHFrom + (g_winHTo - g_winHFrom) * e + 0.5f));
            active = true;
        }
    }

    // 3) 进度条数值平滑（约 150ms 追上目标）+ 高光扫动
    if (g_progVisible) {
        const double diff = g_progTarget - g_progShown;
        if (diff > 0.25 || diff < -0.25) {
            double k = dt / 0.15;
            if (k > 1.0) k = 1.0;
            g_progShown += diff * k;
            active = true;
        }
        else if (g_progShown != g_progTarget) {
            g_progShown = g_progTarget;
        }
        // 高光持续从左到右扫过（每轮约 1.1 秒），即使进度长时间不动也看得出没卡死
        g_sheenX += dt / 1.1f;
        if (g_sheenX > 1.35f) g_sheenX = -0.35f;

        // 剩余时间每 0.5 秒才算一次：逐帧计算会因读写速率波动而快速往复跳变
        const ULONGLONG nowTick = GetTickCount64();
        if (nowTick - g_etaLastTick >= 500) {
            g_etaLastTick = nowTick;
            const double frac = g_progShown / 100.0;
            const ULONGLONG elapsed = nowTick - g_progStartTick;
            g_etaText.clear();
            if (g_trayAdded) {
                TraySetTip(tr(L"文件加密工具 — 后台处理中 ", L"File Encryptor - background ")
                           + std::to_wstring((int)(g_progShown + 0.5)) + L"%");
            }
            if (frac > 0.02 && elapsed > 800) {
                const double eta = (double)elapsed / 1000.0 * (1.0 - frac) / frac;
                g_etaText = tr(L" · 预计剩余 ", L" · about ") + FormatEta(eta) + tr(L"", L" left");
            }
        }

        active = true;
        InvalidateRect(hWnd, NULL, FALSE);
    }

    if (!active) {
        KillTimer(hWnd, ANIM_TIMER_ID);
        g_animRunning = false;
    }
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_mainWnd = hWnd;
        g_statusText = tr(L"就绪", L"Ready");
        SetWindowTextW(hWnd, tr(L"文件加密工具", L"File Encryptor").c_str());

        HFONT uiFont = UiFont(UiPx(14), false);

        // 使用 owner-draw 按钮以便绘制圆角，符合 WinUI 3 风格
        HWND hBtn1 = CreateWindowW(L"BUTTON", tr(L"加密文件", L"Encrypt file").c_str(), WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 30, 30, 140, 40, hWnd, (HMENU)1, GetModuleHandle(NULL), NULL);
        HWND hBtn2 = CreateWindowW(L"BUTTON", tr(L"解密文件", L"Decrypt file").c_str(), WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 190, 30, 140, 40, hWnd, (HMENU)2, GetModuleHandle(NULL), NULL);
        HWND hBtn3 = CreateWindowW(L"BUTTON", tr(L"安全删除", L"Secure delete").c_str(), WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 350, 30, 140, 40, hWnd, (HMENU)3, GetModuleHandle(NULL), NULL);
        // 右上角设置按钮（齿轮图标），同样 owner-draw 以保持风格一致
        HWND hLang = CreateWindowW(L"BUTTON", L"", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 360, 10, 40, 28, hWnd, (HMENU)5, GetModuleHandle(NULL), NULL);
        g_hBtn1 = hBtn1;
        g_hBtn2 = hBtn2;
        g_hBtn3 = hBtn3;
        g_hLangBtn = hLang;

        // 按钮自己跟踪鼠标悬停（父窗口在鼠标位于子控件上时收不到 WM_MOUSEMOVE）
        if (hBtn1) SetWindowSubclass(hBtn1, MainBtnSubclassProc, 1, 0);
        if (hBtn2) SetWindowSubclass(hBtn2, MainBtnSubclassProc, 2, 0);
        if (hBtn3) SetWindowSubclass(hBtn3, MainBtnSubclassProc, 3, 0);
        if (hLang) SetWindowSubclass(hLang, MainBtnSubclassProc, 5, 0);

        if (uiFont) {
            SendMessageW(hBtn1, WM_SETFONT, (WPARAM)uiFont, TRUE);
            SendMessageW(hBtn2, WM_SETFONT, (WPARAM)uiFont, TRUE);
            if (hBtn3) SendMessageW(hBtn3, WM_SETFONT, (WPARAM)uiFont, TRUE);
            if (g_hLangBtn) SendMessageW(g_hLangBtn, WM_SETFONT, (WPARAM)uiFont, TRUE);
        }

        // 进度条不创建子控件：在 WM_PAINT 里自绘（绿色填充 + 白色扫光 + 下方进度/剩余时间）

        // 接受文件拖拽
        DragAcceptFiles(hWnd, TRUE);
        // 「安全删除」按钮单独注册 OLE 拖放目标：只有这样才能在拖拽**经过**时
        // 收到 DragEnter/DragOver，做出「松开删除」的实时反馈。
        // WM_DROPFILES 只在松手那一刻触发，做不出这种效果。
        RegisterDeleteDropTarget(hWnd, hBtn3);

        // 主窗口使用 DWM 圆角 + 跟随系统的深色标题栏
        UiApplyFrame(hWnd, true, UiIsDark());

        // 命令行/右键菜单进来的请求：等窗口建好再处理
        if (g_hasPendingLaunch) PostMessageW(hWnd, WM_APP_LAUNCH, 0, 0);
        break;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lParam;
        if (pdis && pdis->CtlType == ODT_BUTTON) {
            RECT rc = pdis->rcItem;
            const UiPalette& c = UiColors();

            const BOOL isPressed = (pdis->itemState & ODS_SELECTED) != 0;
            const BOOL isDisabled = (pdis->itemState & ODS_DISABLED) != 0;
            const int ctlId = GetDlgCtrlID(pdis->hwndItem);

            // 动画中的悬停插值（0..1），而不是布尔跳变
            const float hover = (ctlId >= 0 && ctlId <= 7) ? g_btnHover[ctlId] : 0.0f;

            // 主操作按钮使用强调色（加密/解密），其余使用标准按钮色
            const bool isPrimary = (ctlId == 1 || ctlId == 2);
            const bool isDelete = (ctlId == 3);

            COLORREF fill, textColor, borderColor;
            if (isDelete && g_dropOnDelete) {
                // 有文件正拖在按钮上：用危险色高亮，配合「松开删除」文案
                fill = c.danger;
                textColor = c.onAccent;
                borderColor = c.danger;
            }
            else if (isPrimary) {
                fill = isPressed ? c.accentPressed : UiBlend(c.accent, c.accentHover, hover);
                textColor = c.onAccent;
                borderColor = fill;
            }
            else {
                fill = isPressed ? c.controlPressed : UiBlend(c.control, c.controlHover, hover);
                textColor = c.text;
                borderColor = UiBlend(c.border, c.textMuted, hover);
            }
            if (isDisabled) {
                fill = c.control;
                textColor = c.textMuted;
                borderColor = c.border;
            }

            RECT inner = rc;
            const bool smallBtn = (ctlId == 5);
            InflateRect(&inner,
                        smallBtn ? -UiPx(1) : -UiPx(2),
                        smallBtn ? -UiPx(1) : -UiPx(3));

            // 按钮自绘也走双缓冲：悬停动画每 15ms 重绘一次，直接画屏幕 DC 会闪
            UiDoubleBuffer buffer(pdis->hDC, rc);
            HDC dc = buffer.Dc();

            // 先把整个控件铺成父窗口底色：圆角之外才不会留下未绘制区域或被裁掉的描边
            UiFillRect(dc, rc, c.bg);
            UiDrawRoundRect(dc, inner, smallBtn ? UiPx(6) : UiPx(10), true, fill, true, borderColor);

            wchar_t buf[128] = { 0 };
            GetWindowTextW(pdis->hwndItem, buf, _countof(buf));
            RECT layoutRect = rc;
            if (ctlId == 5) {
                // 设置按钮：绘制齿轮图标（Segoe Fluent Icons）
                HFONT iconFont = UiIconFont(UiPx(15));
                if (iconFont) {
                    UiDrawTextLine(dc, L"\uE713", layoutRect, iconFont, textColor,
                                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                }
            }
            else {
                // 强调色按钮上的文字用灰度抗锯齿：ClearType 在饱和蓝底上会有明显彩边
                UiDrawTextLine(dc, buf, layoutRect,
                               UiFont(smallBtn ? UiPx(13) : UiPx(15), false, isPrimary),
                               textColor, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            return 0;
        }
        break;
    }

    case WM_MOUSEMOVE: {
        // 通过鼠标位置判断是否悬停在按钮上，并触发重绘
        int x = (int)(short)LOWORD(lParam);
        int y = (int)(short)HIWORD(lParam);
        POINT ptClient = { x, y };
        POINT ptScreen = ptClient;
        ClientToScreen(hWnd, &ptScreen);

        (void)ptScreen;
        // 按钮悬停由各自的子类化过程（MainBtnSubclassProc）负责：
        // 鼠标位于子控件之上时，父窗口根本收不到 WM_MOUSEMOVE。

        if (!g_trackingMouse) {
            // 开始跟踪鼠标离开事件
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_trackingMouse = true;
        }

        break;
    }

    case WM_MOUSELEAVE: {
        // 清除 hover 状态，交给动画定时器平滑淡出
        g_trackingMouse = false;
        break;
    }

    case WM_DROPFILES: {
        HDROP hDrop = (HDROP)wParam;
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
        if (count == 0) { DragFinish(hDrop); break; }

        std::vector<std::wstring> files;
        for (UINT i = 0; i < count; ++i) {
            wchar_t buf[MAX_PATH];
            if (DragQueryFileW(hDrop, i, buf, MAX_PATH)) {
                std::wstring p(buf);
                DWORD attrs = GetFileAttributesW(p.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                    CollectFilesRecursive(p, files);
                }
                else {
                    files.push_back(p);
                }
            }
        }
        DragFinish(hDrop);

        if (files.empty()) break;

        // 注意：这里**不再**拦截“已有任务正在运行”。
        // 再次创建任务会进入队列，由 worker 顺序处理，进度条下方显示“已完成 x / y”。

        // 检测文件类型：是否包含 .enc（加密文件）或普通文件
        bool hasEnc = false, hasPlain = false;
        for (const auto& p : files) {
            if (p.size() >= 4 && p.substr(p.size() - 4) == L".enc") hasEnc = true;
            else hasPlain = true;
        }
        // 不能同时包含加密文件和普通文件（要求用户分开操作）
        if (hasEnc && hasPlain) {
            ShowUiMessage(hWnd, UiIcon::Error, tr(L"错误", L"Error"),
                          tr(L"包含加密文件和普通文件，请分别拖拽进行加密或解密。",
                             L"Contains encrypted and plain files; drag separately for encrypt/decrypt."),
                          false);
            break;
        }

        // 密码收集策略：
        // - 全部为 .enc（解密）：询问是逐文件输入密码，还是统一使用同一密码
        // - 全部为普通文件（加密）：询问是否对所有文件使用同一密码
        std::vector<std::string> perFilePasswords; // 为空表示使用 pwd_single
        std::string pwd_single;
        std::vector<Protection> perFileProts;      // 与 perFilePasswords 同序
        Protection prot_single;

        // 诱饵（可否认加密）只对单文件提供：批量时若每个文件各配一份，交互过重
        std::wstring decoyFile;
        std::string  decoyPwd;
        if (hasEnc) {
            // ---- 解密：每个文件独立走「锁定检查 -> 校验 -> 扣减 -> 重试」 ----
            if (files.size() == 1) {
                std::wstring blocked;
                const PwdCollect r = CollectDecryptPassword(hWnd, files[0], pwd_single, blocked);
                if (r == PwdCollect::Cancelled) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
                if (r == PwdCollect::Blocked) {
                    ShowUiMessage(hWnd, UiIcon::Error, tr(L"无法解密", L"Cannot decrypt"),
                                  files[0] + L"\n" + blocked, true);
                    SetStatusText(tr(L"已拒绝", L"Refused"));
                    break;
                }
            }
            else {
                // 批量提示：带提示音并强制置顶，避免被其他窗口覆盖
                UiMessage prompt;
                prompt.caption = tr(L"批量解密", L"Batch decrypt");
                prompt.text = tr(L"检测到全部为加密文件(.enc)。\n注意：每个文件可能使用不同密码。\n\n选择“是”在每个文件上分别输入密码；选择“否”使用相同密码尝试解密所有文件。",
                                 L"Detected .enc files; each may require a different password.\n\nChoose Yes to enter a password for each file; choose No to try the same password for all.");
                prompt.icon = UiIcon::Question;
                prompt.buttons = MB_YESNO;
                prompt.topmost = true;
                prompt.sound = true;

                if (UiShowMessage(hWnd, prompt) == IDYES) {
                    // 逐文件校验；被锁定或次数用尽的文件会被剔除，不进入处理队列
                    std::vector<std::wstring> keptFiles;
                    std::vector<std::string> keptPwd;
                    bool cancelled = false;
                    for (size_t i = 0; i < files.size(); ++i) {
                        std::string onepwd;
                        std::wstring blocked;
                        const PwdCollect r = CollectDecryptPassword(hWnd, files[i], onepwd, blocked);
                        if (r == PwdCollect::Cancelled) { cancelled = true; break; }
                        if (r == PwdCollect::Blocked) {
                            ShowUiMessage(hWnd, UiIcon::Error, tr(L"无法解密", L"Cannot decrypt"),
                                          files[i] + L"\n" + blocked, true);
                            continue;
                        }
                        keptFiles.push_back(files[i]);
                        keptPwd.push_back(onepwd);
                    }
                    if (cancelled) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
                    if (keptFiles.empty()) {
                        SetStatusText(tr(L"没有可处理的文件", L"Nothing to process"));
                        break;
                    }
                    files = keptFiles;
                    perFilePasswords = keptPwd;
                }
                else {
                    // 所有文件使用同一密码：只以第一个文件为准做校验与重试。
                    // 取舍：避免为 N 个文件逐个弹框；其余文件若密码不同，会在实际解密
                    // 失败时于 worker 中各自扣减次数（见 StartProcessing 里的对应注释）。
                    std::wstring blocked;
                    const PwdCollect r = CollectDecryptPassword(hWnd, files[0], pwd_single, blocked);
                    if (r == PwdCollect::Cancelled) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
                    if (r == PwdCollect::Blocked) {
                        ShowUiMessage(hWnd, UiIcon::Error, tr(L"无法解密", L"Cannot decrypt"),
                                      files[0] + L"\n" + blocked, true);
                        SetStatusText(tr(L"已拒绝", L"Refused"));
                        break;
                    }
                }
            }
        }
        else {
            // ---- 加密：密码框里可以同时配置防暴力破解策略 ----
            if (files.size() == 1) {
                // 单文件加密：要求确认密码
                if (!CollectEncryptPassword(hWnd, files[0], pwd_single, prot_single, true,
                                            !Settings().decoyDisabled, &decoyFile, &decoyPwd)) {
                    SetStatusText(tr(L"已取消", L"Cancelled"));
                    break;
                }
            }
            else {
                // 批量提示：带提示音并强制置顶，避免被其他窗口覆盖
                UiMessage prompt;
                prompt.caption = tr(L"批量加密", L"Batch encrypt");
                prompt.text = tr(L"检测到全部为普通文件。\n\n是否为所有文件使用相同密码？\n选择“是”将对所有文件使用相同密码（需要确认）；\n选择“否”将为每个文件单独设置密码。",
                                 L"Detected plain files.\n\nUse the same password for all files?\nYes requires one confirmation; No prompts for each file.");
                prompt.icon = UiIcon::Question;
                prompt.buttons = MB_YESNO;
                prompt.topmost = true;
                prompt.sound = true;

                if (UiShowMessage(hWnd, prompt) == IDYES) {
                    if (!CollectEncryptPassword(hWnd, files[0], pwd_single, prot_single, true)) {
                        SetStatusText(tr(L"已取消", L"Cancelled"));
                        break;
                    }
                }
                else {
                    bool cancelled = false;
                    for (size_t i = 0; i < files.size(); ++i) {
                        std::string onepwd;
                        Protection oneprot;
                        if (!CollectEncryptPassword(hWnd, PasswordSubtitle(files[i], i, files.size()),
                                                    onepwd, oneprot, true)) {
                            cancelled = true;
                            break;
                        }
                        perFilePasswords.push_back(onepwd);
                        perFileProts.push_back(oneprot);
                    }
                    if (cancelled) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
                }
            }
        }

        StartProcessing(files, perFilePasswords, pwd_single, perFileProts, prot_single,
                        decoyFile, decoyPwd);
        break;
    }

    case WM_SIZE: {
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        const int W = rcClient.right - rcClient.left;

        const int btnW = MainButtonWidth();
        const int btnH = UiPx(44);
        const int btnGap = UiPx(16);
        const int btnY = UiPx(kMainBtnY);

        int btnX = (W - (btnW * 3 + btnGap * 2)) / 2;
        if (btnX < UiPx(12)) btnX = UiPx(12);
        if (g_hBtn1) SetWindowPos(g_hBtn1, NULL, btnX, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hBtn2) SetWindowPos(g_hBtn2, NULL, btnX + btnW + btnGap, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hBtn3) SetWindowPos(g_hBtn3, NULL, btnX + (btnW + btnGap) * 2, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);

        const int pad = UiPx(24);
        (void)pad;

        if (g_hLangBtn) {
            SetWindowPos(g_hLangBtn, NULL, W - UiPx(52), UiPx(10), UiPx(40), UiPx(28), SWP_NOZORDER | SWP_NOACTIVATE);
        }

        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }

    case WM_ERASEBKGND:
        return 1; // 背景由 WM_PAINT 完整绘制（深色模式）

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        const UiPalette& c = UiColors();

        {
            // 整帧先画到内存 DC，最后一次性贴上，避免逐元素绘制产生频闪
            UiDoubleBuffer buffer(hdc, rc);
            HDC dc = buffer.Dc();

            UiFillRect(dc, rc, c.bg);

            // 自绘进度条：上方文件名 + 绿色填充/白色扫光 + 下方进度与队列计数
            if (g_progVisible) {
                RECT fileRc = {}, bar = {}, info = {};
                ProgressRects(rc, fileRc, bar, info);

                // 进度条上方：当前文件名（不含路径，否则显示不全）
                UiDrawTextLine(dc, g_progFileName, fileRc, UiFont(UiPx(13), false), c.text,
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                // 顶点带一点点圆角即可，不要做成两端半圆
                const int radius = UiPx(3);

                UiDrawRoundRect(dc, bar, radius, true, c.track, true, c.border);

                const double f = g_progShown / 100.0;
                if (f > 0.0001) {
                    RECT fill = bar;
                    fill.right = bar.left + (int)((bar.right - bar.left) * f + 0.5);
                    // 圆角矩形的宽度不能小于两倍圆角，否则两端会被压扁
                    if (fill.right < fill.left + radius * 2) fill.right = fill.left + radius * 2;
                    if (fill.right > bar.right) fill.right = bar.right;
                    UiDrawRoundRect(dc, fill, radius, true, ProgressGreen(), false, ProgressGreen());

                    // 高光只在绿色（已完成）部分内扫动，不跨到空白轨道上
                    const int fillW = fill.right - fill.left;
                    const int halfW = fillW / 4 > UiPx(30) ? fillW / 4 : UiPx(30);
                    UiDrawSheen(dc, fill, radius,
                                (float)fill.left + g_sheenX * (float)fillW, halfW, 90);
                }

                // 下方左侧：45% · 预计剩余 12 秒
                std::wstring line = std::to_wstring((int)(g_progShown + 0.5)) + L"%";
                line += g_etaText;   // 始终只显示预计时间（覆写阶段同样按总工作量推算）
                UiDrawTextLine(dc, line, info, UiFont(UiPx(13), false), c.textMuted,
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // 下方右侧：任务队列计数（只有多于一个任务时才显示）
                // 总数必须把“正在执行的那个”也算上：它既不在队列里、也还没计入已完成
                int pending = 0;
                {
                    std::lock_guard<std::mutex> lk(g_queueMutex);
                    pending = (int)g_taskQueue.size();
                }
                const int done  = g_tasksDone.load();
                const int total = done + g_taskRunning.load() + pending;
                if (total > 1) {
                    const std::wstring q = tr(L"已完成 ", L"done ")
                                         + std::to_wstring(done)
                                         + L" / " + std::to_wstring(total);
                    UiDrawTextLine(dc, q, info, UiFont(UiPx(13), false), c.textMuted,
                                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                }
            }
        }

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_SETTINGCHANGE: {
        // lParam 表明变化类别；字体/DPI 变化时重建字体缓存（自动跟随系统字体）
        bool fontMaybeChanged = (lParam == 0);
        if (lParam != 0) {
            const wchar_t* area = reinterpret_cast<const wchar_t*>(lParam);
            if (wcscmp(area, L"WindowMetrics") == 0 || wcscmp(area, L"windows") == 0) {
                fontMaybeChanged = true;
            }
        }
        if (fontMaybeChanged) UiFontsReload();

        if (UiThemeRefresh()) {
            UiApplyFrame(hWnd, true, UiIsDark());
            ApplyProgressBarTheme();
        }
        InvalidateRect(hWnd, NULL, FALSE);
        if (g_hBtn1) InvalidateRect(g_hBtn1, NULL, FALSE);
        if (g_hBtn2) InvalidateRect(g_hBtn2, NULL, FALSE);
        if (g_hLangBtn) InvalidateRect(g_hLangBtn, NULL, FALSE);
        break;
    }

    case WM_THEMECHANGED:
        if (UiThemeRefresh()) {
            UiApplyFrame(hWnd, true, UiIsDark());
            ApplyProgressBarTheme();
        }
        InvalidateRect(hWnd, NULL, FALSE);
        if (g_hBtn1) InvalidateRect(g_hBtn1, NULL, FALSE);
        if (g_hBtn2) InvalidateRect(g_hBtn2, NULL, FALSE);
        if (g_hLangBtn) InvalidateRect(g_hLangBtn, NULL, FALSE);
        break;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);

        // 设置按钮
        if (id == 5) {
            ShowSettingsDialog(hWnd);
            break;
        }

        // 安全删除：选文件 -> 二次确认 -> 入队（与加解密共用批量/进度/托盘）
        if (id == 3) {
            SetStatusText(tr(L"请选择要安全删除的文件...", L"Select files to securely delete..."));
            std::vector<std::wstring> picked = PickFilesForDelete(hWnd);
            if (picked.empty()) {
                SetStatusText(tr(L"已取消", L"Cancelled"));
                break;
            }
            ConfirmAndSecureDelete(hWnd, std::move(picked));
            break;
        }

        if (id == 1 || id == 2) { // 加密 / 解密
            const bool encrypting = (id == 1);
            SetStatusText(encrypting ? tr(L"请选择要加密的文件...", L"Select a file to encrypt...")
                                     : tr(L"请选择要解密的文件...", L"Select a file to decrypt..."));

            std::wstring input_path = OpenFileDialog(!encrypting);
            if (input_path.empty()) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }

            if (!encrypting && (input_path.size() < 4 || input_path.substr(input_path.size() - 4) != L".enc")) {
                ShowUiMessage(hWnd, UiIcon::Error, tr(L"错误", L"Error"),
                              tr(L"请选择 .enc 加密文件！", L"Please select a .enc encrypted file!"), false);
                SetStatusText(tr(L"不是加密文件", L"Not an encrypted file"));
                break;
            }

            // 同上：不再拦截，重复创建的任务进入队列

            std::string password;
            Protection prot;
            std::wstring decoyFile2;
            std::string  decoyPwd2;
            if (encrypting) {
                // 加密：密码框里可以同时配置防暴力破解；单文件可额外启用诱饵
                // 设置里勾了「禁用诱饵文件」时不再提供诱饵选项（单槽写不下诱饵）
                if (!CollectEncryptPassword(hWnd, input_path, password, prot, true,
                                            !Settings().decoyDisabled, &decoyFile2, &decoyPwd2)) {
                    SetStatusText(tr(L"已取消", L"Cancelled"));
                    break;
                }
            }
            else {
                std::wstring blocked;
                const PwdCollect r = CollectDecryptPassword(hWnd, input_path, password, blocked);
                if (r == PwdCollect::Cancelled) {
                    SetStatusText(tr(L"已取消", L"Cancelled"));
                    break;
                }
                if (r == PwdCollect::Blocked) {
                    ShowUiMessage(hWnd, UiIcon::Error, tr(L"无法解密", L"Cannot decrypt"),
                                  input_path + L"\n" + blocked, true);
                    SetStatusText(tr(L"已拒绝", L"Refused"));
                    break;
                }
            }

            // 放到后台线程执行，避免界面卡死（不再出现“无响应”）
            StartProcessing({ input_path }, {}, password, {}, prot, decoyFile2, decoyPwd2);
            break;
        }

        break;
    }

    case WM_CLOSE: {
        // 防误触：有任务在跑时先确认，避免手滑关掉正在处理的文件
        if (g_workerRunning && !g_runInBackground) {
            UiMessage m;
            m.caption = tr(L"任务尚未结束", L"Task still running");
            m.text = tr(
                L"当前还有加/解密任务正在执行。\n\n"
                L"「强制关闭」：立即关闭，可能留下未完成的输出文件\n"
                L"「后台运行」：转入后台继续处理，完成后自动退出\n"
                L"「取消」：返回程序，继续等待任务完成",
                L"A file encryption/decryption task is still running.\n\n"
                L"Force close: close now (may leave an incomplete output file)\n"
                L"Run in background: keep processing, exit automatically when finished\n"
                L"Cancel: go back and wait for the task to finish");
            m.icon = UiIcon::Warning;
            m.buttons = MB_YESNOCANCEL;
            // 左 -> 右：强制关闭 / 后台运行 / 取消。
            // 蓝色主按钮放在“取消”上——破坏性操作不该是默认高亮的那一个，否则仍然容易误触。
            m.labels = { tr(L"强制关闭", L"Force close"),
                         tr(L"后台运行", L"Run in background"),
                         tr(L"取消", L"Cancel") };
            m.primaryId = IDCANCEL;
            m.topmost = true;
            m.sound = true;
            const int r = UiShowMessage(hWnd, m);
            if (r == IDYES) {
                // 强制关闭：直接销毁窗口，进程退出时工作线程一并结束
                DestroyWindow(hWnd);
            }
            else if (r == IDNO) {
                g_runInBackground = true;
                ShowWindow(hWnd, SW_HIDE);
                TrayAdd();   // 后台运行：托盘上留个入口，并能看到进度
            }
            return 0;   // 取消（或处理完毕）都不再继续默认关闭流程
        }
        // 没有任务在跑：必须显式 DestroyWindow。
        // 不能靠 break 落到 DefWindowProc——本 WndProc 的兜底是 return 0，
        // 窗口不会被销毁，结果就是程序关不掉。
        DestroyWindow(hWnd);
        return 0;
    }

    case WM_APP_BACKGROUND_DONE:
        // 后台任务结束。若此时完成提示弹窗还开着，不能立刻销毁主窗口：
        // 弹窗的模态循环会继续派发消息，提前销毁会把弹窗一起带走，用户根本来不及看。
        if (UiModalDepth() > 0) {
            g_pendingBackgroundExit = true;
            SetTimer(hWnd, TIMER_BG_EXIT, 200, NULL);
        }
        else {
            TrayRemove();
            DestroyWindow(hWnd);
        }
        return 0;

    case WM_APP_TRAY: {
        const UINT ev = (UINT)lParam;
        if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
            RestoreFromTray(hWnd);
        }
        else if (ev == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, tr(L"显示主窗口", L"Show window").c_str());
            AppendMenuW(menu, MF_STRING, 2, tr(L"退出", L"Exit").c_str());
            POINT pt = {};
            GetCursorPos(&pt);
            SetForegroundWindow(hWnd);
            const int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                pt.x, pt.y, 0, hWnd, NULL);
            DestroyMenu(menu);
            if (cmd == 1) RestoreFromTray(hWnd);
            else if (cmd == 2) { TrayRemove(); DestroyWindow(hWnd); }
        }
        return 0;
    }

    case WM_DESTROY:
        TrayRemove();
        UiThemeShutdown();
        PostQuitMessage(0);
        break;

    case WM_APP_SET_STATUS: {
        LPWSTR msg = (LPWSTR)lParam;
        if (msg) {
            SetStatusText(std::wstring(msg));
            delete[] msg;
        }
        break;
    }
    case WM_COPYDATA: {
        // 另一个实例（右键菜单为每个文件各起一个进程）把清单转发过来
        const COPYDATASTRUCT* cds = (const COPYDATASTRUCT*)lParam;
        LaunchRequest req;
        if (cds && cds->dwData == kLaunchCopyDataId &&
            UnpackLaunch((const BYTE*)cds->lpData, cds->cbData, req) &&
            !req.files.empty()) {
            QueueLaunchFiles(hWnd, req.files, (int)req.kind);   // 等这一波进程到齐再一起处理
            return TRUE;
        }
        return FALSE;
    }
    case WM_APP_LAUNCH: {
        std::vector<std::wstring> files;
        int kind = 0;
        {
            std::lock_guard<std::mutex> lk(g_dropMutex);
            files = std::move(g_pendingLaunch);
            kind = g_pendingKind;
        }
        QueueLaunchFiles(hWnd, files, kind);   // 与转发的请求合并成一批
        return 0;
    }
    case WM_APP_SECURE_DELETE_DROP: {
        // 有文件被拖到「安全删除」按钮上并松开。清单由 Drop() 先存好再通知，
        // 这里取出来走与按钮点击完全相同的确认流程。
        std::vector<std::wstring> files;
        {
            std::lock_guard<std::mutex> lk(g_dropMutex);
            files = std::move(g_droppedForDelete);
            g_droppedForDelete.clear();
        }
        if (!files.empty()) ConfirmAndSecureDelete(hWnd, std::move(files));
        return 0;
    }
    case WM_APP_QUEUE_CHANGED: {
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;
    }
    case WM_APP_SET_FILE: {
        LPWSTR msg = (LPWSTR)lParam;
        if (msg) {
            g_progFileName = msg;
            delete[] msg;
        }
        InvalidateRect(hWnd, NULL, FALSE);
        break;
    }
    case WM_APP_SHOW_MESSAGE: {
        UiMessagePayload* payload = (UiMessagePayload*)lParam;
        if (payload) {
            ShowUiMessage(hWnd, payload->icon, payload->caption, payload->text, payload->topmost);
            delete payload;
        }
        break;
    }
    case WM_APP_SHOW_PROGRESS: {
        const BOOL show = (wParam != 0);
        // 空闲时窗口是紧凑的；开始/结束任务时在两种高度之间过渡，避免留下大片空白
        SetBusyLayout(show != 0);
        if (show) {
            g_progShown = 0.0;
            g_progTarget = 0.0;
            g_sheenX = -0.35f;
            g_progStartTick = GetTickCount64();   // 用于估算剩余时间
            g_etaLastTick = 0;
            g_etaText.clear();
            g_progVisible = true;
        }
        else {
            // 收尾：直接对齐到目标值，避免进度条停在半路
            g_progShown = g_progTarget;
            g_progVisible = false;
        }
        StartAnimation();
        break;
    }
    case WM_APP_UPDATE_PROGRESS: {
        // 只更新目标值；实际显示由动画定时器平滑逼近
        g_progTarget = (double)(int)wParam;
        StartAnimation();
        break;
    }

    case WM_APP_ASK_OUTPUT_PATH:
        return HandleAskOutputPath(hWnd, lParam);

    case WM_TIMER: {
        if (wParam == TIMER_LAUNCH_GROUP) {
            // 这一波启动请求已经到齐：合并成一次确认、一个任务
            KillTimer(hWnd, TIMER_LAUNCH_GROUP);
            std::vector<std::wstring> files;
            int kind = 0;
            {
                std::lock_guard<std::mutex> lk(g_dropMutex);
                files = std::move(g_launchFiles);
                g_launchFiles.clear();
                kind = g_launchKind;
            }
            DispatchLaunch(hWnd, files, kind);
            return 0;
        }
        if (wParam == TIMER_BG_EXIT) {
            if (UiModalDepth() == 0) {   // 弹窗已关，可以退出了
                KillTimer(hWnd, TIMER_BG_EXIT);
                TrayRemove();
                DestroyWindow(hWnd);
            }
            return 0;
        }
        if (wParam == ANIM_TIMER_ID) {
            TickAnimation(hWnd);
            return 0;
        }
        break;
    }

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// 进度条已自绘，配色随主题自动生效，这里保留空实现以兼容调用点
static void ApplyProgressBarTheme() {}

// 统一的消息提示入口：WinUI 3 风格 + 全局语言 + 提示音
static void ShowUiMessage(HWND owner, UiIcon icon, const std::wstring& caption,
                          const std::wstring& text, bool topmost) {
    UiMessage m;
    m.caption = caption;
    m.text = text;
    m.icon = icon;
    m.buttons = MB_OK;
    m.topmost = topmost;
    m.sound = true;
    UiShowMessage(owner, m);
}

static std::wstring FileNameOf(const std::wstring& p) {
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? p : p.substr(pos + 1);
}

static std::wstring DirOf(const std::wstring& p) {
    const size_t pos = p.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? std::wstring() : p.substr(0, pos + 1);
}

static std::wstring WithTrailingSlash(std::wstring dir) {
    if (!dir.empty() && dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    return dir;
}

// 根据设置计算某个文件的输出路径；askEveryTime 时通过 UI 线程弹保存对话框
static std::wstring ResolveOutputPath(const std::wstring& src, bool encrypting,
                                      OutDirMode mode, const std::wstring& fixedDir,
                                      bool& skipped) {
    skipped = false;
    const std::wstring base = FileNameOf(src);
    std::wstring target;
    if (encrypting) {
        target = base + L".enc";
    }
    else {
        target = (base.size() > 4 && base.substr(base.size() - 4) == L".enc")
                     ? base.substr(0, base.size() - 4) : base;
    }

    std::wstring dir;
    switch (mode) {
    case OutDirMode::FixedDir:
        dir = fixedDir.empty() ? DirOf(src) : WithTrailingSlash(fixedDir);
        break;
    case OutDirMode::SourceDir:
    case OutDirMode::AskEveryTime:
    default:
        dir = DirOf(src);
        break;
    }

    if (mode != OutDirMode::AskEveryTime) return dir + target;

    // 交给 UI 线程弹保存对话框
    AskOutputPath req;
    req.suggested = dir + target;
    req.done = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!req.done) { skipped = true; return std::wstring(); }

    if (!g_mainWnd || !PostMessageW(g_mainWnd, WM_APP_ASK_OUTPUT_PATH, 0,
                                    reinterpret_cast<LPARAM>(&req))) {
        CloseHandle(req.done);
        skipped = true;
        return std::wstring();
    }
    // 等待 UI 线程完成（超时视为取消，避免主窗口先销毁导致永久阻塞）
    const DWORD wr = WaitForSingleObject(req.done, 5 * 60 * 1000);
    CloseHandle(req.done);
    if (wr != WAIT_OBJECT_0 || req.cancelled) {
        skipped = true;
        return std::wstring();
    }
    return req.result;
}

// 批量逐个输入密码时，在密码框标题下方显示当前文件名（含 i/n 计数），
// 便于用户确认正在为哪个文件设置/输入密码。
static std::wstring PasswordSubtitle(const std::wstring& file, size_t index, size_t total) {
    if (total <= 1) return file;
    return file + L"    (" + std::to_wstring(index + 1) + L"/" + std::to_wstring(total) + L")";
}

// 加密前收集密码；allowProtection 为真时在密码框里显示防暴力破解设置区。
// 返回值即 uiProt 的内容，直接写进文件头。
// allowDecoy 为真时（仅单文件加密）密码框里会多出「使用诱饵文件（可否认加密）」。
// 勾选后依次让用户选诱饵文件、设假密码，两者都有效才写回 outDecoy*。
static bool CollectEncryptPassword(HWND owner, const std::wstring& subtitle,
                                   std::string& outPwd, Protection& outProt,
                                   bool allowProtection,
                                   bool allowDecoy,
                                   std::wstring* outDecoyFile,
                                   std::string* outDecoyPwd) {
    // 默认值：保护关闭；勾选后默认 3 次 / 永久删除 / 锁定 7 天
    UiProtection ui;
    ui.enabled = false;
    ui.maxTries = 3;
    ui.action = 0;
    ui.lockDays = 7;

    bool useDecoy = false;
    if (!ShowPasswordDialog(owner, outPwd, true, subtitle, allowProtection, &ui,
                            -1, allowDecoy, &useDecoy))
        return false;

    outProt.enabled = ui.enabled;
    outProt.maxTries = ui.maxTries;
    outProt.action = (ui.action == 1) ? ProtectionAction::Lock : ProtectionAction::Delete;
    outProt.lockDays = ui.lockDays;

    if (allowDecoy && useDecoy && outDecoyFile && outDecoyPwd) {
        const std::wstring decoy = OpenFileDialog(false);
        if (decoy.empty()) return false;   // 用户取消了诱饵文件选择

        std::string fake;
        if (!ShowPasswordDialog(owner, fake, true,
                                tr(L"设置假密码：被胁迫时交出的就是它",
                                   L"Set the decoy password (the one you hand over)"),
                                false, nullptr))
            return false;
        if (fake.empty()) {
            ShowUiMessage(owner, UiIcon::Error, tr(L"无法使用诱饵", L"Decoy unavailable"),
                          tr(L"假密码不能为空。", L"The decoy password cannot be empty."), true);
            return false;
        }
        if (fake == outPwd) {
            ShowUiMessage(owner, UiIcon::Error, tr(L"无法使用诱饵", L"Decoy unavailable"),
                          tr(L"假密码不能与真密码相同，否则诱饵没有意义。",
                             L"The decoy password must differ from the real one."), true);
            return false;
        }
        *outDecoyFile = decoy;
        *outDecoyPwd = fake;
    }
    return true;
}

// 解密前收集密码。受保护文件：
//   * 锁定期内直接拒绝（即使密码正确）
//   * 锁定到期则把剩余次数重置为初始值
//   * 密码错误 -> 扣减 -> 带着新的剩余次数重新弹框，直到用尽或用对
static PwdCollect CollectDecryptPassword(HWND owner, const std::wstring& file,
                                         std::string& outPwd, std::wstring& blockedMsg) {
    FileProtState st;
    const bool haveState = FileEncryptor::ReadProtState(file, st);

    if (haveState && st.isProtected) {
        if (st.lockUntil != 0) {
            const uint64_t now = FileEncryptor::NowUnix();
            if (now < st.lockUntil) {
                blockedMsg = tr(L"该文件已被锁定，剩余 ", L"This file is locked for another ")
                           + FormatDuration(st.lockUntil - now);
                return PwdCollect::Blocked;
            }
            // 锁定已到期：恢复次数并清除锁定时间
            FileEncryptor::WriteMutableState(file, st.maxTries, 0);
            st.remaining = st.maxTries;
            st.lockUntil = 0;
        }

        for (;;) {
            std::string pwd;
            if (!ShowPasswordDialog(owner, pwd, false, file, false, nullptr, st.remaining))
                return PwdCollect::Cancelled;

            FileProtState cur = st;
            const ProtVerify vr = FileEncryptor::VerifyPassword(file, pwd, cur);
            if (vr != ProtVerify::Wrong) {   // Ok 或无法快速校验都交给后续解密判断
                outPwd = pwd;
                return PwdCollect::Ok;
            }

            std::wstring msg;
            const int left = FileEncryptor::ConsumeAttempt(file, cur, msg);
            if (left <= 0) {
                blockedMsg = msg;
                return PwdCollect::Blocked;
            }
            st.remaining = left;   // 带着新的剩余次数重新弹框
        }
    }

    if (!ShowPasswordDialog(owner, outPwd, false, file))
        return PwdCollect::Cancelled;
    return PwdCollect::Ok;
}

// ==================== 「安全删除」任务 ====================
//
// 与加解密共用同一个任务队列，所以批量处理、进度条、后台托盘、
// 「已完成 x / y」队列计数全部自动具备，不需要另写一套。
static void RunSecureDeleteTask(const std::vector<std::wstring>& files, int passes) {
    const bool english   = g_langEnglish;
    // 显式点「安全删除」时至少要覆写一遍：0 在设置里表示"不删除"，
    // 用在这里会让按钮名不副实。总工作量与实际写入必须用同一个值。
    const int  usePasses = (passes > 0) ? passes : 1;

    auto postStatus = [](const std::wstring& text) {
        wchar_t* buf = new wchar_t[text.size() + 1];
        wcscpy_s(buf, text.size() + 1, text.c_str());
        PostMessageW(g_mainWnd, WM_APP_SET_STATUS, 0, (LPARAM)buf);
    };
    auto postFileName = [](const std::wstring& path) {
        const size_t pos = path.find_last_of(L"\\/");
        const std::wstring name = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
        wchar_t* buf = new wchar_t[name.size() + 1];
        wcscpy_s(buf, name.size() + 1, name.c_str());
        PostMessageW(g_mainWnd, WM_APP_SET_FILE, 0, (LPARAM)buf);
    };
    auto postMessage = [](UiIcon icon, const std::wstring& caption,
                          const std::wstring& text, bool topmost) {
        UiMessagePayload* payload = new UiMessagePayload();
        payload->icon = icon;
        payload->caption = caption;
        payload->text = text;
        payload->topmost = topmost;
        PostMessageW(g_mainWnd, WM_APP_SHOW_MESSAGE, 0, (LPARAM)payload);
    };

    // 先把每个文件的工作量算出来：进度必须走完整的"开始文件 / 结束文件"生命周期，
    // 否则 g_progDoneBytes 永远不推进 —— 表现为删完一个卡在 100%，
    // 下一个又从 0% 开始（因为没有 ProgressEndFile 把它计入已完成）。
    std::vector<uint64_t> perFileBytes;
    perFileBytes.reserve(files.size());
    uint64_t total = 0;
    for (const auto& f : files) {
        uint64_t sz = 0;
        HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li = {};
            if (GetFileSizeEx(h, &li) && li.QuadPart > 0) sz = (uint64_t)li.QuadPart;
            CloseHandle(h);
        }
        // 工作量 = 文件大小 × 覆写次数（与 fe::SecureDeleteFile 上报的字节口径一致）
        perFileBytes.push_back(sz * (uint64_t)usePasses);
        total += perFileBytes.back();
    }
    ProgressReset(total);
    PostMessageW(g_mainWnd, WM_APP_SHOW_PROGRESS, 1, 0);

    int okCount = 0;
    std::wstring failedList;
    for (size_t i = 0; i < files.size(); ++i) {
        const std::wstring& f = files[i];
        ProgressBeginFile(perFileBytes[i]);   // 缺了这句 g_progFileTotal 就是 0
        postFileName(f);
        postStatus(english ? L"Securely deleting..." : L"正在安全删除...");
        try {
            SecureDeleteFile(f, usePasses, [](uint64_t n) { ProgressAdd(n); });
            ++okCount;
        }
        catch (const std::exception&) {
            failedList += f + L"\n";
        }
        ProgressEndFile();                    // 缺了这句进度永远不跨文件累加
    }

    // 收尾：对齐到 100% 并隐藏进度条（加解密路径也是这么做的）。
    // 少了它进度条会一直停在 100% 不消失。
    ProgressFinish();

    if (failedList.empty()) {
        postStatus(english ? L"Done" : L"完成");
        const std::wstring text = english
            ? (L"Securely deleted " + std::to_wstring(okCount) + L" file(s).")
            : (L"已安全删除 " + std::to_wstring(okCount) + L" 个文件。");
        postMessage(UiIcon::Info, english ? L"Done" : L"完成", text, true);
    }
    else {
        postStatus(english ? L"Partially failed" : L"部分失败");
        const std::wstring text =
            (english ? L"The following files could not be deleted:\n\n"
                     : L"以下文件删除失败：\n\n") + failedList;
        postMessage(UiIcon::Error, english ? L"Failed" : L"失败", text, true);
    }
}

// 把任务放进队列，并在没有 worker 时启动一个。
// 加解密与安全删除共用它 —— 这是三者（批量 / 进度条 / 后台托盘）行为一致的保证。
static void EnqueueAndRun(TaskRequest task);

// 加解密任务
static void StartProcessing(std::vector<std::wstring> files,
                            std::vector<std::string> perFilePasswords,
                            std::string singlePassword,
                            std::vector<Protection> perFileProts,
                            Protection singleProt,
                            std::wstring decoyFile,
                            std::string  decoyPwd) {
    if (files.empty()) return;

    TaskRequest task;
    task.files = std::move(files);
    task.perFilePasswords = std::move(perFilePasswords);
    task.singlePassword = std::move(singlePassword);
    task.perFileProts = std::move(perFileProts);
    task.singleProt = singleProt;
    task.decoyFile = std::move(decoyFile);
    task.decoyPwd = std::move(decoyPwd);
    EnqueueAndRun(std::move(task));
}

// 安全删除任务：只需文件列表 + 覆写次数
static void StartSecureDelete(std::vector<std::wstring> files, int passes) {
    if (files.empty()) return;
    TaskRequest task;
    task.files = std::move(files);
    task.isSecureDelete = true;
    task.deletePasses = passes;
    EnqueueAndRun(std::move(task));
}

// 安全删除前的二次确认。
// 删除不可撤销，无论从按钮还是拖放进来都必须先过这一关。
static void ConfirmAndSecureDelete(HWND owner, std::vector<std::wstring> files) {
    if (files.empty()) return;

    const bool english = g_langEnglish;
    // 显式删除至少覆写一遍（设置里的 0 表示"加密后不删源文件"，用在这里没有意义）
    const int configured = (int)Settings().secureDelete;
    const int passes = (configured > 0) ? configured : 1;

    std::wstring text;
    if (files.size() == 1) {
        const size_t pos = files[0].find_last_of(L"\\/");
        const std::wstring name = (pos == std::wstring::npos) ? files[0] : files[0].substr(pos + 1);
        text = english
            ? (L"Securely delete this file?\n\n" + name +
               L"\n\nIt will be overwritten " + std::to_wstring(passes) +
               L" time(s) and then deleted. This cannot be undone.")
            : (L"确定要安全删除这个文件吗？\n\n" + name +
               L"\n\n将覆写 " + std::to_wstring(passes) +
               L" 次后删除，此操作不可恢复。");
    }
    else {
        text = english
            ? (L"Securely delete these " + std::to_wstring(files.size()) +
               L" files?\n\nEach will be overwritten " + std::to_wstring(passes) +
               L" time(s) and then deleted. This cannot be undone.")
            : (L"确定要安全删除这 " + std::to_wstring(files.size()) +
               L" 个文件吗？\n\n每个将覆写 " + std::to_wstring(passes) +
               L" 次后删除，此操作不可恢复。");
    }

    UiMessage m;
    m.icon = UiIcon::Warning;
    m.caption = english ? L"Confirm secure delete" : L"确认安全删除";
    m.text = text;
    m.buttons = MB_YESNO;
    m.topmost = true;
    if (UiShowMessage(owner, m) != IDYES) return;

    StartSecureDelete(std::move(files), passes);
}

static void EnqueueAndRun(TaskRequest task) {
    // 入队。已有 worker 在跑时**不再报错**，交给它按顺序处理后续任务。
    bool needStart = false;
    {
        // 入队与「由谁负责跑」的判定必须在同一把锁内。
        // 否则会出现竞态：worker 刚判空准备收工，任务刚好入队，
        // 于是新任务以为"已有 worker 会处理"，而 worker 已经退出 —— 任务永远不执行。
        std::lock_guard<std::mutex> lk(g_queueMutex);
        g_taskQueue.push_back(std::move(task));
        if (!g_workerRunning.exchange(true)) needStart = true;
    }
    if (g_mainWnd) PostMessageW(g_mainWnd, WM_APP_QUEUE_CHANGED, 0, 0);
    if (!needStart) return;   // 已有 worker：它会接着取队列

    std::thread worker([]() {
      for (;;) {
        TaskRequest req;
        {
            std::lock_guard<std::mutex> lk(g_queueMutex);
            if (g_taskQueue.empty()) {
                // 判空与清除运行标志同锁，杜绝上述竞态
                g_workerRunning = false;
                g_tasksDone = 0;    // 一批任务全部结束，计数清零
                break;
            }
            req = std::move(g_taskQueue.front());
            g_taskQueue.pop_front();
        }
        g_taskRunning = 1;   // 开始执行：总数里要算上它

        std::vector<std::wstring> files            = std::move(req.files);
        const bool                isSecureDelete   = req.isSecureDelete;
        const int                 reqDeletePasses  = req.deletePasses;
        std::vector<std::string>  perFilePasswords = std::move(req.perFilePasswords);
        const std::string         singlePassword   = std::move(req.singlePassword);
        std::vector<Protection>   perFileProts     = std::move(req.perFileProts);
        const Protection          singleProt       = req.singleProt;
        const std::wstring        decoyFile        = std::move(req.decoyFile);
        const std::string         decoyPwd         = std::move(req.decoyPwd);

        // ---- 「安全删除」任务：与加解密完全分开的一条路径 ----
        if (isSecureDelete) {
            RunSecureDeleteTask(files, reqDeletePasses);
            g_taskRunning = 0;
            g_tasksDone++;
            if (g_mainWnd) PostMessageW(g_mainWnd, WM_APP_QUEUE_CHANGED, 0, 0);
            continue;
        }

        // 设置快照在每个任务开始时重读 —— 所以设置改动对“下一个任务”生效，
        // 语言与主题则由设置对话框立即应用。
        const AppSettings& cfg = Settings();
        const int iterations = StrengthIterations(cfg.strength);
        const int deletePasses = (int)cfg.secureDelete;
        const bool keepSource = cfg.keepSource;   // 独立的「保留源文件」开关
        const OutDirMode outMode = cfg.outDir;
        const std::wstring fixedDir = cfg.fixedOutDir;
        const CompletionMode completion = cfg.completion;
        const bool english = g_langEnglish;
        const size_t fileCount = files.size();

        // 统计所有输入文件的总字节数，用于跨文件的整体进度。
        // 加密后如果还要覆写删除源文件，那部分耗时同样计入总工作量——
        // 否则大文件会在进度条到 100% 后长时间“卡住”（其实是在覆写）。
        const bool encryptingTask = !(files.empty() ||
            (files[0].size() >= 4 && files[0].substr(files[0].size() - 4) == L".enc"));
        const uint64_t deleteMultiplier =
            (encryptingTask && !keepSource && deletePasses > 0) ? (uint64_t)deletePasses : 0;

        uint64_t totalBytes = 0;
        for (const auto& f : files) {
            HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER li = {};
                if (GetFileSizeEx(h, &li) && li.QuadPart > 0)
                    totalBytes += (uint64_t)li.QuadPart * (1 + deleteMultiplier);
                CloseHandle(h);
            }
        }

        ProgressReset(totalBytes);
        ShowProgressBar(true);   // 所有文件都显示进度条（不再按大小判断）
        std::vector<std::wstring> failures;
        size_t processed = 0;

        auto postStatus = [](const std::wstring& text) {
            wchar_t* buf = new wchar_t[text.size() + 1];
            wcscpy_s(buf, text.size() + 1, text.c_str());
            PostMessageW(g_mainWnd, WM_APP_SET_STATUS, 0, (LPARAM)buf);
        };
        // 进度条上方显示的文件名：只取文件名，带路径会显示不全
        auto postFileName = [](const std::wstring& path) {
            const size_t pos = path.find_last_of(L"\\/");
            const std::wstring name = (pos == std::wstring::npos) ? path : path.substr(pos + 1);
            wchar_t* buf = new wchar_t[name.size() + 1];
            wcscpy_s(buf, name.size() + 1, name.c_str());
            PostMessageW(g_mainWnd, WM_APP_SET_FILE, 0, (LPARAM)buf);
        };
        auto postMessage = [](UiIcon icon, const std::wstring& caption,
                              const std::wstring& text, bool topmost) {
            UiMessagePayload* payload = new UiMessagePayload();
            payload->icon = icon;
            payload->caption = caption;
            payload->text = text;
            payload->topmost = topmost;
            PostMessageW(g_mainWnd, WM_APP_SHOW_MESSAGE, 0, (LPARAM)payload);
        };

        for (size_t idx = 0; idx < files.size(); ++idx) {
            const std::wstring& f = files[idx];
            const bool isEnc = (f.size() >= 4 && f.substr(f.size() - 4) == L".enc");

            // 进度条上方只显示文件名（不再显示整条路径），整体进度由下方“已完成 x / y”体现
            (void)isEnc;
            postFileName(f);

            std::string usePwd;
            if (!perFilePasswords.empty()) {
                if (idx < perFilePasswords.size()) usePwd = perFilePasswords[idx];
            }
            else {
                usePwd = singlePassword;
            }

            bool skipped = false;
            const std::wstring out_path = ResolveOutputPath(f, !isEnc, outMode, fixedDir, skipped);
            if (skipped) continue;   // 用户取消了这一个文件的保存对话框

            // 当前文件的原始大小：用于把覆写删除的工作量也计入进度
            uint64_t fileBytes = 0;
            {
                HANDLE hs = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (hs != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER li = {};
                    if (GetFileSizeEx(hs, &li) && li.QuadPart > 0) fileBytes = (uint64_t)li.QuadPart;
                    CloseHandle(hs);
                }
            }

            try {
                if (isEnc) {
                    FileEncryptor::decryptFileTo(f, out_path, usePwd);
                    // 加密文件本身是密文，不含明文残留，无需覆写；
                    // “禁用”= 保留不删，其余档位直接删除。
                    if (!keepSource) SecureDeleteFile(f, 0);
                }
                else {
                    // 每个文件可以有自己的防暴力破解策略（批量逐个设置密码时）
                    Protection prot = singleProt;
                    if (!perFileProts.empty() && idx < perFileProts.size())
                        prot = perFileProts[idx];
                    // 诱饵只在单文件任务里收集，批量时 decoyFile 为空
                    // 设置里勾了「禁用诱饵文件」时只写一个槽，体积回到约 1 倍
                    const bool singleSlot = Settings().decoyDisabled;
                    FileEncryptor::encryptFileTo(f, out_path, usePwd, iterations, prot,
                                                 singleSlot ? std::wstring() : decoyFile,
                                                 singleSlot ? std::string() : decoyPwd,
                                                 singleSlot);
                    // 源文件含明文，按设置覆写 N 次后删除；“禁用”= 保留不删。
                    // 覆写大文件很耗时，必须上报进度并切换阶段文案，
                    // 否则进度条会一直停在 100% 让人以为卡死。
                    if (!keepSource) {
                        if (deletePasses > 0) {
                            ProgressBeginFile(fileBytes * (uint64_t)deletePasses);
                            SecureDeleteFile(f, deletePasses, [](uint64_t n) { ProgressAdd(n); });
                            ProgressEndFile();
                                        }
                        else {
                            SecureDeleteFile(f, 0);
                        }
                    }
                }
                ++processed;
            }
            catch (const std::exception& e) {
                const std::wstring werr = narrow_to_wstring(e.what());
                std::wstring extra;

                // 批量「使用相同密码」时，只有第一个受保护文件在收集阶段校验过；
                // 其余文件如果实际解密失败（认证失败），就在这里各自扣减次数。
                // 这是有意的取舍：避免为 N 个文件逐个弹框，同时次数仍然会被记上。
                if (isEnc && werr.find(L"认证失败") != std::wstring::npos) {
                    FileProtState pst;
                    if (FileEncryptor::ReadProtState(f, pst) && pst.isProtected) {
                        std::wstring msg;
                        FileEncryptor::ConsumeAttempt(f, pst, msg);
                        if (!msg.empty()) extra = L"\n" + msg;
                    }
                }
                failures.push_back(f + L"\n" + werr + extra);
            }
        }

        ProgressFinish();

        g_taskRunning = 0;
        g_tasksDone++;
        if (g_mainWnd) PostMessageW(g_mainWnd, WM_APP_QUEUE_CHANGED, 0, 0);
        {
            // 重新在锁内确认一次：任务执行期间可能又有新任务入队
            std::lock_guard<std::mutex> lk(g_queueMutex);
            if (!g_taskQueue.empty()) continue;   // 还有排队任务：跳过“完成后行为”
        }

        if (!failures.empty()) {
            std::wstring body;
            for (size_t i = 0; i < failures.size(); ++i) {
                if (i) body += L"\n\n";
                body += failures[i];
            }
            postStatus(english ? L"Completed with errors" : L"完成（存在失败）");
            postMessage(UiIcon::Error,
                        english ? L"Failed" : L"失败",
                        (english ? L"Failed to process the following file(s):\n\n"
                                 : L"以下文件处理失败：\n\n") + body,
                        true);
        }
        else if (processed == 0) {
            // 全部被跳过（用户逐个取消了保存对话框）
            postStatus(english ? L"Cancelled" : L"已取消");
        }
        else if (completion == CompletionMode::CloseWindow) {
            // 完成后关闭窗口
            postStatus(english ? L"Done" : L"完成");
            if (g_mainWnd) PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
        }
        else {
            const bool encryptOp = files.empty() ||
                !(files[0].size() >= 4 && files[0].substr(files[0].size() - 4) == L".enc");
            std::wstring text;
            if (files.size() == 1) {
                if (encryptOp) {
                    text = keepSource ? tr(L"加密成功。", L"Encryption succeeded.")
                                      : tr(L"加密成功，源文件已删除。", L"Encryption succeeded, the source file was deleted.");
                }
                else {
                    text = keepSource ? tr(L"解密成功。", L"Decryption succeeded.")
                                      : tr(L"解密成功，加密文件已删除。", L"Decryption succeeded, the encrypted file was deleted.");
                }
            }
            else {
                text = (encryptOp ? tr(L"加密完成，共 ", L"Encryption completed for ")
                                  : tr(L"解密完成，共 ", L"Decryption completed for "))
                    + std::to_wstring(files.size())
                    + tr(L" 个文件。", L" file(s).");
                if (!keepSource) {
                    text += encryptOp ? tr(L"源文件已删除。", L" Source files were deleted.")
                                      : tr(L"加密文件已删除。", L" Encrypted files were deleted.");
                }
            }
            postStatus(english ? L"Ready" : L"就绪");
            postMessage(UiIcon::Info, english ? L"Success" : L"成功", text, true);
        }

      }   // for (;;) 任务队列循环（g_workerRunning 已在锁内清除）

      // 用户之前选择“后台运行”：任务结束后自动退出（进度条已关闭，这里再关窗口）
      if (g_runInBackground && g_mainWnd) {
          PostMessageW(g_mainWnd, WM_APP_BACKGROUND_DONE, 0, 0);
      }
    });
    worker.detach();
}

// 设置对话框：打开、回填、保存并让改动立即生效
static void ShowSettingsDialog(HWND owner) {
    const AppSettings& s = Settings();
    UiSettingsData d;
    d.language     = (int)s.language;
    d.theme        = (int)s.theme;
    d.outDir       = (int)s.outDir;
    d.fixedDir     = s.fixedOutDir;
    d.secureDelete = (int)s.secureDelete;
    d.completion   = (int)s.completion;
    d.strength     = (int)s.strength;
    d.decoyDisabled = s.decoyDisabled ? 1 : 0;
    d.keepSource   = s.keepSource ? 1 : 0;
    d.shellMenu    = s.shellMenu ? 1 : 0;

    if (!UiShowSettings(owner, d)) return;

    AppSettings& w = Settings();
    w.language     = (LangMode)d.language;
    w.theme        = (ThemeMode)d.theme;
    w.outDir       = (OutDirMode)d.outDir;
    w.fixedOutDir  = d.fixedDir;
    w.secureDelete = d.secureDelete;
    w.completion   = (CompletionMode)d.completion;
    w.strength     = (StrengthMode)d.strength;
    w.decoyDisabled = (d.decoyDisabled != 0);
    w.keepSource = (d.keepSource != 0);
    w.shellMenu  = (d.shellMenu != 0);
    SettingsSave();

    // 右键菜单开关立即生效（勾选就注册、取消就删除）
    ApplyShellMenuSetting();

    // 立即生效：语言 + 主题
    g_langEnglish = (w.language == LangMode::English);
    UpdateLanguageUI();
    ApplyMainWindowWidth();
    UiSetThemeOverride((int)w.theme);
    if (g_mainWnd) {
        UiApplyFrame(g_mainWnd, true, UiIsDark());
        ApplyProgressBarTheme();
        InvalidateRect(g_mainWnd, NULL, FALSE);
    }
    if (g_hBtn1) InvalidateRect(g_hBtn1, NULL, FALSE);
    if (g_hBtn2) InvalidateRect(g_hBtn2, NULL, FALSE);
    if (g_hLangBtn) InvalidateRect(g_hLangBtn, NULL, FALSE);
}

// “每次都询问”输出位置时，工作线程请求 UI 线程弹一次保存对话框
static LRESULT HandleAskOutputPath(HWND hWnd, LPARAM lParam) {
    AskOutputPath* req = reinterpret_cast<AskOutputPath*>(lParam);
    if (req) {
        const std::wstring picked = SaveFileDialog(req->suggested);
        req->cancelled = picked.empty();
        req->result = picked;
        if (req->done) SetEvent(req->done);
    }
    (void)hWnd;
    return 0;
}

// ---------- 主入口：使用 WinMain (纯 GUI 应用，不创建控制台) ----------
// ==================== 资源管理器右键菜单支持 ====================
//
// 右键菜单用 MultiSelectModel=Player：Explorer 会**每个选中的文件启动一个进程**。
// 所以必须做单实例 —— 第一个进程建窗口，后面的把自己的文件清单转发过去再退出。
// 这样批量、进度条、后台托盘、队列计数全都自然复用，不需要另写一套。
//
// 菜单由 HKCU\Software\Classes\*\shell 注册（不需要管理员权限）。

// 解析命令行：FileEncryptor.exe --encrypt|--secure-delete <文件...>
static LaunchRequest ParseLaunchArgs() {
    LaunchRequest req;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return req;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--encrypt")            req.kind = LaunchKind::Encrypt;
        else if (a == L"--secure-delete") req.kind = LaunchKind::SecureDelete;
        else if (!a.empty() && a[0] != L'-') req.files.push_back(a);
    }
    LocalFree(argv);
    return req;
}

// 打包成 WM_COPYDATA 用的扁平缓冲：[int kind][int count][路径\0][路径\0]...
static std::vector<BYTE> PackLaunch(const LaunchRequest& r) {
    std::vector<BYTE> buf;
    const int kind = (int)r.kind;
    const int count = (int)r.files.size();
    const BYTE* k = (const BYTE*)&kind;
    const BYTE* c = (const BYTE*)&count;
    buf.insert(buf.end(), k, k + sizeof(kind));
    buf.insert(buf.end(), c, c + sizeof(count));
    for (const auto& f : r.files) {
        const BYTE* p = (const BYTE*)f.c_str();
        buf.insert(buf.end(), p, p + (f.size() + 1) * sizeof(wchar_t));
    }
    return buf;
}

static bool UnpackLaunch(const BYTE* data, DWORD size, LaunchRequest& out) {
    if (!data || size < sizeof(int) * 2) return false;
    int kind = 0, count = 0;
    memcpy(&kind, data, sizeof(kind));
    memcpy(&count, data + sizeof(int), sizeof(count));
    out.kind = (LaunchKind)kind;
    out.files.clear();

    size_t off = sizeof(int) * 2;
    for (int i = 0; i < count && off + sizeof(wchar_t) <= size; ++i) {
        const wchar_t* s = (const wchar_t*)(data + off);
        const size_t maxChars = (size - off) / sizeof(wchar_t);
        size_t len = 0;
        while (len < maxChars && s[len]) ++len;
        out.files.push_back(std::wstring(s, len));
        off += (len + 1) * sizeof(wchar_t);
    }
    return true;
}

// 注册 / 更新右键菜单（幂等；每次启动都跑一遍，
// 这样即使 exe 被移动过，菜单里的路径也会自动纠正）
static void RegisterShellMenu() {
    wchar_t exe[MAX_PATH * 2] = {};
    if (!GetModuleFileNameW(NULL, exe, _countof(exe))) return;

    struct Verb { const wchar_t* key; const wchar_t* zh; const wchar_t* en; const wchar_t* arg; };
    const Verb verbs[] = {
        { L"FileEncryptor.Encrypt",      L"用FileEncryptor加密",     L"Encrypt with FileEncryptor",      L"--encrypt" },
        { L"FileEncryptor.SecureDelete", L"用FileEncryptor安全删除", L"Securely delete with FileEncryptor", L"--secure-delete" },
    };

    const bool english = (Settings().language == LangMode::English);
    for (const Verb& v : verbs) {
        const std::wstring base = std::wstring(L"Software\\Classes\\*\\shell\\") + v.key;

        HKEY hk = NULL;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, base.c_str(), 0, NULL, 0,
                            KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
            const wchar_t* title = english ? v.en : v.zh;
            RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE*)title,
                           (DWORD)((wcslen(title) + 1) * sizeof(wchar_t)));
            // 图标
            const std::wstring icon = std::wstring(exe) + L",0";
            RegSetValueExW(hk, L"Icon", 0, REG_SZ, (const BYTE*)icon.c_str(),
                           (DWORD)((icon.size() + 1) * sizeof(wchar_t)));
            // Player：每个文件单独启动一次进程（不限制数量），靠单实例转发汇总
            const wchar_t* model = L"Player";
            RegSetValueExW(hk, L"MultiSelectModel", 0, REG_SZ, (const BYTE*)model,
                           (DWORD)((wcslen(model) + 1) * sizeof(wchar_t)));
            RegCloseKey(hk);

            const std::wstring cmdKey = base + L"\\command";
            if (RegCreateKeyExW(HKEY_CURRENT_USER, cmdKey.c_str(), 0, NULL, 0,
                                KEY_WRITE, NULL, &hk, NULL) == ERROR_SUCCESS) {
                const std::wstring cmd = std::wstring(L"\"") + exe + L"\" " + v.arg + L" \"%1\"";
                RegSetValueExW(hk, NULL, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                               (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
                RegCloseKey(hk);
            }
        }
    }
}

// 删除已注册的右键菜单项
static void UnregisterShellMenu() {
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\FileEncryptor.Encrypt");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\*\\shell\\FileEncryptor.SecureDelete");
}

// 按设置注册或注销右键菜单。启动时与设置保存后各调一次。
static void ApplyShellMenuSetting() {
    if (Settings().shellMenu) RegisterShellMenu();
    else                      UnregisterShellMenu();
}

// 把请求转发给已在运行的实例；成功返回 true
static bool ForwardToExisting(const LaunchRequest& req) {
    if (req.files.empty() || req.kind == LaunchKind::None) return false;
    const std::vector<BYTE> buf = PackLaunch(req);

    // 第一个实例可能还在建窗口，重试几秒（右键多选时会同时冒出多个进程）
    for (int i = 0; i < 60; ++i) {
        HWND h = FindWindowW(L"FileEncryptorGUI", NULL);
        if (h) {
            COPYDATASTRUCT cds = {};
            cds.dwData = kLaunchCopyDataId;
            cds.cbData = (DWORD)buf.size();
            cds.lpData = (void*)buf.data();
            if (SendMessageW(h, WM_COPYDATA, 0, (LPARAM)&cds)) return true;
        }
        Sleep(100);
    }
    return false;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    (void)hPrevInstance; (void)lpCmdLine; // 未使用的参数

    g_exe_dir = GetExeDirectoryW();

    // 必须在创建任何窗口之前声明 DPI 感知。
    // 默认进程是 DPI 未感知的，系统会把整个窗口位图拉伸，导致在 125% / 150% 等
    // 缩放下界面发虚；声明为系统级感知后，UiPx() 按真实 DPI 计算尺寸，界面清晰。
    // 这里用 SetProcessDPIAware()（Vista+，user32 一直导出），
    // 不用 SetProcessDpiAwarenessContext()——那是 Win10 1607+ 的导出，
    // 静态链接会让程序在 Win7/8 上因找不到入口点而无法启动。
    SetProcessDPIAware();

    // 文件/文件夹选择对话框需要 COM
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 读取设置并应用语言 / 主题
    SettingsLoad();
    g_langEnglish = (Settings().language == LangMode::English);

    // ---- 资源管理器右键菜单：命令行请求 + 单实例转发 ----
    const LaunchRequest launch = ParseLaunchArgs();

    HANDLE hSingle = CreateMutexW(NULL, TRUE, L"FileEncryptor.SingleInstance");
    const bool isFirstInstance = (hSingle != NULL && GetLastError() != ERROR_ALREADY_EXISTS);
    if (!isFirstInstance && launch.kind != LaunchKind::None) {
        // 已有实例在跑：把文件交过去，自己立刻退出
        if (ForwardToExisting(launch)) return 0;
        // 转发失败（对方还没建好窗口）就自己当第一个实例继续
    }

    // 菜单项标题跟随语言，且每次启动都刷新，exe 移动过也能自动纠正；
    // 设置里关掉时会顺手把已注册的菜单项删掉
    ApplyShellMenuSetting();

    if (launch.kind != LaunchKind::None && !launch.files.empty()) {
        g_pendingLaunch = launch.files;
        g_pendingKind = (int)launch.kind;
        g_hasPendingLaunch = true;
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    // 主题：先应用用户覆盖（跟随系统/浅色/深色），再预热调色板与字体
    UiSetThemeOverride((int)Settings().theme);
    UiThemeInit();

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   // 背景由 WM_PAINT 绘制（支持深色模式）
    wc.lpszClassName = L"FileEncryptorGUI";
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    // 以“客户端区域”为目标尺寸；初始为空闲时的紧凑高度
    // WS_CLIPCHILDREN 必须加：否则父窗口 WM_PAINT 会把整块客户区刷成背景色，
    // 覆盖掉子控件（尤其是刚由 ShowWindow 显示出来的取消按钮）。
    const DWORD style = (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME) | WS_CLIPCHILDREN;
    RECT rcDesired = { 0, 0, MainClientWidth(), MainClientHeight(false) };
    AdjustWindowRectEx(&rcDesired, style, FALSE, 0);

    HWND hWnd = CreateWindowExW(0, L"FileEncryptorGUI", L"",
        style, CW_USEDEFAULT, CW_USEDEFAULT,
        rcDesired.right - rcDesired.left, rcDesired.bottom - rcDesired.top,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) return 0;

    ShowWindow(hWnd, nShowCmd);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}
