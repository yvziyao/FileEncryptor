// Wrapper for WinUI password dialog. If WINUI_AVAILABLE is not defined, falls back to existing Win32 dialog.
#pragma once

#include <windows.h>
#include <string>

bool ShowPasswordDialogWinUI(HWND parent, std::string& password, bool confirm);
