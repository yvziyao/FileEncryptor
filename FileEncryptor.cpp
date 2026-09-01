#define OPENSSL_API_COMPAT 0x10100000L
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <memory>
#include "C:\\Program Files\\OpenSSL-Win64\\include\\openssl\\evp.h"
#include "C:\\Program Files\\OpenSSL-Win64\\include\\openssl\\rand.h"
#include "C:\\Program Files\\OpenSSL-Win64\\include\\openssl\\err.h"
#include "C:\\Program Files\\OpenSSL-Win64\\include\\openssl\\aes.h"
#include "resource.h"
#include "winui_password.h"
#include "FileEncryptor_helpers.h"

#include <d2d1.h>
#include <dwrite.h>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#include <thread>
#include <atomic>

#pragma comment(lib, "Comdlg32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Comdlg32.lib")

// 强制使用 Windows 子系统（GUI），避免产生控制台
#pragma comment(linker, "/SUBSYSTEM:Windows")

// ---------- 全局变量 ----------
std::string g_password;  // 用于在密码对话框和主程序间传递密码
HFONT g_hGuiFont = NULL;
HWND g_hBtn1 = NULL;
HWND g_hBtn2 = NULL;
HWND g_hLangBtn = NULL; // 语言切换按钮
bool g_hoverBtn1 = false;
bool g_hoverBtn2 = false;
bool g_trackingMouse = false;
// Direct2D / DirectWrite
ID2D1Factory* g_pD2DFactory = NULL;
IDWriteFactory* g_pDWriteFactory = NULL;
IDWriteTextFormat* g_pTextFormat = NULL;
HWND g_progressBar = NULL;
std::atomic<bool> g_workerRunning(false);
// 主窗口句柄
HWND g_mainWnd = NULL;
HWND g_hCancel = NULL;

// 语言控制
bool g_langEnglish = false; // false = 中文, true = English
void UpdateLanguageUI();

// 自定义消息
#define WM_APP_SET_STATUS    (WM_APP + 1)
#define WM_APP_SHOW_PROGRESS (WM_APP + 2)
#define WM_APP_UPDATE_PROGRESS (WM_APP + 3)
#define WM_APP_SHOW_ERROR    (WM_APP + 4)

std::atomic<bool> g_cancelRequested(false);



