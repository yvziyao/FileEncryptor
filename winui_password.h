// Wrapper for WinUI password dialog. If WINUI_AVAILABLE is not defined, falls back to existing Win32 dialog.
#pragma once

#include <windows.h>
#include <string>

// 只以指针形式出现在参数里，前向声明即可，避免两个头文件互相包含
struct UiProtection;

// subtitle 非空时显示在标题下方（批量处理时用于指明当前文件）。
// showProtection/ioProt 仅用于加密（设置密码）；remainTries 仅用于解密（<0 表示不显示）。
bool ShowPasswordDialogWinUI(HWND parent, std::string& password, bool confirm,
                             const std::wstring& subtitle = std::wstring(),
                             bool showProtection = false,
                             UiProtection* ioProt = nullptr,
                             int remainTries = -1);
