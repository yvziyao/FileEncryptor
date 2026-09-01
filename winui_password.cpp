// Minimal shim: if WinUI/C++/WinRT available, implement WinUI dialog here.
// Otherwise, fall back to the existing Win32 password dialog implementation.

#include "winui_password.h"
#include "resource.h"
#include "winrt_init.h"
#include <vector>
#include <string>
#include <fstream>
#include <cstdio>
#include <cstdlib>

// Forward declaration of existing Win32 dialog function in FileEncryptor.cpp
// (ShowPasswordDialog is defined earlier)
bool ShowPasswordDialog(HWND hWnd, std::string& password, bool confirm);

bool ShowPasswordDialogWinUI(HWND parent, std::string& password, bool confirm) {
	// Implement independent host by launching this executable with a special flag
	wchar_t exePath[MAX_PATH] = {0};
	if (GetModuleFileNameW(NULL, exePath, MAX_PATH) == 0) {
		return ShowPasswordDialog(parent, password, confirm);
	}

	wchar_t tmpPath[MAX_PATH] = {0};
	if (GetTempPathW(MAX_PATH, tmpPath) == 0) {
		return ShowPasswordDialog(parent, password, confirm);
	}
	wchar_t tmpFile[MAX_PATH] = {0};
	if (GetTempFileNameW(tmpPath, L"pdh", 0, tmpFile) == 0) {
		return ShowPasswordDialog(parent, password, confirm);
	}

	std::wstring cmd = L"\"" + std::wstring(exePath) + L"\" --password-host \"" + std::wstring(tmpFile) + L"\" ";
	cmd += (confirm ? L"1" : L"0");
	// CreateProcess requires writable buffer
	std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
	cmdBuf.push_back(0);

	STARTUPINFOW si = { sizeof(si) };
	PROCESS_INFORMATION pi = {0};
	BOOL ok = CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
	if (!ok) {
		// fallback
		DeleteFileW(tmpFile);
		return ShowPasswordDialog(parent, password, confirm);
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD exitCode = 1;
	GetExitCodeProcess(pi.hProcess, &exitCode);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	if (exitCode == 0) {
		// read tmpFile (UTF-8)
		std::ifstream ifs;
		std::string result;
		std::wstring tmpFileW(tmpFile);
		// open with narrow path? use wifstream not portable; use _wfopen
		FILE* f = NULL;
		_wfopen_s(&f, tmpFileW.c_str(), L"rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long sz = ftell(f);
			fseek(f, 0, SEEK_SET);
			if (sz > 0) {
				result.resize(sz);
				fread(&result[0], 1, sz, f);
			}
			fclose(f);
			// delete tmp file
			DeleteFileW(tmpFileW.c_str());
			password = result;
			return true;
		}
		else {
			DeleteFileW(tmpFileW.c_str());
			return false;
		}
	}
	else {
		// cleanup
		DeleteFileW(tmpFile);
		return false;
	}
}
