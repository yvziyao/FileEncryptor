#include "FileEncryptor_helpers.h"
#include <Windows.h>
#include <string>

extern bool g_langEnglish;
extern HWND g_mainWnd;
extern HWND g_hBtn1;
extern HWND g_hBtn2;
extern HWND g_hBtn3;
extern HWND g_hLangBtn;
extern HWND g_progressBar;

std::wstring tr(const wchar_t* zh, const wchar_t* en) {
	return std::wstring(g_langEnglish ? en : zh);
}

void UpdateLanguageUI() {
	if (g_mainWnd) {
		SetWindowTextW(g_mainWnd, tr(L"文件加密工具", L"File Encryptor").c_str());
	}
	if (g_hBtn1) {
		SetWindowTextW(g_hBtn1, tr(L"加密文件", L"Encrypt file").c_str());
		InvalidateRect(g_hBtn1, NULL, TRUE);
	}
	if (g_hBtn2) {
		SetWindowTextW(g_hBtn2, tr(L"解密文件", L"Decrypt file").c_str());
		InvalidateRect(g_hBtn2, NULL, TRUE);
	}
	if (g_hLangBtn) {
		// 这颗按钮现在是“设置”，画的是齿轮图标；文本仅用于无障碍朗读
		SetWindowTextW(g_hLangBtn, tr(L"设置", L"Settings").c_str());
		InvalidateRect(g_hLangBtn, NULL, TRUE);
	}
	// 安全删除按钮：漏了它的话，切到英文后按钮上还是中文
	// （按钮宽度是按中英文都算过的，所以换文字不会挤坏布局）
	if (g_hBtn3) {
		SetWindowTextW(g_hBtn3, tr(L"安全删除", L"Secure delete").c_str());
		InvalidateRect(g_hBtn3, NULL, TRUE);
	}
	// 状态栏文本也跟着切换（保持当前状态，回到“就绪”语义）
	SetStatusText(tr(L"就绪", L"Ready"));
	if (g_mainWnd) InvalidateRect(g_mainWnd, NULL, TRUE);
}
