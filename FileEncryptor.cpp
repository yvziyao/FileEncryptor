#define OPENSSL_API_COMPAT 0x10100000L
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
#include "C:\\Program Files\\OpenSSL-Win64\\include\\openssl\\evp.h"
#include "C:\\Program Files\\OpenSSL-Win64\\include\\openssl\\rand.h"
#include "C:\\Program Files\\OpenSSL-Win64\\include\\openssl\\err.h"
#include "C:\\Program Files\\OpenSSL-Win64\\include\\openssl\\aes.h"
#include "resource.h"
#include "app_settings.h"
#include "winui_password.h"
#include "winui_dialog.h"
#include "ui_theme.h"
#include "FileEncryptor_helpers.h"

// 圆角/主题绘制统一由 ui_theme 提供（基于 Direct2D），此处不再单独持有 D2D/DWrite 资源
#include <thread>
#include <atomic>

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
HWND g_hLangBtn = NULL; // 语言切换按钮
bool g_hoverBtn[8] = {};      // 下标 = 控件 ID，鼠标是否悬停在该按钮上
bool g_trackingMouse = false;
HWND g_progressBar = NULL;
std::atomic<bool> g_workerRunning(false);
// 主窗口句柄
HWND g_mainWnd = NULL;

// 语言控制
bool g_langEnglish = false; // false = 中文, true = English
void UpdateLanguageUI();

// 底部状态文本（自绘，以支持深色模式）
std::wstring g_statusText;

// 自定义消息
#define WM_APP_SET_STATUS    (WM_APP + 1)
#define WM_APP_SHOW_PROGRESS (WM_APP + 2)
#define WM_APP_UPDATE_PROGRESS (WM_APP + 3)
#define WM_APP_SHOW_MESSAGE  (WM_APP + 4)
#define WM_APP_ASK_OUTPUT_PATH (WM_APP + 5)

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
static void ProgressEndFile();
static void ProgressFinish();
static void CollectFilesRecursive(const std::wstring& path, std::vector<std::wstring>& out);
static void SecureDeleteFile(const std::wstring& wPath, int passes);
static void StartProcessing(std::vector<std::wstring> files,
                            std::vector<std::string> perFilePasswords,
                            std::string singlePassword);
static void ShowUiMessage(HWND owner, UiIcon icon, const std::wstring& caption,
                          const std::wstring& text, bool topmost);
static void ApplyProgressBarTheme();
static void ShowSettingsDialog(HWND owner);
static LRESULT HandleAskOutputPath(HWND hWnd, LPARAM lParam);
static std::wstring PasswordSubtitle(const std::wstring& file, size_t index, size_t total);

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
                        const std::wstring& subtitle = std::wstring()) {
    return ShowPasswordDialogWinUI(hWnd, password, confirm, subtitle);
}


// ---------- 加密核心类 ----------
// 文件格式 v1（AES-256-GCM）：
//   magic[8]      "FENC\r\n\x1a\n"  用于与旧格式（直接以 salt 开头）区分
//   version[1]    = 1
//   reserved[1]   = 0
//   iterations[4] PBKDF2-HMAC-SHA256 迭代次数（小端），即“加密强度”
//   salt[16]
//   iv[12]        GCM 推荐 96-bit nonce
//   ciphertext[N] 与明文等长（GCM 是流模式，无需填充）
//   tag[16]       GCM 认证标签
//
// 旧格式（AES-256-CBC：salt[16] + iv[16] + ciphertext，迭代 100000）仍可解密，
// 保证此前加密的文件不会失效。
class FileEncryptor {
private:
    struct EVP_CIPHER_CTX_deleter {
        void operator()(EVP_CIPHER_CTX* p) { EVP_CIPHER_CTX_free(p); }
    };
    struct HandleGuard {
        HANDLE h;
        explicit HandleGuard(HANDLE x) : h(x) {}
        ~HandleGuard() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
        HandleGuard(const HandleGuard&) = delete;
        HandleGuard& operator=(const HandleGuard&) = delete;
    };

    static const size_t kSaltLen = 16;
    static const size_t kIvLen = 12;      // GCM 推荐 96-bit
    static const size_t kTagLen = 16;
    static const size_t kHeaderLen = 8 + 1 + 1 + 4 + kSaltLen + kIvLen;   // 42

