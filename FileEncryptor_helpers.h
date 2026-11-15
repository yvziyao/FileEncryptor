#pragma once
#include <windows.h>
#include <string>

// 根据全局语言开关返回中文或英文字符串
std::wstring tr(const wchar_t* zh, const wchar_t* en);

// 语言切换后刷新主窗口标题与按钮文本（全局生效）
void UpdateLanguageUI();
