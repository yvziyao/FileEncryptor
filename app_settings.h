// app_settings.h - 应用设置模型与持久化（HKCU\Software\FileEncryptor）。
#pragma once

#include <windows.h>
#include <string>

enum class LangMode { Chinese = 0, English = 1 };
enum class ThemeMode { System = 0, Light = 1, Dark = 2 };
enum class OutDirMode { SourceDir = 0, FixedDir = 1, AskEveryTime = 2 };
enum class SecureDeleteMode { Off = 0, Overwrite1 = 1, Overwrite3 = 3 };
enum class CompletionMode { CloseWindow = 0, ShowDialog = 1 };
enum class StrengthMode { Fast = 0, Standard = 1, Secure = 2, Extreme = 3 };

struct AppSettings {
    LangMode         language = LangMode::Chinese;
    ThemeMode        theme = ThemeMode::System;
    OutDirMode       outDir = OutDirMode::SourceDir;
    std::wstring     fixedOutDir;
    SecureDeleteMode secureDelete = SecureDeleteMode::Overwrite3;
    CompletionMode   completion = CompletionMode::ShowDialog;
    StrengthMode     strength = StrengthMode::Standard;
};

// 全局单例
AppSettings& Settings();

void SettingsLoad();
void SettingsSave();

// 加密强度 -> PBKDF2 迭代次数
int StrengthIterations(StrengthMode s);
// 迭代次数 -> 最接近的强度档位（用于读取文件头后回显）
StrengthMode StrengthFromIterations(unsigned int iterations);

// 设置对话框使用的下拉选项（已本地化）
std::wstring StrengthName(StrengthMode s);
std::wstring ThemeName(ThemeMode t);
std::wstring OutDirName(OutDirMode d);
std::wstring SecureDeleteName(SecureDeleteMode d);
std::wstring CompletionName(CompletionMode c);