    static const uint8_t* Magic() {
        static const uint8_t m[8] = { 'F', 'E', 'N', 'C', '\r', '\n', 0x1A, '\n' };
        return m;
    }

    static void PutU32(std::vector<uint8_t>& v, uint32_t x) {
        v.push_back((uint8_t)(x & 0xFF));
        v.push_back((uint8_t)((x >> 8) & 0xFF));
        v.push_back((uint8_t)((x >> 16) & 0xFF));
        v.push_back((uint8_t)((x >> 24) & 0xFF));
    }
    static uint32_t GetU32(const uint8_t* p) {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }

    static std::vector<uint8_t> DeriveKey(const std::string& password,
                                          const std::vector<uint8_t>& salt,
                                          int iterations) {
        std::vector<uint8_t> key(32);
        if (PKCS5_PBKDF2_HMAC(password.c_str(), (int)password.length(),
                              salt.data(), (int)salt.size(),
                              iterations, EVP_sha256(),
                              (int)key.size(), key.data()) != 1) {
            throw std::runtime_error("密钥派生失败");
        }
        return key;
    }

    static void DecryptImpl(const std::wstring& input_path, const std::wstring& out_path,
                            const std::string& password);

public:
    // 加密：始终写出新格式（AES-256-GCM）
    static void encryptFileTo(const std::wstring& input_path, const std::wstring& out_path,
                              const std::string& password, int iterations) {
        const size_t CHUNK = 4 * 1024 * 1024; // 4MB
        if (iterations <= 0) iterations = 100000;

        std::vector<uint8_t> salt(kSaltLen), iv(kIvLen);
        if (RAND_bytes(salt.data(), (int)salt.size()) != 1) throw std::runtime_error("生成盐失败");
        if (RAND_bytes(iv.data(), (int)iv.size()) != 1) throw std::runtime_error("生成IV失败");

        std::vector<uint8_t> key = DeriveKey(password, salt, iterations);

        HANDLE hFile = CreateFileW(input_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开输入文件");
        HandleGuard inGuard(hFile);

        LARGE_INTEGER fileSizeLI;
        if (!GetFileSizeEx(hFile, &fileSizeLI)) throw std::runtime_error("获取文件大小失败");
        const uint64_t fileSize = (uint64_t)fileSizeLI.QuadPart;

        HANDLE hOutFile = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, NULL,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOutFile == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建输出文件");
        HandleGuard outGuard(hOutFile);

        // 文件头
        std::vector<uint8_t> header;
        header.reserve(kHeaderLen);
        const uint8_t* magic = Magic();
        header.insert(header.end(), magic, magic + 8);
        header.push_back(1);                       // version
        header.push_back(0);                       // reserved
        PutU32(header, (uint32_t)iterations);
        header.insert(header.end(), salt.begin(), salt.end());
        header.insert(header.end(), iv.begin(), iv.end());
        DWORD bytesWritten = 0;
        WriteFile(hOutFile, header.data(), (DWORD)header.size(), &bytesWritten, NULL);

        std::unique_ptr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_deleter> ctx(EVP_CIPHER_CTX_new());
        if (!ctx) throw std::runtime_error("创建EVP上下文失败");
        if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            throw std::runtime_error("AES初始化失败");
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, (int)kIvLen, nullptr) != 1)
            throw std::runtime_error("设置GCM IV长度失败");
        if (EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv.data()) != 1)
            throw std::runtime_error("设置密钥失败");

        std::vector<uint8_t> inbuf(CHUNK);
        std::vector<uint8_t> outbuf(CHUNK + 32);
        DWORD bytesRead = 0;
        ProgressBeginFile(fileSize);

        while (ReadFile(hFile, inbuf.data(), (DWORD)inbuf.size(), &bytesRead, NULL) && bytesRead > 0) {
            int outlen = 0;
            if (EVP_EncryptUpdate(ctx.get(), outbuf.data(), &outlen, inbuf.data(), (int)bytesRead) != 1)
                throw std::runtime_error("AES加密失败");
            if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }
            ProgressAdd(bytesRead);
        }

        int outlen = 0;
        if (EVP_EncryptFinal_ex(ctx.get(), outbuf.data(), &outlen) != 1)
            throw std::runtime_error("AES完成加密失败");
        if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }
        ProgressEndFile();

        uint8_t tag[kTagLen] = {};
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, (int)kTagLen, tag) != 1)
            throw std::runtime_error("获取认证标签失败");
        WriteFile(hOutFile, tag, (DWORD)kTagLen, &bytesWritten, NULL);
    }

    // 解密：自动识别新旧格式，失败时清理半成品输出文件
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
    const size_t CHUNK = 4 * 1024 * 1024; // 4MB

    HANDLE hFile = CreateFileW(input_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开加密文件");
    HandleGuard inGuard(hFile);

    LARGE_INTEGER fileSizeLI;
    if (!GetFileSizeEx(hFile, &fileSizeLI)) throw std::runtime_error("获取文件大小失败");
    const uint64_t fileSize = (uint64_t)fileSizeLI.QuadPart;

    // 嗅探文件头：新版以魔数开头，旧版直接是随机 salt
    uint8_t head[8] = {};
    DWORD got = 0;
    if (!ReadFile(hFile, head, 8, &got, NULL) || got != 8) throw std::runtime_error("加密文件过小或已被截断");
    const bool isNewFormat = (memcmp(head, Magic(), 8) == 0);

    std::vector<uint8_t> salt(kSaltLen);
    std::vector<uint8_t> iv;
    int iterations = 100000;      // 旧格式固定 100000
    size_t prefixLen = 0;

    if (isNewFormat) {
        uint8_t meta[1 + 1 + 4] = {};
        if (!ReadFile(hFile, meta, sizeof(meta), &got, NULL) || got != sizeof(meta))
            throw std::runtime_error("读取文件头失败");
        if (meta[0] != 1) throw std::runtime_error("不支持的加密文件版本");
        iterations = (int)GetU32(&meta[2]);
        if (iterations < 1000 || iterations > 10000000)
            throw std::runtime_error("文件头中的迭代次数非法");

        if (!ReadFile(hFile, salt.data(), (DWORD)salt.size(), &got, NULL) || got != salt.size())
            throw std::runtime_error("读取盐失败");
        iv.resize(kIvLen);
        if (!ReadFile(hFile, iv.data(), (DWORD)iv.size(), &got, NULL) || got != iv.size())
            throw std::runtime_error("读取IV失败");
        prefixLen = kHeaderLen;
    }
    else {
        // 旧格式：salt[16] + iv[16] + CBC 密文
        SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
        if (!ReadFile(hFile, salt.data(), (DWORD)salt.size(), &got, NULL) || got != salt.size())
            throw std::runtime_error("读取盐失败");
        iv.resize(16);
        if (!ReadFile(hFile, iv.data(), (DWORD)iv.size(), &got, NULL) || got != iv.size())
            throw std::runtime_error("读取IV失败");
        prefixLen = salt.size() + iv.size();
    }

    const uint64_t overhead = (uint64_t)prefixLen + (isNewFormat ? kTagLen : 0);
    if (fileSize <= overhead) throw std::runtime_error("加密文件不完整");

    std::vector<uint8_t> key = DeriveKey(password, salt, iterations);

    HANDLE hOutFile = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, NULL,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hOutFile == INVALID_HANDLE_VALUE) throw std::runtime_error("无法创建输出文件");
    HandleGuard outGuard(hOutFile);

    std::unique_ptr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_deleter> ctx(EVP_CIPHER_CTX_new());
    if (!ctx) throw std::runtime_error("创建EVP上下文失败");

    if (isNewFormat) {
        if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
            throw std::runtime_error("AES初始化失败");
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, (int)kIvLen, nullptr) != 1)
            throw std::runtime_error("设置GCM IV长度失败");
        if (EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key.data(), iv.data()) != 1)
            throw std::runtime_error("设置密钥失败");
    }
    else {
        if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1)
            throw std::runtime_error("AES初始化失败");
    }

    const uint64_t cipherLen = fileSize - overhead;
    uint64_t remaining = cipherLen;

    std::vector<uint8_t> inbuf(CHUNK);
    std::vector<uint8_t> outbuf(CHUNK + 32);

    ProgressBeginFile(fileSize);

    while (remaining > 0) {
        const DWORD want = (DWORD)(remaining < (uint64_t)CHUNK ? remaining : (uint64_t)CHUNK);
        DWORD rd = 0;
        if (!ReadFile(hFile, inbuf.data(), want, &rd, NULL) || rd == 0)
            throw std::runtime_error("读取加密文件失败");
        int outlen = 0;
        if (EVP_DecryptUpdate(ctx.get(), outbuf.data(), &outlen, inbuf.data(), (int)rd) != 1)
            throw std::runtime_error("AES解密失败");
        if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }
        remaining -= rd;
        ProgressAdd(rd);
    }

    if (isNewFormat) {
        uint8_t tag[kTagLen] = {};
        DWORD rd = 0;
        if (!ReadFile(hFile, tag, (DWORD)kTagLen, &rd, NULL) || rd != kTagLen)
            throw std::runtime_error("读取认证标签失败");
        if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, (int)kTagLen, tag) != 1)
            throw std::runtime_error("设置认证标签失败");
        int outlen = 0;
        if (EVP_DecryptFinal_ex(ctx.get(), outbuf.data(), &outlen) != 1)
            throw std::runtime_error("认证失败：密码错误或文件已被篡改");
        if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }
    }
    else {
        int outlen = 0;
        if (EVP_DecryptFinal_ex(ctx.get(), outbuf.data(), &outlen) != 1)
            throw std::runtime_error("解密失败：密码错误或文件已被篡改");
        if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }
    }

    ProgressEndFile();
}