// 前置声明（在 FileEncryptor 方法中使用）
static void ShowProgressBar(bool show);
static void UpdateProgressBar(double fraction);
static void CollectFilesRecursive(const std::wstring& path, std::vector<std::wstring>& out);
static void SecureDeleteFile(const std::wstring& wPath);

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
INT_PTR CALLBACK PasswordDlgProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool confirmMode = false;
    static HWND hShowPwd = NULL;
    static HWND hStrength = NULL;

    switch (msg) {
    case WM_INITDIALOG: {
        confirmMode = (lParam == 1);
        // 对话使用与主窗口相同的字体（如果已创建）
        if (g_hGuiFont) {
            SendMessageW(hDlg, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
        }
        HWND hEdit1 = GetDlgItem(hDlg, IDC_PWD_EDIT);
        if (g_hGuiFont) SendMessageW(hEdit1, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
        SendMessage(hEdit1, EM_SETPASSWORDCHAR, '*', 0);
        SetFocus(hEdit1);

        HWND hEdit2 = GetDlgItem(hDlg, IDC_PWD_EDIT2);
        if (g_hGuiFont) SendMessageW(hEdit2, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
        if (confirmMode) {
            SendMessage(hEdit2, EM_SETPASSWORDCHAR, '*', 0);
            ShowWindow(hEdit2, SW_SHOW);
            ShowWindow(GetDlgItem(hDlg, IDC_PWD_LABEL2), SW_SHOW);
            SetWindowTextW(hDlg, L"设置密码");
        }
        else {
            ShowWindow(hEdit2, SW_HIDE);
            ShowWindow(GetDlgItem(hDlg, IDC_PWD_LABEL2), SW_HIDE);
            SetWindowTextW(hDlg, L"输入密码");
        }
        hShowPwd = GetDlgItem(hDlg, IDC_SHOW_PWD);
        hStrength = GetDlgItem(hDlg, IDC_PWD_STRENGTH);
        if (g_hGuiFont) {
            if (hShowPwd) SendMessageW(hShowPwd, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
            if (hStrength) SendMessageW(hStrength, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
        }
        // 根据是否是需要确认（设置密码）来决定是否显示强度提示
        if (hStrength) {
            if (confirmMode) {
                ShowWindow(hStrength, SW_SHOW);
                SetWindowTextW(hStrength, L"");
            }
            else {
                ShowWindow(hStrength, SW_HIDE);
                SetWindowTextW(hStrength, L"");
            }
        }
        // 确保对话内所有文本使用 Unicode 设置，避免 resource 编译时编码问题导致乱码
        SetDlgItemTextW(hDlg, IDC_PWD_LABEL, L"密码:");
        SetDlgItemTextW(hDlg, IDC_PWD_LABEL2, L"确认:");
        SetDlgItemTextW(hDlg, IDOK, L"确定");
        SetDlgItemTextW(hDlg, IDCANCEL, L"取消");
        SetDlgItemTextW(hDlg, IDC_SHOW_PWD, L"显示密码");

        // 初始化 Direct2D / DirectWrite 用于更平滑的圆角绘制（若尚未创建）
        if (!g_pD2DFactory) {
            if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory))) {
                if (!g_pDWriteFactory) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_pDWriteFactory));
                if (g_pDWriteFactory && !g_pTextFormat) {
                    g_pDWriteFactory->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"zh-CN", &g_pTextFormat);
                    if (g_pTextFormat) {
                        g_pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                        g_pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    }
                }
            }
        }

        // 美化：使按钮 owner-draw 并移除编辑框边框，由父窗口绘制圆角背景
        HWND hBtnOk = GetDlgItem(hDlg, IDOK);
        HWND hBtnCancel = GetDlgItem(hDlg, IDCANCEL);
        if (hBtnOk) {
            LONG st = GetWindowLongW(hBtnOk, GWL_STYLE);
            SetWindowLongW(hBtnOk, GWL_STYLE, st | BS_OWNERDRAW);
        }
        if (hBtnCancel) {
            LONG st = GetWindowLongW(hBtnCancel, GWL_STYLE);
            SetWindowLongW(hBtnCancel, GWL_STYLE, st | BS_OWNERDRAW);
        }
        // 移除编辑框边框以便绘制自定义圆角背景
        if (hEdit1) {
            LONG st = GetWindowLongW(hEdit1, GWL_STYLE);
            SetWindowLongW(hEdit1, GWL_STYLE, st & ~WS_BORDER);
        }
        if (hEdit2) {
            LONG st = GetWindowLongW(hEdit2, GWL_STYLE);
            SetWindowLongW(hEdit2, GWL_STYLE, st & ~WS_BORDER);
        }

        InvalidateRect(hDlg, NULL, TRUE);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wParam);
        int code = HIWORD(wParam);

        if (id == IDC_SHOW_PWD) {
            // 切换显示密码
            BOOL checked = (SendMessageW(hShowPwd, BM_GETCHECK, 0, 0) != 0);
            HWND hEdit1 = GetDlgItem(hDlg, IDC_PWD_EDIT);
            HWND hEdit2 = GetDlgItem(hDlg, IDC_PWD_EDIT2);
            if (hEdit1) SendMessageW(hEdit1, EM_SETPASSWORDCHAR, checked ? 0 : '*', 0);
            if (hEdit2) SendMessageW(hEdit2, EM_SETPASSWORDCHAR, checked ? 0 : '*', 0);
            // 强制重绘编辑框（通过重设文本触发）
            if (hEdit1) {
                wchar_t buf[256] = { 0 };
                GetWindowTextW(hEdit1, buf, 256);
                SetWindowTextW(hEdit1, buf);
            }
            if (hEdit2) {
                wchar_t buf[256] = { 0 };
                GetWindowTextW(hEdit2, buf, 256);
                SetWindowTextW(hEdit2, buf);
            }
            // 重绘对话以刷新圆角背景
            InvalidateRect(hDlg, NULL, TRUE);
            return TRUE;
        }

        // 监听编辑框内容变化以更新强度提示（编辑控件发送 EN_CHANGE 到父窗口）
        if (code == EN_CHANGE && (id == IDC_PWD_EDIT || id == IDC_PWD_EDIT2)) {
            HWND hEdit = GetDlgItem(hDlg, IDC_PWD_EDIT);
            wchar_t buf[256] = { 0 };
            if (hEdit) {
                GetWindowTextW(hEdit, buf, 256);
                std::wstring ws(buf);
                // 简单强度评估：长度与字符集
                int score = 0;
                if (ws.length() >= 8) score += 2;
                if (ws.length() >= 12) score += 1;
                bool hasLower = false, hasUpper = false, hasDigit = false, hasSymbol = false;
                for (wchar_t ch : ws) {
                    if (iswlower(ch)) hasLower = true;
                    else if (iswupper(ch)) hasUpper = true;
                    else if (iswdigit(ch)) hasDigit = true;
                    else hasSymbol = true;
                }
                if (hasLower && hasUpper) score += 1;
                if (hasDigit) score += 1;
                if (hasSymbol) score += 1;
                const wchar_t* label = L"很弱";
                if (score <= 1) label = L"很弱";
                else if (score <= 3) label = L"一般";
                else if (score <= 5) label = L"良好";
                else label = L"很强";
                if (hStrength) SetWindowTextW(hStrength, label);
            }
            return TRUE;
        }

        if (id == IDOK) {
            wchar_t buffer1[256] = { 0 };
            GetDlgItemTextW(hDlg, IDC_PWD_EDIT, buffer1, 256);
            std::wstring ws1(buffer1);
            std::string pwd = wstring_to_utf8(ws1);

            if (pwd.empty()) {
                MessageBoxW(hDlg, tr(L"密码不能为空！", L"Password cannot be empty! ").c_str(), tr(L"错误", L"Error").c_str(), MB_OK | MB_ICONERROR);
                return FALSE;
            }

            if (confirmMode) {
                wchar_t buffer2[256] = { 0 };
                GetDlgItemTextW(hDlg, IDC_PWD_EDIT2, buffer2, 256);
                std::wstring ws2(buffer2);
                std::string pwd2 = wstring_to_utf8(ws2);
                if (pwd != pwd2) {
                    MessageBoxW(hDlg, tr(L"两次输入的密码不一致！", L"Passwords do not match!").c_str(), tr(L"错误", L"Error").c_str(), MB_OK | MB_ICONERROR);
                    return FALSE;
                }
            }

            g_password = pwd;
            EndDialog(hDlg, IDOK);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hDlg, IDCANCEL);
            return TRUE;
        }
        break;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lParam;
        if (pdis && pdis->CtlType == ODT_BUTTON) {
            RECT rc = pdis->rcItem;
            HDC dc = pdis->hDC;
            BOOL isPressed = (pdis->itemState & ODS_SELECTED) != 0;
            D2D1_RENDER_TARGET_PROPERTIES dcrtp = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                0.0f, 0.0f,
                D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
            ID2D1DCRenderTarget* pDCRT = nullptr;
            if (g_pD2DFactory && SUCCEEDED(g_pD2DFactory->CreateDCRenderTarget(&dcrtp, &pDCRT)) && pDCRT) {
                pDCRT->BindDC(pdis->hDC, &rc);
                pDCRT->BeginDraw();
                D2D1_COLOR_F fill = isPressed ? D2D1::ColorF(0/255.0f, 120/255.0f, 215/255.0f) : D2D1::ColorF(240/255.0f,240/255.0f,240/255.0f);
                D2D1_COLOR_F border = D2D1::ColorF(200/255.0f,200/255.0f,200/255.0f);
                D2D1_COLOR_F text = isPressed ? D2D1::ColorF(1,1,1) : D2D1::ColorF(0,0,0);
                ID2D1SolidColorBrush* pBrush = nullptr;
                ID2D1SolidColorBrush* pBorder = nullptr;
                ID2D1SolidColorBrush* pTextBrush = nullptr;
                pDCRT->CreateSolidColorBrush(fill, &pBrush);
                pDCRT->CreateSolidColorBrush(border, &pBorder);
                pDCRT->CreateSolidColorBrush(text, &pTextBrush);
                D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF((FLOAT)rc.left, (FLOAT)rc.top, (FLOAT)rc.right, (FLOAT)rc.bottom), 8.0f, 8.0f);
                pDCRT->FillRoundedRectangle(rr, pBrush);
                pDCRT->DrawRoundedRectangle(rr, pBorder, 1.0f);
                // Draw text using DWrite if available
                if (g_pDWriteFactory && g_pTextFormat) {
                    wchar_t buf[128] = {0};
                    GetWindowTextW(pdis->hwndItem, buf, _countof(buf));
                    D2D1_RECT_F layoutRect = D2D1::RectF((FLOAT)rc.left, (FLOAT)rc.top, (FLOAT)rc.right, (FLOAT)rc.bottom);
                    pDCRT->DrawTextW(buf, (UINT32)wcslen(buf), g_pTextFormat, layoutRect, pTextBrush);
                }
                pDCRT->EndDraw();
                if (pBrush) pBrush->Release();
                if (pBorder) pBorder->Release();
                if (pTextBrush) pTextBrush->Release();
                pDCRT->Release();
                return TRUE;
            }
        }
        break;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hDlg, &ps);
        // Draw rounded backgrounds behind edit controls using Direct2D
        HWND hEdit1 = GetDlgItem(hDlg, IDC_PWD_EDIT);
        HWND hEdit2 = GetDlgItem(hDlg, IDC_PWD_EDIT2);
        RECT r1 = {0}, r2 = {0};
        D2D1_RENDER_TARGET_PROPERTIES dcrtp = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        ID2D1DCRenderTarget* pDCRT = nullptr;
        if (g_pD2DFactory && SUCCEEDED(g_pD2DFactory->CreateDCRenderTarget(&dcrtp, &pDCRT)) && pDCRT) {
            // bind to dialog DC across full client rect
            RECT client; GetClientRect(hDlg, &client);
            pDCRT->BindDC(dc, &client);
            pDCRT->BeginDraw();
            ID2D1SolidColorBrush* pBrush = nullptr;
            ID2D1SolidColorBrush* pBorder = nullptr;
            pDCRT->CreateSolidColorBrush(D2D1::ColorF(250/255.0f,250/255.0f,250/255.0f), &pBrush);
            pDCRT->CreateSolidColorBrush(D2D1::ColorF(200/255.0f,200/255.0f,200/255.0f), &pBorder);

            if (hEdit1 && IsWindowVisible(hEdit1)) {
                GetWindowRect(hEdit1, &r1);
                MapWindowPoints(NULL, hDlg, (LPPOINT)&r1, 2);
                D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF((FLOAT)r1.left-2, (FLOAT)r1.top-2, (FLOAT)r1.right+2, (FLOAT)r1.bottom+2), 8.0f, 8.0f);
                pDCRT->FillRoundedRectangle(rr, pBrush);
                pDCRT->DrawRoundedRectangle(rr, pBorder, 1.0f);
            }
            if (hEdit2 && IsWindowVisible(hEdit2)) {
                GetWindowRect(hEdit2, &r2);
                MapWindowPoints(NULL, hDlg, (LPPOINT)&r2, 2);
                D2D1_ROUNDED_RECT rr2 = D2D1::RoundedRect(D2D1::RectF((FLOAT)r2.left-2, (FLOAT)r2.top-2, (FLOAT)r2.right+2, (FLOAT)r2.bottom+2), 8.0f, 8.0f);
                pDCRT->FillRoundedRectangle(rr2, pBrush);
                pDCRT->DrawRoundedRectangle(rr2, pBorder, 1.0f);
            }

            pDCRT->EndDraw();
            if (pBrush) pBrush->Release();
            if (pBorder) pBorder->Release();
            pDCRT->Release();
        }
        EndPaint(hDlg, &ps);
        return TRUE;
    }
    }
    return FALSE;
}

