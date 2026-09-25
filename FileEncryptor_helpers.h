#pragma once
#include <windows.h>
#include <string>

// 根据全局语言开关返回中文或英文字符串
std::wstring tr(const wchar_t* zh, const wchar_t* en);

// 语言切换后刷新主窗口标题、按钮文本与状态栏（全局生效）
void UpdateLanguageUI();

// 由 FileEncryptor.cpp 实现：更新主窗口底部状态文本
void SetStatusText(const std::wstring& text);