// 安全删除：passes 为覆写次数（0 = 直接删除）
static void SecureDeleteFile(const std::wstring& wPath, int passes) {
    if (passes <= 0) {
        DeleteFileW(wPath.c_str());
        return;
    }
    try {
        HANDLE hFile = CreateFileW(wPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER fileSizeLI;
            if (GetFileSizeEx(hFile, &fileSizeLI) && fileSizeLI.QuadPart > 0) {
                uint64_t fileSize = (uint64_t)fileSizeLI.QuadPart;
                const size_t CHUNK = 64 * 1024; // 64KB
                std::vector<uint8_t> buf(CHUNK);
                for (int pass = 0; pass < passes; ++pass) {
                    LARGE_INTEGER zero = { 0 };
                    SetFilePointerEx(hFile, zero, NULL, FILE_BEGIN);
                    uint64_t remaining = fileSize;
                    while (remaining > 0) {
                        size_t toWrite = (size_t)(remaining < (uint64_t)CHUNK ? remaining : (uint64_t)CHUNK);
                        RAND_bytes(buf.data(), (int)toWrite);
                        // 交替写入随机数据与随机数据的反码，覆盖更彻底
                        if (pass % 2 == 1) {
                            for (size_t i = 0; i < toWrite; ++i) buf[i] = (uint8_t)~buf[i];
                        }
                        DWORD written = 0;
                        WriteFile(hFile, buf.data(), (DWORD)toWrite, &written, NULL);
                        remaining -= written;
                    }
                    FlushFileBuffers(hFile);
                }
            }
            CloseHandle(hFile);
        }
    }
    catch (...) {
        // best-effort：无论如何都要尝试删除
    }
    DeleteFileW(wPath.c_str());
}

