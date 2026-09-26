// winui_password.cpp
//
// 历史上这里通过“再启动一个自身进程（--password-host）”来显示密码框，
// 导致密码框无法跟随主窗口的语言/主题切换，并会在任务栏多出一个窗口。
// 现在直接在本进程内调用 WinUI 3 风格的自绘对话框层。

#include "winui_password.h"
#include "winui_dialog.h"

bool ShowPasswordDialogWinUI(HWND parent, std::string& password, bool confirm,
                             const std::wstring& subtitle, bool showProtection,
                             UiProtection* ioProt, int remainTries) {
	return UiShowPassword(parent, confirm, password, subtitle,
	                      showProtection, ioProt, remainTries);
}
