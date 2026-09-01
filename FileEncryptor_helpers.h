#pragma once
#include <string>

// Return localized string: choose zh or en based on global flag
std::wstring tr(const wchar_t* zh, const wchar_t* en);
void UpdateLanguageUI();