// ---------- GUI 部分 ----------
#include "FileEncryptor_helpers.h"

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
        if (!g_progressBar) return;
        ShowWindow(g_progressBar, show ? SW_SHOW : SW_HIDE);
        if (show) {
            SendMessageW(g_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
        }
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
        if (!g_progressBar) return;
        SendMessageW(g_progressBar, PBM_SETPOS, pos, 0);
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
static double    g_progShown = 0.0;           // 进度条当前显示值 0..100
static double    g_progTarget = 0.0;          // 进度条目标值 0..100

static void StartAnimation();

// 主界面按钮的子类化过程：按钮自己跟踪鼠标进入/离开。
// 之前靠父窗口的 WM_MOUSEMOVE 判断，但鼠标落在子控件上时父窗口收不到该消息，
// 导致主界面按钮实际上从来没有悬停高亮。
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

    if (old) SelectObject(dc, old);
    ReleaseDC(NULL, dc);

    const int bw = textW + UiPx(52);   // 左右各 26px 内边距
    return bw > UiPx(108) ? bw : UiPx(108);
}

static int MainClientWidth() {
    return MainButtonWidth() * 2 + UiPx(16) + UiPx(2 * 28);
}

// 顶部第一行留给右上角的设置按钮，主按钮放在它下方，两者不重叠
static const int kMainBtnY = 46;    // 96dpi 设计值

