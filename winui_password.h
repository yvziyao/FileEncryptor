// Wrapper for WinUI password dialog. If WINUI_AVAILABLE is not defined, falls back to existing Win32 dialog.
#pragma once

#include <windows.h>
#include <string>

// subtitle 非空时显示在标题下方（批量处理时用于指明当前文件）
bool ShowPasswordDialogWinUI(HWND parent, std::string& password, bool confirm,
                             const std::wstring& subtitle = std::wstring());