bool ShowPasswordDialog(HWND hWnd, std::string& password, bool confirm) {
    g_password.clear();
    // If WinUI wrapper available, prefer it (may be same as Win32 fallback)
#ifdef USE_WINUI_PASSWORD
    if (ShowPasswordDialogWinUI(hWnd, password, confirm)) return true;
    return false;
#else
    INT_PTR result = DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_PASSWORD_DIALOG), hWnd, PasswordDlgProc, confirm ? 1 : 0);
    if (result == IDOK) {
        password = g_password;
        return true;
    }
    return false;
#endif
}

// ---------- 加密核心类 ----------
class FileEncryptor {
private:
    struct EVP_CIPHER_CTX_deleter { void operator()(EVP_CIPHER_CTX* p) { EVP_CIPHER_CTX_free(p); } };

    static std::vector<uint8_t> pkcs7_pad(const std::vector<uint8_t>& data, size_t block_size = AES_BLOCK_SIZE) {
        size_t pad_len = block_size - (data.size() % block_size);
        std::vector<uint8_t> padded = data;
        padded.resize(data.size() + pad_len, static_cast<uint8_t>(pad_len));
        return padded;
    }

    static std::vector<uint8_t> pkcs7_unpad(const std::vector<uint8_t>& data) {
        if (data.empty()) return data;
        uint8_t pad_len = data.back();
        if (pad_len > AES_BLOCK_SIZE || pad_len == 0) throw std::runtime_error("无效的填充");
        return std::vector<uint8_t>(data.begin(), data.end() - pad_len);
    }

    static std::vector<uint8_t> derive_key(const std::string& password, const std::vector<uint8_t>& salt) {
        std::vector<uint8_t> key(32);
        PKCS5_PBKDF2_HMAC(password.c_str(), (int)password.length(),
            salt.data(), (int)salt.size(),
            100000,
            EVP_sha256(),
            (int)key.size(),
            key.data());
        return key;
    }



public:
    static void encryptFile(const std::wstring& input_path, const std::string& password) {
        // 仍保留原有行为：调用加密到输出并删除原文件
        std::wstring out_path = input_path + L".enc";
        encryptFileTo(input_path, out_path, password);
        SecureDeleteFile(input_path);
    }
    // 将分块流式解密实现为可复用的 decryptFileTo（不删除输入文件）
    static void decryptFileTo(const std::wstring& input_path, const std::wstring& out_path, const std::string& password) {
        const size_t CHUNK = 4 * 1024 * 1024; // 4MB
        HANDLE hFile = CreateFileW(input_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开加密文件");

        LARGE_INTEGER fileSizeLI;
        if (!GetFileSizeEx(hFile, &fileSizeLI)) { CloseHandle(hFile); throw std::runtime_error("获取文件大小失败"); }
        uint64_t fileSize = (uint64_t)fileSizeLI.QuadPart;

        DWORD bytesRead = 0;
        std::vector<uint8_t> salt(16);
        if (!ReadFile(hFile, salt.data(), (DWORD)salt.size(), &bytesRead, NULL) || bytesRead != salt.size()) { CloseHandle(hFile); throw std::runtime_error("读取盐失败"); }

        std::vector<uint8_t> iv(AES_BLOCK_SIZE);
        if (!ReadFile(hFile, iv.data(), (DWORD)iv.size(), &bytesRead, NULL) || bytesRead != iv.size()) { CloseHandle(hFile); throw std::runtime_error("读取IV失败"); }

        std::vector<uint8_t> aes_key = derive_key(password, salt);

        std::unique_ptr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_deleter> ctx(EVP_CIPHER_CTX_new());
        if (!ctx) { CloseHandle(hFile); throw std::runtime_error("创建EVP上下文失败"); }
        if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, aes_key.data(), iv.data()) != 1) { CloseHandle(hFile); throw std::runtime_error("AES初始化失败"); }

