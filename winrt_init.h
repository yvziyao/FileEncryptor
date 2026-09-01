// winrt_init.h - helper to initialize C++/WinRT and provide a small helper to show WinUI dialogs
#pragma once

#include <windows.h>
#include <string>

bool InitWinRT();
void UninitWinRT();

// Show a WinUI password dialog. Requires WINUI_AVAILABLE build.
bool ShowPasswordDialog_WinRT(HWND parent, std::string& password, bool confirm);
