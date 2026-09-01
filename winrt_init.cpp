#include "winrt_init.h"

// For independent host approach we do not use C++/WinRT here; provide stubs.

bool InitWinRT() { return false; }
void UninitWinRT() {}
bool ShowPasswordDialog_WinRT(HWND parent, std::string& password, bool confirm) { (void)parent; (void)password; (void)confirm; return false; }