        HANDLE hOutFile = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOutFile == INVALID_HANDLE_VALUE) { CloseHandle(hFile); throw std::runtime_error("无法创建输出文件"); }

        std::vector<uint8_t> inbuf(CHUNK);
        std::vector<uint8_t> outbuf(CHUNK + AES_BLOCK_SIZE);
        uint64_t processed = salt.size() + iv.size();
        bool showProgress = fileSize >= (uint64_t)500 * 1024 * 1024;
        if (showProgress) ShowProgressBar(true);

        while (true) {
            if (!ReadFile(hFile, inbuf.data(), (DWORD)inbuf.size(), &bytesRead, NULL)) { CloseHandle(hFile); CloseHandle(hOutFile); throw std::runtime_error("读取加密文件失败"); }
            if (bytesRead == 0) break;
            int outlen = 0;
            if (EVP_DecryptUpdate(ctx.get(), outbuf.data(), &outlen, inbuf.data(), (int)bytesRead) != 1) { CloseHandle(hFile); CloseHandle(hOutFile); throw std::runtime_error("AES解密失败"); }
            if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }
            processed += bytesRead;
            if (showProgress) UpdateProgressBar((double)processed / (double)fileSize);
        }
        int outlen = 0;
        if (EVP_DecryptFinal_ex(ctx.get(), outbuf.data(), &outlen) != 1) { CloseHandle(hFile); CloseHandle(hOutFile); throw std::runtime_error("AES完成解密失败（密码错误或文件被篡改）"); }
        if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }

        if (showProgress) UpdateProgressBar(1.0);
        if (showProgress) ShowProgressBar(false);

        CloseHandle(hFile);
        CloseHandle(hOutFile);
    }

    static void encryptFileTo(const std::wstring& input_path, const std::wstring& out_path, const std::string& password) {
        const size_t CHUNK = 4 * 1024 * 1024; // 4MB
        std::vector<uint8_t> salt(16);
        std::vector<uint8_t> iv(AES_BLOCK_SIZE);
        if (RAND_bytes(salt.data(), (int)salt.size()) != 1) throw std::runtime_error("生成盐失败");
        if (RAND_bytes(iv.data(), (int)iv.size()) != 1) throw std::runtime_error("生成IV失败");

        std::vector<uint8_t> aes_key = derive_key(password, salt);

        HANDLE hFile = CreateFileW(input_path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) throw std::runtime_error("无法打开输入文件");

        LARGE_INTEGER fileSizeLI;
        if (!GetFileSizeEx(hFile, &fileSizeLI)) { CloseHandle(hFile); throw std::runtime_error("获取文件大小失败"); }
        uint64_t fileSize = (uint64_t)fileSizeLI.QuadPart;

        HANDLE hOutFile = CreateFileW(out_path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hOutFile == INVALID_HANDLE_VALUE) { CloseHandle(hFile); throw std::runtime_error("无法创建输出文件"); }

        // 写入 salt 和 iv
        DWORD bytesWritten = 0;
        WriteFile(hOutFile, salt.data(), (DWORD)salt.size(), &bytesWritten, NULL);
        WriteFile(hOutFile, iv.data(), (DWORD)iv.size(), &bytesWritten, NULL);

        std::unique_ptr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_deleter> ctx(EVP_CIPHER_CTX_new());
        if (!ctx) { CloseHandle(hFile); CloseHandle(hOutFile); throw std::runtime_error("创建EVP上下文失败"); }
        if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_cbc(), nullptr, aes_key.data(), iv.data()) != 1) { CloseHandle(hFile); CloseHandle(hOutFile); throw std::runtime_error("AES初始化失败"); }

        std::vector<uint8_t> inbuf(CHUNK);
        std::vector<uint8_t> outbuf(CHUNK + AES_BLOCK_SIZE);
        DWORD bytesRead = 0;
        uint64_t processed = 0;
        bool showProgress = fileSize >= (uint64_t)500 * 1024 * 1024;
        if (showProgress) ShowProgressBar(true);

        while (ReadFile(hFile, inbuf.data(), (DWORD)inbuf.size(), &bytesRead, NULL) && bytesRead > 0) {
            int outlen = 0;
            if (EVP_EncryptUpdate(ctx.get(), outbuf.data(), &outlen, inbuf.data(), (int)bytesRead) != 1) {
                CloseHandle(hFile); CloseHandle(hOutFile); throw std::runtime_error("AES加密失败");
            }
            if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }
            processed += bytesRead;
            if (showProgress) UpdateProgressBar((double)processed / (double)fileSize);
        }

        int outlen = 0;
        if (EVP_EncryptFinal_ex(ctx.get(), outbuf.data(), &outlen) != 1) { CloseHandle(hFile); CloseHandle(hOutFile); throw std::runtime_error("AES完成加密失败"); }
        if (outlen > 0) { DWORD bw = 0; WriteFile(hOutFile, outbuf.data(), outlen, &bw, NULL); }

        if (showProgress) UpdateProgressBar(1.0);
        if (showProgress) ShowProgressBar(false);

        CloseHandle(hFile);
        CloseHandle(hOutFile);
    }

    static void decryptFile(const std::wstring& input_path, const std::string& password) {
        if (input_path.size() < 4 || input_path.substr(input_path.size() - 4) != L".enc") {
            throw std::runtime_error("文件不是加密文件（需要 .enc 后缀）");
        }
        // 仍保留原有行为：调用解密到输出并删除加密文件
        std::wstring out_path = input_path.substr(0, input_path.size() - 4);
        decryptFileTo(input_path, out_path, password);
        SecureDeleteFile(input_path);
    }
};

