// app_settings.h - 应用设置模型与持久化（HKCU\Software\FileEncryptor）。
#pragma once

#include <windows.h>
#include <string>

enum class LangMode { Chinese = 0, English = 1 };
enum class ThemeMode { System = 0, Light = 1, Dark = 2 };
enum class OutDirMode { SourceDir = 0, FixedDir = 1, AskEveryTime = 2 };
enum class CompletionMode { CloseWindow = 0, ShowDialog = 1 };
enum class StrengthMode { Fast = 0, Standard = 1, Secure = 2, Extreme = 3 };

struct AppSettings {
    LangMode         language = LangMode::Chinese;
    ThemeMode        theme = ThemeMode::System;
    OutDirMode       outDir = OutDirMode::SourceDir;
    std::wstring     fixedOutDir;
    // 安全删除覆写次数，1-10。设置面板里是个数字输入框。
    int              secureDelete = 3;
    // 加密/解密后保留源文件（不删除）。与覆写次数分开，
    // 否则「不删除」这个选项在 1-10 的范围里没有位置。
    bool             keepSource = false;
    // 是否在资源管理器右键菜单中显示（关闭时会删除已注册的菜单项）
    bool             shellMenu = true;
    CompletionMode   completion = CompletionMode::ShowDialog;
    StrengthMode     strength = StrengthMode::Standard;
    // 禁用诱饵文件：加密时只写一个槽，体积约 1 倍。
    // 代价是文件大小会暴露「没有诱饵」，不可否认性减弱。
    bool             decoyDisabled = false;

    // ---- 随机密码生成器 ----
    // 密码框旁「随机生成」按钮用这些规则。位数 4-128，
    // 四类字符可分别勾选（全不选时回退为只用小写）。
    int              pwGenLength  = 20;
    bool             pwGenUpper   = true;
    bool             pwGenLower   = true;
    bool             pwGenDigits  = true;
    bool             pwGenSymbols = true;
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
std::wstring SecureDeleteName(int passes);
std::wstring CompletionName(CompletionMode c);
