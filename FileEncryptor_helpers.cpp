#include "FileEncryptor_helpers.h"
#include <Windows.h>
#include <commctrl.h>
#include <string>

extern bool g_langEnglish;
extern HWND g_hBtn1;
extern HWND g_hBtn2;
extern HWND g_hCancel;
extern HWND g_hLangBtn;
extern HWND g_statusBar;

std::wstring tr(const wchar_t* zh, const wchar_t* en) {
	return std::wstring(g_langEnglish ? en : zh);
}

void UpdateLanguageUI() {
	if (g_hBtn1) SendMessageW(g_hBtn1, WM_SETTEXT, 0, (LPARAM)(g_langEnglish ? L"Encrypt" : L"加密文件"));
	if (g_hBtn2) SendMessageW(g_hBtn2, WM_SETTEXT, 0, (LPARAM)(g_langEnglish ? L"Decrypt" : L"解密文件"));
	if (g_hCancel) SendMessageW(g_hCancel, WM_SETTEXT, 0, (LPARAM)(g_langEnglish ? L"Cancel" : L"取消"));
	if (g_hLangBtn) SendMessageW(g_hLangBtn, WM_SETTEXT, 0, (LPARAM)(g_langEnglish ? L"EN" : L"中"));
	if (g_statusBar) SendMessageW(g_statusBar, SB_SETTEXT, 0, (LPARAM)(g_langEnglish ? L"Ready" : L"就绪"));
}