// Secure delete helper (free function, visible to other code)
static void SecureDeleteFile(const std::wstring& wPath) {
    try {
        HANDLE hFile = CreateFileW(wPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER fileSizeLI;
            if (GetFileSizeEx(hFile, &fileSizeLI) && fileSizeLI.QuadPart > 0) {
                uint64_t fileSize = (uint64_t)fileSizeLI.QuadPart;
                const size_t CHUNK = 64 * 1024; // 64KB
                std::vector<uint8_t> buf(CHUNK);
                // Overwrite passes: random, ~random, random
                for (int pass = 0; pass < 3; ++pass) {
                    // Seek to start
                    LARGE_INTEGER zero = {0};
                    SetFilePointerEx(hFile, zero, NULL, FILE_BEGIN);
                    uint64_t remaining = fileSize;
                    while (remaining > 0) {
                        size_t toWrite = (size_t)std::min<uint64_t>(CHUNK, remaining);
                        if (pass == 1) {
                            // complement of previous random: generate new random then invert
                            RAND_bytes(buf.data(), (int)toWrite);
                            for (size_t i = 0; i < toWrite; ++i) buf[i] = ~buf[i];
                        } else {
                            RAND_bytes(buf.data(), (int)toWrite);
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
        // Finally delete file
        DeleteFileW(wPath.c_str());
    }
    catch (...) {
        // best-effort cleanup
        DeleteFileW(wPath.c_str());
    }
}

// ---------- GUI 部分 ----------
#include "FileEncryptor_helpers.h"

HWND g_statusBar;
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

std::wstring OpenFileDialog() {
    OPENFILENAMEW ofn = { 0 };
    wchar_t szFile[260] = { 0 };
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
    ofn.lpstrFilter = L"All Files\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) return std::wstring(szFile);
    return L"";
}

void SetStatusText(const std::wstring& text) {
    if (g_statusBar) {
        SendMessageW(g_statusBar, SB_SETTEXT, 0, (LPARAM)text.c_str());
    }
}

static void ShowProgressBar(bool show) {
    // Post to main thread to update UI
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

static void UpdateProgressBar(double fraction) {
    int pos = (int)(fraction * 100.0);
    if (pos < 0) pos = 0; if (pos > 100) pos = 100;
    if (g_mainWnd) {
        PostMessageW(g_mainWnd, WM_APP_UPDATE_PROGRESS, (WPARAM)pos, 0);
    }
    else {
        if (!g_progressBar) return;
        SendMessageW(g_progressBar, PBM_SETPOS, pos, 0);
    }
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

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_mainWnd = hWnd;
        g_statusBar = CreateWindowW(STATUSCLASSNAMEW, L"就绪", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hWnd, NULL, GetModuleHandle(NULL), NULL);
        SetStatusText(g_langEnglish ? L"Ready" : L"就绪");

        // 创建一个适合中文的 UI 字体（微软雅黑），字号适中
        g_hGuiFont = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            VARIABLE_PITCH | FF_SWISS, L"Microsoft YaHei");

        // 使用 owner-draw 按钮以便绘制圆角，符合 Win11 风格
        HWND hBtn1 = CreateWindowW(L"BUTTON", g_langEnglish ? L"Encrypt" : L"加密文件", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 30, 30, 140, 40, hWnd, (HMENU)1, GetModuleHandle(NULL), NULL);
        HWND hBtn2 = CreateWindowW(L"BUTTON", g_langEnglish ? L"Decrypt" : L"解密文件", WS_VISIBLE | WS_CHILD | BS_OWNERDRAW, 190, 30, 140, 40, hWnd, (HMENU)2, GetModuleHandle(NULL), NULL);
        // Language toggle button (top-right)
        HWND hLang = CreateWindowW(L"BUTTON", g_langEnglish ? L"EN" : L"中", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 360, 8, 28, 24, hWnd, (HMENU)5, GetModuleHandle(NULL), NULL);
        g_hBtn1 = hBtn1;
        g_hBtn2 = hBtn2;
        g_hLangBtn = hLang;

        if (g_hGuiFont) {
            SendMessageW(hBtn1, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
            SendMessageW(hBtn2, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
            SendMessageW(g_statusBar, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
            if (g_hLangBtn) SendMessageW(g_hLangBtn, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
        }

        // 创建进度条（默认隐藏）
        g_progressBar = CreateWindowExW(0, PROGRESS_CLASS, NULL, WS_CHILD | PBS_SMOOTH, 20, 110, 340, 16, hWnd, (HMENU)3, GetModuleHandle(NULL), NULL);
        ShowWindow(g_progressBar, SW_HIDE);

        // 接受文件拖拽
        DragAcceptFiles(hWnd, TRUE);

        // 取消按钮（初始隐藏，位于进度条下方，仅在显示进度时出现）
        g_hCancel = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | BS_PUSHBUTTON, 150, 132, 60, 22, hWnd, (HMENU)4, GetModuleHandle(NULL), NULL);
        if (g_hGuiFont) SendMessageW(g_hCancel, WM_SETFONT, (WPARAM)g_hGuiFont, TRUE);
        ShowWindow(g_hCancel, SW_HIDE);

        // 初始化 Direct2D 与 DirectWrite
        if (SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_pD2DFactory))) {
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(&g_pDWriteFactory));
            if (g_pDWriteFactory) {
                // 文本格式用于按钮文本绘制
                g_pDWriteFactory->CreateTextFormat(L"Microsoft YaHei", NULL, DWRITE_FONT_WEIGHT_NORMAL,
                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14.0f, L"zh-CN", &g_pTextFormat);
                if (g_pTextFormat) {
                    g_pTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    g_pTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                }
            }
        }

        break;
    }

    case WM_DRAWITEM: {
        LPDRAWITEMSTRUCT pdis = (LPDRAWITEMSTRUCT)lParam;
        if (pdis && pdis->CtlType == ODT_BUTTON) {
            RECT rc = pdis->rcItem;
            HDC dc = pdis->hDC;

            BOOL isPressed = (pdis->itemState & ODS_SELECTED) != 0;
            BOOL isDisabled = (pdis->itemState & ODS_DISABLED) != 0;

            int ctlId = GetDlgCtrlID(pdis->hwndItem);
            bool isHover = false;
            if (ctlId == 1) isHover = g_hoverBtn1;
            else if (ctlId == 2) isHover = g_hoverBtn2;

            // 颜色方案（类似 Win11 按钮）
            COLORREF clrFillNormal = RGB(240, 240, 240);
            COLORREF clrFillHover = RGB(230, 240, 250);
            COLORREF clrFillPressed = RGB(0, 120, 215);
            COLORREF clrBorder = RGB(200, 200, 200);
            COLORREF clrTextNormal = RGB(0, 0, 0);
            COLORREF clrTextPressed = RGB(255, 255, 255);

            // 简单处理：按下时使用主色，悬停时使用 hover 色，非按下使用浅灰背景
            COLORREF fill = isPressed ? clrFillPressed : (isHover ? clrFillHover : clrFillNormal);
            COLORREF text = isPressed ? clrTextPressed : clrTextNormal;
            if (isDisabled) {
                fill = RGB(240, 240, 240);
                text = RGB(160, 160, 160);
            }

            // 使用 Direct2D 绘制圆角按钮（通过 DCRenderTarget 与 HDC 绑定）
            if (g_pD2DFactory && g_pDWriteFactory && g_pTextFormat) {
                ID2D1DCRenderTarget* pDCRT = nullptr;
                D2D1_RENDER_TARGET_PROPERTIES dcrtp = D2D1::RenderTargetProperties(
                    D2D1_RENDER_TARGET_TYPE_DEFAULT,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                    0.0f, 0.0f,
                    D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
                if (SUCCEEDED(g_pD2DFactory->CreateDCRenderTarget(&dcrtp, &pDCRT)) && pDCRT) {
                    // 使用 innerRect 作为绘制的圆角区域，但绑定时使用控件完整 rc 避免裁剪
                    RECT inner = rc;
                    InflateRect(&inner, -2, -4);
                    // 绑定 HDC（使用控件完整区域，防止下方被裁剪）
                    pDCRT->BindDC(pdis->hDC, &rc);
                    pDCRT->BeginDraw();

                    // 颜色转换
                    D2D1_COLOR_F d2dFill = D2D1::ColorF(((float)GetRValue(fill)) / 255.0f, ((float)GetGValue(fill)) / 255.0f, ((float)GetBValue(fill)) / 255.0f, 1.0f);
                    D2D1_COLOR_F d2dBorder = D2D1::ColorF(((float)GetRValue(clrBorder)) / 255.0f, ((float)GetGValue(clrBorder)) / 255.0f, ((float)GetBValue(clrBorder)) / 255.0f, 1.0f);
                    D2D1_COLOR_F d2dText = D2D1::ColorF(((float)GetRValue(text)) / 255.0f, ((float)GetGValue(text)) / 255.0f, ((float)GetBValue(text)) / 255.0f, 1.0f);

                    ID2D1SolidColorBrush* pBrush = nullptr;
                    ID2D1SolidColorBrush* pBorderBrush = nullptr;
                    ID2D1SolidColorBrush* pTextBrush = nullptr;
                    pDCRT->CreateSolidColorBrush(d2dFill, &pBrush);
                    pDCRT->CreateSolidColorBrush(d2dBorder, &pBorderBrush);
                    pDCRT->CreateSolidColorBrush(d2dText, &pTextBrush);

                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF((FLOAT)inner.left, (FLOAT)inner.top, (FLOAT)inner.right, (FLOAT)inner.bottom), 10.0f, 10.0f);
                    pDCRT->FillRoundedRectangle(rr, pBrush);
                    pDCRT->DrawRoundedRectangle(rr, pBorderBrush, 1.0f);

                    // 绘制文本（使用 DirectWrite 文本格式）
                    wchar_t buf[128] = { 0 };
                    GetWindowTextW(pdis->hwndItem, buf, _countof(buf));
                    D2D1_RECT_F layoutRect = D2D1::RectF((FLOAT)rc.left, (FLOAT)rc.top, (FLOAT)rc.right, (FLOAT)rc.bottom);
                    pDCRT->DrawTextW(buf, (UINT32)wcslen(buf), g_pTextFormat, layoutRect, pTextBrush, D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);

                    pDCRT->EndDraw();

                    if (pBrush) pBrush->Release();
                    if (pBorderBrush) pBorderBrush->Release();
                    if (pTextBrush) pTextBrush->Release();
                    pDCRT->Release();
                }
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

        bool prevHover1 = g_hoverBtn1;
        bool prevHover2 = g_hoverBtn2;

        if (g_hBtn1) {
            RECT r1; GetWindowRect(g_hBtn1, &r1);
            g_hoverBtn1 = PtInRect(&r1, ptScreen) != 0;
        }
        if (g_hBtn2) {
            RECT r2; GetWindowRect(g_hBtn2, &r2);
            g_hoverBtn2 = PtInRect(&r2, ptScreen) != 0;
        }

        if (g_hoverBtn1 != prevHover1 && g_hBtn1) InvalidateRect(g_hBtn1, NULL, TRUE);
        if (g_hoverBtn2 != prevHover2 && g_hBtn2) InvalidateRect(g_hBtn2, NULL, TRUE);

        if (!g_trackingMouse) {
            // 开始跟踪鼠标离开事件
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            g_trackingMouse = true;
        }

        break;
    }

    case WM_MOUSELEAVE: {
        // 清除 hover 状态并重绘按钮
        g_trackingMouse = false;
        bool needRedraw = false;
        if (g_hoverBtn1) { g_hoverBtn1 = false; needRedraw = true; }
        if (g_hoverBtn2) { g_hoverBtn2 = false; needRedraw = true; }
        if (needRedraw) {
            if (g_hBtn1) InvalidateRect(g_hBtn1, NULL, TRUE);
            if (g_hBtn2) InvalidateRect(g_hBtn2, NULL, TRUE);
        }
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

        // 检测文件类型：是否包含 .enc（加密文件）或普通文件
        bool hasEnc = false, hasPlain = false;
        for (const auto& p : files) {
            if (p.size() >= 4 && p.substr(p.size() - 4) == L".enc") hasEnc = true;
            else hasPlain = true;
        }
        // 不能同时包含加密文件和普通文件（要求用户分开操作）
        if (hasEnc && hasPlain) {
            MessageBoxW(hWnd, tr(L"包含加密文件和普通文件，请分别拖拽进行加密或解密。", L"Contains encrypted and plain files; drag separately for encrypt/decrypt.").c_str(), tr(L"错误", L"Error").c_str(), MB_OK | MB_ICONERROR);
            break;
        }

        // 密码收集策略：
        // - 如果都是 .enc（解密），警告用户每个文件可能需要不同密码，询问是否为每个文件分别输入密码
        // - 如果都是普通文件（加密），一次输入并要求确认（确认输入时会显示强度）
        std::vector<std::string> perFilePasswords; // 若为空且 pwd_single 非空，则使用单一密码
        std::string pwd_single;

        if (hasEnc) {
            if (files.size() == 1) {
                // 单文件解密：直接一次性输入密码（不确认）
                if (!ShowPasswordDialog(hWnd, pwd_single, false)) { SetStatusText(L"已取消"); break; }
            }
            else {
                // 批量解密提示：每个文件可能有不同密码
                int choice = MessageBoxW(hWnd,
                    tr(L"检测到全部为加密文件(.enc)。\n注意：每个文件可能使用不同密码。\n\n选择“是”在每个文件上分别输入密码；选择“否”使用相同密码尝试解密所有文件。",
                       L"Detected .enc files; each may require different password.\n\nChoose Yes to enter passwords per file; No to try same password for all.").c_str(),
                    tr(L"批量解密", L"Batch decrypt").c_str(), MB_YESNO | MB_ICONQUESTION);
                if (choice == IDYES) {
                    // 为每个文件单独请求密码（无确认）
                    bool cancelled = false;
                    for (size_t i = 0; i < files.size(); ++i) {
                        std::string onepwd;
                        if (!ShowPasswordDialog(hWnd, onepwd, false)) { cancelled = true; break; }
                        perFilePasswords.push_back(onepwd);
                    }
                    if (cancelled) { SetStatusText(L"已取消"); break; }
                }
                else {
                    // 使用相同密码：只输入一次（不确认）
                    if (!ShowPasswordDialog(hWnd, pwd_single, false)) { SetStatusText(L"已取消"); break; }
                }
            }
        }
        else {
            if (files.size() == 1) {
                // 单文件加密：直接要求确认一次
                if (!ShowPasswordDialog(hWnd, pwd_single, true)) { SetStatusText(L"已取消"); break; }
            }
            else {
                // 全部为普通文件（加密）——询问用户是否为所有文件使用相同密码或为每个文件单独设置
                int choiceEnc = MessageBoxW(hWnd,
                    tr(L"检测到全部为普通文件。\n\n是否为所有文件使用相同密码？\n选择“是”将对所有文件使用相同密码（需要确认）；\n选择“否”将为每个文件单独设置密码。",
                       L"Detected plain files.\n\nUse same password for all files?\nYes will require confirmation; No will prompt per file.").c_str(),
                    tr(L"批量加密", L"Batch encrypt").c_str(), MB_YESNO | MB_ICONQUESTION);
                if (choiceEnc == IDYES) {
                    // 使用相同密码（要求确认）
                    if (!ShowPasswordDialog(hWnd, pwd_single, true)) { SetStatusText(L"已取消"); break; }
                }
                else {
                    // 为每个文件单独设置密码（每次要求确认）
                    bool cancelled = false;
                    for (size_t i = 0; i < files.size(); ++i) {
                        std::string onepwd;
                        if (!ShowPasswordDialog(hWnd, onepwd, true)) { cancelled = true; break; }
                        perFilePasswords.push_back(onepwd);
                    }
                    if (cancelled) { SetStatusText(L"已取消"); break; }
                }
            }
        }

        if (g_workerRunning.exchange(true)) {
            MessageBoxW(hWnd, tr(L"已有任务正在运行，请稍候。", L"A task is already running; please wait.").c_str(), tr(L"提示", L"Notice").c_str(), MB_OK | MB_ICONINFORMATION);
            break;
        }

        // 在后台线程中处理队列，避免阻塞 UI。使用 PostMessage 将 UI 更新委托给主线程。
        std::vector<std::wstring> files_copy = files;
        std::vector<std::string> per_pw_copy = perFilePasswords;
        std::string pwd_single_copy = pwd_single;
        std::thread worker([files_copy, per_pw_copy, pwd_single_copy]() mutable {
            struct ProcessedItem { std::wstring out; bool isEncryption; };
            std::vector<ProcessedItem> processedList;
            for (size_t idx = 0; idx < files_copy.size(); ++idx) {
                const auto& f = files_copy[idx];
                try {
                    bool isEnc = (f.size() >= 4 && f.substr(f.size() - 4) == L".enc");
                    std::wstring status = isEnc ? (std::wstring(L"正在解密: ") + f) : (std::wstring(L"正在加密: ") + f);
                    // overall progress percent
                    double overallFraction = ((double)idx) / (double)files_copy.size();
                    if (g_mainWnd) {
                        // build status with overall percent placeholder (per-file progress will update bar)
                        int overallPercent = (int)(overallFraction * 100.0);
                        std::wstring statusWithPercent = status + L" (整体进度: " + std::to_wstring(overallPercent) + L"%)";
                        size_t len = statusWithPercent.size();
                        wchar_t* buf = new wchar_t[len + 1];
                        wcscpy_s(buf, len + 1, statusWithPercent.c_str());
                        PostMessageW(g_mainWnd, WM_APP_SET_STATUS, 0, (LPARAM)buf);
                    }
                    else {
                        SetStatusText(status);
                    }

                    // 选择用于此文件的密码：如果提供了 per-file 密码列表，按索引使用，否则使用单一密码
                    std::string usePwd;
                    if (!per_pw_copy.empty()) {
                        if (idx < per_pw_copy.size()) usePwd = per_pw_copy[idx];
                        else usePwd = std::string();
                    }
                    else {
                        usePwd = pwd_single_copy;
                    }

                    if (isEnc) {
                        // decrypt: create output, then move original to backup after success
                        std::wstring out_path = f.substr(0, f.size() - 4);
                        FileEncryptor::decryptFileTo(f, out_path, usePwd);
                        // move original .enc to backup
                        // Securely remove the original encrypted file immediately
                        SecureDeleteFile(f);
                        processedList.push_back({ out_path, false });
                    }
                    else {
                        // encrypt: create output, then move original to backup
                        std::wstring out_path = f + L".enc";
                        FileEncryptor::encryptFileTo(f, out_path, usePwd);
                        // Securely remove the original plain file immediately after creating encrypted output
                        SecureDeleteFile(f);
                        processedList.push_back({ out_path, true });
                    }
                }
                catch (const std::exception& e) {
                    std::wstring werr = narrow_to_wstring(e.what());
                    std::wstring msg = std::wstring(L"处理文件失败：\n") + f + L"\n" + werr;
                    if (g_mainWnd) {
                        size_t len = msg.size();
                        wchar_t* buf = new wchar_t[len + 1];
                        wcscpy_s(buf, len + 1, msg.c_str());
                        PostMessageW(g_mainWnd, WM_APP_SHOW_ERROR, 0, (LPARAM)buf);
                    }
                    else {
                        MessageBoxW(NULL, msg.c_str(), tr(L"错误", L"Error").c_str(), MB_OK | MB_ICONERROR);
                    }
                }

                // 检查取消请求：完成当前文件后停止
                if (g_cancelRequested) {
                    // 取消：删除已生成的输出文件（无法恢复已被覆盖的原始文件）
                    for (auto it = processedList.rbegin(); it != processedList.rend(); ++it) {
                        try {
                            // 删除生成的输出
                            SecureDeleteFile(it->out);
                        }
                        catch (...) {
                            // 忽略单个错误
                        }
                    }
                    // 通知用户已取消并已清理生成的输出
                    std::wstring cancelMsg = g_langEnglish ? std::wstring(L"Cancelled: processed outputs removed (originals cannot be restored).") : std::wstring(L"已取消：已删除已生成的文件（原始文件无法恢复）。");
                    if (g_mainWnd) {
                        wchar_t* buf = new wchar_t[cancelMsg.size() + 1];
                        wcscpy_s(buf, cancelMsg.size() + 1, cancelMsg.c_str());
                        PostMessageW(g_mainWnd, WM_APP_SHOW_ERROR, 0, (LPARAM)buf);
                    }
                    break;
                }
            }
            // 未取消，已成功完成所有文件；无备份存在，输出文件已保留，原始已被安全删除（无 .bakproc）。
            // 如果需要，可在此处对生成的输出执行额外清理或日志记录。
            // 完成后设置状态为就绪
            std::wstring ready = L"就绪";
            if (g_mainWnd) {
                wchar_t* buf = new wchar_t[ready.size() + 1];
                wcscpy_s(buf, ready.size() + 1, ready.c_str());
                PostMessageW(g_mainWnd, WM_APP_SET_STATUS, 0, (LPARAM)buf);
            }
            else {
                SetStatusText(ready);
            }
            g_workerRunning = false;
            g_cancelRequested = false;
            });
        worker.detach();
        break;
    }

    case WM_SIZE: {
        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        SendMessageW(g_statusBar, WM_SIZE, 0, 0);

        RECT rcStatus;
        GetWindowRect(g_statusBar, &rcStatus);
        int statusHeight = rcStatus.bottom - rcStatus.top;
        int clientHeight = rcClient.bottom - rcClient.top - statusHeight;
        int buttonAreaY = (clientHeight - 40) / 2;
        if (buttonAreaY < 10) buttonAreaY = 10;

        HWND hBtn = GetWindow(hWnd, GW_CHILD);
        while (hBtn) {
            char className[32];
            GetClassNameA(hBtn, className, sizeof(className));
            if (strcmp(className, "BUTTON") == 0) {
                int id = GetDlgCtrlID(hBtn);
                if (id == 1) {
                    SetWindowPos(hBtn, NULL, (rcClient.right - 300) / 2, buttonAreaY, 140, 40, SWP_NOZORDER);
                }
                else if (id == 2) {
                    SetWindowPos(hBtn, NULL, (rcClient.right - 300) / 2 + 160, buttonAreaY, 140, 40, SWP_NOZORDER);
                }
                else if (id == 5) {
                    // language button top-right
                    SetWindowPos(hBtn, NULL, rcClient.right - 36, 8, 28, 24, SWP_NOZORDER);
                }
            }
            hBtn = GetWindow(hBtn, GW_HWNDNEXT);
        }
        break;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        // 语言切换按钮
        if (id == 5) {
            g_langEnglish = !g_langEnglish;
            UpdateLanguageUI();
            break;
        }

        // 处理取消按钮点击（id == 4）
        if (id == 4) {
            if (g_workerRunning) {
                g_cancelRequested = true;
                SetStatusText(g_langEnglish ? L"Cancel requested; finishing current file and rolling back..." : L"取消请求已发送，正在完成当前文件并回滚...");
            }
            break;
        }

        if (id == 1) { // 加密
            SetStatusText(L"请选择要加密的文件...");
            std::wstring input_path = OpenFileDialog();
            if (input_path.empty()) {
                SetStatusText(L"已取消");
                break;
            }

            std::string password;
            if (!ShowPasswordDialog(hWnd, password, true)) {
                SetStatusText(L"已取消");
                break;
            }

            try {
                SetStatusText(L"正在加密...");
                FileEncryptor::encryptFile(input_path, password);
                SetStatusText(L"加密成功！原文件已安全删除");
                MessageBoxW(hWnd, tr(L"加密成功！原文件已安全删除。", L"Encryption succeeded! Original file securely deleted.").c_str(), tr(L"成功", L"Success").c_str(), MB_OK | MB_ICONINFORMATION);
            }
            catch (const std::exception& e) {
                std::string err = e.what();
                std::wstring werr = narrow_to_wstring(err);
                SetStatusText(std::wstring(L"加密失败: ") + werr);
                MessageBoxW(hWnd, (std::wstring(tr(L"加密失败：\n", L"Encryption failed:\n")).c_str() + werr).c_str(), tr(L"错误", L"Error").c_str(), MB_OK | MB_ICONERROR);
            }
            break;
        }

        if (id == 2) { // 解密
            SetStatusText(L"请选择要解密的文件...");
            std::wstring input_path = OpenFileDialog();
            if (input_path.empty()) {
                SetStatusText(L"已取消");
                break;
            }

            if (input_path.size() < 4 || input_path.substr(input_path.size() - 4) != L".enc") {
                MessageBoxW(hWnd, tr(L"请选择 .enc 加密文件！", L"Please select a .enc encrypted file!").c_str(), tr(L"错误", L"Error").c_str(), MB_OK | MB_ICONERROR);
                SetStatusText(L"不是加密文件");
                break;
            }

            std::string password;
            if (!ShowPasswordDialog(hWnd, password, false)) {
                SetStatusText(L"已取消");
                break;
            }

            try {
                SetStatusText(L"正在解密...");
                FileEncryptor::decryptFile(input_path, password);
                SetStatusText(L"解密成功！加密文件已安全删除");
                MessageBoxW(hWnd, tr(L"解密成功！加密文件已安全删除。", L"Decryption succeeded! Encrypted file securely deleted.").c_str(), tr(L"成功", L"Success").c_str(), MB_OK | MB_ICONINFORMATION);
            }
            catch (const std::exception& e) {
                std::string err = e.what();
                std::wstring werr = narrow_to_wstring(err);
                SetStatusText(std::wstring(L"解密失败: ") + werr);
                MessageBoxW(hWnd, (std::wstring(tr(L"解密失败：\n", L"Decryption failed:\n")).c_str() + werr).c_str(), tr(L"错误", L"Error").c_str(), MB_OK | MB_ICONERROR);
            }
            break;
        }

        break;
    }

    case WM_DESTROY:
        if (g_hGuiFont) {
            DeleteObject(g_hGuiFont);
            g_hGuiFont = NULL;
        }
        // 释放 DirectWrite / Direct2D 资源
        if (g_pTextFormat) { g_pTextFormat->Release(); g_pTextFormat = NULL; }
        if (g_pDWriteFactory) { g_pDWriteFactory->Release(); g_pDWriteFactory = NULL; }
        if (g_pD2DFactory) { g_pD2DFactory->Release(); g_pD2DFactory = NULL; }
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
    case WM_APP_SHOW_ERROR: {
        LPWSTR msg = (LPWSTR)lParam;
        if (msg) {
            MessageBoxW(hWnd, msg, L"错误", MB_OK | MB_ICONERROR);
            delete[] msg;
        }
        break;
    }
    case WM_APP_SHOW_PROGRESS: {
        BOOL show = (wParam != 0);
        if (g_progressBar) {
            ShowWindow(g_progressBar, show ? SW_SHOW : SW_HIDE);
            if (show) {
                SendMessageW(g_progressBar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
                SendMessageW(g_progressBar, PBM_SETPOS, 0, 0);
                // 同时显示/隐藏取消按钮（只有在显示进度时才可取消）
                if (g_hCancel) {
                    ShowWindow(g_hCancel, show ? SW_SHOW : SW_HIDE);
                }
            }
        }
        break;
    }
    case WM_APP_UPDATE_PROGRESS: {
        int pos = (int)wParam;
        if (g_progressBar) {
            SendMessageW(g_progressBar, PBM_SETPOS, pos, 0);
        }
        break;
    }

    default:
        return DefWindowProcW(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ---------- 主入口：使用 WinMain (纯 GUI 应用，不创建控制台) ----------
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd) {
    (void)hPrevInstance; (void)lpCmdLine; // 未使用的参数

    // Check command line for password-host mode
    int argc = 0; PWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 0; i < argc; ++i) {
            if (wcscmp(argv[i], L"--password-host") == 0) {
                // Expect: --password-host <tmpfile> <confirm>
                if (i + 1 < argc) {
                    const wchar_t* tmpFile = argv[i+1];
                    int confirm = 0;
                    if (i + 2 < argc) confirm = _wtoi(argv[i+2]);
                    // Show password dialog (modal, no parent)
                    g_exe_dir = GetExeDirectoryW();
                    INT_PTR res = DialogBoxParamW(GetModuleHandle(NULL), MAKEINTRESOURCEW(IDD_PASSWORD_DIALOG), NULL, PasswordDlgProc, confirm ? 1 : 0);
                    if (res == IDOK) {
                        // write g_password (utf-8) to tmpFile
                        std::string out = g_password;
                        FILE* f = NULL;
                        _wfopen_s(&f, tmpFile, L"wb");
                        if (f) {
                            fwrite(out.data(), 1, out.size(), f);
                            fclose(f);
                            LocalFree(argv);
                            return 0; // success
                        }
                        else {
                            LocalFree(argv);
                            return 2; // cannot write
                        }
                    }
                    else {
                        LocalFree(argv);
                        return 1; // cancelled
                    }
                }
            }
        }
        LocalFree(argv);
    }

    g_exe_dir = GetExeDirectoryW();

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc = { 0 };
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"FileEncryptorGUI";
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(0, L"FileEncryptorGUI", L"文件加密工具",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 160,
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