static int MainClientHeight(bool busy) {
    const int btnY = UiPx(kMainBtnY);
    const int btnH = UiPx(44);
    const int statusH = UiPx(28);
    if (!busy) return btnY + btnH + UiPx(20) + statusH;

    // 工作中只需要多让出进度条的位置（已无取消按钮）
    const int progY = btnY + btnH + UiPx(18);
    const int progH = UiPx(14);
    return progY + progH + UiPx(20) + statusH;
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

    // 3) 进度条数值平滑（约 150ms 追上目标）
    if (g_progressBar) {
        const double diff = g_progTarget - g_progShown;
        if (diff > 0.25 || diff < -0.25) {
            double k = dt / 0.15;
            if (k > 1.0) k = 1.0;
            g_progShown += diff * k;
            SendMessageW(g_progressBar, PBM_SETPOS, (int)(g_progShown + 0.5), 0);
            active = true;
        }
        else if (g_progShown != g_progTarget) {
            g_progShown = g_progTarget;
            SendMessageW(g_progressBar, PBM_SETPOS, (int)(g_progShown + 0.5), 0);
        }
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
        // 语言切换按钮（右上角），同样 owner-draw 以保持风格一致
        HWND hLang = CreateWindowW(L"BUTTON", g_langEnglish ? L"EN" : L"中", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 360, 10, 40, 28, hWnd, (HMENU)5, GetModuleHandle(NULL), NULL);
        g_hBtn1 = hBtn1;
        g_hBtn2 = hBtn2;
        g_hLangBtn = hLang;

        // 按钮自己跟踪鼠标悬停（父窗口在鼠标位于子控件上时收不到 WM_MOUSEMOVE）
        if (hBtn1) SetWindowSubclass(hBtn1, MainBtnSubclassProc, 1, 0);
        if (hBtn2) SetWindowSubclass(hBtn2, MainBtnSubclassProc, 2, 0);
        if (hLang) SetWindowSubclass(hLang, MainBtnSubclassProc, 5, 0);

        if (uiFont) {
            SendMessageW(hBtn1, WM_SETFONT, (WPARAM)uiFont, TRUE);
            SendMessageW(hBtn2, WM_SETFONT, (WPARAM)uiFont, TRUE);
            if (g_hLangBtn) SendMessageW(g_hLangBtn, WM_SETFONT, (WPARAM)uiFont, TRUE);
        }

        // 创建进度条（默认隐藏）。关闭视觉样式后 PBM_SETBKCOLOR/PBM_SETBARCOLOR
        // 才会生效，从而让进度条跟随深/浅色主题。
        g_progressBar = CreateWindowExW(0, PROGRESS_CLASS, NULL, WS_CHILD | PBS_SMOOTH, 20, 110, 340, 16, hWnd, (HMENU)3, GetModuleHandle(NULL), NULL);
        if (g_progressBar) {
            UiDetheme(g_progressBar);
            SendMessageW(g_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
            ApplyProgressBarTheme();
            ShowWindow(g_progressBar, SW_HIDE);
        }

        // 接受文件拖拽
        DragAcceptFiles(hWnd, TRUE);

        // 主窗口使用 DWM 圆角 + 跟随系统的深色标题栏
        UiApplyFrame(hWnd, true, UiIsDark());
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

            COLORREF fill, textColor, borderColor;
            if (isPrimary) {
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

        if (g_workerRunning) {
            ShowUiMessage(hWnd, UiIcon::Info, tr(L"提示", L"Notice"),
                          tr(L"已有任务正在运行，请稍候。", L"A task is already running; please wait."), false);
            break;
        }

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

        if (hasEnc) {
            if (files.size() == 1) {
                // 单文件解密：直接输入密码（不确认）
                if (!ShowPasswordDialog(hWnd, pwd_single, false, files[0])) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
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
                    bool cancelled = false;
                    for (size_t i = 0; i < files.size(); ++i) {
                        std::string onepwd;
                        if (!ShowPasswordDialog(hWnd, onepwd, false,
                                                PasswordSubtitle(files[i], i, files.size()))) {
                            cancelled = true;
                            break;
                        }
                        perFilePasswords.push_back(onepwd);
                    }
                    if (cancelled) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
                }
                else {
                    if (!ShowPasswordDialog(hWnd, pwd_single, false, files[0])) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
                }
            }
        }
        else {
            if (files.size() == 1) {
                // 单文件加密：要求确认密码
                if (!ShowPasswordDialog(hWnd, pwd_single, true, files[0])) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
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
                    if (!ShowPasswordDialog(hWnd, pwd_single, true, files[0])) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
                }
                else {
                    bool cancelled = false;
                    for (size_t i = 0; i < files.size(); ++i) {
                        std::string onepwd;
                        if (!ShowPasswordDialog(hWnd, onepwd, true,
                                                PasswordSubtitle(files[i], i, files.size()))) {
                            cancelled = true;
                            break;
                        }
                        perFilePasswords.push_back(onepwd);
                    }
                    if (cancelled) { SetStatusText(tr(L"已取消", L"Cancelled")); break; }
                }
            }
        }

        StartProcessing(files, perFilePasswords, pwd_single);
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

        int btnX = (W - (btnW * 2 + btnGap)) / 2;
        if (btnX < UiPx(12)) btnX = UiPx(12);
        if (g_hBtn1) SetWindowPos(g_hBtn1, NULL, btnX, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_hBtn2) SetWindowPos(g_hBtn2, NULL, btnX + btnW + btnGap, btnY, btnW, btnH, SWP_NOZORDER | SWP_NOACTIVATE);

        const int pad = UiPx(24);
        const int progY = btnY + btnH + UiPx(20);
        const int progH = UiPx(14);
        if (g_progressBar) {
            SetWindowPos(g_progressBar, NULL, pad, progY, W - pad * 2, progH, SWP_NOZORDER | SWP_NOACTIVATE);
        }

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

            // 底部状态栏（自绘，跟随深浅色主题）
            const int statusH = UiPx(28);
            RECT statusRc = { 0, rc.bottom - statusH, rc.right, rc.bottom };
            UiFillRect(dc, statusRc, c.bgAlt);
            RECT line = { 0, statusRc.top, rc.right, statusRc.top + 1 };
            UiFillRect(dc, line, c.border);

            RECT textRc = { UiPx(12), statusRc.top, rc.right - UiPx(12), statusRc.bottom };
            UiDrawTextLine(dc, g_statusText, textRc, UiFont(UiPx(13), false), c.textMuted,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
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

            if (g_workerRunning) {
                ShowUiMessage(hWnd, UiIcon::Info, tr(L"提示", L"Notice"),
                              tr(L"已有任务正在运行，请稍候。", L"A task is already running; please wait."), false);
                break;
            }

            std::string password;
            if (!ShowPasswordDialog(hWnd, password, encrypting, input_path)) {
                SetStatusText(tr(L"已取消", L"Cancelled"));
                break;
            }

            // 放到后台线程执行，避免界面卡死（不再出现“无响应”）
            StartProcessing({ input_path }, {}, password);
            break;
        }

        break;
    }

    case WM_DESTROY:
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
        if (g_progressBar) {
            if (show) {
                g_progShown = 0.0;
                g_progTarget = 0.0;
                SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
                ShowWindow(g_progressBar, SW_SHOW);
            }
            else {
                // 收尾：直接对齐到目标值，避免进度条停在半路
                g_progShown = g_progTarget;
                ShowWindow(g_progressBar, SW_HIDE);
            }
        }
        break;
    }
    case WM_APP_UPDATE_PROGRESS: {
        // 只更新目标值；实际显示由动画定时器平滑逼近
        g_progTarget = (double)(int)wParam;
        if (g_progressBar) StartAnimation();
        break;
    }

    case WM_APP_ASK_OUTPUT_PATH:
        return HandleAskOutputPath(hWnd, lParam);

    case WM_TIMER: {
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

// 进度条配色（关闭视觉样式后 PBM_SETBKCOLOR / PBM_SETBARCOLOR 才会生效）
static void ApplyProgressBarTheme() {
    if (!g_progressBar) return;
    const UiPalette& c = UiColors();
    SendMessageW(g_progressBar, PBM_SETBKCOLOR, 0, (LPARAM)c.track);
    SendMessageW(g_progressBar, PBM_SETBARCOLOR, 0, (LPARAM)c.accent);
}

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

// 在后台线程中处理文件队列，避免阻塞 UI 线程（消除“无响应”）
static void StartProcessing(std::vector<std::wstring> files,
                            std::vector<std::string> perFilePasswords,
                            std::string singlePassword) {
    if (g_workerRunning.exchange(true)) {
        ShowUiMessage(g_mainWnd, UiIcon::Info, tr(L"提示", L"Notice"),
                      tr(L"已有任务正在运行，请稍候。", L"A task is already running; please wait."), false);
        return;
    }

    // 取一份设置快照：本次任务全程使用，中途改设置不影响正在进行的工作
    const AppSettings& cfg = Settings();
    const int iterations = StrengthIterations(cfg.strength);
    const int deletePasses = (int)cfg.secureDelete;
    const bool keepSource = (cfg.secureDelete == SecureDeleteMode::Off);   // 禁用 = 保留不删
    const OutDirMode outMode = cfg.outDir;
    const std::wstring fixedDir = cfg.fixedOutDir;
    const CompletionMode completion = cfg.completion;
    const bool english = g_langEnglish;
    const size_t fileCount = files.size();

    // 统计所有输入文件的总字节数，用于跨文件的整体进度
    uint64_t totalBytes = 0;
    for (const auto& f : files) {
        HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li = {};
            if (GetFileSizeEx(h, &li) && li.QuadPart > 0) totalBytes += (uint64_t)li.QuadPart;
            CloseHandle(h);
        }
    }

    ProgressReset(totalBytes);
    ShowProgressBar(true);   // 所有文件都显示进度条（不再按大小判断）

    std::thread worker([files, perFilePasswords, singlePassword, english, fileCount,
                        iterations, deletePasses, keepSource, outMode, fixedDir, completion]() mutable {
        std::vector<std::wstring> failures;
        size_t processed = 0;

        auto postStatus = [](const std::wstring& text) {
            wchar_t* buf = new wchar_t[text.size() + 1];
            wcscpy_s(buf, text.size() + 1, text.c_str());
            PostMessageW(g_mainWnd, WM_APP_SET_STATUS, 0, (LPARAM)buf);
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

            const std::wstring prefix = isEnc
                ? (english ? L"Decrypting: " : L"正在解密: ")
                : (english ? L"Encrypting: " : L"正在加密: ");
            const int overallPercent = (int)(100.0 * (double)idx / (double)(fileCount ? fileCount : 1));
            postStatus(prefix + f +
                       (english ? L"  (overall " : L"  (整体进度 ") +
                       std::to_wstring(overallPercent) + L"%)");

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

            try {
                if (isEnc) {
                    FileEncryptor::decryptFileTo(f, out_path, usePwd);
                    // 加密文件本身是密文，不含明文残留，无需覆写；
                    // “禁用”= 保留不删，其余档位直接删除。
                    if (!keepSource) SecureDeleteFile(f, 0);
                }
                else {
                    FileEncryptor::encryptFileTo(f, out_path, usePwd, iterations);
                    // 源文件含明文，按设置覆写 N 次后删除；“禁用”= 保留不删。
                    if (!keepSource) SecureDeleteFile(f, deletePasses);
                }
                ++processed;
            }
            catch (const std::exception& e) {
                const std::wstring werr = narrow_to_wstring(e.what());
                failures.push_back(f + L"\n" + werr);
            }
        }

        ProgressFinish();

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

        g_workerRunning = false;
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

    if (!UiShowSettings(owner, d)) return;

    AppSettings& w = Settings();
    w.language     = (LangMode)d.language;
    w.theme        = (ThemeMode)d.theme;
    w.outDir       = (OutDirMode)d.outDir;
    w.fixedOutDir  = d.fixedDir;
    w.secureDelete = (SecureDeleteMode)d.secureDelete;
    w.completion   = (CompletionMode)d.completion;
    w.strength     = (StrengthMode)d.strength;
    SettingsSave();

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
